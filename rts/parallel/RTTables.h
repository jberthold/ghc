/*
 * RTTables.h: Runtime tables for Parallel Haskell
 * Author: 
 *  Jost Berthold, (2006)
 *  Philipps-University of Marburg
 * Review 08/2009
 *
 * In parallel Haskells, the RTS needs to store information about
 * connections to other nodes.  Specifically, these are encoded here
 * as "inports" and "outports" of "processes".
 *
 * Relations are as follows:
 * 
 * A process consists of one or more TSOs and open
 * inports (referring to BLACKHOLE placeholders in the heap).
 *
 * Processes can send data to another process, when one of its TSOs
 * sends it through an "outport" to an "inport" (Eden-Speech: via a
 * "channel" modeled by this construction). Outports can connect
 * explicitly to their receiver before sending a first message, and
 * inports (for termination, see below) know their sender only after a
 * first message arrives (which might be a connect message). 
 *
 * A TSO can fork a new TSO, in this case it is added to the process.
 * A TSO which terminates leaves the process. A process should
 * terminate instantly when it contains no TSOs any more (every
 * process has a counter for its TSOs). On process shutdown, all
 * inports are closed.
 * 
 * When a process closes an inport, it sends a termination message to
 * (remote) threads which have sent data to this inport (usually 1:1
 * connection). The sender is stored in the inport structure.
 *
 * Processes and inports are stored as linked lists, and found by IDs,
 * which are StgWords and stored inside the linked structure.(2)
 * 

 * We use the TSOs ID as an outport ID. The receiver (inport) is
 * stored in the TSO. We use a linked list of ProcessData entries,
 * where one entry is essentially linked lists of inports and tsos.
 * 
 * The previous (Eden<=6.8.3) version used an extension to the TSO
 * structure by additional fields for the mappings tso->process and
 * tso->receiver. The inverse mappings are not needed.  In order to
 * keep the TSO structure untouched, this version of the module uses
 * two hash tables and interface functions for this purpose.
 *
 ***********************************************/

#if !defined(RTTABLES_H)
#define RTTABLES_H

#if defined(PARALLEL_RTS) // whole file

#if !defined(RTS_H) // should be after Rts.h  
#error "RTTables.h: include Rts.h before"
#endif

// a port type, stands for an inport (pe, proc,inport->id), an outport
// (pe,proc,tso->id) and processes (pe, proc, 0).
// 
// And the globally visible system ports: RTS port and NoPort, living
// in RTTables.c
 
/* About Ports:
 *
 *  Ports are a triple of (machine-ID, process-ID, ID), where 2nd and
 *  3rd ID can have multiple meaning. 
 *
 *  Zero in every place is reserved, and specially treated by the
 *  system! The port {0,0,0} is thus invalid in any case (NoPort).
 *
 *  The first component gives the machine number (1..nPe) != 0.
 *
 *  If the 2nd ID (process-ID) is zero, we address a machine (an
 *  RTSPort). Otherwise it is the process ID, (1.. maxVal(StgWord)).
 *  In the optimised version(2), processID can be any mem.address != 0
 *
 *  When the 3rd ID is zero, we address a process (not a port). Valid
 *  port IDs can be the ID of an inport (1..maxVal(StgWord)), or of
 *  a thread (also StgWord, using StgTSO->id) for an outport.

DEFINED IN RTSTYPES_H
typedef struct Port_ {
  nat machine;
  StgWord process;
  StgWord id;
} Port;
typedef Port Proc;
 *
 */
extern Port RtsPort, NoPort;

// NoPort = Port {0,0,0}
#define isNoPort(p)  (!((p).machine | (p).process | (p).id))
// RtsPort = Port { thisPe, 0, 0}
#define isRtsPort(p) ( (p).machine && !((p).process | (p).id))
// Port comparison
rtsBool equalPorts(Port p, Port q);

typedef struct Inport_ {
  struct Inport_ *next;
  StgWord id;
  StgClosure *closure; // update after GC!
  Port sender;         // can be mergeport!
  StgPtr pendingUnpack;// stores Pack.c::UnpackInfo
} Inport;

typedef struct ProcessData_ {
  struct ProcessData_ *next;
  StgWord id;
  Inport* inports;
  nat     tsos; // counter only
} ProcessData;

// the runtime table, globally in scope (though not needed by now...): 
// a linked list of processData
// extern ProcessData* processtable;

// initialise: set processtable=NULL, declared in Parallel.h
// void initRTT(void);

// processTable actions are implemented OO-style,
// all methods have a first argument id
// newProcess is the constructor.

// Process creation: on creating the first TSO.  Allocate
//   ProcessData*, spend fresh ID, fill in TSO, set inports=NULL
// declared in Parallel.h
// void newProcess(StgTSO* firstTSO);

/* Process termination:
 *  should happen when tsos == 0, or on external request
 *  needs to close all inports (-> send messages!) 
 *    and to kill all TSOs (unless 0) 
 *  _then_ remove entry from linked list and deallocate it.
 */

/* modify a process:
 *   Event                       modification
 *  ---------------------------------------------------------
 *  make new inport     add fresh inport to process of caller
 *                       (uses: process field in calling TSO)
 *  receive data      update inport (no process modification)
 *  close inport               remove inport from linked list
 *  fork a thread        add created TSO to process of caller
 *  terminate a thread      remove TSO from process of caller 
 *                             (check no. of TSOs afterwards)
 */

// find a process by its id: 
// used internally for every action in basic version: messages (from
// other machines) contain ids, not memory addresses.
// in optimised version (2): debug check only
// not called from outside
// STATIC_INLINE ProcessData* findProcess(StgWord processId);

// locate an inport in a process: not called from outside 
// STATIC_INLINE Inport* findInport(StgWord processId, StgWord id);

// create and add new inport (placeholder already existing)
// called by primop "expectData#", returns assigned inport ID 
StgWord addInport(StgWord processId, StgClosure* blackhole);

// connnect an inport to a sender. Reconnecting not allowed!
// not called from outside
void connectInport(StgWord processId, StgWord id, Port sender);

// set a receiver for a TSO. Called by primop "connectToPort#".
void setReceiver(StgTSO* tso, nat pe, StgWord proc, StgWord id);

// close/remove an inport from a process 
// (gets deallocated, TERM message to send by caller before!)
// not called from outside
void removeInport(StgWord processId, StgWord inportId);

// add a thread to a process
// called by primop "fork#"
void addTSO(StgWord processId, StgTSO* tso);

// find a TSO in a process:
// not called from outside
StgTSO* findTSO(StgWord processId, StgWord tsoId);

// remove a thread from a process
// called from Schedule.c on thread termination
void removeTSO(StgWord tsoId);

// update whole table after a garbage collection
// called from GC::GarbageCollect() after GC
//  (better call it from inside GC.c, even though Schedule.c is the
//  only caller. Must be done on *every* GC)
void updateRTT(void);

// EXTERNAL INTERFACE:
// hiding internals by using the Port type, simply mapped...:

// Pack.c, HLComms.c:
Inport* findInportByP(Port p);
// Schedule.c, HLComms.c:
void connectInportByP(Port p, Port sender);
// HLComms.c:
void removeInportByP(Port p);
// Schedule.c:
StgTSO* findTSOByP(Port p);

// new functions for accessing the hash tables:
// tso -> processID/ receiverPort
StgWord MyProcess(StgTSO* tso);
Port* MyReceiver(StgTSO* tso);

#endif // PARALLEL_HASKELL, whole file

#endif // RTTABLES_H

