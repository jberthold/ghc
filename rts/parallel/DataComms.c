/* ----------------------------------------------------------------------------
 * Time-stamp: <Wed Mar 21 2001 16:34:41 Stardate: [-30]6363.45 hwloidl>
 *
 * High Level Communications Routines (HLComms.lc)
 *
 * Contains the high-level routines (i.e. communication
 * subsystem independent) used by GUM
 * 
 * GUM 0.2x: Phil Trinder, Glasgow University, 12 December 1994
 * GUM 3.xx: Phil Trinder, Simon Marlow July 1998
 * GUM 4.xx: H-W. Loidl, Heriot-Watt University, November 1999 -
 * 
 * GenPar 6.x: J.Berthold, Philipps-Universität Marburg 2006
 *      this file contains routines for *data* communication only
 * ------------------------------------------------------------------------- */

#ifdef PARALLEL_RTS /* whole file */

#include "Rts.h"
#include "RtsUtils.h"
#include "RTTables.h"
#include "PEOpCodes.h"
#include "MPSystem.h"
#include "Trace.h"

#if defined(mingw32_HOST_OS)
#define srand48(s) srand(s)
#define drand48() (((double)rand())/((double)RAND_MAX))
#define lrand48() rand()
#endif

#include "Threads.h" // updateThunk

#include <unistd.h> // getpid, in choosePE

nat targetPE = 0; 

/*
 * ChoosePE selects a PE number between 1 and nPEs, either 'at
 * random' or round-robin, starting with thisPE+1
 */
static nat 
choosePE(void)
{
  nat temp;

  // initialisation
  if (targetPE == 0) { 
    targetPE = (nPEs == thisPE) ? 1 : (thisPE + 1); 
    srand48(getpid());  // seed for random placement
  }

  if (RtsFlags.ParFlags.placement & 1) // random
    temp = 1 + (nat) (lrand48() % nPEs);
  else {// round-robin
    temp = targetPE; 
    targetPE = (targetPE >= nPEs) ? 1 : (targetPE + 1);
  }
  if ((RtsFlags.ParFlags.placement & 2) // no local placement
      && (temp ==  thisPE)) { 
    temp = (temp ==  nPEs) ? 1 : (temp + 1);
  }
  IF_PAR_DEBUG(procs,
	       debugBelch("chosen: %d, new targetPE == %d\n", 
			  temp,targetPE));
  return temp;
}


/* Communication between machines and processes:
 *   Machines and Processes communicate messages tagged with 
 *   "OpCodes" (defined in PEOpCodes.h) and exchange packed
 *   subgraphs of heap closures. 
 *   Besides, the MP-System has its own messages, not treated here.
 *
 * Structure of standard (non-MPSystem) messages:
 *    Port      Port       (optional) data section
 * -------------------------------------------------------------
 * | Sender | Receiver | id | tso | nelem | data(length nelem) |
 * -------------------------------------------------------------
 * |<------------- rtsPackBuffer ----------------------------->|
 * 
 * The rtsPackBuffer is defined to capture exactly this structure
 * (includes the sender and receiver), so we can just send it away
 * as-is and only have to compute the size.

 * More on ports in RTTables.h, on packing data in Pack.c
 */


/* sendMsg()
 *  sends a message tagged, with given tag, via given port,
 *  if buffer is not empty:  containing given packed graph
 * otherwise just using the sender/receiver fields
 */
rtsBool sendMsg(OpCode tag, rtsPackBuffer* dataBuffer) {
  int size;
  int destinationPE = 0;

  // TODO should be checked in configure! (compile time constant)
  ASSERT(sizeof(StgWord) == sizeof(long));

  ASSERT(!(isNoPort(dataBuffer->sender)));
  ASSERT(!(isNoPort(dataBuffer->receiver)));
  
  ASSERT(dataBuffer->sender.machine == thisPE);

  destinationPE = dataBuffer->receiver.machine;
  ASSERT(destinationPE != 0); // PEs are (1..nPEs), 0 impossible

  IF_PAR_DEBUG(ports,
	       debugBelch("sending message %s (%#0x) to machine %d\n",
			  getOpName(tag), tag, destinationPE);
	       debugBelch("Sender: (%d,%d,%d), Receiver (%d,%d,%d)\n",
			  dataBuffer->sender.machine, 
			  dataBuffer->sender.process, 
			  dataBuffer->sender.id,
			  dataBuffer->receiver.machine, 
			  dataBuffer->receiver.process, 
			  dataBuffer->receiver.id));

  if (dataBuffer->size != 0) {
    size = sizeof(rtsPackBuffer)/sizeof(long) + dataBuffer->size;
  } else {
    size = sizeof(rtsPackBuffer)/sizeof(long);
  }

  if (MP_send(destinationPE, tag, (long*) dataBuffer, size)) {
    
    // edentrace: emit event sendMessage(tag,dataBuffer)
    traceSendMessageEvent(tag,dataBuffer);
    IF_PAR_DEBUG(ports,
	       debugBelch("finished sending message to %d\n",
			  destinationPE));
    return rtsTrue;
  } else {
    return rtsFalse;
  }
}


