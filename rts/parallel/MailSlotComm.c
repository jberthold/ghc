/* 
 * Generalised RTE for parallel Haskells.
 *
 * Windows Mail slot / POSIX msg box version for shared memory mcores
 */

#if defined(PARALLEL_RTS) && defined(USE_SLOTS) // whole file

#include "MPSystem.h" // this includes Rts.h as well...
#include "RtsUtils.h"
#include "PEOpCodes.h"

#if defined(mingw32_HOST_OS) 
#include "windows.h"

// TODO check these (HAVE_STRING_H or such)
#include <string.h>
#include <stdlib.h>
#include <unistd.h> // getpid, for random seed

/* global constants, declared in Parallel.h:
 *
 * nPEs   - nat: number of PEs in the parallel system
 * thisPE - nat: logical address of this PE btw. 1 and nPEs
 * IAmMainThread - rtsBool: indicating main PE (thisPE == 1)
 */
nat nPEs = 0;
nat thisPE = 0;
rtsBool IAmMainThread = rtsFalse;

/* shutdown counter */
nat finishRecvd = 0;

/* All processes create their input mail slots when going through
 * here. All slots use the following name pattern:
 *          \\.\mailslot\<prog-name>\<rnd-string>\<thisPE>
 * where rnd-string is an 8-char rnd string created by the main PE
 * and distributed via env.-variable "EdenSlot".
 * 
 */

/* this is the common prefix with prog.name and rnd string for slots */
char slotPrefix[252] = "";

/* this function uses slotPrefix to create a slot name following the
   given pattern, in the (256 char pre-allocated) slotName */
rtsBool mkSlotName(char slotName[], nat proc);

/* this will be the array of handles (write end for sending) */
HANDLE *mailslot;

/* this is our own slot (read end) */
HANDLE mySlot;

/* mail slots do not use message tags, and all data needs to be
   received in one piece. Therefore, make an internal type and copy
   all data from messages */
typedef struct SlotMsg_ {
  nat proc;  // sender
  char tag;  // message tag ("PEOpCode")
  char data[FLEXIBLE_ARRAY];
} SlotMsg;

/* this is data space to copy messages (proc, tag, DATASPACEWORDS words) */
SlotMsg* msg;

