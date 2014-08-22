/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2009
 *
 * Parallelism-related functionality
 *
 * Do not #include this file directly: #include "Rts.h" instead.
 *
 * To understand the structure of the RTS headers, see the wiki:
 *   http://ghc.haskell.org/trac/ghc/wiki/Commentary/SourceTree/Includes
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

  Some parts of this file are always active to support serialisation
  in the sequential and threaded RTS.

*/

/* even when not parallel, these should be present (and 1) when
   implementing noPe and selfPe as foreign imports.
   Reside in MPSystem files, or in ParInit.c when not parallel.
*/
extern nat nPEs, thisPE;


// packing and sending:
// Pack Buffer for constructing messages between PEs
// defined here instead of in RtsTypes.h due to FLEXIBLE_ARRAY usage
typedef struct rtsPackBuffer_ {
    // Eden channel communication
    Port                 sender;
    Port                 receiver;
    // for data messages only,
    StgInt /* nat */     id;            // currently unused
    StgInt /* nat */     size;          // payload size in units of StgWord
    StgInt /* nat */     unpacked_size; // currently unused
    StgWord              buffer[FLEXIBLE_ARRAY]; // payload
} rtsPackBuffer;

// minimum sizes for message buffers:

/* (arbitrary) amount of additional StgWords to remain free */
#define DEBUG_HEADROOM  2

// =>  minimum data space for a MessageBuffer (in words!) is max. msg.size:
#define DATASPACEWORDS (((int)RtsFlags.ParFlags.packBufferSize/sizeof(StgWord))\
                        + (sizeof(rtsPackBuffer)/sizeof(StgWord))       \
                        + DEBUG_HEADROOM)

// following functions internal if PACKING, used externally when PARALLEL_RTS

// Check, defined in Pack.c as well.
// Is there still a macro for it somewhere else?
rtsBool isBlackhole(StgClosure* closure);

// interfaces for (un-)packing, defined in Pack.c.

// packs to buffer, returns size-in-bytes + P_ERRCODEMAX, or an error code
int packToBuffer(StgClosure* closure,
                 StgWord *buffer, nat bufsize, StgTSO *caller);

// packing can fail for different reasons, encoded in small ints which are
// returned by packToBuffer:
// constant definition in includes/Constants.h:
// #define P_SUCCESS       0x00 /* used for return value of PackToMemory only */
// #define P_BLACKHOLE     0x01 /* possibly also blocking the packing thread */
// #define P_NOBUFFER      0x02 /* buffer too small */
// #define P_CANNOTPACK    0x03 /* type cannot be packed (MVar, TVar) */
// #define P_UNSUPPORTED   0x04 /* type not supported (but could/should be) */
// #define P_IMPOSSIBLE    0x05 /* impossible type (stack frame,msg, etc) */
// #define P_GARBLED       0x06 /* invalid data for deserialisation */
// #define P_ERRCODEMAX    0x06

// If packing succeeds, size + P_ERRCODEMAX is returned

// predicate for checks:
#define isPackError(val) (((StgWord) (val)) <= P_ERRCODEMAX)

// serialisation into a Haskell Byte array, returning error codes on failure
StgClosure* tryPackToMemory(StgClosure* graphroot, StgTSO* tso,
                            Capability* cap);

// unpack a graph from packBuffer (wiping the buffer), aborts if unsuccessful
StgClosure* unpackGraph(rtsPackBuffer *packBuffer, Capability* cap);

// respective deserialisation (global pack buffer used for unpacking)
StgClosure* unpackGraphWrapper(StgArrWords *packBufferArray, Capability *cap);

// creating a blackhole from scratch. Defined in Pack.c (where it is
// used), but mainly used by the primitive for channel creation.
StgClosure* createBH(Capability *cap);
// and creating a list node (CONS).
// used in DataComms.c, defined in Pack.c
StgClosure* createListNode(Capability *cap,
                           StgClosure *head, StgClosure *tail);

#if defined(PARALLEL_RTS)

// parallel machine setup, startup / shutdown
// in MPSystem file (PVMComm | MPIComm | CpComm currently)
extern rtsBool IAmMainThread;

void          startupParallelSystem(int* argc, char** argv[]);
void          synchroniseSystem(void);
void          shutdownParallelSystem(StgInt errorcode);

// defined in ParInit.c, called in RtsStartup.c
void          emitStartupEvents(void);
// defined in ParInit.c, called in RtsStartup.c (after shutdown when tracing)
void          zipTraceFiles(void);

// packbuffer resides in DataComms.c
void initPackBuffer(void);
void freePackBuffer(void);

// RecvBuffer resides in Schedule.c:
// init on demand
void freeRecvBuffer(void);

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
