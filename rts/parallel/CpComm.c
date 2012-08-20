/* 
 * Generalised RTE for parallel Haskells.
 *
 * File: ghc/rts/parallel/CpComm.h
 *
 *
 * 
 *
 *
 * Purpose: map generalised Comm.functions to processes 
 * communicating via the shared memory on a single machine.
 */


#if defined(PARALLEL_RTS)&&defined(USE_COPY) /* whole file */

/*
 * By including "Rts.h" here, we can use types GlobalTaskId, rtsBool, etc.
 * Normally, Rts.h should be included before including this file "MPSystem.h", 
 * but it is save to use here (protected from double-inclusion) and
 * brings useful other stuff (e.g. stdlib). 
 */
#include "Rts.h"
#include "MPSystem.h"
#include "RtsUtils.h" // utilities for error msg. etc.
#include "PEOpCodes.h" // message codes

#if !defined(mingw32_HOST_OS) /* Posix Version */

#include <semaphore.h> /* POSIX Semaphores */
#include <sys/types.h> /* Types for PID, pthreads, ... */
#if !defined(__APPLE__)
#include <sys/stat.h>
#endif
#include <sys/mman.h>  /* mmap */
#include <fcntl.h>     /* O_CREAT, ... */
#include <unistd.h>    /* fork(), ftruncate(), ... */
#include <errno.h>     /* error codes */
#include <string.h>    /* strlen(), strcat(), ... */
#include <stdio.h>
#include <stdlib.h>    /* atoi() */
#include <time.h>
#include <sys/time.h>  /* gettimeofday() */
#include <sys/wait.h>  /* wait() */

/*============*
 * IPC Global *
 *============*/
#define CPW_MAX_FILENAME_LENGTH  250

/*=============*
 * Error Codes *
 *=============*/
#define CPW_NOERROR         0
#define CPW_FILENAME_FAIL   1
#define CPW_SHM_FAIL        2
#define CPW_SEM_FAIL        3
#define CPW_SEM_WOULD_LOCK  4
#define CPW_SYNC_FAIL       5
#define CPW_FORK_FAIL       6
#define CPW_SEND_FAIL       7

/*============*
 * Semaphores *
 *============*/
#define CPW_SEM_CREAT_OFLGS (O_CREAT | O_EXCL | O_RDWR)
#define CPW_SEM_SFLGS (S_IWUSR | S_IRUSR)
#define CPW_SEM_PSHARED 1

/* Argh...! Unnamed POSIX semaphores currently are
   NOT implemented/supported in OS X
   Maybe? if defined(__APPLE__) for this and use
   unnamed semaphores on linux */
struct cpw_sem_tag {
  char  unique_filename[CPW_MAX_FILENAME_LENGTH];
  sem_t *semaphore;
};
typedef struct cpw_sem_tag cpw_sem_t;

static int cpw_sem_create(cpw_sem_t *sem, unsigned int val);
static int cpw_sem_wait(cpw_sem_t *sem);
static int cpw_sem_trywait(cpw_sem_t *sem);
static int cpw_sem_timedwait(cpw_sem_t *sem, struct timespec *abs_timeout);
static int cpw_sem_post(cpw_sem_t *sem);
static int cpw_sem_close(cpw_sem_t *sem);

/*==========*
 * Messages *
 *==========*/

struct cpw_msg_tag {
  nat      sender;
  OpCode   tag;
  int      length;
  StgWord *data;
};
typedef struct cpw_msg_tag cpw_msg_t;

/*===============*
 * Shared Memory *
 *===============*/
#define CPW_SHM_FPERM_FLGS (S_IWUSR | S_IRUSR) /* R/W for user */
#define CPW_SHM_CREAT_FLGS (O_CREAT | O_EXCL | O_RDWR) /* create excl with rw */
#define CPW_SHM_PROT_FLGS (PROT_READ | PROT_WRITE)
#define CPW_SHM_MMAP_FLGS (MAP_SHARED)

struct cpw_shm_slot_tag {
  cpw_msg_t               *addr;
  short                   is_hole;
  struct cpw_shm_slot_tag *next;
};
typedef struct cpw_shm_slot_tag cpw_shm_slot_t;

struct cpw_shm_tag {
  char		  unique_filename[CPW_MAX_FILENAME_LENGTH];
  size_t	  size;
  int		  file_descriptor;
  void		  *base;
  StgWord16       *status;
  StgWord16       *counter;
  cpw_shm_slot_t  **free_slots;
  cpw_shm_slot_t  **msgs_read;
  cpw_shm_slot_t  **msgs_write;
};
typedef struct cpw_shm_tag cpw_shm_t;

static int cpw_shm_create(void);

static void cpw_shm_check_errors(void);

static int cpw_shm_send_msg(nat toPE, OpCode tag, int length, long *data);
static int cpw_shm_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data);
static cpw_shm_slot_t* cpw_shm_acquire_slot(void);


static int cpw_shm_probe(void);
static int cpw_shm_probe_sys(void);

static int cpw_shm_free_pending_msg(void);
static int cpw_shm_close(cpw_shm_t *shm);

static void cpw_shm_debug_info(cpw_shm_t *shm);

static int cpw_self_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data);
static int cpw_self_probe(void);
static int cpw_self_probe_sys(void);

/*===========================*
 * Sync Point (not reusable) *
 *===========================*/
/* how long to wait for child processes at sync point  */
#define CPW_SYNC_TIMEOUT 10

struct cpw_sync_tag {
  unsigned short *count; /* How many nodes are waiting at sync point -> maps to counter in shm */
  cpw_sem_t mutex, wait;
};
typedef struct cpw_sync_tag cpw_sync_t;

static int cpw_sync_create(void);
static int cpw_sync_synchronize(cpw_sync_t *syn);
static int cpw_sync_close(cpw_sync_t *syn);


/*==================*
 * Utiliy Functions *
 *==================*/
static int cpw_mk_name(char *res);
static void parse_rts_flags(int* argc, char** argv);


/*=================*
 * <MAIN> MPSystem *
 *=================*/

/* global constants, declared in Parallel.h:
 *
 * nPEs   - nat: number of PEs in the parallel system
 * thisPE - nat: logical address of this PE btw. 1 and nPEs
 * IAmMainThread - rtsBool: indicating main PE (thisPE == 1)
 */
nat nPEs = 0;
nat thisPE = 0;
rtsBool IAmMainThread = rtsFalse;

int num_msgs = 0; /* How many messages can be stored in buffer */

cpw_shm_slot_t *stored_msgs = NULL; /* Linked List used to free buffer */

cpw_shm_t  shared_memory;        /* shared memory structure */
cpw_sync_t sync_point;           /* used to synchronize nodes */
cpw_sem_t  can_alloc, do_alloc;  /* synchronize buffer slots */
cpw_sem_t  *w_sems, *r_sems;     /* synchronize msg queues */

/**************************************************************
 * Startup and Shutdown routines (used inside ParInit.c only) */

/* MP_start starts up the node: 
 *   - connects to the MP-System used, 
 *   - determines wether we are main thread
 *   - starts up other nodes in case we are first and 
 *     the MP-System requires to spawn nodes from here.
 *     sets globar var.s:
 *      nPEs          - int: no. of PEs to expect/start
 *      IAmMainThread - rtsBool: whether this node is main PE
 * Parameters: 
 *     IN    argv  - char**: program arguments
 * Returns: Bool: success or failure
 */
rtsBool MP_start(int* argc, char** argv) {
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_Start: Starting CopyWay system... :)\n"));

  /* HACK:
     RtsFlags are parsed between calls to MP_start() and MP_sync(),
     but needed here!
     Either move the startup entirely to MP_sync() or move the parsing.
     For now just do an additional parse...
  */
  parse_rts_flags(argc,argv);

  /* first argument is number of PEs
     (handled by startup script in compiler/main/DriverPipeline.hs */
  ASSERT(argv && argv[1]);
  nPEs = (nat)atoi(argv[1]);
  if (nPEs == (nat)0)
    nPEs = 1;

  thisPE = 1;
  IAmMainThread = rtsTrue;

  /* set buffer sizes */
  num_msgs = RtsFlags.ParFlags.sendBufferSize * (int)nPEs;
  IF_PAR_DEBUG(mpcomm,
	       debugBelch(" Buffer: %i messages/%i PEs\n", num_msgs, nPEs));

  /* 
     init process
  */

  /* allocate memory for semaphores */
  w_sems = stgMallocBytes(sizeof(cpw_sem_t)*(int)nPEs, "cpwWrSems");
  r_sems = stgMallocBytes(sizeof(cpw_sem_t)*(int)nPEs, "cpwReSems");

  /* create shared memory*/
  if (cpw_shm_create() != CPW_NOERROR) {
    barf(" MPSystem CpWay: error creating shared memory!\n");
  }
  IF_PAR_DEBUG(mpcomm, cpw_shm_debug_info(&shared_memory));

  /* create sync point */
  cpw_sync_create();

  /* create semaphores */
  cpw_sem_create(&can_alloc, num_msgs);
  cpw_sem_create(&do_alloc, 1);
  int i;
  for (i = 0; i < (int)nPEs; i++) {
    cpw_sem_create(&w_sems[i], 1);
    cpw_sem_create(&r_sems[i], 0);
  }

  /* check errors before forking */
  cpw_shm_check_errors();

  /* start other processes */
  int child;
  for (i = 2; i <= (int)nPEs; i++) {
    child = fork();
		
    switch (child) {
    case -1:
      /* error while forking! */
      *shared_memory.status = CPW_FORK_FAIL;
      barf(" MPSystem CpWay: error starting other processes!\n");
      break;
    case 0:
      /* we are the child */
      thisPE = i;
      IAmMainThread = rtsFalse;
      return rtsTrue;
    default:
      break;
    }
  }

  return rtsTrue;
}

/* MP_sync synchronises all nodes in a parallel computation:
 *  sets global var.: 
 *    thisPE - GlobalTaskId: node's own task Id 
 *             (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_sync(void){
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_sync()\n"));

  /* check for fork errors */
  cpw_shm_check_errors();
  /* wait until all nodes ready */
  cpw_sync_synchronize(&sync_point); /* Fails after CPW_SYNC_TIMEOUT seconds */
  /* GO */
  return rtsTrue;
}

/* MP_quit disconnects current node from MP-System:
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 */

rtsBool MP_quit(int isError){
  IF_PAR_DEBUG(mpcomm,
	       debugBelch(" MP_quit()\n"));

  if (IAmMainThread) {
    /* send FINISH to other PEs */
    long data[1] = {isError};
    int i;
    for (i=2; i<=(int)nPEs; i++) {
      while (cpw_shm_send_msg(i, PP_FINISH, 1, data) != CPW_NOERROR);
    }

    /* wait for children to return */
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Waiting for children to return.\n"));
    while (1) {
      if(wait(NULL) < 0 && errno == ECHILD) {
	break;
      }
    }
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("All kids are safe home.\n"));
  }

  /* free unreceived messages */
  if (cpw_shm_probe()) {
    cpw_shm_free_pending_msg();
  }
  cpw_shm_slot_t *tmp = stored_msgs;
  while (stored_msgs != NULL) {
    tmp = stored_msgs->next;
    stgFree(stored_msgs->addr->data);
    stgFree(stored_msgs->addr);
    stgFree(stored_msgs);
    stored_msgs = tmp;
  }
  
  /* close semaphores */
  cpw_sem_close(&can_alloc);
  cpw_sem_close(&do_alloc);
  int i;
  for (i = 0; i < (int)nPEs; i++) {
    cpw_sem_close(&w_sems[i]);
    cpw_sem_close(&r_sems[i]);
  }
  stgFree(w_sems);
  stgFree(r_sems);
	
  /* close synchronize barrier */
  cpw_sync_close(&sync_point);
	
  /* close shared memory*/
  cpw_shm_close(&shared_memory);
	
  return rtsTrue;
}


