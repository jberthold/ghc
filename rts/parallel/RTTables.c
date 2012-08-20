/*
 * RTTables.c: Runtime tables for Parallel Haskell
 * Author: 
 *  Jost Berthold, 
 *  Philipps-University of Marburg
 *
 * implements RTTables.h, see there for more descriptions...
 *
 * Review 2009: use hash tables for tso->process and tso->receiver
 * (avoid changing TSO structure)
 *
 ***********************************************/


#if defined(PARALLEL_RTS) // whole file

#include "Rts.h"
#include "parallel/RTTables.h"

#include "RtsUtils.h"  // allocates system space
#include "parallel/PEOpCodes.h" // sends message when closing inports
#include "sm/GC.h"   // updates table after GC: isAlive

#include "Trace.h"

#if defined(DEBUG)
#include "sm/Sanity.h"
#include "Threads.h"
#endif

#include "Hash.h"

// the runtime table, globally in scope: 
// a linked list of processData
ProcessData* processtable;

// We maintain a hash table ThreadID->ProcessId
// as well as a hash table ThreadID->Receiver(Port). 
// TSO->par.process, TSO->par.receiver

HashTable *threadproctable, *threadrecvtable;

// ID factory; Both IDs are StgWords, unlikely to
// be too small. Any ID 0 is reserved for the system, so these vars
// hold the _existing_ max.IDs so far, respectively.
StgWord procmax = 0; // obsolete in non-debugging setup!
StgWord inportmax = 0;


// Constant System ports:
Port RtsPort = (Port) {0, 0, 0}; // machine field set by initRTT()
Port NoPort  = (Port) {0, 0, 0}; // constant for error returns

#define localPort(p, id) ((Port) {thisPE, (p), (id)})

/* Forward Declarations (if not included in header...) */
void CommCheckFailed(void);
void killProcess_(ProcessData* p);
Inport* addInport_(ProcessData *p, StgClosure *blackhole);
rtsBool updateTSOList(ProcessData *p);
void updateInports(ProcessData *p);

// action if 1 to 1 check fails:
void CommCheckFailed(void) {
#if defined(DEBUG)
  errorBelch("1 to 1 connection check failed: inspect channel usage in program");
  trace(RtsFlags.TraceFlags.user, "COMM CHECK FAILED\n");
/*   errorBelch("1 to 1 check failed: check program (RTS bug?)"); */
/*   barf("Aborting program, suspected RTS bug"); */
/*   stg_exit(EXIT_FAILURE); // in case barf returns */
#endif
}

// initialise Runtime Table: set processtable=NULL and machine ID
// Declared in Parallel.h
void initRTT(void) { 
  processtable=NULL; 
  RtsPort.machine = thisPE;
  //  dummyPort.machine = thisPE;
  threadproctable = allocHashTable();
  threadrecvtable = allocHashTable();
}

// free space allocated by runtime table: we expect it to be empty!
// Declared in Parallel.h
void freePort(void* port);
void freePort(void* port) { 
  stgFree(port); 
}
void freeRTT(void) {
  // NOPE: ASSERT(processtable == NULL);

  // there can still be processes around, childprocesses on the main
  // PE might still have threads, and thus entries on shutdown
  if (processtable!= NULL) {
    // kill all entries. The program is going to terminate.
    ProcessData* last = processtable;
    Inport *inport;
    while (last != NULL) {
      processtable = processtable->next;
      // no killProcess_, which is during normal running conditions
      // but we free any allocated inports
      while ( last->inports != NULL ) {
	inport = last->inports;
	last->inports = inport->next;
	stgFree(inport);
      }
      // and free the process table entry
      stgFree(last);
      last = processtable;
    }
  }
  freeHashTable(threadproctable, NULL); // do not free entries
  freeHashTable(threadrecvtable, freePort); // free allocated ports
}

