/* 
 * Generalised RTE for parallel Haskells.
 *
 * File: ghc/rts/parallel/PVMComm.c
 *
 * Purpose: map generalised Comm.functions to PVM, 
 * abstracting from the concrete MP-System
 *
 * Comments:

note that we separate mp-messages, system messages and program messages!

send data format is always PvmDataRaw, containing longs

 */

#if defined(PARALLEL_RTS)&&defined(USE_PVM)  /* whole file */

#include "Rts.h" // common include file for whole RTS
#include "MPSystem.h" // communication interface to RTS

#include "pvm3.h" // => we must set the include path properly!
#include "RtsUtils.h" // utilities for error msg. etc.
#include "PEOpCodes.h" // message codes only


// pvm-specific error control:
#define checkComms(c,s)		do {                  \
                                  if ((c)<0) {        \
                                    pvm_perror(s);    \
                                    Failure = rtsTrue;\
                                    exit(-1);     \
                                }} while(0)
// JB 2007: stg_exit in this macro does not work, leads to failure
// shutdown loop, not to be stopped by ^C! TODO

// PVM-specific receive parameters:
#define ANY_TASK (-1)
#define ANY_CODE (-1)


// Global conditions defined here.
// main thread (PE 1 in logical numbering)
rtsBool	IAmMainThread = rtsFalse;	// Set this for the main PE
rtsBool Failure = rtsFalse; // Set this in case of error shutdown
// nPEs, thisPE
nat nPEs = 0; // number of PEs in system
nat thisPE=0;

// these were in SysMan in the good old days
// GlobalTaskId  mytid; 

int pvmMyself; // node's own address in pvm
int pvmParent; // the parent's address (parent is master node)

int allPEs[MAX_PES]; // array of all PEs (mapping from logical node no.s to pvm addresses)

/***************************************************
 * a handler for internal messages of the MP-System:
 *
 * This is the place where the system could be opened to new PEs,
 * and possibly PEs could disconnect without causing an error.
 * We only implement stubs for the handlers for the moment.
 *
 * Note that messages with ISMPCODE(msg-code) must be entirely
 * handled inside this file, and therefore have absolute priority.
 * 
 * Every implementation of MPSystem.h can decide which messages 
 * to snatch in this way and how to process them. The solution 
 * below is only for PVM.
 */ 

// MP messages, concern the internals of the MP system only
#define ISMPCODE(code)    ((code) == PP_READY  || \
			   (code) == PP_NEWPE  || \
                           (code) == PP_PETIDS || \
                           (code) == PP_FAIL)

static void MPMsgHandle(OpCode code, int buffer) {
  int task;   // originator of a message
  int bytes;  // size of message

  // for PE failure notification
  nat whofailed = 1;
  long t;

  ASSERT(ISMPCODE(code)); // we only want to see internal messages...
  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("MPMsgHandle: handling a message with tag %x\n", code));

  if (buffer == 0)  // recv. message, if not previously received
    buffer = pvm_recv(ANY_TASK, code);
  pvm_bufinfo(buffer, &bytes, &code, &task);

  switch(code) {
  case PP_NEWPE: 
    // new PE wants to join the system.
    ASSERT(IAmMainThread);
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("Ignoring NEWPE(%x) message from PE %x\n", code, task));
    break;
  case PP_FAIL: 
    // one of the PEs has failed
    ASSERT(IAmMainThread);
    pvm_upklong(&t,1,1);
    // t is PVM-ID (sent by pvm-demon), we need the logical PE number
    // in [2..nPEs]
    while (whofailed < nPEs && allPEs[whofailed] != t) {
      whofailed++;
    }
    debugBelch("System failure on node %d (%lx).\n", whofailed+1, t);
    // delete from PE address table (avoid errors in shutdown). 
    if (whofailed < nPEs) { // found the terminated PE
      allPEs[whofailed] = 0; 
    }
    // JB: q&d solution for debugging GUM (global stop on first
    // error). RACE CONDITION on multiple failures!
    errorBelch("remote PE failure, aborting execution.\n");
    Failure = rtsTrue;
    shutdownHaskellAndExit(-2);
    break;
  case PP_READY:
    //  new PE is ready to receive work.
  case PP_PETIDS:
    // update of PE addresses (update due to PE failure, or a new PE).

  // stop execution! (not implemented)
    barf("MPSystem PVM: receiving MP-Code %x from PE %x outside startup phase\n", code, task);
  default: 
    barf("MPMsgHandle: Strange unimplemented OpCode %x",code);
  }
}