/**************************************
 * Data Communication between nodes:  */

/* 
Data is always communicated in "packets" (buffering), 
but possibly trivial ones (if dyn.chunking turned off)

Needed functionality: */

/* a send operation for peer2peer communication: 
 * sends the included data (array of length length) to the indicated node
 * (numbered from 1 to the requested nodecount nPEs) with the given message
 * tag. Length 0 is allowed and leads to a message containing no payload data.
 * The send action may fail, in which case rtsFalse is returned, and the 
 * caller is expected to handle this situation.
 *
 * Parameters:
 *   IN node     -- destination node, number between 1 and nPEs
 *   IN tag      -- message tag
 *   IN data     -- array of long values to send out
 *   IN length   -- length of data array. Allowed to be zero (no data).
 * Returns:
 *   rtsBool: success or failure inside comm. subsystem
 */
rtsBool MP_send(int node, OpCode tag, long *data, int length){
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_send(%s)\n", getOpName(tag)));

  /* check for errors */
  cpw_shm_check_errors();

  /* check length */
  ASSERT(((uint)length) <= DATASPACEWORDS);
  
  /* send */
  switch (cpw_shm_send_msg(node, tag, length, data)) {
  case CPW_NOERROR:
    return rtsTrue;
  case CPW_SEND_FAIL:
  default:
    return rtsFalse;    
  }
}

/* - a blocking receive operation
 *   where system messages from main node have priority! 
 * Effect:
 *   A message is received from a peer. 
 *   Data stored in destination (maximum space given), and 
 *   opcode and sender fields are set.
 *   If no messages were waiting, the method blocks until a 
 *   message is available. If too much data arrives (> maxlength),
 *   the program stops with an error (resp. of higher levels).
 * 
 * Parameters: 
 *   IN  maxlength   -- maximum data length (security only)
 *   IN  destination -- where to unpack data (all of type long)
 *   OUT code   -- OpCode of message (aka message tag)
 *   OUT sender -- originator of this message 
 * Returns: 
 *   int: length of data received with message
 */
int MP_recv(STG_UNUSED int maxlength, long *destination, // IN
	    OpCode *code, nat *sender){       // OUT
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_recv()\n"));

  /* check for errors */
  cpw_shm_check_errors();

  /* receive message */
  int length;
  if (cpw_self_probe()) {
    /* might be a sys_msg in shm queue */
    if(!cpw_self_probe_sys() && cpw_shm_probe_sys()) {
      /* sys_msg now available in front of shm queue */
      cpw_shm_recv_msg(sender, code, &length, destination);
    }
    else {
      /* no sys_msg in shm queue; if in local queue, its in front pos */
      cpw_self_recv_msg(sender, code, &length, destination);
    }
  }
  else {
    /* move sys_msg to front */
    cpw_shm_probe_sys();
    cpw_shm_recv_msg(sender, code, &length, destination);
  }
  
  return length;
}       

/* - a non-blocking probe operation 
 * (unspecified sender, no receive buffers any more) 
 */
rtsBool MP_probe(void) {
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_probe()\n"));
  /* check for errors */
  cpw_shm_check_errors();

  if(cpw_shm_probe() || cpw_self_probe())
    return rtsTrue;
  else 
    return rtsFalse;
} 


/*============*
 * Semaphores *
 *============*/

