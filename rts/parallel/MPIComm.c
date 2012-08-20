
/* 
 * Generalised RTE for parallel Haskells.
 *
 * File: ghc/rts/parallel/MPIComm.c
 *
 * 
 *
 *
 * Purpose: map generalised Comm.functions to MPI, 
 * abstracting from the concrete MP-System
 */

#if defined(PARALLEL_RTS)&&defined(USE_MPI)  /* whole file */

#include "Rts.h"      // general Rts definitions
#include "MPSystem.h" // general interface for message passing

#include "mpi.h" // MPI standard include, appropriate include path
		 // needed
#include "RtsUtils.h" // utilities for error msg., allocation, etc.
#include "PEOpCodes.h" // message codes
#include "Rts.h" //sendBufferSize
#include <string.h>

/* Global conditions defined here. */
// main thread (PE 1 in logical numbering)
rtsBool	IAmMainThread = rtsFalse;	// Set for the main thread
// nPEs, thisPE
nat nPEs = 0; // number of PEs in system
nat thisPE=0; // node's own ID

/* overall variables for MPI (for internal use only) */
// group size, own ID, Buffers 
int mpiWorldSize;
int mpiMyRank;

// request for send operations, one for each comm. partner (dynamic!)
// UNUSED YET, see MP_send()
MPI_Request *commReq[MAX_PES];

int maxMsgs;
// MPI Message buffer, can hold maxMsgs messages to every PE.
int bufsize;
void *mpiMsgBuffer;
MPI_Request *requests;
int msgCount; // to detach/attach buffer to avoid buffer overflow
//request and buffer for Ping on sysComm
MPI_Request sysRequest;
int pingMessage=0;
int pingMessage2=0;

// status for receive and probe operations:
MPI_Status status;
/* mpi.h sez:
struct _status
	int		MPI_SOURCE;
	int		MPI_TAG;
	int		MPI_ERROR;
	.. more, implementation-dependent
*/

// communicator for system messages
MPI_Comm sysComm;

/**************************************************************
 * Startup and Shutdown routines (used inside ParInit.c only) */

/* MP_start starts up the node: 
 *   - connects to the MP-System used, 
 *   - determines wether we are main thread
 *   - starts up other nodes in case we are first and 
 *     the MP-System requires to spawn nodes from here.
 * Parameters: 
 *     IN    argv  - char**: program arguments
 * Sets:
 *           nPEs - int: no. of PEs to expect/start
 *  IAmMainThread - rtsBool: wether this node is main PE
 * Returns: Bool: success or failure
 * 
 * MPI Version: 
 *   nodes are spawned by startup script calling mpirun
 *   This function only connects to MPI and determines the main PE.
 */
rtsBool MP_start(int* argc, char** argv) {

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MPI_Init: starting MPI-Comm...\n"));

  MPI_Init(argc, &argv); // MPI sez: can modify args
  
  MPI_Comm_rank(MPI_COMM_WORLD, &mpiMyRank);
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("I am node %d.\n", mpiMyRank));

  if (!mpiMyRank) // we declare node 0 as main PE.
    IAmMainThread = rtsTrue;

  MPI_Comm_size(MPI_COMM_WORLD, &mpiWorldSize);

  // we should have a correct argument...
  ASSERT(argv && argv[1]);
  nPEs = atoi(argv[1]);

  if (nPEs) { // we have been given a size, so check it:
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("Expecting to find %d processors, found %d.",
			    nPEs, mpiWorldSize));
    if ((int)nPEs > mpiWorldSize) 
      IF_PAR_DEBUG(mpcomm,
		   debugBelch("WARNING: Too few processors started!"));
  } else {  // otherwise, no size was given
    IF_PAR_DEBUG(mpcomm,
		 debugBelch("No size, given, started program on %d processors.",
			    mpiWorldSize));
  }
  nPEs = mpiWorldSize; //  (re-)set size from MPI (in any case)
  
  // System communicator sysComm is duplicated from COMM_WORLD
  // but has its own context
  MPI_Comm_dup(MPI_COMM_WORLD, &sysComm); 
  
  return rtsTrue;
}

/* MP_sync synchronises all nodes in a parallel computation:
 * sets: 
 *     thisPE - GlobalTaskId: node's own task Id 
 *                     (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 *
 * MPI Version:
 *   the number of nodes is checked by counting nodes in WORLD
 *   (could also be done by sync message)
 *   Own ID is known before, but returned only here.
 */