// fwd declaration 
int fakeDataMsg(StgClosure *graph, Port sender, Port receiver, 
		Capability * cap, OpCode tag);

int sendWrapper(StgTSO *sendingtso, int mode, StgClosure *data);
/* sendWrapper
 * 
 * This function is intended as a lean interface for primitive
 * operations. From the RTS, we should use sendMsg above instead of
 * bloating this place with even more functionality. However, we may
 * safely use what we have here for "the normal case".
 *
 * Hardcoded send modes:
 *   1: connection message, makes receiver know its sender
 *   2: stream data, one list element is sent
 *   3: single data, receiver's inport is closed
 *   4: rFork, receiver creates thread to evaluate received graph
 *
 * HACK:
 *  "real" mode is (mode & 007), last 3 bits. Rest is payload data.
 *
 * Ports:
 *  1-3 are sent via a "normal"/"data" port (machine,process,threadid)
 *        and received on a normal inport   (machine,process,inportid)
 *
 *  4 (rFork) is sent via a "process" port  (machine,process, 0 )
 *        and received on the RtsPort       (target ,  0    , 0 )
 */
int sendWrapper(StgTSO *sendingtso, int mode, StgClosure *data) {
  
  rtsPackBuffer *packedData = (rtsPackBuffer*) NULL;
  rtsPackBuffer dummyBuffer;
  OpCode sendTag = 0;
  int success=2; // indicates successful packing, becomes return value 
                 // CONVENTION: 0 = packing failed, 1 = sending failed
                 // other values = all fine

  // mode splits into ( data payload | last 3 bits: "real" mode)
  int m; // mode 0..7
  nat d; // data payload inside mode

  Port* receiver;
  Port sender;
  // if this message creates a new process, we do not want to destroy
  // the thread's registered port

  Port rtsPort = (Port) {0, 0, 0};

  // set sender (partially, id only set when not sending an rFork)
  sender = (Port) { thisPE , MyProcess(sendingtso), 0 };

  receiver = MyReceiver(sendingtso);
  // for rFork, we need to protect the sendertso's receiver

  // split mode into d and m:
  m = mode & 007;
  d = mode >> 3;
  IF_PAR_DEBUG(ports,
	       debugBelch("sendWrapper: mode %d = ( %d | %d )\n", 
			  mode, d, m));
  switch (m) {
  case 1: // connection message, could be part of "connectToPort#"
    sendTag = PP_CONNECT;
    ASSERT(!(isNoPort(*receiver)));
    sender.id = sendingtso->id;

    // same machine, same runtime tables, so we connect directly
    if ( sender.machine == receiver->machine ) {
      connectInportByP(*receiver, sender);
      return 2;
    }

    // no data needed
    packedData = &dummyBuffer;
    packedData->size = 0;
    break;
  case 2: // stream data
    sendTag = PP_HEAD;
    ASSERT(!(isNoPort(*receiver)));
    sender.id = sendingtso->id;
    goto packData; // brrr..

  case 3: // single data
    sendTag = PP_DATA;
    ASSERT(!(isNoPort(*receiver)));
    sender.id = sendingtso->id;
    goto packData; // brrr..

  case 4: // rFork: single data, different tag, d is target
    sendTag = PP_RFORK;

    // d might be an explicit target PE... 
    if (d == 0) { // select by RTS placement policy
      d = choosePE();
    } else {      // gets adjusted to 1..nPEs
      d %= nPEs;
      d = d == 0 ? nPEs : d;
    }

    // do not use the sender thread's registered port!
    receiver = &rtsPort;
    receiver->machine = d; // other machine's RtsPort
    goto packData; // brrr..
    
    /* insert new modes here, document them above and in
     * primops.txt.pp. 
     * 
     * Possible free modes are 0,5,6,7 (see PLACEMENT HACK)
     * and may carry payload data in higher bits.
     */

  packData:
    // shortcut if sender and receiver share the same heap
    if ( (sender.machine == receiver->machine) 
	 //   conceptually OK if same process, in practice, same machine is enough
#ifdef PEDANTIC
	 && (sender.process == receiver->process)
#endif
	 //   we do this only for DATA and HEAD messages, tags 2 and 3
	 && (m & 2)) {
      // do processDataMsg() directly w/o unpacking, defined at the bottom
      return fakeDataMsg(data, sender, *receiver, sendingtso->cap, sendTag);
    }

    // pack the graph, needed in modes 2-4
    // TODO: later, we need to save/restore the sendingtso's receiver.
    // PackNearbyGraph will send parts to sendingtso's receiver and
    // change the tag to PP_PART if the graph is too large for one
    // message.
    packedData = PackNearbyGraph(data, sendingtso, &sendTag);

    // graph might contain blackholes, in which case sendingtso 
    // blocks (state set in PackNearbyGraph, blocked when returning
    // success == 0 to the calling primop sendData# )
    success = (packedData != (rtsPackBuffer *) NULL); 

    break;

  default:
      barf("sendWrapper: unimplemented send mode %d", mode);

  } // switch

  if (success != 0) {
    // successfully packed, or not packed at all => OK, send it away
    packedData->receiver = *receiver;
    packedData->sender = sender;
    if ( !sendMsg(sendTag, packedData)) {
      // handle failing send, by returning -1 to the caller (primitive op.)
      success = 1;
    } else {
      success = 2;
    }
    IF_PAR_DEBUG(mpcomm,debugBelch("Sending of message from thread %d returned code %d\n", (int) sendingtso->id, success));
  }
  if (success == 0 || success == 1) {
    // in rFork case, packing might have failed, so RR-placement must
    // restore the last targetPE for the next call
    if ( m==4 && (mode >> 3) == 0) {
      targetPE = (targetPE == 1) ? nPEs : (targetPE - 1);
      IF_PAR_DEBUG(pack,
		   debugBelch("packing failed, resetting target PE to %d)\n", 
			      targetPE));
    }
  }

  // restore receiver (for rFork case only)
  //  setReceiver(sendingtso,
  //	      savePort.machine, savePort.process, savePort.id);

  return success;
}