/* Helper to re-assemble a cmd line from argv, for CreateProcess */
static char* mkCmdLineString(int argc, char ** argv);

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
  /* Child processes are created here, and the mailslots are reserved,
   * but not used yet. Later, in MP_sync, we know DATASPACEWORDS and
   * limit the max. msg. size to it.
   *
   * As in CpComm, children are detected by checking environment
   * variable "EdenChild" (containing the child number), and a random
   * string for the mailslots is distributed via "EdenSlot"). 
   *
   * The main node creates its slot, spawns all children, and returns.
   * Children just set thisPE, create their slots and return.
   */
  char slotName[256], slotkey[9], buffer[9];
  int ret;

  /* Future: parse args to find out how many procs (+debug mode)
   * If we want to do anything here, we need to parse the arguments
   * (parseRtsFlags did not run yet). We extract the -N<count> or
   * -qp<count> flag(s), replace them by a harmless value (empty?) and
   * spawn as many children as given by the number. if no number is
   * given, we run with one process. */

  // Present: we just use the first argument as nPEs
  // (ParInit::startupParallelSystem cuts away the first arg.)

  if (*argc < 2) {
    errorBelch("Need argument to specify number of PEs\n");
    exit(EXIT_FAILURE);
  }

  // start in debug mode if negative number (or "-0")
  if (argv[1][0] == '-') {
    RtsFlags.ParFlags.Debug.mpcomm = rtsTrue;
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Mailslot debug mode, MP_start\n"));
    nPEs = (nat)atoi(argv[1]+1);
  } else {
    nPEs = (nat)atoi(argv[1]);
  }

  if (nPEs == (nat)0)
    nPEs = 1;

  /* is Environment Var set? then we are a child */
  buffer[0] = '0';
  buffer[1] = '\0';
  GetEnvironmentVariable("EdenChild", buffer, 9);

  sscanf(buffer, "%i", &thisPE);
  IF_PAR_DEBUG(mpcomm,
               debugBelch("EdenChild %i\n", thisPE));

  if(thisPE == 0) {
    thisPE = 1;
    IAmMainThread = rtsTrue;
  } else {
    IAmMainThread = rtsFalse;
  }

  if (IAmMainThread) {
    // generate an 8-character random string for the mailslot
    srand(getpid());
    snprintf(slotkey, 5, "%.4x", (unsigned int) rand());
    snprintf(slotkey+4, 5, "%.4x", (unsigned int) rand());
    IF_PAR_DEBUG(mpcomm, debugBelch("Chosen slotkey |%s|\n", slotkey));
  } else {
    // read the 8-character random string for the mailslot
    slotkey[0] = '\0';
    GetEnvironmentVariable("EdenSlot", slotkey, 9);
    ASSERT(strlen(slotkey) == 8);
  }

  // initialise slotPrefix (used in mkSlotName)
  ret=snprintf(slotPrefix, 252, "\\\\.\\mailslot\\%s\\%s\\", 
	       argv[0], slotkey);
  if (ret < 0 || ret == 252-1 || !(mkSlotName(slotName, thisPE))) {
    nPEs=0; barf("Failure during startup: failed to init slotPrefix");
  }

  // create own mail slot
  IF_PAR_DEBUG(mpcomm, debugBelch("creating slot %s\n", slotName));
  mySlot = CreateMailslot(slotName,
	       0,                             // no max. msg. size 
	       MAILSLOT_WAIT_FOREVER,         // no time-out
	       (LPSECURITY_ATTRIBUTES) NULL); // default security
 
  if (mySlot == INVALID_HANDLE_VALUE) { 
    sysErrorBelch("CreateMailslot failed\n");
    nPEs=0; // cannot communicate, so do not try clean shutdown
    barf("Comm.system malfunction during startup, aborting");
  }

  /* if main: spawn the other processes */
  if (IAmMainThread) {
    STARTUPINFO si;
    PROCESS_INFORMATION *pi;
    int i;
    char* cmdLine;

    GetStartupInfo(&si);
    pi = stgMallocBytes(sizeof(PROCESS_INFORMATION) *(nPEs -1), "PI");

    SetEnvironmentVariable("EdenSlot", slotkey);

    cmdLine = mkCmdLineString(*argc, argv);

    for (i = 2; i <= (int)nPEs; i++) {
      /* start other processes */
      IF_PAR_DEBUG(mpcomm, debugBelch("fork child %d\n", i));

      sprintf(buffer, "%u", i);
      SetEnvironmentVariable("EdenChild", buffer);

      // CreateProcess might modify the cmdLine string, so make a copy
      char *argsTmp = stgMallocBytes(strlen(cmdLine)+1, "cmdLine");
      strcpy(argsTmp, cmdLine);
      
      CreateProcess(NULL, cmdLine, NULL, NULL,
		    TRUE, 0, NULL, NULL, &si, &pi[i-2]);
      
      free(argsTmp);
      IF_PAR_DEBUG(mpcomm, debugBelch("child %d forked\n", i));
    }
    stgFree(cmdLine);
    stgFree(pi);
  }

  return rtsTrue;
}