rtsBool MP_sync(void) {
  // initialise counters/constants and allocate/attach the buffer

  // buffer size default is 20, use RTS option -qq<N> to change it
  maxMsgs = RtsFlags.ParFlags.sendBufferSize;
  // and resulting buffer space
  bufsize = maxMsgs * DATASPACEWORDS * sizeof(StgWord);

  mpiMsgBuffer = (void*) stgMallocBytes(bufsize, "mpiMsgBuffer");
  
  requests = (MPI_Request*) stgMallocBytes(maxMsgs * sizeof(MPI_Request), "requests");

  msgCount = 0; // when maxMsgs reached

  thisPE = mpiMyRank + 1;

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("Node %d synchronising.\n", thisPE));

  MPI_Barrier(MPI_COMM_WORLD); // unnecessary...
                               // but currently used to synchronize system times

  return rtsTrue;
}

/* MP_quit disconnects current node from MP-System:
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 *
 * MPI Version: MPI requires that all sent messages must be received
 * before quitting. Receive or cancel all pending messages (using msg.
 * count), then quit from MPI.
 */
rtsBool MP_quit(int isError) {
  long data[2];
  MPI_Request sysRequest2;
  if (IAmMainThread) {
    int i;

    IF_PAR_DEBUG(mpcomm,
	       debugBelch("Main PE stopping MPI system (exit code: %d)\n",
			  isError));
    // bcast FINISH to other PEs
    data[0] = PP_FINISH;
    data[1] = isError;

    for (i=2; i<=(int)nPEs; i++){
      // synchronous send operation in order 2..nPEs ... might slow down. 
      MPI_Isend(&pingMessage, 1, MPI_INT, i-1, PP_FINISH, sysComm, &sysRequest2);  
      MPI_Send(data,2,MPI_LONG,i-1, PP_FINISH, MPI_COMM_WORLD);
      MPI_Wait(&sysRequest2, MPI_STATUS_IGNORE);
    }
  }
  
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("shutting down MPI now (exit code: %d)\n",
			  isError));

  // TODO: receive or cancel all pending messages...
  /* ------------------------------------------------
   *q&d solution:
   * receive anything retrievable by MPI_Probe
   * then get in sync
   * then again receive remaining messages
   *
   * (since buffering is used, and buffers are detached to force
   * messages, a PE might get stuck detaching its mpiMsgBuffer, and
   * send another message as soon as buffer space is available again.
   * The other PEs will not )
   *
   * ---------------------------------------------- */
  {
    // allocate fresh buffer to avoid overflow
    void* voidbuffer;
    
    voidbuffer = (void*) 
	  stgMallocBytes(DATASPACEWORDS * sizeof(StgWord), "voidBuffer");
      
    // receive whatever is out there...
    while (MP_probe()) {
      MPI_Recv(voidbuffer, DATASPACEWORDS, MPI_LONG, 
	       MPI_ANY_SOURCE, MPI_ANY_TAG, 
	       MPI_COMM_WORLD, &status);
      if (ISSYSCODE(status.MPI_TAG)) 
        MPI_Recv(voidbuffer, 1, MPI_INT, 
                 MPI_ANY_SOURCE, MPI_ANY_TAG, 
                 sysComm, MPI_STATUS_IGNORE);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    // all in sync (noone sends further messages), receive rest
    while (MP_probe()) { 
      MPI_Recv(voidbuffer, DATASPACEWORDS, MPI_LONG, 
	       MPI_ANY_SOURCE, MPI_ANY_TAG, 
	       MPI_COMM_WORLD, &status);
      if (ISSYSCODE(status.MPI_TAG)) 
        MPI_Recv(voidbuffer, 1, MPI_INT, 
                 MPI_ANY_SOURCE, MPI_ANY_TAG, 
                 sysComm, MPI_STATUS_IGNORE);
    }
    stgFree(voidbuffer);
  }
  // end of q&d

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("detaching MPI buffer\n"));
  stgFree(mpiMsgBuffer);

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("Goodbye\n"));
  MPI_Finalize();

  return rtsTrue;
}

/* Dummies, TODO
 */