// Port comparison, is trivial...
rtsBool equalPorts(Port p, Port q) {
  if ( p.machine == q.machine 
       && p.process == q.process 
       && p.id      == q.id)
    return rtsTrue;
  return rtsFalse; 
}

// processTable actions are implemented OO-style,
// all methods have a first argument processId
// newProcess is the constructor.

/* Process creation: on creating the first TSO.  Allocate
 *   ProcessData*, spend fresh ID, fill in TSO, set inports=NULL
 */
void newProcess(StgTSO* firstTSO) {
  ProcessData* newProc;
  
  ASSERT(firstTSO); // we have a 1st member thread
  IF_PAR_DEBUG(procs,
	       debugBelch("New Process\n"));

  newProc = (ProcessData*) stgMallocBytes(sizeof(ProcessData), 
					  "New Process");
  newProc->id = ++procmax;

  IF_PAR_DEBUG(procs,
	       debugBelch("process %d(max.%d), first thread is %d\n",
			  (int) newProc->id, (int) procmax, 
			  (int) firstTSO->id));

  newProc->inports = NULL;
  newProc->tsos = 1;

  insertHashTable(threadproctable, firstTSO->id, (void*) newProc->id);

  newProc->next = processtable;
  processtable = newProc;

  // edentrace: new process

  traceCreateProcess(newProc->id);
  //edentrace: assign thread to (new) process 
  traceAssignThreadToProcessEvent(firstTSO->cap, firstTSO->id, newProc->id);
}

// used virtually for every action messages (from other machines) only
// contain ids, not memory addresses.  

// find a process by its id:
STATIC_INLINE
ProcessData* findProcess(StgWord processId) {
  ProcessData* p = processtable;

  if ( processId > procmax ) {
    // impossible!
    debugBelch("findProcess: Process %d (> max=%d) requested.",
	       (int) processId, (int) procmax );
    return NULL; // caller supposed to handle this accurately (fail or ignore op.)
  }

  while(p != NULL) {
    if (p->id == processId) break;
    p = p->next;
  }
  if (p == NULL) 
    debugBelch("findProcess: non-existent process %d\n", 
	       (int)processId);
  return p; 

}

/* Process termination:
 *  should happen when tsos == END_TSO_QUEUE, or on external request
 *  needs to close all inports (-> could send messages!) 
 *    and to kill all TSOs (-> raiseException) 
 *    and to remove entry from linked list 
 *    and deallocate it.
 */

void killProcess_(ProcessData *p) {
  Inport *inport, *inports;
  ProcessData* temp;
  rtsPackBuffer termMsgBuffer;

  // p is our process
  ASSERT(p!= NULL);

  // only used in here, only when no TSOs left in process
  ASSERT(p->tsos == 0);
  IF_PAR_DEBUG(procs,
	       debugBelch("killing Process %d at %p.\n",
			  (int) p->id, p));
  temp = processtable;
  ASSERT(NULL != temp);
  if (p == temp) {
    processtable = temp->next;
  } else {
    while (p != temp->next )
      temp = temp->next;
    temp->next = p->next;
  }

  // edentrace kill process
  traceKillProcess(p->id);

  // process might have no inports! Otherwise close them
  inports = p->inports;

  if ( inports != NULL ) {
    // initialise shared fields of the message
    termMsgBuffer.size  =0;
    termMsgBuffer.id    =0;
    termMsgBuffer.sender=localPort(p->id, 0); // inport ID set later
  }
  while ( inports != NULL ) {
    inport = inports;
    IF_PAR_DEBUG(ports,
	 debugBelch("closing inport %d, sender (%d,%d,%d)\n",
		    (int) inport->id,inport->sender.machine,
		    (int) inport->sender.process,
		    (int) inport->sender.id));

    if (!isNoPort(inport->sender)) {
      IF_PAR_DEBUG(ports,
		   debugBelch("sending %s to sender\n", 
			      getOpName(PP_TERMINATE)));
      termMsgBuffer.sender.id= inport->id;
      termMsgBuffer.receiver = inport->sender;
      sendMsg(PP_TERMINATE, &termMsgBuffer);
      // TODO handle send failures. Since we cannot just retry, module should
      // buffer messages and send all at once, in a separate procedure.
    }
    inports = inports->next;
    stgFree(inport);
  }

  p->id = 0;
  stgFree(p);
}


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