/* MP_sync synchronises all nodes in a parallel computation:
 *  sets global var.: 
 *    thisPE - GlobalTaskId: node's own task Id 
 *             (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_sync(void) {
  /* All processes maintain an array mailslot[nPEs], with mailslot
   * write handles opened here.
   *
   * Main node receives all PP_READY from the children (in order
   * 2..nPEs), opens their slots and sends PP_PETIDS (just the number)
   * to children and returns (thisPE=1 has been set before).
   *
   * Children send PP_READY (as text) to the main node, receive
   * PP_PETIDS, and open all slots before they return.
   * 
   * thisPE should be set before, in MP_start.
   */

  char slotName[256];
  HANDLE writeEnd;

  BOOL fRes;
  DWORD rwCount;
  nat i;

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_sync\n"));

  // allocate the mailslot array (nPEs * sizeof(HANDLE)).
  mailslot = (HANDLE*) stgMallocBytes((int) nPEs*sizeof(HANDLE),
				      "Mailslot handles");

  // at this point we know DATASPACEWORDS
  msg = (SlotMsg*) stgMallocBytes(sizeof(SlotMsg)
				  + DATASPACEWORDS*sizeof(StgWord), "Slot");

  if (IAmMainThread) {
    nat proc;

    /* We could rule out that we send messages to ourselves.
     * DataComms does that for DATA and HEAD messages at the moment,
     * but not for RFORKs. Therefore, we have to also keep a handle to
     * our own slot for local sends.
     */
    mkSlotName(slotName, 1);
    writeEnd = CreateFile(slotName, GENERIC_WRITE, FILE_SHARE_READ,
			  (LPSECURITY_ATTRIBUTES) NULL, 
			  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			  (HANDLE) NULL); 
    if (writeEnd == INVALID_HANDLE_VALUE) { 
      nPEs = 0; barf("MP_sync error: cannot create local mailslot");
    }
    mailslot[ 0 ] = writeEnd;

    // for each child proc, receive PP_READY, then open its slot for writing.
    for (i = 1; i < nPEs; i++) {
      IF_PAR_DEBUG(mpcomm,
		   debugBelch("Awaiting PP_READY (%i of %i)\n", i, nPEs-1));
      fRes = ReadFile(mySlot, (char*) msg, sizeof(SlotMsg),
		      &rwCount, NULL);
      if (!fRes || msg->tag != PP_READY ) {
	sysErrorBelch("MP_sync: failed to read sync msg (%i of %i).",
		      i, nPEs-1);
	nPEs = 0; barf("aborting");
      }
      proc = msg->proc;
      if (proc < 2 || proc > nPEs) {
	nPEs = 0; barf("Inconsistent sync message (proc = %i)", proc);
      }
      IF_PAR_DEBUG(mpcomm, debugBelch("Received from proc %i\n", proc));

      mkSlotName(slotName, proc);
      writeEnd = CreateFile(slotName, GENERIC_WRITE, FILE_SHARE_READ,
			    (LPSECURITY_ATTRIBUTES) NULL, 
			    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			    (HANDLE) NULL); 
      if (writeEnd == INVALID_HANDLE_VALUE) { 
	nPEs = 0; barf("MP_sync error: cannot create mailslot for %i (%i of %i)",
		       proc, i, nPEs);
      }
      mailslot[ proc-1 ] = writeEnd;
    }

    // once this is done, send a message with PP_PETIDS to all
    IF_PAR_DEBUG(mpcomm, debugBelch("All received, BCast PP_PETIDS\n"));
    msg->proc = thisPE;
    msg->tag  = PP_PETIDS;
    for (i = 2; i <= nPEs; i++) {
      // send to mailslot[i]
      fRes = WriteFile(mailslot[i-1], (char*)msg, sizeof(SlotMsg),
		       &rwCount, (LPOVERLAPPED) NULL);
      if (!fRes) {
	nPEs = 0; barf("MP_sync error: cannot reach main node");
      }
    }
  } else {
    // open main node slot(1), send PP_READY
    if (!(mkSlotName(slotName, 1))) {
      nPEs = 0; barf("MP_sync error: cannot create mailslot name");
    }

    writeEnd = CreateFile(slotName, GENERIC_WRITE, FILE_SHARE_READ,
			  (LPSECURITY_ATTRIBUTES) NULL, 
			  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			  (HANDLE) NULL); 
    if (writeEnd == INVALID_HANDLE_VALUE) { 
      nPEs = 0; barf("MP_sync error: cannot create mailslot write end 1");
    }
    mailslot[0] = writeEnd;

    // can start using the global msg at this point...
    msg->proc = thisPE;
    msg->tag  = PP_READY;
    fRes = WriteFile(mailslot[0], (char*)msg, sizeof(SlotMsg),
		     &rwCount, (LPOVERLAPPED) NULL);
    if (!fRes) {
      nPEs = 0; barf("MP_sync error: cannot reach main node");
    }

    // receive PP_PETIDS (blocking until available)
    fRes = ReadFile(mySlot, (char*) msg, sizeof(SlotMsg),
		    &rwCount, NULL);
    if (!fRes || msg->tag != PP_PETIDS || msg->proc != 1 ) {
      sysErrorBelch("MP_sync: failed to read sync msg.");
      nPEs = 0; barf("aborting");
    }

    // then open all other slots
    for (i = 2; i <= nPEs; i++) {
      mkSlotName(slotName, i);
      writeEnd = CreateFile(slotName, GENERIC_WRITE, FILE_SHARE_READ,
			    (LPSECURITY_ATTRIBUTES) NULL, 
			    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 
			    (HANDLE) NULL); 
      if (writeEnd == INVALID_HANDLE_VALUE) { 
	nPEs = 0; barf("MP_sync error: cannot create mailslots (%i of %i)",
		       i, nPEs);
      }
      mailslot[i-1] = writeEnd;
    }
  }

  IF_PAR_DEBUG(mpcomm, debugBelch("MP_sync.ed PE %i\n", thisPE));
  return rtsTrue;
}