rtsBool MP_send(int node, OpCode tag, long *data, int length){
  /* MPI normally uses blocking send operations (MPI_*send). When
   * using nonblocking operations (MPI_I*send), dataspace must remain
   * untouched until the message has been delivered (MPI_Wait)!
   *
   * We copy the data to be sent into the mpiMsgBuffer and call MPI_Isend.
   * We can reuse slots in the buffer where messages are already delivered. 
   * The requests array stores a request for each send operation which can be tested by 
   * MPI_Testany for delivered messages.
   * MP_send should return false to indicate a send failure (the message buffer has 0 free 
   * slots). 
   * 
   */
  int sendIndex;
  int hasFreeSpace;
  //MPI_Status* status;

  StgPtr sendPos; // used for pointer arithmetics, so not void*. Assuming
                  // sizeof(StgPtr)==sizeof(long) (see includes/stg/Types.h)

  ASSERT(node > 0 && (nat)node <= nPEs);

  
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MPI sending message to PE %d "
			  "(tag %d (%s), datasize %d)\n",
			  node, tag, getOpName(tag), length));
  // adjust node no.
  node--;
  //case each slot in buffer has been used
  if (msgCount == maxMsgs) {
    // looking for free space in buffer
    IF_PAR_DEBUG(mpcomm,
                 debugBelch("looking for free space in buffer\n"));
    MPI_Testany(msgCount, requests, &sendIndex, &hasFreeSpace, MPI_STATUS_IGNORE);
    //if (status->MPI_ERROR) barf("a send operation returned an error with code %d and sendIndex is %d and hasFreeSpace %d",status->MPI_ERROR,  sendIndex,  hasFreeSpace); 
  }
  //case still slots in buffer unused
  else {
    hasFreeSpace = 1;
    sendIndex = msgCount++;
  } 
  // send the message
  if (!hasFreeSpace){
    IF_PAR_DEBUG(mpcomm,
	       debugBelch("MPI CANCELED sending message to PE %d "
                          "(tag %d (%s), datasize %d)\n",
			  node, tag, getOpName(tag), length));   
    return rtsFalse;
  }
  //calculate offset in mpiMsgBuffer
  // using ptr. arithmetics and void* size (see includes/stg/Types.h)
  sendPos = ((StgPtr)mpiMsgBuffer) + sendIndex * DATASPACEWORDS;
  memcpy((void*)sendPos, data, length * sizeof(StgWord));

  if (ISSYSCODE(tag)){  //case system message (workaroud: send it on both communicators, because there is no receive on two comunicators.)
      MPI_Isend(&pingMessage, 1, MPI_INT, node, tag, 
            sysComm, &sysRequest);    
  }
  MPI_Isend(sendPos, length, MPI_LONG, node, tag, 
            MPI_COMM_WORLD, &(requests[sendIndex]));
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("Done sending message to PE %d\n", node+1));
  return rtsTrue;
}

/* - a blocking receive operation
   where system messages from main node have priority! */

int MP_recv(int maxlength, long *destination,
	    OpCode *retcode, nat *sender) {
  /* MPI: Use MPI_Probe to get size, sender and code; check maxlength;
   *      receive required data size (must be known when receiving in
   *      MPI)
   *   No special buffer is needed here, can receive into *destination.
   */
  int source, size, code;
  int haveSysMsg = rtsFalse;
  code = MIN_PEOPS-1;

  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MP_recv for MPI.\n"));

    // priority for system messages, probed before accepting anything
    // non-blocking probe,
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, sysComm, &haveSysMsg, &status);
    // blocking probe for other message, returns source and tag in status
  if (!haveSysMsg){ 
    // there is no system message: get meta data of the first message on MPI_COMM_WORLD
   MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    source = status.MPI_SOURCE;
    code = status.MPI_TAG;
  }
  else{
    // there is a system message:
    // still need to probe on MPI_COMM_WORLD for the system message to get status of the message and the messages size
    source = status.MPI_SOURCE;
    code = status.MPI_TAG;
    MPI_Probe(source, code, MPI_COMM_WORLD, &status);
  }
  if (status.MPI_ERROR != MPI_SUCCESS) {
    debugBelch("MPI: Error receiving message.\n"); 
    barf("PE %d aborting execution.\n", thisPE);
  }

  // get and check msg. size
  // size = status.st_length;
  MPI_Get_count(&status, MPI_LONG, &size);
  if (maxlength < size) 
    barf("wrong MPI message length (%d, too big)!!!", size);
  MPI_Recv(destination, size, MPI_LONG, source, code, MPI_COMM_WORLD, &status);

  *retcode = status.MPI_TAG;
  *sender  = 1+status.MPI_SOURCE;

  // If we received a sys-message on COMM_WORLD, we need to receive it also on sysComm:
  // if MPI_Iprobe on sysComm has failed, a sysMessage might have arived later - 
  // don't use haveSysMsg for the decission, use ISSYSCODE(code) instead.
  if (ISSYSCODE(code)){
    MPI_Recv(&pingMessage2, 1, MPI_INT, source, code, sysComm, &status);
  }
  IF_PAR_DEBUG(mpcomm,
	       debugBelch("MPI Message from PE %d with code %d.\n",
			  *sender, *retcode));
  
  ASSERT(*sender == (nat)source+1 && *retcode == code);
  return size;
}

/* - a non-blocking probe operation (unspecified sender) 
 */
rtsBool MP_probe(void){
  int flag = 0;

  // non-blocking probe: either flag is true and status filled, or no
  // message waiting to be received. Using ignore-status...
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
  // We send and receive all sys-messages twice. Don't need to probe additionaly on the sysCom communictor.
  // Use code beneth when we switch to a counting procedure.

  //if (flag == 0){ // check for message on system communicator if there is no message on MPI_COMM_WORLD
  //  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, sysComm, &flag, MPI_STATUS_IGNORE);
  //}
  return (flag != 0);
}


#endif /* whole file */