// create and add new inport (placeholder already existing)
Inport* addInport_(ProcessData *p, StgClosure *blackhole) {
  Inport* newIn;

  ASSERT(p != NULL);
  ASSERT(IsBlackhole(blackhole));

  newIn = (Inport*) stgMallocBytes(sizeof(Inport),
				   "new inport");
  newIn->id = ++inportmax;
  IF_PAR_DEBUG(ports,
      debugBelch("inport %d(%d) for process %d, closure %p\n",
		 (int) newIn->id, (int) inportmax, 
		 (int) p->id, blackhole));
  trace(RtsFlags.TraceFlags.user, "newInport (%d,%d), blackhole %p\n",
        (int) p->id, (int) newIn->id, blackhole);

  newIn->closure = blackhole;
  newIn->sender = NoPort;

  newIn->next = p->inports;
  p->inports = newIn;

  return newIn;
}

// version to be used in primitive operation (returns ID only)
StgWord 
addInport(StgWord processId, StgClosure* blackhole ) {
  Inport* newIn;
  ProcessData* p = findProcess(processId);

  if (p == NULL) {
    barf("addInport: no process found!");
  }
  IF_PAR_DEBUG(ports,
      debugBelch("new inport for process %d:\n",
		    (int) processId));
  newIn = addInport_(p, blackhole);
  return newIn->id;
}



// locate an inport in a process: called when receiving data
STATIC_INLINE
Inport* findInport(StgWord processId, StgWord id){
  // FIND_IN_LIST(FIND_IN_LIST(processId,processtable)->inports,id);
  ProcessData* p = findProcess(processId);

  if (p != NULL) { 
    Inport* i = p->inports;
    while(i != NULL) {
      if (i->id == id) { return i; }
      i = i->next;
    }
  }
  return NULL;
}

// connnect an inport to a sender. Reconnecting not allowed!
void connectInport(StgWord processId, StgWord id, Port sender) {
  Inport* i;

  IF_PAR_DEBUG(ports,
      debugBelch("connect inport %d for process %d to (%d,%d,%d)\n",
		 (int) id, (int) processId, sender.machine,
		 (int) sender.process, (int) sender.id));

  i = findInport(processId, id);

  if (i == NULL) {
    IF_PAR_DEBUG(ports,
	 debugBelch("connection request for nonexistent port (.,%d,%d), "
		    "ignoring it.\n", (int) processId, (int) id));
    return;
  }
  ASSERT(i);
  
  // inports should be assigned a sender only once!
  if (!(isNoPort(i->sender))) { // 1:1 check failed!
    CommCheckFailed();
  }
  i->sender = sender;
}

// set a receiver for a TSO. Called by primop "connectToPort#"
// directly.  Reconnecting allowed (but problems might arise when
// reconnection intended: thread maybe terminated on previous inport).
void setReceiver(StgTSO* tso, nat pe, StgWord proc, StgWord id) {
  Port* portInHashTable, *oldPort;
  
  ASSERT(get_itbl(tso)->type == TSO);

  IF_PAR_DEBUG(ports,
      debugBelch("connect TSO %d to inport (%d,%d,%d)\n",
		 (int) tso->id, 
		 (int) pe, (int) proc, (int) id ));

  ASSERT(pe != 0 && proc != 0 && id != 0);

  trace(RtsFlags.TraceFlags.user,"connectTSO %d to inport (%d,%d,%d)\n",
        (int) tso->id, (int) pe, (int) proc, (int) id );

  // reconnection allowed, remove old entry.
  oldPort = lookupHashTable(threadrecvtable, tso->id);
  if (oldPort != NULL) {
    stgFree(oldPort);
    // currently not needed, insert will overwrite
    removeHashTable(threadrecvtable, tso->id, oldPort);
  }

  // we have to allocate a port which we put in the hash table
  portInHashTable = (Port*) stgMallocBytes(sizeof(Port), "setReceiver");
  portInHashTable->machine = pe;
  portInHashTable->process = proc;
  portInHashTable->id      = id;
  
  insertHashTable(threadrecvtable, tso->id, portInHashTable);

  return;
}