/* MP_quit disconnects current node from MP-System:
 * Main PE will shut down the parallel system, others just inform main PE.
 * Sets nPEs to 0 on exit, can be checked to avoid duplicate calls. 
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_quit(int isError) {
  /* same shutdown protocol as in CpComm:
   * if IAmMainThread: send PP_FINISH to all, await replies (count)
   * otherwise: send PP_FINISH to main, await reply if error
   *
   * Awaiting replies may be difficult, as messages need to be
   * received and data cannot be ignored. Unfortunately, one cannot
   * read from a mailslot piecemeal (in 1KB portions), so we need to
   * use the msg dataspace (one more reason to have it).
   *
   * both cases: close all client-side slots after this operation, and
   * free the mailslot array.
   */
  StgWord *data;
  DWORD rwCount;
  BOOL fRes;
  nat i;

  IF_PAR_DEBUG(mpcomm, debugBelch("MP_quit (%i%s)\n",
				  isError, isError?": ERROR!":"" ));

  msg->proc = thisPE;
  msg->tag  = PP_FINISH;
  data = (StgWord*) &msg->data; // haaack
  *data = isError;

  if (IAmMainThread) {
    /* send FINISH to other PEs */
    for (i=2; i <= nPEs; i++) {
      if (mailslot[i-1] != 0) {
	fRes = WriteFile(mailslot[i-1], (char*)msg, 
			 sizeof(SlotMsg)+ sizeof(StgWord),
			 &rwCount, (LPOVERLAPPED) NULL);
	if (!fRes) {
	  errorBelch("MP_quit: cannot PP_FINISH from main node to %i", i);
	}
      }
    }

    IF_PAR_DEBUG(mpcomm,
                 debugBelch("awaiting FINISH replies from children (have %i)\n",
                            finishRecvd));
    while (finishRecvd != nPEs-1) {
      /* receive msg.s, discard all but PP_FINISH messages */
      fRes = ReadFile(mySlot, (char*) msg, 
		      sizeof(SlotMsg) + sizeof(StgWord)*DATASPACEWORDS,
		      &rwCount, NULL);
      if (!fRes) {
	sysErrorBelch("MP_quit: failed to receive msg.");
      }
      if (msg->tag == PP_FINISH) {
          finishRecvd++;
          IF_PAR_DEBUG(mpcomm,
                       debugBelch("received reply from %i, now %i\n",
				  msg->proc, finishRecvd));
        }
    }
    IF_PAR_DEBUG(mpcomm,
                 debugBelch("All kids are safe home.\n"));

  } else {
    /* send PP_FINISH to parent, including error code received */
    IF_PAR_DEBUG(mpcomm,
                 debugBelch("child finishing (code %i),"
                            "sending FINISH\n", isError));
    fRes = WriteFile(mailslot[0], (char*)msg, 
		     sizeof(SlotMsg)+ sizeof(StgWord),
		     &rwCount, (LPOVERLAPPED) NULL);
    if (!fRes) {
      errorBelch("MP_quit: cannot PP_FINISH to main node");
      nPEs = 0; barf("aborting clean shutdown!");
    }

    /* child must stay alive until answer arrives if error shutdown */
    if (isError != 0) {
      IF_PAR_DEBUG(mpcomm,
            debugBelch("waiting for reply (error case)\n"));
      while (msg->tag != PP_FINISH) {
	fRes = ReadFile(mySlot, (char*) msg, 
			sizeof(SlotMsg) + sizeof(StgWord)*DATASPACEWORDS,
			&rwCount, NULL);
	if (!fRes) {
	  sysErrorBelch("MP_quit: failed to receive msg.");
	}
      }

      IF_PAR_DEBUG(mpcomm,
            debugBelch("child received reply, shutting down (error case)\n"));
    }
  }

  /* close all write handles */
  for (i = 1; i <= nPEs; i++) {
    CloseHandle(mailslot[i-1]);
  }

  /* free data structures */
  stgFree(mailslot);
  stgFree(msg);

  /* close mySlot read handle ... but how? no API */

  nPEs=0; // indicate shutdown has completed

  return rtsTrue;
}