/**************************************************************
 * Startup and Shutdown routines (used inside ParInit.c only) */

/* MP_start starts up the node: 
 *   - connects to the MP-System used, 
 *   - determines wether we are main thread
 *   - starts up other nodes in case we are first and 
 *     the MP-System requires to spawn nodes from here.
 * Global Var.s set here:
 *   nPEs (main only)- int: no. of PEs
 *   IAmMainThread - Bool: wether this node is main PE
 * Parameters: 
 *     IN    argv  - char**: program arguments (for pvm-spawn/mpi-init)
 * Returns: Bool: success (1) or failure (0)
 *
 * For PVM: 
 *   MP_start first registers with PVM. 
 *   Then it checks wether it has a pvm-parent. 
 *   If not, it must be the first (i.e. main) node; it will start
 *   (spawn) the others (care that all available nodes are used. PVM
 *   tends to start multiple tasks on the local host).
 *   In the other case, a synchronising message is sent (received in
 *   MP_sync).
 *
 * JB 01/2007: start in debug mode when nPEs parameter starts with '-'
 *
 */
rtsBool MP_start(int* argc, char* argv[]) {
  
  if (*argc < 2) {
    debugBelch("Need argument to specify number of PEs");
    exit(EXIT_FAILURE);
  }

  // start in debug mode if negative number (or "-0")
  if (argv[1][0] == '-') {
    RtsFlags.ParFlags.Debug.mpcomm = rtsTrue;
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("PVM debug mode! Starting\n"));
  }

  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("Entered MP startup\n"));

  checkComms(pvmMyself = pvm_mytid(),// node starts or joins pvm
	     "PVM -- Failure on startup: ");

  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("Connected to pvm\n"));

  pvmParent = pvm_parent(); // determine master

  if (pvmParent == PvmNoParent) {
// code for the main node:
    char *progname;
    int nArch, nHost;
    struct pvmhostinfo *hostp; 
    int taskTag = PvmTaskDefault;
  
    // no parent => we are the main node
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("I am main node\n"));
    IAmMainThread = rtsTrue;
    allPEs[0]=pvmMyself; // first node in array is main

    // get pvm config for spawning other tasks
    checkComms(pvm_config(&nHost,&nArch,&hostp),
	       "PVM -- get config: ");

    // we should have a correct argument...
    ASSERT(argv && argv[1]);

    // correct argument if PVM debugging
    if (argv[1][0] == '-') {
      taskTag = taskTag | PvmTaskDebug;
      nPEs = atoi(argv[1]+1);
    } else {
      nPEs = atoi(argv[1]);
    }


    // determine number of nodes, if not given
    if (!(nPEs)) {
      IF_PAR_DEBUG(mpcomm, 
		   debugBelch("nPEs not set explicitly (arg is %s)\n",argv[1]));
      nPEs = nHost;
    }
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("Nodes requested: %d\n", nPEs));

    // refuse to create more PEs than the system can contain
    // see MPSystem.h for MAX_PES
    if (nPEs > MAX_PES) {
      errorBelch("Unable to create more than %d processes, "
                 "using available maximum.", MAX_PES);
      nPEs = MAX_PES;
    }

    if (nPEs > 1) {
      /*   if needed, we spawn the program name set in ENV("PE"), 
	   assuming it is in scope in $PVM_ROOT/bin/$PVM_ARCH.
	   This variable has been set by the generated startup script. */
      int i, myHost;
      nat tasks;

      // determine program name
      progname = argv[0];

      IF_PAR_DEBUG(mpcomm, 
		   debugBelch("Spawning pvm-program %s\n",progname));

      // spawn all other nodes, specify available hosts until all used
      // or enough tasks spawned
      myHost = pvm_tidtohost(pvmMyself);
      for(tasks=1, i=0; tasks < nPEs && i < nHost; i++) {
	if(hostp[i].hi_tid != myHost) {
	  checkComms(-1+pvm_spawn(progname, &(argv[1]), taskTag | PvmTaskHost,
			          hostp[i].hi_name, 1, allPEs+tasks),
		     "PVM -- task startup");
	  tasks++;
	}
      }
      // rest anywhere pvm likes: !!Here should be an error (use of argv in pvm_spawn, use &nPEs instead)
      if (tasks < nPEs) 
	tasks += pvm_spawn(progname, &(argv[1]), taskTag, 
			   (char*)NULL, nPEs-tasks, allPEs+tasks);

      IF_PAR_DEBUG(mpcomm, 
		   debugBelch("%d tasks in total\n", tasks));


      // possibly correct nPEs value:
      nPEs=tasks;
      
      // broadcast returned addresses
      pvm_initsend(PvmDataRaw);
      pvm_pkint((int*)&nPEs, 1, 1);
      IF_PAR_DEBUG(mpcomm, 
		   debugBelch("Packing allPEs array\n"));
      pvm_pkint(allPEs, nPEs, 1);
      checkComms(pvm_mcast(allPEs+1, nPEs-1,PP_PETIDS),
		 "PVM -- Multicast of PE mapping failed");
      IF_PAR_DEBUG(mpcomm, 
		   debugBelch("Broadcasted addresses: \n"));

      // register for receiving failure notice (by PP_FAIL) from children
      checkComms(pvm_notify(PvmTaskExit, PP_FAIL, nPEs-1, allPEs+1),
      		 "pvm_notify error");
    }

    // set back debug option (will be set again while digesting RTS flags)
    RtsFlags.ParFlags.Debug.mpcomm = rtsFalse;

  } else { 
// we have a parent => slave node
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("I am slave node\n"));
    IAmMainThread = rtsFalse;

    // send a synchronisation message
    pvm_initsend(PvmDataRaw);
    checkComms(pvm_send(pvmParent, PP_READY), // node READY
	       "PVM -- Failed to send sync. message: ");
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("Sent sync message.\n"));

  }

  return rtsTrue;
}