// remove an inport from a process (and deallocate it)
// does NOT send a PP_TERMINATE message
void removeInport(StgWord processId, StgWord inportId) {
  ProcessData *p;

  Inport **last, *remv;

  IF_PAR_DEBUG(ports,
      debugBelch("remove inport %d from process %d\n",
		 (int) inportId, (int) processId));

  p = findProcess(processId);
  if (p == NULL) {
    IF_PAR_DEBUG(ports,
	 debugBelch("removeInport: non-existent process, ignoring."));
    return; // shrug...
  }

  ASSERT(p != NULL);

  last = &(p->inports);
  remv = p->inports;
  while (remv != NULL && remv->id != inportId) {
      ASSERT(*last == remv); // INV: last points to remv (from
			     // previous one or process entry)
      last = &(remv->next);
      remv = remv->next;
  }
  
  if (remv == NULL) {// not found!
    ASSERT(last != NULL && *last == NULL);
    IF_PAR_DEBUG(ports,
	 debugBelch("Inport %d: not found in process %d.\n",
				(nat) inportId, (nat) processId));
  } else {
    // found, remove and free it.
    // last points to the field where remv was referenced in the list
    ASSERT(remv);
    *last = remv->next; 

    stgFree(remv);
    IF_PAR_DEBUG(ports,
	 debugBelch("inport %d removed (process %d)\n",
		    (int) inportId, (int) p->id));
  }
}

// add a thread to a process
void addTSO(StgWord processId, StgTSO* tso) {
  ProcessData *p;

  if (processId == 0) {
    IF_PAR_DEBUG(procs,
		 debugBelch("addTSO for process 0, skipping.\n"));
    return;
  }

  ASSERT(processId != 0);
  p = findProcess(processId);;

  IF_PAR_DEBUG(procs,
	       debugBelch("add thread %d to process %d\n",
			  (int) tso->id, (int) processId));

  ASSERT(tso != NULL);
  ASSERT(p != NULL);

  insertHashTable(threadproctable, tso->id, (void*) processId);
  p->tsos += 1;

  // newThreadEvent emitted only here (thread is created before), we
  // do not track tsos internal to the RTS (finalizers etc.)

  // edentrace: new thread (now in schedule.c)
  // edentrace: assign thread to (existing) process
  traceAssignThreadToProcessEvent(tso->cap, tso->id, processId);
  return;

}

// find a TSO in a process: needed for external termination..
// Avoiding to touch the TSO struct, we need to traverse a list of all
// threads per GC "step", for all steps. See Threads.c::printAllThreads.