/* Heap Data Messages: Data, Head, Constr
 *   contains a subgraph. Receiving triggers a new process which 
 *   evaluates the sent subgraph (see Schedule.c::processMessages)
 *
 *   Sending is triggered by a primitive operation.
 */
void
processDataMsg(Capability * cap, OpCode tag, rtsPackBuffer *gumPackBuffer) {
  StgClosure *graph;
  Inport* inport;
  StgClosure *placeholder;

  IF_PAR_DEBUG(pack,
	       debugBelch("Processing data message (%s, tag %#0x)\n",
			  getOpName(tag), tag));

  ASSERT(gumPackBuffer->receiver.process != 0);
  inport = findInportByP(gumPackBuffer->receiver);

  if (inport == NULL) {
    IF_PAR_DEBUG(ports,
		 errorBelch("unknown inport: Port (%d,%d,%d)\n",
			    gumPackBuffer->receiver.machine, 
			    gumPackBuffer->receiver.process, 
			    gumPackBuffer->receiver.id));
    // just ignore the message... (shrug)
    return;
  }

  if (!(equalPorts(inport->sender,gumPackBuffer->sender))) {
    IF_PAR_DEBUG(ports,
		 debugBelch("Sender (%d,%d,%d) not connected yet\n",
			    gumPackBuffer->sender.machine, 
			    gumPackBuffer->sender.process, 
			    gumPackBuffer->sender.id));
    if (tag != PP_DATA)
      connectInportByP(gumPackBuffer->receiver, gumPackBuffer->sender);
  }

  // select placeholder closure (assumed: blackhole!)
  placeholder = inport->closure;
  ASSERT(IsBlackhole(placeholder));

  // unpack the graph
  graph = UnpackGraph(gumPackBuffer, gumPackBuffer->receiver, cap);

  // replace placeholder by received data, possibly leaving port open
  switch(tag) {
  case PP_CONSTR:
    // future work. no idea how to multiplex the inport...
    barf("PP_Constr received");
    break;
  case PP_HEAD:
    { StgClosure *temp, *list;
      temp            = createBH(cap); // new tail node
      inport->closure = temp;
      // graph becomes head, BH becomes tail:
      list = createListNode(cap, graph, temp); 
      IF_PAR_DEBUG(pack,
		   debugBelch("HEAD message: created list node %p/new BH %p\n",
			      list, temp));
      graph = list;
      break;
    }
  case PP_DATA:
    IF_PAR_DEBUG(pack,
		 debugBelch("DATA message, removing inport %d\n",
			    (int) gumPackBuffer->receiver.id));
    removeInportByP(gumPackBuffer->receiver);
    break;

  default:
    barf("processDataMsg: unexpected tag %#0x \n", tag);
  }

    // edentrace: write event iff message is accepted
  traceReceiveMessageEvent(cap,tag,gumPackBuffer);
  // and update the old blackhole
  IF_PAR_DEBUG(pack,
	       debugBelch("Replacing Blackhole @ %p by node %p\n",
			  placeholder, graph));

  // use system tso as owner when waking up blocked threads
  updateThunk(cap, (StgTSO*) &stg_system_tso, placeholder, graph);

}