/* MP_sync synchronises all nodes in a parallel computation:
 * Global Var.s set here: 
 *   nPEs (slaves) - int: no. of PEs to expect/start (if 0: should equal no. of hosts)
 *   thisPE        - GlobalTaskId: node's own task Id 
 *               (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_sync(void) {

  if (IAmMainThread) {
    int nodesArrived=1;
    int buffer=0;

    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("Synchronisation (main)...\n"));

    thisPE = 1; // set myId correctly (node 1 is main node)
    ASSERT(allPEs[0]==pvmMyself);

    // expect sync messages from all other nodes, 
    //   and check allPEs array for completeness
    while(nodesArrived < (int) nPEs) {
	  checkComms(buffer=pvm_nrecv(allPEs[nodesArrived], PP_READY),
		     "PVM: Failed to receive sync message");
	  if (buffer==0) {
	  /*
	    IF_PAR_DEBUG(mpcomm, 
			 debugBelch("Missing PE %d.\n", nodesArrived+1));
	  */
	  } else {
	    IF_PAR_DEBUG(mpcomm, 
			 debugBelch("Node %d [%x] has joined the system.\n",
				    nodesArrived+1, allPEs[nodesArrived]));
	    nodesArrived++;
	  }
    }

  } else {
    int i;

    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("Synchronisation (child)...\n"));

    // code for children... receive allPEs[] and detect node no.
    checkComms(pvm_recv(pvmParent, PP_PETIDS),
	       "PVM: Failed to receive node address array");
    pvm_upkint((int*)&nPEs,1,1);
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("%d PEs in the system\n",nPEs));
    pvm_upkint(allPEs,nPEs,1);

    // set own node id
    thisPE = i = 0;
    while ((!thisPE) && i < (int) nPEs ) {
      if (allPEs[i] == pvmMyself) thisPE=i+1;
      i++;
    }
    if (!thisPE) return rtsFalse;
  }

  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("I am node %d, synchronised.\n",thisPE));
  return rtsTrue;
}