StgTSO* findTSO(StgWord processId, StgWord id) {
  // Capability* cap;
  // nat i;
  nat g;
  StgTSO *t;
  StgWord proc;
  ProcessData *p = findProcess(processId);

  if (p == NULL ) {
    IF_PAR_DEBUG(procs,
		 debugBelch("findTSO %d: Unknown process %d.\n", 
			    id, processId));
    return NULL;
  }

  IF_PAR_DEBUG(procs, 
      debugBelch("Searching thread %d in process %d (%d threads).\n", 
		 id, processId, p->tsos);
      printAllThreads());

  proc = (StgWord) lookupHashTable(threadproctable, id);
  if (processId != proc) {
    IF_PAR_DEBUG(procs,
		 debugBelch("findTSO: wrong process ID %d. "
			    "Thread %d claims it belongs to process %d.\n",
			    processId, id, proc));
    return NULL;
  }

  // see Threads.c::printAllThreads
  /*
  // if thread is not blocked, it is listed in one capability
  for (i=0; i < n_capabilities; i++) {
    cap = &capabilities[i];
    t = cap->run_queue_hd;
    while (t != END_TSO_QUEUE) {
      if (t->id == id) 
	return t;
      else 
	t = t->_link;
    }
  }
  */
  // otherwise, thread could be blocked. Traverse all generations.
  // NB: this looks at non-blocked threads again, so only do the 2nd loop
  for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
    t = generations[g].threads;
    while(t != END_TSO_QUEUE) {
      if (t->id == id) {
	  return t;
      } else {
	t = t->global_link;
      }
    }
  }

  // If we reach here, something is very wrong(tm): the thread does not exist,
  // but we found it earlier in the threadproctable (otherwise would be
  // proc=0, early exit. Complain (in debug mode)!
  ASSERT(rtsFalse);
  return NULL;
}

// remove a thread from a process (does NOT terminate it!)
void removeTSO(StgWord id) {
  ProcessData *p;
  StgWord proc;

  IF_PAR_DEBUG(procs,
	       debugBelch("remove thread %d from its process\n",
			  (int) id));
  proc = (StgWord) lookupHashTable(threadproctable, id);

  if (proc == (StgWord) NULL) {
    IF_PAR_DEBUG(procs,
		 debugBelch("removeTSO: no process registered\n"));
    return ; // virtually "removed", shrug (might be system thread, e.g. finalizer
  }
  p = findProcess(proc);
  if (p == NULL) {
    IF_PAR_DEBUG(procs,
		 debugBelch("removeTSO: nonexistent process\n"));
    return ; // virtually "removed", shrug (might be system thread, e.g. finalizer
  } else {
    Port* registeredPort = NULL;
    
    ASSERT(p != NULL);
    ASSERT(p->tsos != 0); // we have a process with >= 1 thread
    
    removeHashTable(threadproctable, id, (void*) proc);
    
    // remove (+ deallocate) potential entry in threadrecvtable
    registeredPort = lookupHashTable(threadrecvtable, id);
    if (registeredPort != NULL) {
      removeHashTable(threadrecvtable, id, registeredPort);
      stgFree(registeredPort);
    }
    
    // edentrace: emit killthread event (now in schedule.c)

    p->tsos -= 1;
    
    // if last tso: kill the whole process!
    if (p->tsos == 0) {
      killProcess_(p);
    }
  }
}

/* mark all threads reachable from the process table, unless they are
 * killed or completed.
 *
 * These threads are additional roots, since they produce output for
 * other processes. Not found if they are blocked on blackholes with
 * inport! OTOH, if we evacuated all blackholes with inport, we do not
 * find out whether an input is actually needed.
 * => evacuate all "live" threads  
 *
 * Our logic of evacuation is reverse to that of the sequential system:
 *   Sequential:
 *   evacuate heads of runnable + ccalling + sleeping + "blocked" queue
 *            => scavenge them (evacuate stack + link field = next in q)
 *            => evacuate some blackholes
 *            => evacuate TSOs blocked on *these* blackholes
 *                       (found on the blackhole_queue)
 *            => scavenge... (see above)
 *      non-evacuated threads (killed/complete/blocked-on-dead-BH) are
 *      evacuated later, to be "resurrected", so the scheduler cleanly
 *      removes them (!!)
 *
 * Parallel:
 *           evacuate *all* tsos *except* Thread[Complete|Killed]
 *             + if BlockedOnBlackHole: evacuate block_info (the blackhole)
 *            => scavenge these (stack + (unnecessarily:) link)
 *            => ...(see above)
 * 
 * The evacuation described is now done inside MarkWeak.c, where the
 * blackhole queue is traversed. Code here removed.
 */