/* fake a Heap Data Message (Data, Head). Called when sender was on the same
 *   machine (and same process? see sendWrapper), avoids pack/unpack
 *   duplication. Returns sendWrapper return codes (see sendWrapper).
 */
int
fakeDataMsg(StgClosure *graph, 
	    Port sender, Port receiver, 
	    Capability * cap, OpCode tag) {
  Inport* inport;
  StgClosure *placeholder;

  IF_PAR_DEBUG(pack,
	       debugBelch("shortcut for data message (%s, tag %#0x), data %p\n",
			  getOpName(tag), tag, graph));

  // should only be called when sender and receiver share heap
  ASSERT(sender.machine == receiver.machine
#ifdef PEDANTIC
	 && sender.process == receiver.process
#endif
	 );

  inport = findInportByP(receiver);

  if (inport == NULL) {
    IF_PAR_DEBUG(ports,
		 errorBelch("fakeDataMsg: unknown inport: Port (%d,%d,%d)\n",
			    receiver.machine, 
			    receiver.process, 
			    receiver.id));
    // should not happen... but just give up
    return 2;
  }

  if (!(equalPorts(inport->sender,sender))) {
    IF_PAR_DEBUG(ports,
		 debugBelch("fakeDataMsg: Sender (%d,%d,%d)" 
			    " not connected yet\n",
			    sender.machine, 
			    sender.process, 
			    sender.id));
    if (tag != PP_DATA)
      connectInportByP(receiver, sender);
  }

  // select placeholder closure (assumed: blackhole!)
  placeholder = inport->closure;
  ASSERT(IsBlackhole(placeholder));

  // replace placeholder by received data, possibly leaving port open
  switch(tag) {
  case PP_HEAD:
    { StgClosure *temp, *list;
      temp            = createBH(cap); // new tail node
      inport->closure = temp;
      // graph becomes head, BH becomes tail:
      list = createListNode(cap, graph, temp); 
      IF_PAR_DEBUG(pack,
		   debugBelch("fakeDataMsg: HEAD message:" 
			      " created list node %p/new BH %p\n",
			      list, temp));
      graph = list;
      break;
    }
  case PP_DATA:
    IF_PAR_DEBUG(pack,
		 debugBelch("fakeDataMsg: DATA message, removing inport %d\n",
			    (int) receiver.id));
    removeInportByP(receiver);
    break;

  default:
    barf("fakeDataMsg: unexpected tag %#0x \n", tag);
  }
  // edentrace: emit event sendMessage(tag,dataBuffer)
  traceSendReceiveLocalMessageEvent(tag,sender.process,sender.id,receiver.process,receiver.id);
  
  // and update the old blackhole
  IF_PAR_DEBUG(pack,
	       debugBelch("fakeDataMsg: Replacing Blackhole @ %p by node %p\n",
			  placeholder, graph));

  // use system tso as owner when waking up blocked threads
  updateThunk(cap, (StgTSO*) &stg_system_tso, placeholder, graph);

  return 2;
}

#endif /* PARALLEL_HASKELL -- whole file */