/* MP_quit disconnects current node from MP-System:
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_quit(int isError) {
  long errval;

  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("MP_quit: leaving system (exit code %d).\n", isError));
  
  // pack a PP_FINISH message
  pvm_initsend(PvmDataRaw);
  // must repeat OpCode as a long int inside message data (common format)
  errval = PP_FINISH;
  pvm_pklong(&errval,1,1);
  errval = isError;
  pvm_pklong(&errval,1,1);

  if (!IAmMainThread) {
    //inform / reply to our parent (if any)
      checkComms(pvm_send(pvmParent, PP_FINISH),
		 "PVM: Error sending finish (error condition).");
  } else {
    int finishRecvd=1; // main node is finishing, so 1 node known already...

    // unregister failure notice
    if ( nPEs > 1 && !Failure ) 
      checkComms(pvm_notify(PvmTaskExit | PvmNotifyCancel, 
			    PP_FAIL, nPEs-1, allPEs+1),
		 "pvm_notify error");
    // This launches the PP_FAIL messages we ordered! We ignore these
    // messages (only accept PP_FINISH in shutdown phase).

    // if we are main node, we broadcast a PP_FINISH to the remaining group
    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("MP_quit: Main node sends FINISH.\n"));
    if (!Failure) 
      checkComms(pvm_mcast(allPEs+1,nPEs-1, PP_FINISH),
		 "shutdown: Failed to broadcast PP_FINISH");
    else {
      // correct PEs array (for clean shutdown on remote PE failure)
      nat j,k;
      int mcastArr[MAX_PES];

      k = 0;
      for (j = 1; j < nPEs; j++) {
	if (allPEs[j] != 0) 
	  // if allPEs[j] then send finish to it... (build a new array)
	  mcastArr[k++] = allPEs[j];
	else 
	  finishRecvd++; // otherwise, PE already terminated (with error)
      }
      checkComms(pvm_mcast(mcastArr,k,PP_FINISH),
	 "error shutdown: failed to broadcast PP_FINISH to remaining PEs");
    }

    // main node should wait for all others to terminate
    while (finishRecvd < (int) nPEs){
      long errorcode;
      int buffer, task, tag, bytes;

      buffer = pvm_recv(ANY_TASK, PP_FINISH);
      pvm_bufinfo(buffer, &bytes, &tag, &task);
      pvm_upklong(&errorcode,1,1); // consume PP_FINISH field
      ASSERT(errorcode == PP_FINISH);
      pvm_upklong(&errorcode,1,1);
      IF_PAR_DEBUG(mpcomm,
		   debugBelch("Received msg from task %x: Code %ld\n",
			      task, errorcode));
      if (errorcode) {
	debugBelch("Main node: Task %x signals error %ld (H%lx).\n\n", 
		   task, errorcode, errorcode);
      }
      finishRecvd++;
    }

    IF_PAR_DEBUG(mpcomm, 
		 debugBelch("MP_quit: Main node received %d replies, exiting from pvm now.\n", 
			    finishRecvd));
  }

  checkComms(pvm_exit(),
	     "PVM: Failed to shut down pvm.");

  return rtsTrue;
}

/**************************************
 * Data Communication between nodes:  */

// note that any buffering of messages happens one level above!

/* send operation directly using PVM */
rtsBool MP_send(int node, OpCode tag, long *data, int length) {

  ASSERT(node); // node > 0
  ASSERT(node <= (int) nPEs); // node is valid PE number
  ASSERT(ISOPCODE(tag));

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_send for PVM: sending buffer@%p "
                          "(length %d) to %d with tag %x (%s)\n",
			  data, length, node, tag, getOpName(tag)));
  pvm_initsend(PvmDataRaw);
  
  if (length > 0) {
    pvm_pklong(data, length, 1);
  }
  checkComms(pvm_send(allPEs[node-1],tag),
	     "PVM:send failed");
  return rtsTrue;
}