// helper: traverse inport list, update closure field or send
// PP_TERMINATE to sender. Called after Garbage Collection

void updateInports(ProcessData *p) {
  Inport *inp, *temp;
  Inport **last;

  rtsPackBuffer termMsgBuffer;

  ASSERT( p != NULL);

  inp = p->inports;

  // could be the process does not have any inports...
  if (inp == NULL) 
    return;

  // initialise
  termMsgBuffer.size=0;
  termMsgBuffer.id=0;
  termMsgBuffer.sender.machine=thisPE;
  termMsgBuffer.sender.process=p->id;

  last = &(p->inports);
  
  while(inp != NULL) {
    ASSERT(*last == inp);

    temp = inp;       // only work on temp, inp set to next
    inp = inp->next;

    IF_PAR_DEBUG(ports,
		 debugBelch("updating inport %d\n", (nat)temp->id));

    temp->closure = isAlive(temp->closure); // updated to new BH
    if (temp->closure == NULL) { // a garbage inport, remove it

      IF_PAR_DEBUG(ports,
		   debugBelch("\t ...inport is garbage, removing it\n"));
 
      if (!isNoPort(temp->sender)) { // known sender?
	IF_PAR_DEBUG(ports,
		     debugBelch("sending terminate to sender (%d,%d,%d)\n",
				temp->sender.machine, 
				temp->sender.process,
				temp->sender.id));
	termMsgBuffer.sender.id = temp->id;
	termMsgBuffer.receiver  = temp->sender;
	sendMsg(PP_TERMINATE, &termMsgBuffer);
	// TODO handle send failure (buffer messages)
      }
      *last = inp; // skip temp reference (inp already set to next)
      stgFree(temp); // and remove the inport
    } else {
      // inport is alive, BH field already updated, nothing more to do
      last = &(temp->next);
    }
  }
}

void updateRTT(void) {
  ProcessData *p;

  IF_PAR_DEBUG(procs,
	       debugBelch("updateRTTable: processtable %p\n",
			  processtable));
  if (processtable != NULL) {
    p = processtable; 
    while (p != NULL) {
      IF_PAR_DEBUG(procs,
		   debugBelch("updating process %d (table @ %p)\n",
			      (int) p->id, p));
//      if (!updateTSOList(p)) {
	  if (p->tsos == 0) {
		StgWord killID;
		killID = p->id;
		killProcess_(p); // invalidates p, updates processtable! 
		// advance to ID after killID in processtable
		p = processtable;
		while (p != NULL && p->id < killID) p = p->next;
      } else {
		updateInports(p);
		p = p->next;
      }
    }
    IF_PAR_DEBUG(procs,
				 debugBelch("UpdateRTTable done\n"));
  } else {
    IF_PAR_DEBUG(procs,
				 debugBelch("No Processes, table is NULL!\n"));
  }
}

// EXTERNAL INTERFACE:
// using the Port type instead:
Inport* findInportByP(Port p) { 
  return findInport(p.process,p.id);
}
void connectInportByP(Port p, Port sender) {
  // called from HLComms and Schedule.c
  connectInport(p.process, p.id, sender);
}
void removeInportByP(Port p) { 
  removeInport(p.process, p.id); 
}
// Port for outport actions contains tso->id as id
StgTSO* findTSOByP(Port p) { 
  return findTSO(p.process, p.id); 
}

StgWord MyProcess(StgTSO* tso) {
  return (StgWord) lookupHashTable(threadproctable, tso->id);
}

Port* MyReceiver(StgTSO* tso) {
  Port* receiver;
  receiver = (Port*) lookupHashTable(threadrecvtable, tso->id);
  if (receiver == NULL) {
    return &NoPort; // or just null?
  } else {
    return receiver;
  }
}

#endif // PARALLEL_HASKELL, whole file

 