/**************************************
 * Data Communication between nodes:  */

/* a send operation for peer2peer communication: 
 * sends the included data (array of length length) to the indicated node
 * (numbered from 1 to the requested nodecount nPEs) with the given message
 * tag. Length 0 is allowed and leads to a message containing no payload data.
 * The send action may fail, in which case rtsFalse is returned, and the 
 * caller is expected to handle this situation.
 * Data length should be given in bytes, data will be sent raw
 * (unsigned char).
 *
 * Parameters:
 *   IN node     -- destination node, number between 1 and nPEs
 *   IN tag      -- message tag
 *   IN data     -- array of raw data (unsigned char values) to send out
 *   IN length   -- length of data array. Allowed to be zero (no data).
 * Returns:
 *   rtsBool: success or failure inside comm. subsystem
 */

rtsBool MP_send(int node, OpCode tag, StgWord8 *data, int length) {
  /*
   * use the respective slot to send out the data (as a byte string)
   *
   * Mailslots do not have tags, therefore the tag is sent as the
   * first data of the message. Tags are in a range 0x50-0x5c,
   * therefore one byte.
   * Lame: requires copying the data... could hack around it by
   * manipulating the first four bytes (sender port) in data
   * messages - we only send data messages with this routine.
   *
   * We use the custom type SlotMsg, and have a static "msg" to copy
   * tag and data.
   */
  DWORD rwCount;
  BOOL fRes;

  IF_PAR_DEBUG(mpcomm, debugBelch("MP_send(%s) to %i\n", 
				  getOpName(tag), node));

  ASSERT(node >= 1 && node <= (int) nPEs);

  msg->proc = thisPE;
  msg->tag  = tag;
  memcpy(&msg->data, data, length);
  
  // send "msg" of length+sizeof(SlotMsg) bytes to mailslot[ node-1 ]
  fRes = WriteFile(mailslot[node-1], (char*)msg, sizeof(SlotMsg)+length,
		   &rwCount, (LPOVERLAPPED) NULL);
  if (!fRes) {
    // we could return rtsFalse here, but that would probably loop...
    sysErrorBelch("MP_send failed");
    barf("Comm. system malfunction, aborting.");
  }

  IF_PAR_DEBUG(mpcomm, 
	       debugBelch("MP_send: sent %i Bytes (== %i?) in %s message\n",
			  (int) rwCount-sizeof(SlotMsg),
			  length, getOpName(tag)));

  return rtsTrue;
}

/* - a blocking receive operation
 *   where system messages from main node have priority! 
 * Effect:
 *   A message is received from a peer. 
 *   Data are received as unsigned char values and stored in
 *   destination (capacity given in Bytes), and opcode and sender
 *   fields set.
 *   If no messages were waiting, the method blocks until a 
 *   message is available. If too much data arrives (> maxlength),
 *   the program stops with an error (resp. of higher levels).
 * 
 * Parameters: 
 *   IN  maxlength   -- maximum data length (in bytes)
 *   IN  destination -- where to unpack data (unsigned char array)
 *   OUT code   -- OpCode of message (aka message tag)
 *   OUT sender -- originator of this message 
 * Returns: 
 *   int: amount of data (in bytes) received with message
 */