/* - a blocking receive operation
   where system messages have priority! */
int MP_recv(int maxlength, long *destination,
	    OpCode *retcode, nat *sender) {
  int bytes; // return value
  OpCode code; // internal use...
  int buffer=0;
  int sendPE, i;
  
  IF_PAR_DEBUG(mpcomm, debugBelch("MP_recv for PVM.\n"));

  // absolute priority for internal messages of MPSystem:
  for (code=MIN_PEOPS; code<=MAX_PEOPS; code++){
    // receive _all_ MP-internal messages,
    while (ISMPCODE(code) && pvm_probe(ANY_TASK, code))
      // handle an MP-internal message
      MPMsgHandle(code, 0);
  }
  // when we are here, we have consumed all pending MP-Messages
  // internally. But more MP-Messages may arrive..

  IF_PAR_DEBUG(mpcomm, debugBelch("MP_recv: system.\n"));
  // go through all possible OpCodes, pick system message
  for (code=MIN_PEOPS; code<=MAX_PEOPS; code++){
    // if code is for priority (system) messages:
    if ( ISSYSCODE(code) &&  
	 pvm_probe(ANY_TASK, code)) {
      buffer = pvm_recv(ANY_TASK, code); // receive 
      IF_PAR_DEBUG(mpcomm, debugBelch("Syscode received.\n"));
      pvm_bufinfo(buffer, &bytes, retcode, &sendPE); // inspect message
      ASSERT( *retcode==code );
      break; // got one message, no further check
    }
  }

  if (!buffer) { // no system messages received, receive anything (blocking)
    IF_PAR_DEBUG(mpcomm, debugBelch("MP_recv: data.\n"));
    buffer = pvm_recv(ANY_TASK,ANY_CODE);
    IF_PAR_DEBUG(mpcomm, debugBelch("received.\n"));
    pvm_bufinfo(buffer, &bytes, retcode, &sendPE); // inspect message
  }
  
  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("Packet No. (pvm-%d) (code %x (%s), "
                          "size %d bytes) from PE %x.\n",
			  buffer, *retcode, getOpName(*retcode), 
                          bytes, sendPE));

  // could happen that we pick up an MPCODE message here :-(
  if (ISMPCODE(*retcode)) {
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Urk: accidentally picked up an internal message!\n"));
    // handle message and make a recursive call to get another message
    MPMsgHandle(*retcode, buffer);
    return MP_recv(maxlength, destination, retcode, sender);
  }

  // for SYSCODE and normal messages:
  // identify sender (high-level uses only 1..nPEs)
  i=0;
  *sender=0;
  while (!(*sender) && (i < (int) nPEs)) {
    if (allPEs[i] == sendPE) *sender =i+1;
    i++;
  }
  if (!(*sender)) {
    // errorBelch: complain, but do not stop
    errorBelch("MPSystem(PVM): unable to find ID of PE # %x, aborting.",
	       sendPE);
#ifdef DEBUG
    exit(EXIT_FAILURE);
#else
    // ignore error, discard message and make a recursive 
    // call to get another message
    return MP_recv(maxlength, destination, retcode, sender);
#endif
  }

  // unpack data, if enough space, abort if not
  bytes = bytes / sizeof(long); // adjust bytes to longs
  if (bytes > maxlength) 
    // should never happen, higher levels send and expect at most maxlength bytes!
    barf("MPSystem(PVM): not enough space for packet (needed %d, have %d)!",
	 bytes, maxlength);
  pvm_upklong(destination, bytes, 1);

  return bytes; // data and all variables set, ready 
}

/* - a non-blocking probe operation (unspecified sender)
 */
rtsBool MP_probe(void){

  return (pvm_probe(ANY_TASK, ANY_CODE) > 0);
}

#endif /* PARALLEL_RTS && USE_PVM */