/* Create named semaphore at address sem, with inital value of val */
static int cpw_sem_create(cpw_sem_t *sem, unsigned int val) {
  /* set unique name */
  if (cpw_mk_name(sem->unique_filename) != CPW_NOERROR) {
    errorBelch("Could not create a unique filename!\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
	
  /* even if unlikely, unlink here */
  sem_unlink(sem->unique_filename);
	
  /* create a new NAMED semaphore */
  sem->semaphore = sem_open(sem->unique_filename, 
			    CPW_SEM_CREAT_OFLGS, 
			    CPW_SEM_SFLGS, 
			    val);
  if (sem->semaphore == SEM_FAILED) {
    sysErrorBelch("Could not create semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
	
  sem_unlink(sem->unique_filename);

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("Created semaphore \"%s\"\n", sem->unique_filename));
  return CPW_NOERROR;
}

/* wait on semaphore, blocks until count > 0 */
static int cpw_sem_wait(cpw_sem_t *sem) {
  while (sem_wait(sem->semaphore) != 0) {
    if (errno == EINTR) continue;
    sysErrorBelch("Could not wait on semaphore: \"%s\"\n", 
		  sem->unique_filename);
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* wait on semaphore, if wait would block CPW_SEM_WOULD_LOCK is returned */
static int cpw_sem_trywait(cpw_sem_t *sem) {
  while (sem_trywait(sem->semaphore) != 0) {
    if (errno == EAGAIN) {
      return CPW_SEM_WOULD_LOCK;
    }
    else if (errno == EINTR) {
      continue;
    }
    /* Some other error while waiting */
    sysErrorBelch("Could not trywait on semaphore: \"%s\"\n", 
		  sem->unique_filename);
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* wait on semaphore, after abs_timeout time passed Fail */
static int cpw_sem_timedwait(cpw_sem_t *sem, struct timespec *abs_timeout) {
#if defined(__APPLE__) 
  /* not even this o_O ... srsly?... beware of hack */	
  struct timeval tv;
  struct timespec now;
  gettimeofday(&tv,NULL);
  now.tv_sec = tv.tv_sec;
  now.tv_nsec = tv.tv_usec * 1000;
  while(now.tv_sec < abs_timeout->tv_sec || 
	(now.tv_sec == abs_timeout->tv_sec &&
	 now.tv_nsec < abs_timeout->tv_nsec)) {		
    switch(cpw_sem_trywait(sem)) {
    case CPW_NOERROR:
      return CPW_NOERROR;
    case CPW_SEM_WOULD_LOCK:
      break;
    default:
      sysErrorBelch("Could not timewait on semaphore: \"%s\"\n",
		    sem->unique_filename);
      *shared_memory.status = CPW_SEM_FAIL;
      return CPW_SEM_FAIL;
    }
    usleep(100);
    gettimeofday(&tv,NULL);
    now.tv_sec = tv.tv_sec;
    now.tv_nsec = tv.tv_usec * 1000;
  }	
  return cpw_sem_trywait(sem);
#else
  if (sem_timedwait(sem->semaphore, abs_timeout) != 0) {
    sysErrorBelch("Could not timewait on semaphore: \"%s\"\n", 
		  sem->unique_filename);
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
#endif
}

/* Post on semaphore, increases count by one */
static int cpw_sem_post(cpw_sem_t *sem) {
  if(sem_post(sem->semaphore) != 0) {
    /* Some error while post on this semaphore */
    sysErrorBelch("Could not post on semaphore: \"%s\"\n", 
		  sem->unique_filename);
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* close semaphore */
static int cpw_sem_close(cpw_sem_t *sem) {
  if (sem_close(sem->semaphore) == -1) {
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Could not close semaphore: \"%s\"\n", 
		  sem->unique_filename));
    /* Not necessary to handle */
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/*===============*
 * Shared Memory *
 *===============*/

/* print debug information for shared memory */
static void cpw_shm_debug_info(cpw_shm_t *shm) {	
  debugBelch("#############################\n"
	     "      # Shared Memory Debug Info: #\n"
	     "      #############################\n"
	     "\n"
	     "      General Information:\n"
	     "      - unique filename: \"%s\"\n"
	     "      - size: %i bytes\n"
	     "      - base address: %p\n"
	     "      \n"
	     "      Structure Information:\n"
	     "      - status (@%p): %i\n"
	     "        (ALGNMT: %i, is aligned: %s)\n"
	     "      - counter (@%p): %i\n"
	     "        (ALGNMT: %i, is aligned: %s)\n"
	     "      - free_slots (@%p): %p\n"
	     "        (ALGNMT: %i, is aligned: %s)\n"
	     "      - msgs_read@%p:\n"
	     "        (ALGNMT: %i, is aligned: %s)\n"
	     "      - msgs_write@%p:\n"
	     "        (ALGNMT: %i, is aligned: %s)\n"
	     "\n", 
	     shm->unique_filename, 
	     (int)shm->size, 
	     shm->base, 
	     
	     shm->status, *shm->status, 
	     ALIGNMENT_WORD16, 
	     (long)shm->status % ALIGNMENT_WORD16 ? "FALSE" : "TRUE",
	     
	     shm->counter, *shm->counter,
	     ALIGNMENT_WORD16, 
	     (long)shm->counter % ALIGNMENT_WORD16 ? "FALSE" : "TRUE",
	     
	     shm->free_slots, *shm->free_slots,
	     ALIGNMENT_VOID_P, 
	     (long)shm->free_slots % ALIGNMENT_VOID_P ? "FALSE" : "TRUE",
	     
	     shm->msgs_read,
	     ALIGNMENT_VOID_P, 
	     (long)shm->msgs_read % ALIGNMENT_VOID_P ? "FALSE" : "TRUE",
	     
	     shm->msgs_write,
	     ALIGNMENT_VOID_P, 
	     (long)shm->msgs_write % ALIGNMENT_VOID_P ? "FALSE" : "TRUE");


  debugBelch("free message slots:\n");
  cpw_shm_slot_t *next_slot = *shm->free_slots;
  int num = 0;
  while (next_slot != NULL) {
    num++;
    debugBelch("- %i (@%p)\n"
	       "      | (ALGNMT: %i, is aligned: %s)\n"
	       "      +-> points to message @%p\n"
	       "        |  (ALGNMT: %i, is aligned: %s)\n"
	       "        +-> points to data @%p\n"
	       "              (ALGNMT: %i, is aligned: %s)\n", 
	       num, next_slot,
	       sizeof(cpw_shm_slot_t), 
	       (long)next_slot % sizeof(cpw_shm_slot_t) ? "FALSE" : "TRUE",
	       next_slot->addr,
	       sizeof(cpw_msg_t), 
	       (long)next_slot->addr % sizeof(cpw_msg_t) ? "FALSE" : "TRUE",
	       next_slot->addr->data,
#if SIZEOF_VOID_P == 8
	       ALIGNMENT_WORD64 , 
	       (long)next_slot->addr->data % ALIGNMENT_WORD64 ? "FALSE" : "TRUE"
#else 
#if SIZEOF_VOID_P == 4
	       ALIGNMENT_WORD32 , 
	       (long)next_slot->addr->data % ALIGNMENT_WORD32 ? "FALSE" : "TRUE"
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif
	       );
    next_slot = next_slot->next;
  }

  debugBelch("total of %i free slots\n\n", num);
  debugBelch("message queues:\n");
  int i; 
  for (i = 0; i < (int)nPEs; i++) {
    num = 0;
    cpw_shm_slot_t *slot = shm->msgs_read[i];
    while(slot != NULL && slot->is_hole == 0) {
      num++;
      printf("num is %i\n",num);
      slot = slot->next;
    }

    debugBelch("- msgs_read[%i] (@%p)\n"
	       "      | (ALGNMT: %i, is aligned: %s)\n"
	       "      +-> points to slot @%p\n"
	       "      | +-> is hole = %i\n"
	       "      +-> has %i message linked\n\n",
	       i,&shm->msgs_read[i],
	       ALIGNMENT_VOID_P, 
	       (long)&shm->msgs_read[i] % ALIGNMENT_VOID_P ? "FALSE" : "TRUE",
	       shm->msgs_read[i],  shm->msgs_read[i]->is_hole,
	       num);
 
    debugBelch("- msgs_write[%i] (@%p)\n"
	       "      | (ALGNMT: %i, is aligned: %s)\n"
	       "      +-> points to slot @%p\n"
	       "        +-> is hole = %i\n",
	       i,&shm->msgs_write[i],
	       ALIGNMENT_VOID_P, 
	       (long)&shm->msgs_write[i] % ALIGNMENT_VOID_P ? "FALSE" : "TRUE",
	       shm->msgs_write[i],shm->msgs_write[i]->is_hole);
  }
}

/* create and initialize shared memory */
static int cpw_shm_create()
{
  shared_memory.file_descriptor = -1;

  /* set unique name */
  if (cpw_mk_name(shared_memory.unique_filename) != CPW_NOERROR){
    errorBelch("Could not create a unique filename!\n");
    return CPW_SHM_FAIL;
  }
  /* calculate memory size */
  shared_memory.size = 
    ALIGNMENT_WORD16 + SIZEOF_WORD16 /* status */
    + SIZEOF_WORD16 /* counter, is aligned when status is aligned */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P /* freeslots */
    + sizeof(cpw_shm_slot_t) + sizeof(cpw_shm_slot_t) * num_msgs /* stack for free slots */
    + sizeof(cpw_shm_slot_t) * (int)nPEs /* and additional holes */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P * (int)nPEs /* root nodes for msg lists */
    + SIZEOF_VOID_P * (int)nPEs /* root nodes for msg lists */
    + sizeof(cpw_msg_t) + sizeof(cpw_msg_t) * num_msgs /* structure for msgs */
#if SIZEOF_VOID_P == 8
    + ALIGNMENT_WORD64 + SIZEOF_WORD64 * DATASPACEWORDS * num_msgs; /* message data */
#else 
#if SIZEOF_VOID_P == 4
  + ALIGNMENT_WORD32 + SIZEOF_WORD32 * DATASPACEWORDS * num_msgs; /* message data */
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif

  /* create shared memory region */
  shm_unlink(shared_memory.unique_filename); /* in unlikely case, unlink old */
  shared_memory.file_descriptor = 
    shm_open(shared_memory.unique_filename, 
	     CPW_SHM_CREAT_FLGS, CPW_SHM_FPERM_FLGS);
  
  if (shared_memory.file_descriptor == -1) {
    /* could not create shared memory */
    sysErrorBelch("Could not open shm object!\n");
    return CPW_SHM_FAIL;
  }
  if (ftruncate(shared_memory.file_descriptor, shared_memory.size) == -1) {
    /* could not set memory size */
    sysErrorBelch("Couldn't set shm size! (not enough memory?)\n");
    close(shared_memory.file_descriptor);
    shm_unlink(shared_memory.unique_filename);
    return CPW_SHM_FAIL;
  }
  /* map shm object in own address space */
  shared_memory.base = mmap(NULL,                          /* no pref address */
			    shared_memory.size,            /* size */
			    CPW_SHM_PROT_FLGS,             /* flags */
			    CPW_SHM_MMAP_FLGS, 
			    shared_memory.file_descriptor, /* shm object */
			    0                              /* no offset */
			    );
  if (shared_memory.base == (void *) MAP_FAILED){
    /* could not map shared memory in own adress space */
    sysErrorBelch("Couldn't mmap shm object! (not enough memory?)\n");
    close(shared_memory.file_descriptor);
    shm_unlink(shared_memory.unique_filename);
    return CPW_SHM_FAIL;
  }
  
  /* closing, shared memory object still exists until munmap! 
     But safer in case of crash...*/
  close(shared_memory.file_descriptor);
  shared_memory.file_descriptor = -1;
  shm_unlink(shared_memory.unique_filename);
	
  /* set status */
  size_t status_unaligned = (size_t) (shared_memory.base);
  shared_memory.status  = 
    (StgWord16 *) (status_unaligned + ALIGNMENT_WORD16 - 
		   (status_unaligned % ALIGNMENT_WORD16));
  *shared_memory.status = CPW_NOERROR;
	
  /* set counter */
  shared_memory.counter = shared_memory.status + 1;
  *shared_memory.counter = 0;
	
  /* generate msg slots */
  cpw_shm_slot_t *free_list  = NULL;
  size_t slots_unaligned = (size_t) shared_memory.base 
    + ALIGNMENT_WORD16 + SIZEOF_WORD16 * 2; /* status & counter */
  shared_memory.free_slots = 
    (cpw_shm_slot_t **) ( slots_unaligned
			  + ALIGNMENT_VOID_P 
			  - (slots_unaligned % ALIGNMENT_VOID_P));

  /* start memory address of messages */
  size_t msg_base_unaligned = (size_t) shared_memory.base 
    + ALIGNMENT_WORD16 + SIZEOF_WORD16 * 2 /* status & counter */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P /* free_slots */
    + sizeof(cpw_shm_slot_t) * (num_msgs+1+(int)nPEs) /* slots */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P * (int)nPEs * 2 /* r/w pointer */
    ;
  cpw_msg_t *msg_base = 
    (cpw_msg_t *) (msg_base_unaligned 
		   + sizeof(cpw_msg_t) 
		   - (msg_base_unaligned % sizeof(cpw_msg_t))); 


  /* start memory address of data for messages */
  size_t data_base_unaligned = (size_t) shared_memory.base 
    + ALIGNMENT_WORD16 + SIZEOF_WORD16 * 2 /* status & counter */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P /* free_slots */
    + sizeof(cpw_shm_slot_t) * (num_msgs+1+(int)nPEs) /* slots */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P * (int)nPEs * 2 /* r/w pointer */
    + sizeof(cpw_msg_t) + sizeof(cpw_msg_t) * num_msgs; /* messages */
  StgWord *data_base = (StgWord *) (data_base_unaligned 
#if SIZEOF_VOID_P == 8
				      + ALIGNMENT_WORD64 - data_base_unaligned % ALIGNMENT_WORD64 /* message data */
#else 
#if SIZEOF_VOID_P == 4
				      + ALIGNMENT_WORD32 - data_base_unaligned % ALIGNMENT_WORD32 /* message data */
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif
				      );
 
  size_t slot_base_unaligned = (size_t) shared_memory.base
    + ALIGNMENT_WORD16 + SIZEOF_WORD16 /* status */
    + SIZEOF_WORD16 /* counter, is aligned when status is aligned */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P /* freeslots */
    ;

  cpw_shm_slot_t *slot_base = 
    (cpw_shm_slot_t *) (slot_base_unaligned 
			+ sizeof(cpw_shm_slot_t) 
			- (slot_base_unaligned % sizeof(cpw_shm_slot_t)));


  /* holes */
  cpw_shm_slot_t *hole_base  = slot_base + num_msgs;

  int i;
  for (i = 0; i < num_msgs; i++) {
    /* next slot */
    cpw_shm_slot_t *next = slot_base + i;
    
    /* set next slot in list */
    next->next = free_list;
    /* set mem-address of message for this slot */
    next->addr = msg_base + i;
    /* set mem-address of data for this message */
    next->addr->data = data_base + i*DATASPACEWORDS;

    free_list = next;
  }
  *shared_memory.free_slots = free_list;
	
	
  /* initialize msg queues */
  size_t msgs_r_unaligned = (size_t) shared_memory.base
    + ALIGNMENT_WORD16 + SIZEOF_WORD16 /* status */
    + SIZEOF_WORD16 /* counter, is aligned when status is aligned */
    + ALIGNMENT_VOID_P + SIZEOF_VOID_P /* freeslots */
    + sizeof(cpw_shm_slot_t) * (num_msgs+1+(int)nPEs) /* stack for free slots */
    ;
  shared_memory.msgs_read = 
    (cpw_shm_slot_t **)(msgs_r_unaligned 
			+ ALIGNMENT_VOID_P 
			- (msgs_r_unaligned % ALIGNMENT_VOID_P));
  shared_memory.msgs_write = shared_memory.msgs_read + (int)nPEs;
  for (i = 0; i < (int)nPEs; i++) {
    shared_memory.msgs_read[i] = hole_base+i;
    shared_memory.msgs_write[i] = hole_base+i;
    shared_memory.msgs_read[i]->is_hole = 1;
    shared_memory.msgs_read[i]->next = NULL;
    shared_memory.msgs_read[i]->addr = NULL;
  }

  /* finish */
  return CPW_NOERROR;
}

/* try to send a message */
static int cpw_shm_send_msg(nat toPE, OpCode tag, int length, long *data) {	
  cpw_shm_slot_t *free_slot = NULL;
  
  /* can we get a free slot? */
  if (cpw_sem_trywait(&can_alloc) == CPW_SEM_WOULD_LOCK) {
		
    /* no, try to store a message */
    free_slot = cpw_shm_acquire_slot(); 
    
    if (free_slot == NULL) {
      /* could not store message, sending failed */
      return CPW_SEND_FAIL;
    }
  }
  if (free_slot == NULL) {//	cpw_sem_wait(can_alloc);
    cpw_sem_wait(&do_alloc);
	
    free_slot = *shared_memory.free_slots;
    *shared_memory.free_slots = free_slot->next;
	
    cpw_sem_post(&do_alloc);
  }
	
  /* copy message */
  free_slot->is_hole = 1; /* free_slot to become new hole */
  free_slot->next = NULL;
  free_slot->addr->sender = thisPE;
  free_slot->addr->tag    = tag;
  free_slot->addr->length = length;
  memcpy(free_slot->addr->data, data, sizeof(long) * length);


  IF_PAR_DEBUG(mpcomm,
	       debugBelch(" sending msg to %i, tag %i\n", toPE, 
			  free_slot->addr->tag));
	
  /* enqueue message */
  toPE--;
  /* wait for other write to finish */
  cpw_sem_wait(&w_sems[toPE]); 

  cpw_shm_slot_t *hole = shared_memory.msgs_write[toPE];
  hole->next = free_slot;
  hole->addr = free_slot->addr;
  free_slot->addr = NULL;
  shared_memory.msgs_write[toPE] = free_slot; /* new hole */
  hole->is_hole = 0;
  /* finish */
  cpw_sem_post(&w_sems[toPE]);

  /* wake receiver */
  cpw_sem_post(&r_sems[toPE]);
  return CPW_NOERROR;
}

/* try to receive a message */
static int cpw_shm_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data) {
  int me = thisPE - 1;
	
  /* wait until msgs there */
  cpw_sem_wait(&r_sems[me]);


  cpw_shm_slot_t *used_slot = shared_memory.msgs_read[me];
  shared_memory.msgs_read[me] = used_slot->next;

  /* copy data */
  *fromPE = used_slot->addr->sender;
  *tag    = used_slot->addr->tag;
  *length = used_slot->addr->length;
  memcpy(data, used_slot->addr->data, sizeof(long) * used_slot->addr->length);

	
  IF_PAR_DEBUG(mpcomm,
	       debugBelch(" got a message from %i, tag = %i\n", *fromPE, *tag));

  /* freeing slot */
  cpw_sem_wait(&do_alloc);
  used_slot->next = *shared_memory.free_slots;
  *shared_memory.free_slots = used_slot;
  cpw_sem_post(&do_alloc);
  /* wake waiting senders */
  cpw_sem_post(&can_alloc);

  return CPW_NOERROR;	
}

static cpw_shm_slot_t* cpw_shm_acquire_slot() {
  int me = thisPE - 1;
	
  if(cpw_sem_trywait(&r_sems[me]) == CPW_SEM_WOULD_LOCK)
    return NULL;

  /* store message for later use */
  cpw_shm_slot_t *new_node = 
    (cpw_shm_slot_t *)stgMallocBytes((int)sizeof(cpw_shm_slot_t), "StoredNode");
  cpw_msg_t *new_msg  = (cpw_msg_t *)stgMallocBytes((int)sizeof(cpw_msg_t),"StoredMsg");
  StgWord *new_data = (StgWord *)stgMallocBytes((int)(sizeof(StgWord)*DATASPACEWORDS),"StoredData");
  new_node->addr = new_msg;
  new_node->next = NULL;
  new_msg->data  = new_data;


  cpw_shm_slot_t *used_slot = shared_memory.msgs_read[me];
  shared_memory.msgs_read[me] = used_slot->next;

  /* copy data */
  new_msg->sender = used_slot->addr->sender;
  new_msg->tag    = used_slot->addr->tag;
  new_msg->length = used_slot->addr->length;
  memcpy(new_data, used_slot->addr->data, sizeof(long) * used_slot->addr->length);

  if (stored_msgs == NULL) {
    stored_msgs = new_node;
  }
  else {
    cpw_shm_slot_t *insert = stored_msgs;
    while(insert->next != NULL) {
      insert = insert->next;
    }
    insert->next = new_node;
  }

  return used_slot;
}

static int cpw_self_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data) {
  cpw_shm_slot_t *used_slot = stored_msgs;
  stored_msgs = used_slot->next;
 

  /* copy data */
  *fromPE = used_slot->addr->sender;
  *tag    = used_slot->addr->tag;
  *length = used_slot->addr->length;
  memcpy(data, used_slot->addr->data, sizeof(long) * used_slot->addr->length);
	
  stgFree(used_slot->addr->data);
  stgFree(used_slot->addr);
  stgFree(used_slot);
  
  return CPW_NOERROR;	
}

/* test if messages available */
static int cpw_shm_probe() {
  switch(cpw_sem_trywait(&r_sems[thisPE-1])) {
  case CPW_NOERROR:
    cpw_sem_post(&r_sems[thisPE-1]);
    return 1;
  default:
    return 0;
  }
}

/* test if a sys msg exists in shm queue 
 if so sysmsg is moved to front of queue */
static int cpw_shm_probe_sys(void) {
  switch(cpw_sem_trywait(&r_sems[thisPE-1])) {
  case CPW_NOERROR:
  {
    cpw_shm_slot_t *queue_front = shared_memory.msgs_read[thisPE-1];
    cpw_shm_slot_t *pre_sys_slot = NULL;
    cpw_shm_slot_t *sys_slot = queue_front;

    while(!ISSYSCODE(sys_slot->addr->tag)) {
      pre_sys_slot = sys_slot;
      sys_slot = sys_slot->next;
      if(sys_slot->is_hole == 1) {
	cpw_sem_post(&r_sems[thisPE-1]); /* no msg taken */
	return 0; /* no sys msgs found */
      }
    }
    /* possibly move sysmsg to front */
    if(pre_sys_slot != NULL) {
      pre_sys_slot->next = sys_slot->next;
      sys_slot->next = queue_front;
      shared_memory.msgs_read[thisPE-1] = sys_slot;
    }

    cpw_sem_post(&r_sems[thisPE-1]); /* no msg taken */
    /* sys_msg now in front */
    return 1;
  }
  default:
    return 0;
  }
}

static int cpw_self_probe() {
  return (stored_msgs != NULL);
}

/* test if a sys msg exists in local queue 
 if so sysmsg is moved to front of queue */
static int cpw_self_probe_sys(void) {
  switch(stored_msgs != NULL) {
  case 1:
    {
      cpw_shm_slot_t *queue_front = stored_msgs;
      cpw_shm_slot_t *pre_sys_slot = NULL;
      cpw_shm_slot_t *sys_slot = queue_front;

      while(!ISSYSCODE(sys_slot->addr->tag)) {
	pre_sys_slot = sys_slot;
	sys_slot = sys_slot->next;
	if(sys_slot == NULL) {
	  return 0; /* no sys msgs found */
	}
      }
      /* possibly move sysmsg to front */
      if(pre_sys_slot != NULL) {
	pre_sys_slot->next = sys_slot->next;
	sys_slot->next = queue_front;
	stored_msgs = sys_slot;
      }
      /* sys_msg now in front */
      return 1;
    }
  default:
    return 0;
  }
}

static int cpw_shm_free_pending_msg() {
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("freeing pending messages\n"));
  int me = thisPE - 1;
	
  while(cpw_sem_trywait(&r_sems[me]) == CPW_NOERROR) {
    cpw_shm_slot_t *used_slot = shared_memory.msgs_read[me];
    shared_memory.msgs_read[me] = used_slot->next;

    /* freeing slot */
    cpw_sem_wait(&do_alloc);
    used_slot->next = *shared_memory.free_slots;
    *shared_memory.free_slots = used_slot;
    cpw_sem_post(&do_alloc);
    /* wake waiting senders */
    cpw_sem_post(&can_alloc);
  }

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("done freeing pending messages\n"));
  return CPW_NOERROR;	
}

static int cpw_shm_close(cpw_shm_t *shm) {
  if (munmap(shm->base, shm->size) == -1) {
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Couldn't unmap shared memory\n"));
    return CPW_SHM_FAIL;
  }
  return CPW_NOERROR;
}

static void cpw_shm_check_errors() {
  switch (*shared_memory.status) 
  {
  case CPW_NOERROR:
    return;
  case CPW_FILENAME_FAIL:
    barf("MPSystem CpWay: unique filename error!\n");
  case CPW_SHM_FAIL:
    barf("MPSystem CpWay: shared memory error!\n");
  case CPW_SEM_FAIL:
    barf("MPSystem CpWay: semaphore error!\n");
  case CPW_SYNC_FAIL:
    barf("MPSystem CpWay: barrier error!\n");
  case CPW_FORK_FAIL:
    barf("MPSystem CpWay: fork error!\n");
  default:
    return;
  }
}

/*===========================*
 * Sync Point (not reusable) *
 *===========================*/
static int cpw_sync_create() {
  sync_point.count = shared_memory.counter;
  *(sync_point.count) = 0;
	
  if(cpw_sem_create(&sync_point.mutex, 1) != CPW_NOERROR ||
     cpw_sem_create(&sync_point.wait, 0) != CPW_NOERROR) 
    {
      *shared_memory.status = CPW_SYNC_FAIL;
      return CPW_SYNC_FAIL;
    }
  return CPW_NOERROR;
}

static int cpw_sync_synchronize(cpw_sync_t *syn) {
	
  if(cpw_sem_wait(&syn->mutex) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
  *(syn->count) = *(syn->count) + 1;
	
  if(*(syn->count) == (int)nPEs) {
    /* all nodes checked in */
    int i;
    for(i = 0; i < (int)nPEs; i++) {
      if(cpw_sem_post(&syn->wait) != CPW_NOERROR) {
	return CPW_SYNC_FAIL;
      }
    }
  }
	
  if(cpw_sem_post(&syn->mutex) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
	
  /* If not all processes started this semaphore will block
     forever. HACK timedwait*/
  struct timespec timeout;
  struct timeval tv;
  gettimeofday(&tv,NULL);
  timeout.tv_sec = tv.tv_sec;
  timeout.tv_nsec = tv.tv_usec * 1000;
  timeout.tv_sec += CPW_SYNC_TIMEOUT;
	
  if(cpw_sem_timedwait(&syn->wait, &timeout) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
		
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("GO!\n"));
	
  return CPW_NOERROR;
}

static int cpw_sync_close(cpw_sync_t *syn) {
  int em = cpw_sem_close(&syn->mutex);
  int ew = cpw_sem_close(&syn->wait);
	
  if (em != CPW_NOERROR || ew != CPW_NOERROR) {
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Couldn't close sync point.\n"));
    return CPW_SYNC_FAIL;
  }
  return CPW_NOERROR;
}

/*==================*
 * Utiliy Functions *
 *==================*/

/* 
 * Make unique name to be used in shm_open or _named_ semaphores.
 * Returns pointer to unique name which has to be freed!       
 */
static int cpw_mk_name(char *res) {
  static unsigned int next_unique = 0;
	
  char *s_magic = "eden"; /* TODO use executable name here? */
	
  ASSERT(1 + strlen(s_magic) + 12 + 1 <= CPW_MAX_FILENAME_LENGTH);
	
  if (sprintf(res, "/%s%08x%04x", s_magic, getpid(), next_unique++) < 0) {
    return CPW_FILENAME_FAIL;
  }
	
  return CPW_NOERROR;
}

static void parse_rts_flags(int* argc, char** argv) {
  int my_argc = (*argc)-1;
  char *my_argv[my_argc];
  my_argv[0] = stgMallocBytes(strlen(argv[0])+1, "argv0");
  strcpy(my_argv[0], argv[0]);
  int k;
  for (k=2; k < *argc; k++) {
    my_argv[k-1] = stgMallocBytes(strlen(argv[k])+1, "argv");
    strcpy(my_argv[k-1], argv[k]); 
  }
  initRtsFlagsDefaults();

  setupRtsFlags(&my_argc, my_argv, &rts_argc, rts_argv);

  for (k=0; k < my_argc; k++) stgFree(my_argv[k]);
}

#else  /* not win32 */

#include <Windows.h>
#include <string.h>    /* strlen(), strcat(), ... */
#include <stdio.h>
#include <stdlib.h>    /* atoi() */
#include <time.h>


/*============*
 * IPC Global *
 *============*/
#define CPW_MAX_FILENAME_LENGTH  250

/*=============*
 * Error Codes *
 *=============*/
#define CPW_NOERROR         0
#define CPW_FILENAME_FAIL   1
#define CPW_SHM_FAIL        2
#define CPW_SEM_FAIL        3
#define CPW_SEM_WOULD_LOCK  4
#define CPW_SYNC_FAIL       5
#define CPW_FORK_FAIL       6
#define CPW_SEND_FAIL       7

/*============*
 * Semaphores *
 *============*/
struct cpw_sem_tag {
  HANDLE semaphore;
};
typedef struct cpw_sem_tag cpw_sem_t;

static int cpw_sem_create(cpw_sem_t *sem, unsigned int val);
static int cpw_sem_wait(cpw_sem_t *sem);
static int cpw_sem_trywait(cpw_sem_t *sem);
static int cpw_sem_timedwait(cpw_sem_t *sem, unsigned int abs_timeout);
static int cpw_sem_post(cpw_sem_t *sem);
static int cpw_sem_close(cpw_sem_t *sem);

/*==========*
 * Messages *
 *==========*/

struct cpw_msg_off_tag {
  nat      sender;
  OpCode   tag;
  int      length;
  size_t   data;
};
typedef struct cpw_msg_off_tag cpw_msg_off_t;

struct cpw_msg_tag {
  nat      sender;
  OpCode   tag;
  int      length;
  StgWord *data;
};
typedef struct cpw_msg_tag cpw_msg_t;

/*===========================*
 * Sync Point (not reusable) *
 *===========================*/
/* how long to wait for child processes at sync point  */
#define CPW_SYNC_TIMEOUT 10

struct cpw_sync_tag {
  unsigned short count; /* How many nodes are waiting at sync point */
  cpw_sem_t mutex, wait;
};
typedef struct cpw_sync_tag cpw_sync_t;

static int cpw_sync_create(cpw_sync_t *syn);
static int cpw_sync_synchronize(cpw_sync_t *syn);
static int cpw_sync_close(cpw_sync_t *syn);

/*===============*
 * Shared Memory *
 *===============*/

struct cpw_shm_slot_off_tag {
  size_t addr;
  short  is_hole;
  size_t next;
};
typedef struct cpw_shm_slot_off_tag cpw_shm_slot_off_t;

struct cpw_shm_slot_tag {
  cpw_msg_t               *addr;
  short                   is_hole;
  struct cpw_shm_slot_tag *next;
};
typedef struct cpw_shm_slot_tag cpw_shm_slot_t;

struct cpw_shm_tag {
  HANDLE          hShm;
  size_t	  size;

  size_t          slots_start;
  size_t          msgs_start;
  size_t          data_start;

  void		  *base;
  StgWord16       *status;
  StgWord16       *counter;

  cpw_sync_t		*hSync;
  cpw_sem_t		*hDo_alloc;
  cpw_sem_t		*hCan_alloc;
  cpw_sem_t		*hW_sems;
  cpw_sem_t		*hR_sems;


  size_t *free_slots;
  size_t  *msgs_read;
  size_t  *msgs_write;
};
typedef struct cpw_shm_tag cpw_shm_t;

static int cpw_shm_create(void);
static int cpw_shm_init(void);

static void cpw_shm_check_errors(void);

static int cpw_shm_send_msg(nat toPE, OpCode tag, int length, long *data);
static int cpw_shm_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data);
static size_t cpw_shm_acquire_slot(void);


static int cpw_shm_probe(void);
static int cpw_shm_probe_sys(void);

static int cpw_shm_free_pending_msg(void);
static int cpw_shm_close(cpw_shm_t *shm);

static void cpw_shm_debug_info(cpw_shm_t *shm);

static int cpw_self_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data);
static int cpw_self_probe(void);
static int cpw_self_probe_sys(void);




/*==================*
 * Utiliy Functions *
 *==================*/
static int cpw_mk_name(char *res);
static void parse_rts_flags(int* argc, char** argv);
static char* cpw_mk_argv_string(int argc, char ** argv);


/*=================*
 * <MAIN> MPSystem *
 *=================*/

/* global constants, declared in Parallel.h:
 *
 * nPEs   - nat: number of PEs in the parallel system
 * thisPE - nat: logical address of this PE btw. 1 and nPEs
 * IAmMainThread - rtsBool: indicating main PE (thisPE == 1)
 */
nat nPEs = 0;
nat thisPE = 0;
rtsBool IAmMainThread = rtsFalse;

int num_msgs = 0; /* How many messages can be stored in buffer */

cpw_shm_slot_t *stored_msgs = NULL; /* Linked List used to free buffer */

cpw_shm_t  shared_memory;        /* shared memory structure */

char *args; /* Save string for argv in MPStart to use it in MPSync */
char buffer[256];

/**************************************************************
 * Startup and Shutdown routines (used inside ParInit.c only) */

/* MP_start starts up the node: 
 *   - connects to the MP-System used, 
 *   - determines wether we are main thread
 *   - starts up other nodes in case we are first and 
 *     the MP-System requires to spawn nodes from here.
 *     sets globar var.s:
 *      nPEs          - int: no. of PEs to expect/start
 *      IAmMainThread - rtsBool: whether this node is main PE
 * Parameters: 
 *     IN    argv  - char**: program arguments
 * Returns: Bool: success or failure
 */
rtsBool MP_start(int* argc, char** argv) {
	//printf("MP_Start: Starting CopyWay system... :)\n");

  /* HACK:
     RtsFlags are parsed between calls to MP_start() and MP_sync(),
     but needed here!
     Either move the startup entirely to MP_sync() or move the parsing.
     For now just do an additional parse...
  */
//  parse_rts_flags(argc,argv);
  /* first argument is number of PEs
     (handled by startup script in compiler/main/DriverPipeline.hs */
  ASSERT(argv && argv[1]);
  nPEs = (nat)atoi(argv[1]);
  if (nPEs == (nat)0)
    nPEs = 1;

  args = cpw_mk_argv_string(*argc, argv);
 
  /* is Environment Var set? then we are a child */
  buffer[0] = '0';
  buffer[1] = '\0';
  GetEnvironmentVariable("IsEdenChild", buffer, 256);
  sscanf(buffer, "%i", &thisPE);
  //printf("IsEdenChild = %i\n", thisPE);
 if(thisPE == 0) {
    thisPE = 1;
    IAmMainThread = rtsTrue;
  }

  return rtsTrue;
}

/* MP_sync synchronises all nodes in a parallel computation:
 *  sets global var.: 
 *    thisPE - GlobalTaskId: node's own task Id 
 *             (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_sync(void){

  /* set buffer sizes */
  num_msgs = RtsFlags.ParFlags.sendBufferSize * (int)nPEs;
  //printf(" Buffer: %i messages/%i PEs\n", num_msgs, nPEs);

  /* 
     init process
  */

  if(thisPE == 1) {
    if (cpw_shm_create() != CPW_NOERROR) {
      barf(" MPSystem CpWay: error creating shared memory!\n");
    }
    STARTUPINFO si;
    GetStartupInfo(&si);
    PROCESS_INFORMATION *pi = malloc(sizeof(PROCESS_INFORMATION) *(nPEs -1));
    int i;
    for (i = 2; i <= (int)nPEs; i++) {
			
      /* start other processes */
			
      sprintf(buffer, "%u", i);
      SetEnvironmentVariable("IsEdenChild", buffer);
      sprintf(buffer, "%lu", (DWORD) shared_memory.hShm);
      SetEnvironmentVariable("SHMHandle", buffer);		
      char *argsTmp = stgMallocBytes(strlen(args)+1, "args");
	  strcpy(argsTmp, args);
	  
      CreateProcess(NULL, argsTmp, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi[i-2]);
      
	  free(argsTmp);
    }
	free(args);
  }
  else {
    GetEnvironmentVariable("SHMHandle", buffer, 256);
    sscanf(buffer, "%lu", &shared_memory.hShm);

    if (cpw_shm_init() != CPW_NOERROR) {
      barf(" MPSystem CpWay: error init shared memory!\n");
    }
  }

	cpw_shm_debug_info(&shared_memory);
  /* check errors before forking */
  cpw_shm_check_errors();
  //printf("MP_sync()\n");

  /* check for fork errors */
  cpw_shm_check_errors();
  /* wait until all nodes ready */
  cpw_sync_synchronize(shared_memory.hSync); /* Fails after CPW_SYNC_TIMEOUT seconds */
  /* GO */
  return rtsTrue;
}

/* MP_quit disconnects current node from MP-System:
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 */

rtsBool MP_quit(int isError){
  //printf(" MP_quit()\n");

  if (IAmMainThread) {
    /* send FINISH to other PEs */
    long data[1] = {isError};
    int i;
    for (i=2; i<=(int)nPEs; i++) {
      while (cpw_shm_send_msg(i, PP_FINISH, 1, data) != CPW_NOERROR);
    }

    /* wait for children to return */
    //printf("Waiting for children to return.\n");
    /* TODO */
    //printf("All kids are safe home.\n");
  }

  /* free unreceived messages */
  if (cpw_shm_probe()) {
    cpw_shm_free_pending_msg();
  }
  cpw_shm_slot_t *tmp = stored_msgs;
  while (stored_msgs != NULL) {
    tmp = stored_msgs->next;
    stgFree(stored_msgs->addr->data);
    stgFree(stored_msgs->addr);
    stgFree(stored_msgs);
    stored_msgs = tmp;
  }
  
  /* close semaphores */
  cpw_sem_close(shared_memory.hCan_alloc);
  cpw_sem_close(shared_memory.hDo_alloc);
  int i;
  for (i = 0; i < (int)nPEs; i++) {
    cpw_sem_close(shared_memory.hW_sems + i);
    cpw_sem_close(shared_memory.hR_sems + i);
  }

  /* close synchronize barrier */
  cpw_sync_close(shared_memory.hSync);
	
  /* close shared memory*/
  cpw_shm_close(&shared_memory);
	
  return rtsTrue;
}


/**************************************
 * Data Communication between nodes:  */

/* 
Data is always communicated in "packets" (buffering), 
but possibly trivial ones (if dyn.chunking turned off)

Needed functionality: */

/* a send operation for peer2peer communication: 
 * sends the included data (array of length length) to the indicated node
 * (numbered from 1 to the requested nodecount nPEs) with the given message
 * tag. Length 0 is allowed and leads to a message containing no payload data.
 * The send action may fail, in which case rtsFalse is returned, and the 
 * caller is expected to handle this situation.
 *
 * Parameters:
 *   IN node     -- destination node, number between 1 and nPEs
 *   IN tag      -- message tag
 *   IN data     -- array of long values to send out
 *   IN length   -- length of data array. Allowed to be zero (no data).
 * Returns:
 *   rtsBool: success or failure inside comm. subsystem
 */
rtsBool MP_send(int node, OpCode tag, long *data, int length){
  //printf("MP_send()\n");

  /* check for errors */
  cpw_shm_check_errors();

  /* check length */
  ASSERT(length <= DATASPACEWORDS);
  
  /* send */
  switch (cpw_shm_send_msg(node, tag, length, data)) {
  case CPW_NOERROR:
    return rtsTrue;
  case CPW_SEND_FAIL:
  default:
    return rtsFalse;    
  }
}

/* - a blocking receive operation
 *   where system messages from main node have priority! 
 * Effect:
 *   A message is received from a peer. 
 *   Data stored in destination (maximum space given), and 
 *   opcode and sender fields are set.
 *   If no messages were waiting, the method blocks until a 
 *   message is available. If too much data arrives (> maxlength),
 *   the program stops with an error (resp. of higher levels).
 * 
 * Parameters: 
 *   IN  maxlength   -- maximum data length (security only)
 *   IN  destination -- where to unpack data (all of type long)
 *   OUT code   -- OpCode of message (aka message tag)
 *   OUT sender -- originator of this message 
 * Returns: 
 *   int: length of data received with message
 */
int MP_recv(STG_UNUSED int maxlength, long *destination, // IN
	    OpCode *code, nat *sender){       // OUT
  //printf("MP_recv()\n");

  /* check for errors */
  cpw_shm_check_errors();

  /* receive message */
  int length;
  if (cpw_self_probe()) {
    /* might be a sys_msg in shm queue */
    if(!cpw_self_probe_sys() && cpw_shm_probe_sys()) {
      /* sys_msg now available in front of shm queue */
      cpw_shm_recv_msg(sender, code, &length, destination);
    }
    else {
      /* no sys_msg in shm queue; if in local queue, its in front pos */
      cpw_self_recv_msg(sender, code, &length, destination);
    }
  }
  else {
    /* move sys_msg to front */
    cpw_shm_probe_sys();
    cpw_shm_recv_msg(sender, code, &length, destination);
  }
  
  return length;
}       

/* - a non-blocking probe operation 
 * (unspecified sender, no receive buffers any more) 
 */
rtsBool MP_probe(void) {
  //printf("MP_probe()\n");
  /* check for errors */
  cpw_shm_check_errors();

  if(cpw_shm_probe() || cpw_self_probe())
    return rtsTrue;
  else 
    return rtsFalse;
} 


/*============*
 * Semaphores *
 *============*/

/* Create named semaphore at address sem, with inital value of val */
static int cpw_sem_create(cpw_sem_t *sem, unsigned int val) {
  SECURITY_ATTRIBUTES sec_attr;
  sec_attr.nLength = sizeof(sec_attr);
  sec_attr.lpSecurityDescriptor = NULL;
  sec_attr.bInheritHandle = TRUE;
	
  int maxCount = (num_msgs < (int)nPEs) ? (int)nPEs : num_msgs;
	
  sem->semaphore = CreateSemaphore(&sec_attr, val, maxCount, NULL);
	
  if (sem->semaphore == NULL) {
    sysErrorBelch("Could not create semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* wait on semaphore, blocks until count > 0 */
static int cpw_sem_wait(cpw_sem_t *sem) {
  //printf("Waiting on %lui\n",sem->semaphore);
  DWORD ret = WaitForSingleObject(sem->semaphore, INFINITE);
  
  if (ret != WAIT_OBJECT_0) {
    sysErrorBelch("Could not wait on semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* wait on semaphore, if wait would block CPW_SEM_WOULD_LOCK is returned */
static int cpw_sem_trywait(cpw_sem_t *sem) {
  DWORD wait = WaitForSingleObject(sem->semaphore, 0L);
  switch (wait) {
  case WAIT_OBJECT_0:
    return CPW_NOERROR;
  case WAIT_TIMEOUT:
    return CPW_SEM_WOULD_LOCK;
  default:
    sysErrorBelch("Could not trywait on semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  } 
}

/* wait on semaphore, after abs_timeout time passed Fail */
static int cpw_sem_timedwait(cpw_sem_t *sem, unsigned int timeout) {
  DWORD wait = WaitForSingleObject(sem->semaphore, timeout*1000);
  switch (wait) {
  case WAIT_OBJECT_0:
    return CPW_NOERROR;
  default:
    sysErrorBelch("Could not timedwait on semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
}

/* Post on semaphore, increases count by one */
static int cpw_sem_post(cpw_sem_t *sem) {
  if(!ReleaseSemaphore(sem->semaphore, 1, NULL)) {
    /* Some error while post on this semaphore */
    sysErrorBelch("Could not post on semaphore\n");
    *shared_memory.status = CPW_SEM_FAIL;
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/* close semaphore */
static int cpw_sem_close(cpw_sem_t *sem) {
  if (CloseHandle(sem->semaphore) == 0) {
    //printf("Could not close semaphore\n");
    /* Not necessary to handle */
    return CPW_SEM_FAIL;
  }
  return CPW_NOERROR;
}

/*===============*
 * Shared Memory *
 *===============*/

/* print debug information for shared memory */
static void cpw_shm_debug_info(cpw_shm_t *shm) {	
  /*printf("#############################\n"
	     "      # Shared Memory Debug Info: #\n"
	     "      #############################\n"
	     "\n"
	     "      General Information:\n"
	     "      - size: %i bytes\n"
	     "      - base address: %p\n"
	     "      \n"
	     "      Structure Information:\n"
	     "      - status (@%p): %i\n"
		 
		 "      - hSync (@%p)\n"
		 "      - hSync counter: %i\n"
		 "      - hSync mutex (@%p): %i\n"
		 "      - hSync wait (@%p): %i\n"
		 
		 "      - hCanAlloc (@%p): %i\n"
		 "      - hDoAlloc (@%p): %i\n"
		 "      - wSems[1] (@%p): %i\n"
		 "      - rSems[1] (@%p): %i\n"
		 
	     "      - free_slots (@%p): %i\n"
	     "      - msgs_read@%p:\n"
	     "      - msgs_write@%p:\n"
	     "\n", 
	     (int)shm->size, 
	     shm->base, 
	     shm->status, *shm->status, 
		 
		 shm->hSync,
		 shm->hSync->count,
		 &shm->hSync->mutex, shm->hSync->mutex.semaphore,
		 &shm->hSync->wait, shm->hSync->wait.semaphore,
		 
		 shm->hCan_alloc, shm->hCan_alloc->semaphore,
		 shm->hDo_alloc, shm->hDo_alloc->semaphore,
		 shm->hW_sems, shm->hW_sems->semaphore,
		 shm->hR_sems, shm->hR_sems->semaphore,
		 
	     shm->free_slots, *shm->free_slots,
	     shm->msgs_read,
	     shm->msgs_write); */


  //printf("free message slots:\n");
  size_t next_slot_off = *shm->free_slots;
  cpw_shm_slot_off_t *next_slot = (cpw_shm_slot_off_t *) ((size_t)shared_memory.base + shared_memory.slots_start + next_slot_off);

  int num = 0;
  while (next_slot_off != 0) {
      cpw_msg_off_t *msg = (cpw_msg_off_t *) ((size_t) shared_memory.base + shared_memory.msgs_start + next_slot->addr);
    num++;
    /*printf("- %i (@%p) (offset: %i)\n"
	       "      +-> points to message @%i\n"
	       "        +-> points to data @%i\n", 
	       num, next_slot, next_slot_off,
	       next_slot->addr, msg->data); */

    next_slot_off = next_slot->next;
    next_slot = (cpw_shm_slot_off_t *) ((size_t)shared_memory.base + shared_memory.slots_start + next_slot_off);
  }

  //printf("total of %i free slots\n\n", num);
  //printf("message queues:\n");
  int i; 
  for (i = 0; i < (int)nPEs; i++) {
    num = 0;
    size_t slot_off = shm->msgs_read[i];
    cpw_shm_slot_off_t *slot = (cpw_shm_slot_off_t *) ((size_t) shared_memory.base + shared_memory.slots_start + slot_off);
    while(slot_off != 0 && slot->is_hole == 0) {
      num++;
      slot_off = slot->next;
      slot = (cpw_shm_slot_off_t *) ((size_t) shared_memory.base + shared_memory.slots_start + slot_off);
    }
    
    slot_off = shm->msgs_read[i];
    slot = (cpw_shm_slot_off_t *) ((size_t) shared_memory.base + shared_memory.slots_start + slot_off);
    /*printf("- msgs_read[%i] (@%p)\n"
	       "      +-> points to slot @%i\n"
	       "      | +-> is hole = %i\n"
	       "      +-> has %i message linked\n\n",
	       i,&shm->msgs_read[i],
	       shm->msgs_read[i],  slot->is_hole,
	       num); */

    slot_off = shm->msgs_write[i];
    slot = (cpw_shm_slot_off_t *) ((size_t) shared_memory.base + shared_memory.slots_start + slot_off);
    /*printf("- msgs_write[%i] (@%p)\n"
	       "      +-> points to slot @%i\n"
	       "        +-> is hole = %i\n",
	       i,&shm->msgs_write[i],
	       shm->msgs_write[i],slot->is_hole); */
  }
}

/* create and initialize shared memory */
static int cpw_shm_create()
{
  SECURITY_ATTRIBUTES sec_attr;
  sec_attr.nLength = sizeof(sec_attr);
  sec_attr.lpSecurityDescriptor = NULL;
  sec_attr.bInheritHandle = TRUE;


  /* calculate memory size */
  shared_memory.size = 
    SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    + sizeof(cpw_msg_off_t) * num_msgs /* structure for msgs */
#if SIZEOF_VOID_P == 8
    + SIZEOF_WORD64 * DATASPACEWORDS * num_msgs; /* message data */
#else 
#if SIZEOF_VOID_P == 4
  + SIZEOF_WORD32 * DATASPACEWORDS * num_msgs; /* message data */
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif

  /* create shared memory region */
  shared_memory.hShm = CreateFileMapping(INVALID_HANDLE_VALUE, 
				&sec_attr, 
				PAGE_READWRITE,
				0,
				shared_memory.size,
				"edenSHM");
  
  if (shared_memory.hShm == NULL) {
    /* could not create shared memory */
    sysErrorBelch("Could not open shm object!\n");
    return CPW_SHM_FAIL;
  }

  /* map shm object in own address space */
  shared_memory.base = MapViewOfFile(shared_memory.hShm, FILE_MAP_WRITE, 0, 0, 0);

  if (shared_memory.base == NULL){
    /* could not map shared memory in own adress space */
    sysErrorBelch("Couldn't mmap shm object! (not enough memory?)\n");
    return CPW_SHM_FAIL;
  }
  
	
  /* set status */
  shared_memory.status  = 
    (StgWord16 *) shared_memory.base;
  *shared_memory.status = CPW_NOERROR;
	
  /* create sync point */
  shared_memory.hSync = (cpw_sync_t *) 
    ( (char*) (shared_memory.base) + SIZEOF_WORD16);
  cpw_sync_create(shared_memory.hSync);

  /* create do_alloc */
  shared_memory.hDo_alloc = (cpw_sem_t *) 
    ((char *) (shared_memory.base)
     + SIZEOF_WORD16 /* status */
     + sizeof(cpw_sync_t) /* hSync */
     );
  cpw_sem_create(shared_memory.hDo_alloc, 1);

  /* create can_alloc */
  shared_memory.hCan_alloc = shared_memory.hDo_alloc + 1;
  cpw_sem_create(shared_memory.hCan_alloc, num_msgs);

  /* create w_sems/r_sems */
  shared_memory.hW_sems = shared_memory.hCan_alloc + 1;
  shared_memory.hR_sems = shared_memory.hW_sems + (int)nPEs;
  int i;
  for (i = 0; i < (int)nPEs; i++) {
    cpw_sem_create(shared_memory.hW_sems+i, 1);
    cpw_sem_create(shared_memory.hR_sems+i, 0);
  }


/* init list offset pointers addresses */
  shared_memory.free_slots = (size_t *) ((char *)shared_memory.base
					 + SIZEOF_WORD16
					 + sizeof(cpw_sync_t)
					 + sizeof(cpw_sem_t)
					 + sizeof(cpw_sem_t)
					 + sizeof(cpw_sem_t) * (int) nPEs
					 + sizeof(cpw_sem_t) * (int) nPEs);
	
  shared_memory.msgs_read = (size_t *) ((char*)shared_memory.base
				     + SIZEOF_WORD16
				     + sizeof(cpw_sync_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(size_t)
				     + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
				     + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
				     );

  shared_memory.msgs_write = (size_t *) ((char*)shared_memory.base
				     + SIZEOF_WORD16
				     + sizeof(cpw_sync_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(size_t)
				     + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
				     + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
				     + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
				     );
								  
  /* create slot nodes */
  size_t addr_nodes_start = SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    ;
  shared_memory.slots_start = addr_nodes_start;

  size_t msg_nodes_start =  SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    ;
  shared_memory.msgs_start = msg_nodes_start;

  size_t data_nodes_start = SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    + sizeof(cpw_msg_off_t) * num_msgs /* structure for msgs */
    ;
  shared_memory.data_start = data_nodes_start;


  size_t prev_addr = 0;
  for (i = 0; i < num_msgs; i++) {
    size_t next_addr_off =  sizeof(cpw_shm_slot_off_t) * (i+1);
    cpw_shm_slot_off_t *next_addr = (cpw_shm_slot_off_t *) ((char*)shared_memory.base + addr_nodes_start + next_addr_off);
    size_t next_msg_off =  sizeof(cpw_msg_off_t) * i;
    cpw_msg_off_t *next_msg = (cpw_msg_off_t *) ((char*)shared_memory.base + msg_nodes_start + next_msg_off);
    size_t next_data_off = 
#if SIZEOF_VOID_P == 8
      SIZEOF_WORD64 * DATASPACEWORDS * i; /* message data */
#else 
#if SIZEOF_VOID_P == 4
      SIZEOF_WORD32 * DATASPACEWORDS * i; /* message data */
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif

		
    next_msg->data     = next_data_off;
    next_addr->addr    = next_msg_off;
    next_addr->next    = prev_addr;
    next_addr->is_hole = 0;
		
		
    prev_addr = next_addr_off;
    
    //printf("created slot at @%p -> points to messate @%p\n", next_addr, next_msg);
  }
	
  *shared_memory.free_slots = prev_addr;
  for (i = 0; i < (int) nPEs; i++) {
    size_t next_addr_off =  sizeof(cpw_shm_slot_off_t) * (i+1+num_msgs);
    cpw_shm_slot_off_t *next_addr = (cpw_shm_slot_off_t *) ((char*)shared_memory.base + addr_nodes_start + next_addr_off);
    next_addr->next = 0;
    next_addr->addr = 0;
    next_addr->is_hole = 1;
    shared_memory.msgs_read[i] = next_addr_off;
    shared_memory.msgs_write[i] = next_addr_off;
  }  
  
  /* finish */
  return CPW_NOERROR;
}

static int cpw_shm_init()
{

  /* calculate memory size */
  shared_memory.size = 
    SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    + sizeof(cpw_msg_off_t) * num_msgs /* structure for msgs */
#if SIZEOF_VOID_P == 8
    + SIZEOF_WORD64 * DATASPACEWORDS * num_msgs; /* message data */
#else 
#if SIZEOF_VOID_P == 4
  + SIZEOF_WORD32 * DATASPACEWORDS * num_msgs; /* message data */
#else
#error GHC untested on this architecture: sizeof(void *) != 4 or 8
#endif
#endif

  /* map shm object in own address space */
  shared_memory.base = MapViewOfFile(shared_memory.hShm, FILE_MAP_WRITE, 0, 0, 0);

  if (shared_memory.base == NULL){
    /* could not map shared memory in own adress space */
    sysErrorBelch("Couldn't mmap shm object! (not enough memory?)\n");
    return CPW_SHM_FAIL;
  }
  
	
  /* set status */
  shared_memory.status  = (StgWord16 *) shared_memory.base;

	
  /* create sync point */
  shared_memory.hSync = (cpw_sync_t *) 
    ( (char*) (shared_memory.base) + SIZEOF_WORD16);

  /* create do_alloc */
    shared_memory.hDo_alloc = (cpw_sem_t *) 
    ((char*) (shared_memory.base)
     + SIZEOF_WORD16 /* status */
     + sizeof(cpw_sync_t) /* hSync */
     );

  /* create can_alloc */
  shared_memory.hCan_alloc = shared_memory.hDo_alloc + 1;

  /* create w_sems/r_sems */
  shared_memory.hW_sems = shared_memory.hCan_alloc + 1;
  shared_memory.hR_sems = shared_memory.hW_sems + (int)nPEs;

/* init list offset pointers addresses */
  shared_memory.free_slots = (size_t *) ((char*)shared_memory.base
					 + SIZEOF_WORD16
					 + sizeof(cpw_sync_t)
					 + sizeof(cpw_sem_t)
					 + sizeof(cpw_sem_t)
					 + sizeof(cpw_sem_t) * (int) nPEs
					 + sizeof(cpw_sem_t) * (int) nPEs);
	
  shared_memory.msgs_read = (size_t *) ((char*)shared_memory.base
				     + SIZEOF_WORD16
				     + sizeof(cpw_sync_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(size_t)
				     + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
				     + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
				     );

  shared_memory.msgs_write = (size_t *) ((char*)shared_memory.base
				     + SIZEOF_WORD16
				     + sizeof(cpw_sync_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t)
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(cpw_sem_t) * (int) nPEs
				     + sizeof(size_t)
				     + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
				     + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
				     + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
				     );
								  
  /* create slot nodes */
  size_t addr_nodes_start = SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    ;
  shared_memory.slots_start = addr_nodes_start;

  size_t msg_nodes_start =  SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    ;
  shared_memory.msgs_start = msg_nodes_start;

  size_t data_nodes_start = SIZEOF_WORD16 /* status */
    + sizeof(cpw_sync_t) /* hSync */
    + sizeof(cpw_sem_t) * 2  /* hAllocs */
    + sizeof(cpw_sem_t) * (int)nPEs  /* W_sems */
    + sizeof(cpw_sem_t) * (int)nPEs  /* R_Sems */
    + sizeof(size_t) /* freeslots */
    + sizeof(cpw_shm_slot_off_t) * (num_msgs+1) /* stack for free slots */
    + sizeof(cpw_shm_slot_off_t) * (int)nPEs /* and additional holes */
    + sizeof(size_t) * (int)nPEs /* root nodes for r-msg lists */
    + sizeof(size_t) * (int)nPEs /* root nodes for w-msg lists */
    + sizeof(cpw_msg_off_t) * num_msgs /* structure for msgs */
    ;
  shared_memory.data_start = data_nodes_start;

  
  /* finish */
  return CPW_NOERROR;
}

/* try to send a message */
static int cpw_shm_send_msg(nat toPE, OpCode tag, int length, long *data) {	
  size_t free_slot_off = 0;
  //printf("sending msg\n");
  /* can we get a free slot? */
  if (cpw_sem_trywait(shared_memory.hCan_alloc) == CPW_SEM_WOULD_LOCK) {
	//printf("send can alloc lock\n");	
    /* no, try to store a message */
    free_slot_off = cpw_shm_acquire_slot(); 
    
    if (free_slot_off == 0) {
      /* could not store message, sending failed */
      return CPW_SEND_FAIL;
    }
  }
  //printf("send wait do alloc\n");
  cpw_shm_slot_off_t *free_slot = NULL;
  if (free_slot_off == 0) {//	cpw_sem_wait(can_alloc);
    cpw_sem_wait(shared_memory.hDo_alloc);

    free_slot_off = *shared_memory.free_slots;
    free_slot = (cpw_shm_slot_off_t *) ((char*) (shared_memory.base) + shared_memory.slots_start +  free_slot_off);
    *shared_memory.free_slots = free_slot->next;
	
    cpw_sem_post(shared_memory.hDo_alloc);
  }
  else {
    free_slot = (cpw_shm_slot_off_t *) ((char*) (shared_memory.base) + shared_memory.slots_start +  free_slot_off);
  }
	
  //printf("send copy msg\n");
  /* copy message */
  cpw_msg_off_t *msg = (cpw_msg_off_t *) ((char*)shared_memory.base + shared_memory.msgs_start + free_slot->addr);
  free_slot->is_hole = 1; /* free_slot to become new hole */
  free_slot->next = 0;
  msg->sender = thisPE;
  msg->tag    = tag;
  msg->length = length;
  memcpy((char*)shared_memory.base + shared_memory.data_start + msg->data, data, sizeof(long) * length);


  //printf(" sending msg to %i, tag %i\n", toPE,tag);
	
  /* enqueue message */
  toPE--;
  /* wait for other write to finish */
  cpw_sem_wait(shared_memory.hW_sems + toPE); 

  cpw_shm_slot_off_t *hole = (cpw_shm_slot_off_t *) ((char*)shared_memory.base + shared_memory.slots_start + shared_memory.msgs_write[toPE]);
  hole->next = free_slot_off;
  hole->addr = free_slot->addr;
  free_slot->addr = 0;
  shared_memory.msgs_write[toPE] = free_slot_off; /* new hole */
  hole->is_hole = 0;
  /* finish */
  cpw_sem_post(shared_memory.hW_sems + toPE );

  /* wake receiver */
  cpw_sem_post(shared_memory.hR_sems + toPE);
  
  cpw_shm_debug_info(&shared_memory);
  return CPW_NOERROR;
}

/* try to receive a message */
static int cpw_shm_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data) {
  int me = thisPE - 1;
	
  /* wait until msgs there */
  cpw_sem_wait(shared_memory.hR_sems + me);


  size_t used_slot_off = shared_memory.msgs_read[me];
  cpw_shm_slot_off_t *used_slot = (cpw_shm_slot_off_t *) ((char*)shared_memory.base + shared_memory.slots_start + used_slot_off);
  shared_memory.msgs_read[me] = used_slot->next;

  cpw_msg_off_t *msg = (cpw_msg_off_t *) ((char*)shared_memory.base + shared_memory.msgs_start + used_slot->addr);
  /* copy data */
  *fromPE = msg->sender;
  *tag    = msg->tag;
  *length = msg->length;
  memcpy(data, (char*)shared_memory.base + shared_memory.data_start + msg->data, sizeof(long) * msg->length);

	
  //printf(" got a message from %i, tag = %i\n", *fromPE, *tag);

  /* freeing slot */
  cpw_sem_wait(shared_memory.hDo_alloc);
  used_slot->next = *shared_memory.free_slots;
  *shared_memory.free_slots = used_slot_off;
  cpw_sem_post(shared_memory.hDo_alloc);
  /* wake waiting senders */
  cpw_sem_post(shared_memory.hCan_alloc);

  return CPW_NOERROR;	
}

static size_t cpw_shm_acquire_slot() {
  int me = thisPE - 1;
	
  if(cpw_sem_trywait(shared_memory.hR_sems + me) == CPW_SEM_WOULD_LOCK)
    return 0;

  /* store message for later use */
  cpw_shm_slot_t *new_node = 
    (cpw_shm_slot_t *)stgMallocBytes((int)sizeof(cpw_shm_slot_t), "StoredNode");
  cpw_msg_t *new_msg  = (cpw_msg_t *)stgMallocBytes((int)sizeof(cpw_msg_t),"StoredMsg");
  StgWord *new_data = (StgWord *)stgMallocBytes((int)(sizeof(StgWord)*DATASPACEWORDS),"StoredData");
  new_node->addr = new_msg;
  new_node->next = NULL;
  new_msg->data  = new_data;


  size_t used_slot_off = shared_memory.msgs_read[me];
  cpw_shm_slot_off_t *used_slot = (cpw_shm_slot_off_t *) ((char*)shared_memory.base + shared_memory.slots_start + used_slot_off);
  shared_memory.msgs_read[me] = used_slot->next;

  /* copy data */
  cpw_msg_off_t *msg = (cpw_msg_off_t *) ((char*)shared_memory.base + shared_memory.msgs_start + used_slot->addr);
  new_msg->sender = msg->sender;
  new_msg->tag    = msg->tag;
  new_msg->length = msg->length;
  memcpy(new_data, (char*)shared_memory.base + shared_memory.data_start + msg->data, sizeof(long) * msg->length);

  if (stored_msgs == NULL) {
    stored_msgs = new_node;
  }
  else {
    cpw_shm_slot_t *insert = stored_msgs;
    while(insert->next != NULL) {
      insert = insert->next;
    }
    insert->next = new_node;
  }

  return used_slot_off;
}

static int cpw_self_recv_msg(nat *fromPE, OpCode *tag, int *length, long *data) {
  cpw_shm_slot_t *used_slot = stored_msgs;
  stored_msgs = used_slot->next;
 

  /* copy data */
  *fromPE = used_slot->addr->sender;
  *tag    = used_slot->addr->tag;
  *length = used_slot->addr->length;
  memcpy(data, used_slot->addr->data, sizeof(long) * used_slot->addr->length);
	
  stgFree(used_slot->addr->data);
  stgFree(used_slot->addr);
  stgFree(used_slot);
  
  return CPW_NOERROR;	
}

/* test if messages available */
static int cpw_shm_probe() {
  switch(cpw_sem_trywait(shared_memory.hR_sems + (thisPE-1))) {
  case CPW_NOERROR:
    cpw_sem_post(shared_memory.hR_sems + (thisPE-1));
    return 1;
  default:
    return 0;
  }
}

/* test if a sys msg exists in shm queue 
 if so sysmsg is moved to front of queue */
static int cpw_shm_probe_sys(void) {
  switch(cpw_sem_trywait(shared_memory.hR_sems + (thisPE-1))) {
  case CPW_NOERROR:
  {
    size_t queue_front_off = shared_memory.msgs_read[thisPE-1];
    cpw_shm_slot_off_t *queue_front = (cpw_shm_slot_off_t *) ((size_t)shared_memory.base + shared_memory.slots_start + queue_front_off);

    size_t pre_sys_slot_off = 0;
    cpw_shm_slot_off_t *pre_sys_slot = NULL;

    size_t sys_slot_off = queue_front_off;
    cpw_shm_slot_off_t *sys_slot = queue_front;

    cpw_msg_off_t *msg = (cpw_msg_off_t *) ((size_t)shared_memory.base + shared_memory.msgs_start + sys_slot->addr);
    while(!ISSYSCODE(msg->tag)) {
      pre_sys_slot = sys_slot;
      pre_sys_slot_off = sys_slot_off;

      sys_slot_off = sys_slot->next;
      sys_slot = (cpw_shm_slot_off_t *) ((size_t)shared_memory.base + shared_memory.slots_start + sys_slot_off);
      if(sys_slot->is_hole == 1) {
	cpw_sem_post(shared_memory.hR_sems + (thisPE-1)); /* no msg taken */
	return 0; /* no sys msgs found */
      }
      msg = (cpw_msg_off_t *) ((size_t)shared_memory.base + shared_memory.msgs_start + sys_slot->addr);
    }
    /* possibly move sysmsg to front */
    if(pre_sys_slot_off != 0) {
      pre_sys_slot->next = sys_slot->next;
      sys_slot->next = queue_front_off;
      shared_memory.msgs_read[thisPE-1] = sys_slot_off;
    }

    cpw_sem_post(shared_memory.hR_sems + (thisPE-1)); /* no msg taken */
    /* sys_msg now in front */
    return 1;
  }
  default:
    return 0;
  }
}

static int cpw_self_probe() {
  return (stored_msgs != NULL);
}

/* test if a sys msg exists in local queue 
 if so sysmsg is moved to front of queue */
static int cpw_self_probe_sys(void) {
  switch(stored_msgs != NULL) {
  case 1:
    {
      cpw_shm_slot_t *queue_front = stored_msgs;
      cpw_shm_slot_t *pre_sys_slot = NULL;
      cpw_shm_slot_t *sys_slot = queue_front;

      while(!ISSYSCODE(sys_slot->addr->tag)) {
	pre_sys_slot = sys_slot;
	sys_slot = sys_slot->next;
	if(sys_slot == NULL) {
	  return 0; /* no sys msgs found */
	}
      }
      /* possibly move sysmsg to front */
      if(pre_sys_slot != NULL) {
	pre_sys_slot->next = sys_slot->next;
	sys_slot->next = queue_front;
	stored_msgs = sys_slot;
      }
      /* sys_msg now in front */
      return 1;
    }
  default:
    return 0;
  }
}

static int cpw_shm_free_pending_msg() {
  //printf("freeing pending messages\n");
  int me = thisPE - 1;
	
  while(cpw_sem_trywait(shared_memory.hR_sems + me) == CPW_NOERROR) {
    size_t used_slot_off = shared_memory.msgs_read[me];
    cpw_shm_slot_off_t *used_slot = (cpw_shm_slot_off_t *) ((size_t)shared_memory.base + shared_memory.slots_start + used_slot_off);
    shared_memory.msgs_read[me] = used_slot->next;

    /* freeing slot */
    cpw_sem_wait(shared_memory.hDo_alloc);
    used_slot->next = *shared_memory.free_slots;
    *shared_memory.free_slots = used_slot_off;
    cpw_sem_post(shared_memory.hDo_alloc);
    /* wake waiting senders */
    cpw_sem_post(shared_memory.hCan_alloc);
  }

  //printf("done freeing pending messages\n");
  return CPW_NOERROR;	
}

static int cpw_shm_close(cpw_shm_t *shm) {
  if (CloseHandle(shm->hShm) == 0) {
    //printf("Couldn't unmap shared memory\n");
    return CPW_SHM_FAIL;
  }
  return CPW_NOERROR;
}

static void cpw_shm_check_errors() {
  switch (*shared_memory.status) 
  {
  case CPW_NOERROR:
    return;
  case CPW_FILENAME_FAIL:
    barf("MPSystem CpWay: unique filename error!\n");
  case CPW_SHM_FAIL:
    barf("MPSystem CpWay: shared memory error!\n");
  case CPW_SEM_FAIL:
    barf("MPSystem CpWay: semaphore error!\n");
  case CPW_SYNC_FAIL:
    barf("MPSystem CpWay: barrier error!\n");
  case CPW_FORK_FAIL:
    barf("MPSystem CpWay: fork error!\n");
  default:
    return;
  }
}

/*===========================*
 * Sync Point (not reusable) *
 *===========================*/
static int cpw_sync_create(cpw_sync_t *syn) {
  syn->count = 0;
  if(cpw_sem_create(&syn->mutex, 1) != CPW_NOERROR ||
     cpw_sem_create(&syn->wait, 0) != CPW_NOERROR) {
    *shared_memory.status = CPW_SYNC_FAIL;
    return CPW_SYNC_FAIL;
  }
  return CPW_NOERROR;
}

static int cpw_sync_synchronize(cpw_sync_t *syn) {
	
  if(cpw_sem_wait(&syn->mutex) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
  syn->count = syn->count + 1;
	
  if(syn->count == (int)nPEs) {
    /* all nodes checked in */
    int i;
    for(i = 0; i < (int)nPEs; i++) {
      if(cpw_sem_post(&syn->wait) != CPW_NOERROR) {
	return CPW_SYNC_FAIL;
      }
    }
  }
	
  if(cpw_sem_post(&syn->mutex) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
	
  if(cpw_sem_timedwait(&syn->wait, CPW_SYNC_TIMEOUT) != CPW_NOERROR) {
    return CPW_SYNC_FAIL;
  }
		
  //printf("GO!\n");
	
  return CPW_NOERROR;
}

static int cpw_sync_close(cpw_sync_t *syn) {
  int em = cpw_sem_close(&syn->mutex);
  int ew = cpw_sem_close(&syn->wait);
	
  if (em != CPW_NOERROR || ew != CPW_NOERROR) {
    //printf("Couldn't close sync point.\n");
    return CPW_SYNC_FAIL;
  }
  return CPW_NOERROR;
}

/*==================*
 * Utiliy Functions *
 *==================*/

/* 
 * Make unique name to be used in shm_open or _named_ semaphores.
 * Returns pointer to unique name which has to be freed!       
 */
static int cpw_mk_name(char *res) {
  static unsigned int next_unique = 0;
	
  char *s_magic = "eden"; /* TODO use executable name here? */
	
  ASSERT(1 + strlen(s_magic) + 12 + 1 <= CPW_MAX_FILENAME_LENGTH);
	
  if (sprintf(res, "/%s%08x%04x", s_magic, "1234", next_unique++) < 0) {
    return CPW_FILENAME_FAIL;
  }
	
  return CPW_NOERROR;
}

static void parse_rts_flags(int* argc, char** argv) {
  int my_argc = (*argc)-1;
  char *my_argv[my_argc];
  my_argv[0] = stgMallocBytes(strlen(argv[0])+1, "argv0");
  strcpy(my_argv[0], argv[0]);
  int k;
  for (k=2; k < *argc; k++) {
    my_argv[k-1] = stgMallocBytes(strlen(argv[k])+1, "argv");
    strcpy(my_argv[k-1], argv[k]); 
  }
  initRtsFlagsDefaults();

  setupRtsFlags(&my_argc, my_argv, &rts_argc, rts_argv);

  for (k=0; k < my_argc; k++) stgFree(my_argv[k]);
}

static char* cpw_mk_argv_string(int argc, char ** argv) {
  int len = argc*3;
  int i;
  for (i = 0; i < argc; i++) {
    len += strlen(argv[i]);
  }
	
  char *result = malloc(sizeof(char) * (len+1));
  result[0] = '\0';
	
  for (i = 0; i < argc; i++) {
    strcat(result, "\"");
    strcat(result, argv[i]);
    strcat(result, "\" ");
  }
	
  return result;
}
#endif /* win32 */

#endif /* whole file */

