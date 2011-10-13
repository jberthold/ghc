/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2009
 *
 * Parallelism-related functionality
 *
 * Do not #include this file directly: #include "Rts.h" instead.
 *
 * To understand the structure of the RTS headers, see the wiki:
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/SourceTree/Includes
 *
 * -------------------------------------------------------------------------- */

#ifndef RTS_PARALLEL_H
#define RTS_PARALLEL_H

StgInt newSpark (StgRegTable *reg, StgClosure *p);

/*
  Definitions for running on a parallel machine.

  This section contains definitions applicable only to programs
  compiled to run on a parallel runtime system (with distributed
  memory). Some of these definitions also need to be present in the
  sequential system to work consistently.

  We only put globally visible things here.
*/

/* even when not parallel, these should be present (and 1) when
   implementing noPe and selfPe as foreign imports. 
   Reside in MPSystem files, or in ParInit.c when not parallel.
*/
extern nat nPEs, thisPE;

#if defined(PARALLEL_RTS) 

// parallel machine setup, startup / shutdown
// in MPSystem file (PVMComm | MPIComm | CpComm currently)
extern rtsBool IAmMainThread;

void          startupParallelSystem(int* argc, char** argv[]);
void          synchroniseSystem(void);
void          shutdownParallelSystem(StgInt errorcode);

// defined in ParInit.c, called in RtsStartup.c
void          emitStartupEvents(void);

 
// packing and sending:
// Pack Buffer for constructing messages between PEs
// defined here instead of in RtsTypes.h due to FLEXIBLE_ARRAY usage
typedef struct rtsPackBuffer_ {
  // Eden channel communication
  Port                 sender;
  Port                 receiver;
  // for data messages only,  
  StgInt /* nat */     id; 
  StgInt /* nat */     size;
  StgInt /* nat */     unpacked_size;
  struct StgTSO_       *tso;
  StgWord              buffer[FLEXIBLE_ARRAY];
} rtsPackBuffer;

// defined in Pack.c
// void InitPackBuffer(void); not needed, done on demand
void freePackBuffer(void);

// resides in Schedule.c:
// init on demand
void freeRecvBuffer(void);

// interfaces for (un-)packing, defined in Pack.c
rtsPackBuffer* PackNearbyGraph(StgClosure* closure, StgTSO* tso,
                               OpCode *msgtag);

StgClosure*    UnpackGraph(rtsPackBuffer *packBuffer,
			   Port inPort,
			   Capability* cap);

// testing: pack and unpack locally
StgClosure* DuplicateNearbyGraph(StgClosure* graphroot, StgTSO* tso,
				 Capability* cap);

// serialisation into a Haskell Byte array
StgClosure* PackToMemory(StgClosure* graphroot, StgTSO* tso,
			 Capability* cap);
// respective deserialisation (using local pack buffer for unpacking)
StgClosure* UnpackGraphWrapper(StgArrWords* packBufferArray,
			       Capability* cap);

// minimum sizes for message buffers: 

/* (arbitrary) amount of additional StgWords to remain free */
#define DEBUG_HEADROOM  2

// =>  minimum data space for a MessageBuffer (in words!) is max. msg.size:
#define DATASPACEWORDS (((int)RtsFlags.ParFlags.packBufferSize/sizeof(StgWord))\
			+ (sizeof(rtsPackBuffer)/sizeof(StgWord)) \
			+ DEBUG_HEADROOM)

// creating a blackhole from scratch. Defined in Pack.c (where it is
// used), but mainly used by the primitive for channel creation.
StgClosure* createBH(Capability *cap); 
// and creating a list node (CONS).
// used in HLComms.c, defined in Pack.c
StgClosure* createListNode(Capability *cap, 
              StgClosure *head, StgClosure *tail);
// Check, defined in Pack.c as well. 
// Is there still a macro for it somewhere else?
rtsBool IsBlackhole(StgClosure* closure);

// runtime table initialisation and release
void initRTT(void);
void freeRTT(void);

// creation of a new process (+registering the first thread)
// used in Rts API, defined in RTTables.c
void newProcess(StgTSO* firstTSO);

// Message processing functions, defined inside DataComms.c

// Sending messages. sender and receiver included in the buffer
// Send operation may fail inside the communication subsystem.
rtsBool sendMsg(OpCode tag, rtsPackBuffer* dataBuffer);
// sendWrapper is called by primitive operations, does not need
// declaration here.

// Unpacking and updating placeholders (if valid data)
void processDataMsg(Capability* cap, OpCode opcode, 
		    rtsPackBuffer *recvBuffer);

// special structure used as the "owning thread" of system-generated
// blackholes.  Layout [ hdr | payload ], holds a TSO header.info and blocking
// queues in the payload field.
extern StgInd stg_system_tso;

#endif /* PARALLEL_RTS */

#endif /* RTS_PARALLEL_H */