int MP_recv(int maxlength, StgWord8 *destination, // IN
	    OpCode *code, nat *sender) {          // OUT
  /* use GetMailslotInfo to determine whether messages are waiting,
   * receive the first one (unless size is too big, in which case
   * the entire system fails).
   */

  DWORD msgBytes;
  BOOL fResult;

  IF_PAR_DEBUG(mpcomm, debugBelch("MP_recv\n"));
  ASSERT((nat) maxlength <= DATASPACEWORDS*sizeof(StgWord));

  fResult = ReadFile(mySlot, msg, sizeof(SlotMsg) + maxlength,
		     &msgBytes, NULL);
  if (!fResult) { 
    sysErrorBelch("failed to MP_recv.");
    barf("Comm. system malfunction, aborting.");
  }


  // retrieve information from mailslot (necessary? can also just "read")
  // receive msg of given length into msg
  // take it apart:
  *sender = msg->proc;
  *code   = msg->tag;
  memcpy(destination, &msg->data, msgBytes-sizeof(SlotMsg));
  /* attention, hack                       ^^^^^^^^^^^^^^ */

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_recv: received %i Byte in %s message\n",
			  (int) msgBytes-sizeof(SlotMsg), getOpName(*code)));

  if (*code == PP_FINISH) finishRecvd++; /* for error shutdown */

  return ((int)msgBytes - sizeof(SlotMsg));
}

/* - a non-blocking probe operation 
 * (unspecified sender, no receive buffers any more) 
 */
rtsBool MP_probe(void) {
  /* use GetMailslotInfo to determine whether a message is waiting,
   * check for MAILSLOT_NO_MESSAGE
   */
  
  DWORD fResult, msgBytes, msgCount;

  fResult = GetMailslotInfo(mySlot, NULL,
			    &msgBytes, &msgCount, NULL);
 
  if (!fResult) { 
    sysErrorBelch("failed to GetMailslotInfo"); 
    barf("Comm. system malfunction, aborting.");
  }
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_probe: %i messages waiting (first: %i Byte).\n",
			  (int) msgCount, (int) msgBytes));

  return (msgBytes != MAILSLOT_NO_MESSAGE);
}

/* collate all args to a single string, passed to CreateProces */
static char* mkCmdLineString(int argc, char ** argv) {
  int len = argc*3;
  int i;
  for (i = 0; i < argc; i++) {
    len += strlen(argv[i]);
  }
	
  char *result = stgMallocBytes(sizeof(char) * (len+1), "cmd line string");
  result[0] = '\0';
	
  for (i = 0; i < argc; i++) {
    strcat(result, "\"");
    strcat(result, argv[i]);
    strcat(result, "\" ");
  }

  return result;
}

/* create a (255-char) slot name from an (8-char) rnd string and a
 * proc number between 1 and 999.
 *
 * All slots use the common name pattern:
 *          \\.\mailslot\<prog-name>\<rnd-string>\<thisPE>
 * where rnd-string is an 8-char rnd string created by the main PE
 * The function assumes slotPrefix to be initialised to this string,
 * except the final <thisPE>.
 *
 * Parameters: 
 *    OUT slotName (assumed 256 char allocated)
 *    IN  proc     (proc number)
 * Returns rtsFalse on failures (to be caught by caller)
 */
rtsBool mkSlotName(char slotName[], nat proc) {
  char procStr[4];

  if (proc > 999) return rtsFalse;
  ASSERT(strlen(slotPrefix) != 0 && strlen(slotPrefix) < 251);

  slotName[0] = slotName[252] = '\0';
  strncpy(slotName, slotPrefix, 251);
  snprintf(procStr, 4, "%i", proc);
  strncat(slotName, procStr, 3);

  return rtsTrue;
}

#else /* not windows... */

#error "Slot version for POSIX does not exist yet."

#endif

#endif // USE_SLOTS
