/* 
   Packing for the Generic RTE: 
--------
   
   Graph packing and unpacking code for sending it to another processor
   and retrieving the original graph structure from the packet.
   Used in GUM and Eden.

   (Outdated) Documentation for heap closures can be found at
   http://hackage.haskell.org/trac/ghc/wiki/Commentary/Rts/Storage/HeapObjects
   However, the best documentation is includes/Closure*h and rts/sm/Scav.c
*/


#if defined(PARALLEL_RTS) /* whole file */

#include "Rts.h"
#include "RtsUtils.h"
#include "Hash.h"
#include "Threads.h" // updateThunk
#include "Messages.h" // messageBlackHole
#include "Apply.h"   // to get stg_arg_bitmaps

# if defined(DEBUG)
# include "sm/Sanity.h"
# endif

#include "Printer.h" // printing closure info (also non-debug-enabled)

#include <string.h>

/* later:
#include "RTTables.h" // packet split operates on inports,
                      // needs types and methods
*/


// for better reading only... ATTENTION: given in bytes!
#define RTS_PACK_BUFFER_SIZE   RtsFlags.ParFlags.packBufferSize

// size of the (fixed) Closure header in words
#define HEADERSIZE sizeof(StgHeader)/sizeof(StgWord)

// some sizes for packed parts of closures
#define PACK_PLC_SIZE	2	/* Size of a packed PLC in words */
#define PACK_GA_SIZE	3	/* Size of a packed GA in words */
#define PACK_FETCHME_SIZE (PACK_GA_SIZE + HEADERSIZE)
			        /* Size of a packed fetch-me in words */

// markers for packed/unpacked type
#define PLC     0L
#define OFFSET  1L
#define CLOSURE 2L

#define END_OF_BUFFER_MARKER 0xedededed

// arbitrary maximum size (compile time constant)... only for reverting moved thunks.
#define MAX_THUNKS_PER_PACKET  256

// forward declarations:

/*
  Tagging macros will work for any word-sized type, not only
  closures. In the packet, we tag info pointers instead of
  closure pointers.
*/
#define UNTAG_CAST(type,p) ((type) UNTAG_CLOSURE((StgClosure*) (p)))

//   ADT of closure queues
STATIC_INLINE void    	  InitClosureQueue(void);
STATIC_INLINE void    	  StuffClosureQueue(void);
STATIC_INLINE rtsBool 	  QueueEmpty(void);
STATIC_INLINE nat         QueueSize(void);
STATIC_INLINE void    	  QueueClosure(StgClosure *closure);
STATIC_INLINE StgClosure     *DeQueueClosure(void);

//   Init for packing
static void     InitPackBuffer(void);
static void     InitPacking(rtsBool unpack);
static void     ClearPackBuffer(void);

// de-init:
static void DonePacking(void);

// little helpers: 
STATIC_INLINE void 	RegisterOffset(StgClosure *closure);
STATIC_INLINE nat  	OffsetFor(StgClosure *closure);
STATIC_INLINE rtsBool   AlreadyPacked(int offset);
STATIC_INLINE StgInfoTable* 
    get_closure_info(StgClosure* node, nat *size, nat *ptrs, 
		     nat *nonptrs, nat *vhs);

// declared in Parallel.h
// rtsBool IsBlackhole(StgClosure* closure);


/* used here and by the primitive which creates new channels:
   creating a blackhole closure from scratch.
   Declared in Parallel.h
StgClosure* createBH(Capability *cap); 

   used in HLComms: creating a list node
   Declared in Parallel.h
StgClosure* createListNode(Capability *cap, 
                           StgClosure *head, StgClosure *tail);
*/

/* TODO: for packet splitting, make RoomToPack a procedure, which
   always succeeds, by sending partial data away when no room is
   left. => Omit ptrs argument.
*/
STATIC_INLINE void RoomToPack (nat size);

// Packing and helpers

// external interface, declared in Parallel.h: 
// rtsPackBuffer* PackNearbyGraph(StgClosure* closure, StgTSO* tso,
//                                nat *msgtag);

// for the testing primitive:
StgClosure* DuplicateNearbyGraph(StgClosure* graphroot, StgTSO* tso,
				 Capability* cap)
{
  StgClosure* copy;
  rtsPackBuffer* buffer;
  Port noPort =  (Port) {0,0,0};

  buffer = PackNearbyGraph(graphroot, tso, 0);
  if (!buffer) {
    debugBelch("Duplication failed while packing, using original\n");
    return graphroot;
  }
  copy = UnpackGraph(buffer, noPort, cap);
  return copy;
}

// packing routine, branches into special cases
static void PackClosure (StgClosure *closure);
// packing static addresses and offsets
STATIC_INLINE void PackPLC(StgPtr addr);
STATIC_INLINE void PackOffset(int offset);
// the standard case: a heap-alloc'ed closure
static void PackGeneric(StgClosure *closure);

// special cases:
static void    PackPAP(StgPAP *pap);
static void PackArray(StgClosure* array);

// low-level packing: fill one StgWord of data into the globalPackBuffer
STATIC_INLINE void Pack(StgWord data);


// Unpacking routines:

// unpacking state (saved & restored)
/* this structure saves internal data from Pack.c.
 * Not used outside, only saved as an StgPtr in the inport structure
 */
typedef struct UnpackInfo_ {
  StgClosure *parent; // current parent
  nat pptr; // current child pointer
  nat pptrs;// no. of pointers
  nat  pvhs;// var. hdr. size (offset for filling in ptrs.)
  StgClosure* graphroot; // for GC (hard but true: always evacuate the  whole 
               //graph, since following message can contain offset references
  nat queue_length;
  StgClosure** queue; // closure queue, variable size
  nat offsetpadding;     // padding to adjust offset between several packets
  HashTable* offsets; // set of offsets, stored in a Hashtable
} UnpackInfo;

/* Future use: global unpack state to support fragmented subgraphs
static StgClosure* restoreUnpackState(UnpackInfo* unpack,StgClosure** graphroot, 
				      nat* pptr, nat* pptrs, nat* pvhs);
static UnpackInfo* saveUnpackState(StgClosure* graphroot, StgClosure* parent, 
				   nat pptr, nat pptrs, nat pvhs);
*/

// external interface, declared in Parallel.h:
/*
StgClosure        *UnpackGraph(rtsPackBuffer *packBuffer,
			       Port inPort,
			       Capability* cap);
*/

// unpacks one closure (common prelude + switches to special cases)
static  StgClosure *UnpackClosure (StgWord **bufptrP, Capability* cap);

// normal case:
STATIC_INLINE void LocateNextParent(StgClosure **parentP, nat *pptrP, 
				    nat *pptrsP, nat *pvhsP);
// special cases:
STATIC_INLINE  StgClosure *UnpackOffset(StgWord **bufptrP);
STATIC_INLINE  StgClosure *UnpackPLC(StgWord **bufptrP);
static StgClosure *UnpackPAP(StgInfoTable *ip,StgWord **bufptrP, 
			     Capability* cap);
static StgClosure *UnpackArray(StgInfoTable *info, StgWord **bufptrP, 
			       Capability* cap);

/* A special structure used as the "owning thread" of system-generated
 * blackholes.  Layout [ hdr | payload ], holds a TSO header.info and blocking
 * queues in the payload field.
 *
 * Used in: createBH (here), 
 * Threads::updateThunk + Messages::messageBlackHole (special treatment)
 * ParInit::synchroniseSystem(init), 
 * Evac::evacuate (do not evacuate) and GC::garbageCollect (evac. BQueue)
 */
StgInd stg_system_tso;


/* Global (static) variables and declarations: As soon as we allow
   threaded+parallel, we need a lock, or all will become dynamic. */

/* The pack buffer, space for packing a graph. NB Unpack buffer
   will later be defined and allocated in HLComms.c */
static rtsPackBuffer *globalPackBuffer = NULL;

/* packing and unpacking misc: */
static nat     pack_locn,           /* ptr to first free loc in pack buffer */
               buf_id = 1;          /* identifier for buffer */
static nat     unpacked_size;
static rtsBool packing_aborted; // global variable reporting whether
				// packing was aborted
//static StgClosure *thunks[MAX_THUNKS_PER_PACKET]; 
                        // for reverting thunks (only when *moving*)
static int thunks_packed; // used as index of thunks_packed

// for sending incompletely packed parts
static rtsBool roomInBuffer;
static OpCode *tagP;
// sendertso included in globalPackBuffer

/* The offset hash table is used during packing to record the location
   in the pack buffer of each closure which is packed */
static HashTable *offsetTable;

static nat offsetpadding = 0; // padding for offsets in subsequent
			      // packets
/* the closure queue */
static StgClosure **ClosureQueue = NULL;
static nat        clq_size, clq_pos;

#if defined(DEBUG)
// finger print: "type hash" of packed graph, for quick debugging
// checks
#define MAX_FINGER_PRINT_LEN  1023
static char fingerPrintStr[MAX_FINGER_PRINT_LEN];
static void GraphFingerPrint(StgClosure *graphroot);
static HashTable *tmpClosureTable;  // used in GraphFingerPrint and PrintGraph

#endif

// functionality:

//   utilities and helpers

/* @initPacking@ initialises the packing buffer etc. */
void InitPackBuffer(void)
{
  ASSERT(RTS_PACK_BUFFER_SIZE > 0);

  if (globalPackBuffer==(rtsPackBuffer*)NULL) {
    if ((globalPackBuffer = (rtsPackBuffer *) 
	 stgMallocBytes(sizeof(rtsPackBuffer)
			+ RTS_PACK_BUFFER_SIZE 
			+ sizeof(StgWord)*DEBUG_HEADROOM,
			"InitPackBuffer")) == NULL)
      barf("InitPackBuffer: could not allocate.");
  }
}

void freePackBuffer(void) {
  if (globalPackBuffer) // has been allocated (called from ParInit, so always)
    stgFree(globalPackBuffer);
  if (ClosureQueue) // has been allocated
    stgFree(ClosureQueue); 
}

//@cindex InitPacking
static void InitPacking(rtsBool unpack)
{
  if (unpack) {
    /* allocate a GA-to-GA map (needed for ACK message) */
    // InitPendingGABuffer(RtsFlags.ParFlags.packBufferSize);
  } else {
    /* allocate memory to pack the graph into */
    InitPackBuffer();
  }
  /* init queue of closures seen during packing */
  InitClosureQueue();

  offsetpadding = 1; // will be modified after sending partial message
                     // We start at 1 for the case that the graph root
                     // (with pack_locn=0) is found again.

 // we need to store and recall offsets quickly also when unpacking
 // partially filled packets.
  offsetTable = allocHashTable();
  if (unpack) 
    return;
    
  globalPackBuffer->id = buf_id++;  /* buffer id are only used for debugging! */
  pack_locn = 0;         /* the index into the actual pack buffer */
  unpacked_size = 0;     /* the size of the whole graph when unpacked */
  roomInBuffer = rtsTrue;
  thunks_packed = 0;  /* total number of thunks packed so far */
  packing_aborted = rtsFalse; /* stops packing (with possibly blocked tso) */
}

/* clear buffer, but use the old queue (stuffed) and the old offset table
   essentially a copy of InitPacking without offsetTable
   important: recall old pack_location as "offsetpadding" to allow
   cross-packet offsets. */
static void ClearPackBuffer(void)
{
  // no need to allocate memory again
  ASSERT(globalPackBuffer != NULL);
  ASSERT(ClosureQueue != NULL);

  // stuff the closure queue (would soon be full if we just continue)
  StuffClosureQueue();

  offsetpadding += pack_locn; // set to 1 when started (un)packing...

  // Buffer remains the same, admin. fields invalidated
  globalPackBuffer->id = buf_id++;  // buffer id are only used for debugging!
  pack_locn = 0;         // the index into the actual pack buffer
  unpacked_size = 0;     // the size of the whole graph when unpacked
  roomInBuffer = rtsTrue; 
}

/* DonePacking is called when we've finished packing.  It releases
   memory etc.  */

static void DonePacking(void)
{
  freeHashTable(offsetTable, NULL);
  offsetTable = NULL;
  offsetpadding = 0; // which is invalid.
}

/* RegisterOffset records that/where the closure is packed. */
STATIC_INLINE void
RegisterOffset(StgClosure *closure){

  insertHashTable(offsetTable, 
		  // remove tag for offset
		  UNTAG_CAST(StgWord, closure), 
		  (void *) (StgWord) (pack_locn + offsetpadding));
                  // note: offset is never 0, padding starts at 1
}

/* OffsetFor returns an offset for a closure which is already being packed. */
STATIC_INLINE nat
OffsetFor(StgClosure *closure) {
  // avoid typecast warnings...
  void* offset;
  offset = (lookupHashTable(offsetTable, 
			    // remove tag for offset
			    UNTAG_CAST(StgWord, closure)));
  return (nat) offset;
}

/* AlreadyPacked determines whether the closure's already being packed.
   Offset == 0 means no.  */
STATIC_INLINE rtsBool
AlreadyPacked(int offset) {
  // When root is found again, it will have offset 1 (offsetpadding).
  return(offset != 0); 
}

/***************************************************************
 * general helper functions used here:
 * Perhaps find a different home for some of them? 
 * ----------------------------------------------------------- */

/*  get_closure_info: returns payload structure/name/... 
    Only used here */
STATIC_INLINE StgInfoTable*
    get_closure_info(StgClosure* node, nat *size, nat *ptrs, 
		     nat *nonptrs, nat *vhs)
{
  StgInfoTable *info;

  /* We remove the potential tag before doing anything. */
  node = UNTAG_CLOSURE(node);

  info = get_itbl(node);

  // from Storage.h => included it here! (not part of Rts.h!)
  *size = closure_sizeW(node);

  /* Caution: layout field is union, may contain different information
     according to closure type! see InfoTables.h:
     THUNK_SELECTOR: selector_offset
     stack frames, ret. vec.s, whatever: bitmap / ptr. to large_bitmap
     other closures: ptrs | nptrs 
  */
  switch (info->type) {
  case THUNK_SELECTOR:
    *ptrs = 1; // selectee is a pointer
    *vhs  = *size - 1 - sizeofW(StgHeader);
    *nonptrs = 0;
    break;

    /* PAP/AP/AP_STACK contain a function field,
       treat this field as a (= the one single) pointer */
  case PAP:
    *vhs = 1; /* arity/args */
    *ptrs = 1;
    *nonptrs = 0; /* wrong, but not used in the unpacking code! */
    break;
  case AP_STACK:
  case AP:
    *vhs = sizeofW(StgThunkHeader) - sizeofW(StgHeader) + 1;
    *ptrs = 1;
    *nonptrs = 0; /* wrong, but not used in the unpacking code! */
    break;

    /* For Word arrays, no pointers need to be filled in. 
     * (the default case would work for them as well)
     */
  case ARR_WORDS:
    *vhs = 1;
    *ptrs = 0;
    *nonptrs = (((StgArrWords*) node)->bytes)/ sizeof(StgWord);
    break;

    /* For Arrays of pointers, we need to fill in all the pointers */
  case MUT_ARR_PTRS_CLEAN:
  case MUT_ARR_PTRS_DIRTY:
  case MUT_ARR_PTRS_FROZEN0:
  case MUT_ARR_PTRS_FROZEN:
    *vhs = 2;
    *ptrs = ((StgMutArrPtrs*) node)->ptrs;
    *nonptrs = 0; // could indicate card table... (?)
    break;

    /* we do not want to see these here (until thread migration) */
  case CATCH_STM_FRAME:
  case CATCH_RETRY_FRAME:
  case ATOMICALLY_FRAME:
  case UPDATE_FRAME:
  case CATCH_FRAME:
  case UNDERFLOW_FRAME:
  case STOP_FRAME:
  case RET_SMALL:
  case RET_BIG:
  case RET_BCO:
    barf("get_closure_info: stack frame!");
    break;
    
  default:
    /* this works for all pointers-first layouts */
    *ptrs = (nat) (info->layout.payload.ptrs);
    *nonptrs = (nat) (info->layout.payload.nptrs);
    *vhs = *size - *ptrs - *nonptrs - sizeofW(StgHeader);
  }

  return info;

} 

/* quick test for blackholes. Available somewhere else? */
rtsBool IsBlackhole(StgClosure* node)          
{ 

  // since ghc-7.0, blackholes are used as indirections. inspect indirectee.
  if(((StgInfoTable*)get_itbl(UNTAG_CLOSURE(node)))->type == BLACKHOLE) {

    StgClosure* indirectee = ((StgInd*)node)->indirectee;
    // some Blackholes are actually indirections since ghc-7.0
    switch (((StgInfoTable*)get_itbl(UNTAG_CLOSURE(indirectee)))->type) {

    case TSO:
    case BLOCKING_QUEUE:
      return rtsTrue;
    default:
      return rtsFalse;
    }
  }
  return rtsFalse;
}

StgClosure* createBH(Capability *cap) {
  StgClosure *new;

  // a blackhole carries one pointer of payload, see StgMiscClosures.cmm, so
  // we allocate 2 words. The payload indicates the blackhole owner, in our
  // case it is the "system" (or later, the cap, for -threaded rts).
  new = (StgClosure*) allocate(cap, 2);

  SET_HDR(new, &stg_BLACKHOLE_info, CCS_SYSTEM); // ccs to be checked!

  new->payload[0] = (StgClosure*) &stg_system_tso; 
          // see above. Pseudo-TSO (has TSO info pointer) owning all
          // system-created black holes, and storing BQs.

  return new;
}

// cons node info pointer, from GHC.Base
#define CONS_INFO ghczmprim_GHCziTypes_ZC_con_info
// constructor tag for pointer tagging. We return a tagged pointer here!
#define CONS_TAG  2
extern const StgInfoTable CONS_INFO[];

// creating a list node. returns a tagged pointer.
StgClosure* createListNode(Capability *cap, StgClosure *head, StgClosure *tail) {
  StgClosure *new;

  // a list node (CONS) carries two pointers => 3 words to allocate
  // if we have given a capability, we can allocateLocal (cheaper, no lock)
  new = (StgClosure*) allocate(cap, 3);

  SET_HDR(new, CONS_INFO, CCS_SYSTEM); // to be checked!!!
  new->payload[0] = head;
  new->payload[1] = tail;

  return TAG_CLOSURE(CONS_TAG,new);
}


/*
  RoomToPack determines whether there's room to pack the closure into
  the pack buffer. The buffer must be able to hold at least <size>
  more StgWords, plus one StgWord - the type tag (PLC,OFFSET, CLOSURE).

  Otherwise, it sends partial data away when no room is left.
  For GUM, we will have to include the queue size (as FETCH_MEs) as well.
*/
STATIC_INLINE void
RoomToPack(nat size) {
  if (roomInBuffer &&
      ( (pack_locn +                 // where we are in the buffer right now
	 size +                      // space needed for the current closure
	 // for GUM: 
	 // QueueSize * sizeof(FETCH_ME-in-buffer)
	 + 1)*sizeof(StgWord)        // tag 
	 >= 
       RTS_PACK_BUFFER_SIZE))
    {
      IF_PAR_DEBUG(pack, 
		   debugBelch("Pack buffer full (size %d). " 
			      "Sending partially to receiver.", 
			      pack_locn));
      barf("   sendWhilePacking() unimplemented.\n" 
	   "Current buffer size is %lu bytes, "
	   "try bigger pack buffer with +RTS -qQ<size>",
	   RTS_PACK_BUFFER_SIZE);
      ClearPackBuffer();
      IF_PAR_DEBUG(pack,
		   debugBelch("Sent partially, now continue packing."));
    }
  return;
}


//      Closure Queue:
/* InitClosureQueue allocates and initialises the closure queue. */
STATIC_INLINE 
void InitClosureQueue(void)
{
  clq_pos = clq_size = 0;

  if (ClosureQueue==NULL)
    ClosureQueue = (StgClosure**) 
      stgMallocBytes(RTS_PACK_BUFFER_SIZE, 
		     "InitClosureQueue");
}

/* PrintClosureQueue prints the whole closure queue. */
#if defined(DEBUG)
static void
PrintClosureQueue(void)
{
  nat i;

  debugBelch("Closure queue:\n");
  for (i=clq_pos; i < clq_size; i++) {
    debugBelch("at %d: %p (%s), \n", 
	    clq_size-i,
	    (StgClosure *)ClosureQueue[i], 
	    info_type(UNTAG_CLOSURE(ClosureQueue[i])));
  }
}
#endif

/* StuffClosureQueue moves the enqueued closures to the beginning of
   the allocated area (could use a modulo instead, easier like
   this, since not common case) */ 
STATIC_INLINE
void StuffClosureQueue(void)
{

  ASSERT(ClosureQueue != NULL);
  ASSERT(clq_pos<=clq_size);
  IF_PAR_DEBUG(packet, 
	       debugBelch("Stuffing closure queue (length %d).", 
			  QueueSize());
	       PrintClosureQueue());
  if ( clq_pos < clq_size ) {
  // move content of queue to start of allocated memory (if any content)
    memmove(ClosureQueue, ClosureQueue + clq_pos, 
	    (clq_size - clq_pos) * sizeof(StgClosure*));
  }
  // adjust position and size
  clq_size=clq_size - clq_pos;
  clq_pos=0;
  IF_PAR_DEBUG(packet, 
	       debugBelch("Closure queue now:");
	       PrintClosureQueue());
  return;
}

/* QueueEmpty is rtsTrue if closure queue empty; rtsFalse otherwise */
STATIC_INLINE 
rtsBool QueueEmpty(void)
{
  ASSERT(clq_pos <= clq_size);
  return(clq_pos == clq_size);
}

/* QueueSize is the queue size */
STATIC_INLINE 
nat QueueSize(void)
{
  ASSERT(clq_pos <= clq_size);
  return(clq_size - clq_pos);
}

/* QueueClosure adds its argument to the closure queue. */
STATIC_INLINE 
void QueueClosure(StgClosure* closure)
{
  ASSERT(clq_pos <= clq_size);
  if(clq_size < RTS_PACK_BUFFER_SIZE ) {
    IF_PAR_DEBUG(packet,
		 debugBelch(">__> Q: %p (%s); %ld elems in q\n",
			    closure, 
			    info_type(UNTAG_CLOSURE(closure)), (long)clq_size-clq_pos+1));
    ClosureQueue[clq_size++] = closure;
  } else { 
    barf("Pack.c: Closure Queue Overflow (EnQueueing %p (%s))", 
	 closure, info_type(UNTAG_CLOSURE(closure)));
  }
}

/* DeQueueClosure returns the head of the closure queue. */
STATIC_INLINE 
StgClosure* DeQueueClosure(void)
{
  if(!QueueEmpty()) {
    IF_PAR_DEBUG(packet,
		 debugBelch(">__> DeQ: %p (%s); %ld elems in q\n",
			    ClosureQueue[clq_pos], 
			    info_type(UNTAG_CLOSURE(ClosureQueue[clq_pos])), 
			    (long)clq_size-clq_pos-1));
    return(ClosureQueue[clq_pos++]);
  } else {
  IF_PAR_DEBUG(packet, debugBelch("Q empty\n "));
    return((StgClosure*)NULL);
  }
}

/*******************************************************************
 * packing a graph structure: 
 *
 * The graph is packed breadth-first into a static buffer.  
 * 
 * Interface: PackNearbyGraph(IN graph_root, IN packing_tso, 
 *                            IN/OUT msgtag)
 * main functionality: PackClosure (switches over type)
 *                     PackGeneric (copies, follows generic layout) 
 *
 * mid-level packing: a closure is preceded by a marker for its type
 *  0L - closure with static address        - PackPLC
 *  1L - offset (closure already in packet) - PackOffset
 *  2L - a heap closure follows             - PackGeneric/specialised routines
 *
 *  About the GHC feature "pointer tagging":
 *   Every closure pointer carries a tag in its l.s. bits (those which
 *   are not needed since closures are word-aligned anyway). These
 *   tags should survive packing-sending-unpacking, so we must store
 *   them in the packet somehow.
 *
 *   Retrieving the tag/closure ptr: the tagged pointers are
 *   *references* to a closure. NB: RTS must ensure that every
 *   occurrence of one and the same pointer has a correct tag (minor,
 *   or same tag)! OTOH, the tag must be inside the packed closure in
 *   the packet.  
 *
 *   => Closure pointers in the closure queue are stored *WITH TAGS*,
 *   and we pack the tags together with the closure.
 *   OTOH, *offsets* (HashTable entries) are stored without tags, in
 *   order to catch the case when two references with different tags
 *   exist (possible?)
 *   (the tag of the first occurrence will win, a problem?)
 *
 *   a) Could use last bits of info-ptr (stored anyway) ? Is it
 *   aligned just as closure pointers, in word size.
 *   b) spend an extra word on every heap-closure (for 3 bits :-| )
 *
 *   We clearly opt for a.
 *
 *   Anyway, closures are enqueued with tags, and the tag handled in
 *   functions called from PackClosure(): PackGeneric, or specialised
 *   ones.
 *
 *   Restoring the tags: Tags must be restored at every place where we
 *   put a reference to the closure. Here: when we fill in the
 *   pointers to a closure. The tags are restored right after
 *   unpacking, inside unpackClosure(). See comments there for details.
 * 
 *******************************************************************/

// helper accessing the pack buffer
STATIC_INLINE void Pack(StgWord data) {
  ASSERT(pack_locn*sizeof(StgWord) < RTS_PACK_BUFFER_SIZE);
  globalPackBuffer->buffer[pack_locn++] = data;
}

// packing a static value
STATIC_INLINE void PackPLC(StgPtr addr) {
  Pack(PLC);			/* weight */
  // pointer tag of addr still present, packed as-is
  Pack((StgWord) addr);		/* address */
}

// packing an offset (repeatedly packed same closure)
STATIC_INLINE void PackOffset(int offset) {
  Pack(OFFSET);			/* weight */
  //  Pack(0L);			/* pe */
  Pack(offset);		        /* slot/offset */
}

/*
  Packing Sections of Nearby Graph

  PackNearbyGraph packs a closure and associated graph into a static
  buffer (PackBuffer).  It returns the address of this buffer (which
  includes the size of the data packed into it, buffer->size !). The
  associated graph is packed in a breadth-first manner, hence it uses
  an explicit queue of closures to be packed rather than simply using
  a recursive algorithm.

  A request for packing is accompanied by the actual TSO. The TSO can
  be enqueued, if packing hits a Black Hole. Hitting a black hole
  means we have to retry after evaluation.

  TODO TODO TODO
  If the packet is full, the packing routine sends a packet on itself
  (using TSO->receiver, TODO which current Msg.?
  */

rtsPackBuffer* PackNearbyGraph(StgClosure* closure, StgTSO* tso, 
			       OpCode *msgtag)
{
  InitPacking(rtsFalse);
  
  IF_PAR_DEBUG(verbose,
	       debugBelch("Packing subgraph @ %p\n", closure));

  IF_PAR_DEBUG(pack,
	       debugBelch("packing:");
	       debugBelch("id <%ld> (buffer @ %p); graph root @ %p [PE %d]\n",
			  (long)globalPackBuffer->id, globalPackBuffer, 
			  closure, thisPE);
	       GraphFingerPrint(closure);
	       debugBelch("    demanded by TSO %d (%p); Fingerprint is\n"
			  "\t{%s}\n",
			  (int)tso->id, tso, fingerPrintStr));

  /*
    IF_PAR_DEBUG(packet,
	       debugBelch("** PrintGraph of %p is:", closure);
	       debugBelch("** pack_locn=%d\n", pack_locn);
	       PrintGraph(closure,0));
  */

  // save the packing TSO (to block/enqueue it and for partial sending)
  globalPackBuffer->tso = tso;

  // save pointer to message tag for this message,
  // we will need to change it when we send a partial message.
  tagP = msgtag; 

  QueueClosure(closure);
  do {
    PackClosure(DeQueueClosure());
    if (packing_aborted) 
      return ((rtsPackBuffer *)NULL);
  } while (!QueueEmpty());
  

  /* Check for buffer overflow (again) */
  ASSERT((pack_locn - DEBUG_HEADROOM) * sizeof(StgWord) 
	 <= RTS_PACK_BUFFER_SIZE);
  IF_DEBUG(sanity, // write magic end-of-buffer word
	   globalPackBuffer->buffer[pack_locn++] = END_OF_BUFFER_MARKER);

  /* Record how much space the graph needs in packet and in heap */
  globalPackBuffer->unpacked_size = unpacked_size;
  globalPackBuffer->size = pack_locn;

  DonePacking();

  IF_PAR_DEBUG(pack,
		debugBelch("** Finished <<%ld>> packing graph %p (%s); closures packed: %ld; thunks packed: %d; size of graph: %ld\n",
		      (long)globalPackBuffer->id, closure, info_type(UNTAG_CLOSURE(closure)),
		      (long)globalPackBuffer->size, thunks_packed, 
		      (long)globalPackBuffer->unpacked_size));;

  // TODO: IF_DEBUG(sanity, checkPacket(...));

  return (globalPackBuffer);
}

STATIC_INLINE StgClosure* UNWIND_IND(StgClosure *closure)
{
  StgClosure *start = closure;

  while (closure_IND(start)) 
    start = ((StgInd*) UNTAG_CLOSURE(start))->indirectee;

  return start;
}


/*
  @PackClosure@ is the heart of the normal packing code.  It packs a
  single closure into the pack buffer, skipping over any indirections,
  queues any child pointers for further packing.
*/

static void PackClosure(StgClosure* closure) {

  StgInfoTable *info;
  nat offset;

  // Ensure we can always pack this closure as an offset/PLC.
  RoomToPack(sizeofW(StgWord));

 loop:
  closure = UNWIND_IND(closure);
  /* now closure is the thing we want to pack */
  // ... but might still be tagged.
  offset = OffsetFor(closure);

  /* If the closure has been packed already, just pack an indirection to it
     to guarantee that the graph doesn't become a tree when unpacked */
  if (AlreadyPacked(offset)) {
    PackOffset(offset);
    return;
  }

  // remove the tag (temporary, subroutines will handle tag as needed)
  info = get_itbl(UNTAG_CLOSURE(closure));

  // we rely on info-pointers being word-aligned!
  ASSERT( info == UNTAG_CAST(StgInfoTable*, info));

  switch (info->type) {

    // follows order of ClosureTypes.h...
  case INVALID_OBJECT:
    barf("Found invalid object");

  case CONSTR:
  case CONSTR_1_0:
  case CONSTR_0_1:
  case CONSTR_2_0:
  case CONSTR_1_1:
  case CONSTR_0_2:
    PackGeneric(closure);
    return;

  case CONSTR_STATIC:
  case CONSTR_NOCAF_STATIC: // For now we ship indirections to CAFs: They are
			    // evaluated on each PE if needed
  case FUN_STATIC:          // ToDo: check whether that's ok
  case THUNK_STATIC:        // ToDo: check whether that's ok
    // TODO: based on discussion with SimonM we need to
    //       re-construct the tag for these closures -- HWL GUM6EDEN
    IF_PAR_DEBUG(packet,
		 debugBelch("*>~~ Packing a %p (%s) as a PLC\n", 
		       closure, info_type_by_ip(info)));

    PackPLC((StgPtr)closure);
    // NB: unpacked_size of a PLC is 0
    return;

  case  FUN:
  case	FUN_1_0:
  case	FUN_0_1:
  case	FUN_2_0:
  case	FUN_1_1:
  case	FUN_0_2:
    PackGeneric(closure);
    return;

  case  THUNK:
  case	THUNK_1_0:
  case	THUNK_0_1:
  case	THUNK_2_0:
  case	THUNK_1_1:
  case	THUNK_0_2:
    // !different layout! (smp update field, see Closures.h)
    // the update field should better not be shipped...
    PackGeneric(closure);
    return;

  case THUNK_SELECTOR: 
    {
      /* a thunk selector means we want to extract one of the
       * arguments of another closure. See GC.c::eval_thunk_selector:
       * selectee might be CONSTR*, or IND*, or unevaluated (THUNK*,
       * AP, AP_STACK, BLACKHOLE).
       *
       * GC tries to evaluate and eliminate THUNK_SELECTORS by
       * following them. For packing, we could include them in
       * UNWIND_IND, but this is fatal in case of a loop (UNWIND_IND
       * will loop). So we just pack the selectee as well
       * instead. get_closure_info treats the selectee in this closure
       * type as a pointer field.
       */

      IF_PAR_DEBUG(packet,
		   StgClosure *selectee 
		     = ((StgSelector *) UNTAG_CLOSURE(closure))->selectee;
		   debugBelch("*>** Found THUNK_SELECTOR at %p (%s)" 
			      "pointing to %p (%s)\n", 
			      closure, info_type_by_ip(info), 
			      selectee, info_type(UNTAG_CLOSURE(selectee))));
      PackGeneric(closure);

      return;
    }

  case BCO: 
    barf("Packing: BCO, not implemented");

  case AP:
  case PAP:
    PackPAP((StgPAP *)closure); // also handles other stack-containing types
    return;
 
  case AP_STACK:
    barf("Pack: unable to pack a stack");

  case IND:
  case IND_PERM:
  case IND_STATIC:
    barf("Pack: found IND_... after shorting out indirections %d (%s)", 
	 (nat)(info->type), info_type_by_ip(info));

    // return vectors
  case RET_BCO:
  case RET_SMALL:
  case RET_BIG:
  case RET_DYN:
  case RET_FUN:
    barf("{Pack}Daq Qagh: found return vector %p (%s) when packing", 
	 closure, info_type_by_ip(info));

    // stack frames
  case UPDATE_FRAME:
  case CATCH_FRAME:
  case UNDERFLOW_FRAME:
  case STOP_FRAME:
    barf("{Pack}Daq Qagh: found stack frame %p (%s) when packing (thread migration not implemented)", 
	 closure, info_type_by_ip(info));

  case BLOCKING_QUEUE:
    barf("Hit blocking queue when packing!");

  case BLACKHOLE:

    //  case RBH:
    {
      StgTSO* tso = globalPackBuffer->tso;
      StgClosure* indirectee = ((StgInd*)closure)->indirectee;

      // some Blackholes are actually indirections since ghc-7.0
      switch (((StgInfoTable*)get_itbl(UNTAG_CLOSURE(indirectee)))->type) {

      case IND: // race cond. when threaded? 
	// We keep this (unobvious) case in analogy to StgMiscClosures.cmm
	goto loop;
      case TSO: // no blocking queue yet. msgBlackHole will create one.
      case BLOCKING_QUEUE: // another thread already blocked here. Enqueue

	// If a TSO called a primOp, it must be blocked on this BH
	// until the BH gets updated/data arrives. On the awakening of
	// the BlockingQueue, the PrimOp calls packClosure again.
	if (tso != NULL) {
	  MessageBlackHole *msg = NULL;

	  IF_PAR_DEBUG(packet, 
	       debugBelch("TSO %d blocking on %s at %p while packing.\n",
			  (int)tso->id, info_type_by_ip(info), closure));

	  // everything for blocking the tso is done here: create a message,
	  // call msgBlackHole, set fields in tso. If msgBlackHole signals we
	  // can continue (threaded rts case), we jump back.

	  msg = (MessageBlackHole*) allocate(tso->cap, 
					     sizeofW(MessageBlackHole));
	  SET_HDR(msg, &stg_MSG_BLACKHOLE_info, CCS_SYSTEM);
	  msg->tso = tso;
	  msg->bh  = closure;

	  if (messageBlackHole(tso->cap, msg)) {
	    tso->why_blocked = BlockedOnBlackHole;
	    tso->block_info.bh = msg;
	    packing_aborted = rtsTrue; // packing failed, will get buffer=NULL
	  } else {
	    goto loop; // failed to block (race condition), try again
	  }
	} else {
	  // GUM TODO: if tso == NULL (RTS has requested packing,
	  // i.e. GUM is exporting a spark), just pack a fetchMe.
	  
	  barf("PackClosure->blackhole case: no TSO");
	}
	return;
      default: // an indirection, pack the indirectee (jump back to start)
	closure = indirectee;
	// this is a new case of "UNWIND_IND", a blackhole which is indeed an
	// indirection. Difficult to catch in UNWIND_IND, so jump back here.
	goto loop;
      }
    }

  case MVAR_CLEAN:
  case MVAR_DIRTY:
    barf("Pack: packing type %s (%p) not implemented", 
	 info_type_by_ip(info), closure);
    

  case ARR_WORDS:
    PackArray(closure);
    return;

  case MUT_ARR_PTRS_CLEAN:
  case MUT_ARR_PTRS_DIRTY:
  case MUT_ARR_PTRS_FROZEN0:
  case MUT_ARR_PTRS_FROZEN:
    /* We use the same routine as for ARR_WORDS The implementation of
       (immutable!) arrays uses these closure types, as well as that
       of mutable arrays.  => perhaps impossible to find out from the
       RTS whether we should allow duplication of the array or not.
    */
    IF_PAR_DEBUG(packet,
		 debugBelch("Packing pointer array @ %p!", closure));
    PackArray(closure);
    return;

  case MUT_VAR_CLEAN:
  case MUT_VAR_DIRTY:
    barf("MUT_VAR packing, not imlemented yet.\n (DEBUG: packing %s @ %p)",
	 info_type_by_ip(info),closure);

  case WEAK:
  case PRIM:
    barf("Pack: packing type %s (%p) not implemented", 
	 info_type_by_ip(info), closure);
  case MUT_PRIM:
    debugBelch("Pack: found mutable thing in %x (%s); aborting packing", 
	       (nat)(info->type), info_type_by_ip(info));
    packing_aborted = rtsTrue;
    return;
    
  case TSO:
  case STACK:
    barf("{Pack}Daq Qagh: found TSO/STACK %p when packing (thread migration not implemented)", 
	 closure, info_type_by_ip(info));

  case TREC_CHUNK:
    barf("Pack: packing type %s (%p) not implemented", 
	 info_type_by_ip(info), closure);

    // more stack frames:
  case ATOMICALLY_FRAME:
  case CATCH_RETRY_FRAME:
  case CATCH_STM_FRAME:
    barf("{Pack}Daq Qagh: found stack frame %p (%s) when packing (thread migration not implemented)", 
	 closure, info_type_by_ip(info));

  case WHITEHOLE:
    /* something's very wrong */
    barf("Pack: found %s (%p) when packing", 
	 info_type_by_ip(info), closure);

  default:
    barf("Pack: strange closure %d", (nat)(info->type));
    packing_aborted = rtsTrue; // not reached...
    return;
  } /* switch */
}

static void PackGeneric(StgClosure* closure) {
  nat size, ptrs, nonptrs, vhs, i;
  StgWord tag=0;

  /* store tag separately, pack with info ptr. */
  tag = GET_CLOSURE_TAG(closure);
  closure = UNTAG_CLOSURE(closure);

  /* get info about basic layout of the closure */
  get_closure_info(closure, &size, &ptrs, &nonptrs, &vhs);

  ASSERT(!IsBlackhole(closure));

  IF_PAR_DEBUG(packet,
	       debugBelch("*>== %p (%s): generic packing" 
			  "(size=%d, ptrs=%d, nonptrs=%d, and tag %d)\n",
			  closure, info_type(closure), size, ptrs, nonptrs, 
			  (int)tag));

  /* make sure we can pack this closure into the current buffer 
     (otherwise this routine sends a message and resets the buffer) */
  RoomToPack(HEADERSIZE + vhs + nonptrs);

  /* Record the location of the GA */
  RegisterOffset(closure);

  /* Allocate a GA for this closure and put it into the buffer
     Checks for globalisation scheme; default: globalise everything thunks 
  if ( RtsFlags.ParFlags.globalising == 0 || 
       (closure_THUNK(closure) && !closure_UNPOINTED(closure)) )
    GlobaliseAndPackGA(closure);
  else 
  */
    {
      Pack((StgWord) CLOSURE);  // marker for unglobalised closure
    }

  /* At last! A closure we can actually pack! */
      
  /* 
     Remember, the generic closure layout is as follows:
        +-------------------------------------------------+
	| FIXED HEADER | VARIABLE HEADER | PTRS | NON-PRS |
        +-------------------------------------------------+
  */
  /* pack fixed and variable header */
  // store the tag inside the first word (==infopointer)
  Pack((StgWord) (TAG_CLOSURE(tag, (StgClosure*) *((StgPtr) closure ))));
  // pack the rest of the header (variable header)
  for (i = 1; i < HEADERSIZE + vhs; ++i)
    Pack((StgWord)*(((StgPtr)closure)+i));
      
  /* register all ptrs for further packing */
  for (i = 0; i < ptrs; ++i)
    QueueClosure(((StgClosure *) *(((StgPtr)closure)+(HEADERSIZE+vhs)+i)));

  /* pack non-ptrs */
  for (i = 0; i < nonptrs; ++i)
    Pack((StgWord)*(((StgPtr)closure)+(HEADERSIZE+vhs)+ptrs+i));
      
  ASSERT(HEADERSIZE+vhs+ptrs+nonptrs==size); // no slop in closure, all packed

  unpacked_size += size;

  /* Record that this is a revertable black hole so that we can fill in
     its address from the fetch reply.  Problem: unshared thunks may cause
     space leaks this way, their GAs should be deallocated following an
     ACK.
   */
      
  /* future GUM code: convert to RBH
  if (closure_THUNK(closure) && !closure_UNPOINTED(closure)) { 

    StgClosure *rbh;
    rbh = convertToRBH(closure);
    ASSERT(size>=HEADERSIZE+MIN_UPD_SIZE); // min size for any updatable closure
    ASSERT(rbh == closure);         // rbh at the same position (minced version)

    // record the thunk that has been packed so that we may abort and revert
    if (thunks_packed < MAX_THUNKS_PER_PACKET) 
      thunks[thunks_packed++] = closure;
    // otherwise: abort packing right now (should not happen at all).
  }
  */
}

/* Packing PAPs: a PAP (partial application) represents a function
 * which has been given too few arguments for complete evaluation
 * (thereby defining a new function with fewer arguments).  PAPs are
 * packed by packing the function and the argument stack, where both
 * can point to static or dynamic (heap-allocated) closures which must
 * be packed later, and enqueued here.
 */
static void PackPAP(StgPAP *pap) {
  nat i;
  nat hsize;         // StgHeader size
  StgPtr p;          // stack object currently packed...
  nat args;          // no. of arguments
  StgWord bitmap;    // ptr indicator
  nat size;          // size of bitmap
  StgFunInfoTable *funInfo; // where to get bitmap...

  // JB 08/2007: remove, and store the tag separately...
  StgWord tag=0;

  tag = GET_CLOSURE_TAG((StgClosure*) pap);
  pap = (StgPAP*) UNTAG_CLOSURE((StgClosure*) pap);

  ASSERT(LOOKS_LIKE_CLOSURE_PTR(pap));

  ASSERT(get_itbl(pap)->type == PAP || 
	 get_itbl(pap)->type == AP);

  /* PAP/AP closure layout in GHC-6.x (see Closures.h):
   * +--------------------------------------------------------------+
   * | Header | (arity | n_args) | Function | Stack.|Stack.|Stack...|
   * +--------------------------------------------------------------+
   *   particularities:
   *   ----------------
   * PAP     : as described, normal Header
   * AP      : Thunk Header (1 extra word, see Closures.h)
   *
   * In previous versions, we treated AP_STACK in the same way. This
   * is wrong, AP_STACK closures can contain update frames and other
   * stack-only objects. 
   */

  switch (get_itbl(pap)->type) {
  case PAP: 
    size  = pap_sizeW(pap);
    args  = pap->n_args;
    hsize = HEADERSIZE+1;
    break;
  case AP:
    size  = ap_sizeW((StgAP *)pap);
    args  = ((StgAP*) pap)->n_args;
    hsize = sizeofW(StgThunkHeader)+1;
    break;
  default: 
    barf("PackPAP: strange info pointer, type %d ", 
	 get_itbl(pap)->type);
  }
  IF_PAR_DEBUG(packet,
	       debugBelch("Packing Closure with stack (%s) @ %p,"
			  "stack size %d\n",
			  info_type((StgClosure*) pap), pap, args));

  // preliminaries: register closure, check for enough space...
  RegisterOffset((StgClosure*) pap);
  RoomToPack(size - 1 + args);// double stack size, but no
                              // function field

  // pack closure marker
  Pack((StgWord) CLOSURE);

  /*
   * first pack the header, which starts with non-ptrs: 
   *   { StgHeader , (arity,args) } 
   * StgHeader is where AP (ThunkHeader) differs from
   * PAP. Besides, both have one extra StgWord containing
   * (arity|n_args) resp. size. hsize got adjusted above, now pack
   * exactly this amount of StgWords.
   * And store tag in first word of header (==infopointer)
   */
  Pack((StgWord) (TAG_CLOSURE(tag, (StgClosure*) *((StgPtr) pap ))));
  for(i = 1; i < hsize; i++) { 
    Pack((StgWord) *(((StgWord*)pap)+i));
  }

  /* Next, we find the function, which might be heap-allocated.
   * Instead of checking if it is heap_alloc.ed, we always queue it
   * and pack it later, unpacking is the reverse (needs to treat the
   * field as a pointer arg.).
   * Queue fun closure with tags, analyse without them...
   */
  funInfo = get_fun_itbl(UNTAG_CLOSURE(pap->fun));
  QueueClosure(pap->fun);

  unpacked_size += pap_sizeW(pap); // minimum: unpack only the PAP

  /* Now the payload, which is the argument stack of the function. A
   * packed bitmap inside the function closure indicates which stack
   * addresses are pointers(0 for ptr).  See GC:scavenge_PAP_payload()
   * and InfoTables.h,Constants.h for details...
   *
   * Saving the bitmap in the packet does not work easily, since it
   * can vary in size (bitmap/large bitmap). So we tag every stack
   * element with its type for unpacking (tags: PLC/CLOSURE), doubling
   * the stack size. We could pack only the tag for pointers, but then
   * we do not know the size in the buffer, so we pack the tag twice
   * for a pointer.
   *
   * Unpacking a PAP is the counterpart, which
   * creates an extra indirection for every pointer on the stack and
   * puts the indirection on the stack.
   */
  switch(funInfo->f.fun_type) {

    // these two use a large bitmap. 
  case ARG_GEN_BIG:
  case ARG_BCO:
    barf("PackPAP: large bitmap, not implemented");
    // should be sort of: 
    //  packLargeBitmapStack(pap->payload,GET_FUN_LARGE_BITMAP(funInfo),args); 
    //  return;
    break;

    // another clever solution: fields in info table different for
    // some cases... and referring to autogenerated constants (Apply.h)
  case ARG_GEN:
    bitmap = funInfo->f.b.bitmap;
    break;
  default:
    bitmap = stg_arg_bitmaps[funInfo->f.fun_type];
  }

  p = (StgPtr) pap->payload;  // points to first stack element
  args = pap->n_args;// tells how many slots we find on the stack

  // extract bits and  size from bitmap (see Constants.h):
  size = BITMAP_SIZE(bitmap);
  // ASSERT(size == args); NO, not always n_args == BITMAP_SIZE(bitmap).
  // size refers to the bitmap for the whole function.
  bitmap = BITMAP_BITS(bitmap);

    IF_PAR_DEBUG(packet,
		 debugBelch("Packing stack chunk, size %d (PAP.n_args=%d), bitmap %#o\n",
			    size, (int)args, (nat)bitmap));

  /* now go through the small bitmap (its size should be == args???)
   * The bitmap contains 5/6 bit size, which should AFAICT be
   * identical to args.  Not ones, but zeros indicate pointers on the

   * stack! According to Scav.c, the "direction" of the bitmap traversal 
   * is least to most significant ( bitmap>>1 corresponds to p++).
   */
  size = 0;
  while (args > 0) {
    if ((bitmap & 1)== 0) { /* Zero: a pointer*/
      ASSERT(LOOKS_LIKE_CLOSURE_PTR((StgClosure*) *p));
      Pack((StgWord) CLOSURE);         // pointer tag
      Pack((StgWord) CLOSURE);         // padding for constant size...
      QueueClosure((StgClosure*) *p);  // closure will be packed later
      unpacked_size += sizeofW(StgInd);// unpacking creates add. IND
      size++;
    } else {
      Pack((StgWord) PLC); // constant tag
      Pack((StgWord) *p); // and the argument
    }
    p++;                // advance in payload, next bit, next arg
    bitmap = bitmap>>1;
    args--;
  }

  /* PAP/AP in the pack buffer:
   * +-----------------------------------------------------------------+
   * | Header | (arity | n_args) | Tag | Arg/Tag | Tag | Arg/Tag | ... |
   * +-----------------------------------------------------------------+
   * Header can be 1 (normal) or 2 StgWords (Thunk Header)
   */

  IF_PAR_DEBUG(packet,
	       debugBelch("packed PAP, stack contained %d pointers\n",
			  size));
  
}

/* Packing Arrays.
 *
 * An Array in the heap can contain StgWords or Pointers (to
 * closures), and is thus of type StgArrWords or StgMutArrPtrs.
 *
 *     Array layout in heap/buffer is the following:
 * +----------------------------------------------------------------------+
 * | Header | size(in bytes) | word1 | word2 | ... | word_(size*wordsize) |
 * +----------------------------------------------------------------------+
 * 
 * Packing ArrWords means to just pack all the words (as non-ptrs).
 * The array size is stored in bytes, but will always be word-aligned.
 *
 * MutArrPtrs (MUT_ARRAY_PTRS_* types) contain pointers to other 
 * closures instead of words. 
 * Packing MutArrPtrs means to enqueue/pack all pointers found.
 * OTOH, packing=copying a mutable array is not a good idea at all.
 * We implement it even though, leave it to higher levels to restrict.
 *
 */
static void
PackArray(StgClosure *closure) {
  StgInfoTable *info;
  nat i, payloadsize, packsize;

  /* remove tag, store it in infopointer (same as above) */
  StgWord tag=0;

  tag = GET_CLOSURE_TAG(closure);
  closure = UNTAG_CLOSURE(closure);

  /* get info about basic layout of the closure */
  info = get_itbl(closure);

  ASSERT(info->type == ARR_WORDS
	 || info->type == MUT_ARR_PTRS_CLEAN
	 || info->type == MUT_ARR_PTRS_DIRTY
	 || info->type == MUT_ARR_PTRS_FROZEN0
	 || info->type == MUT_ARR_PTRS_FROZEN);

  if (info->type == ARR_WORDS) {
    payloadsize = arr_words_words((StgArrWords *)closure);
    packsize = payloadsize + 2; // size in words in buffer, for check only
  } else {
    // MUT_ARR_PTRS_* {info,(no. of)ptrs,size(total incl.card table)}
    // Only pack header, not card table which follows the data.
    packsize = 3;
    payloadsize = ((StgMutArrPtrs *)closure)->ptrs;
  }

  // the function in ClosureMacros.h would include the header:
  // arr_words_sizeW(stgCast(StgArrWords*,q));
  IF_PAR_DEBUG(pack,
	       debugBelch("*>== %p (%s): packing array" 
			  "(%d words) (size=%d)\n",
			  closure, info_type(closure), payloadsize,
			  (int)closure_sizeW(closure)));

  /* TODO: make enough room in the pack buffer, see PackPAP code */
  RoomToPack(packsize);
  
  /* record offset of the closure */
  RegisterOffset(closure);

  /* future GUM code: 
     Check for globalisation scheme; default: globalise every thunk
  if ( RtsFlags.ParFlags.globalising == 0 || 
       (closure_THUNK(closure) && !closure_UNPOINTED(closure)) )
    GlobaliseAndPackGA(closure);
  else
  */
    Pack((StgWord) CLOSURE);  // marker for unglobalised closure

  /* Pack the header (2 words: info ptr and the number of bytes to follow) 
   */
  Pack((StgWord) (TAG_CLOSURE(tag, (StgClosure*) *((StgPtr) closure ))));
  Pack((StgWord) ((StgArrWords *)closure)->bytes);

  if (info->type == ARR_WORDS) {
    /* pack the payload of the closure (all non-ptrs) */
/*    for (i=0; i<payloadsize; i++)
        Pack((StgWord)((StgArrWords *)closure)->payload[i]);
*/
     memcpy((globalPackBuffer->buffer) + pack_locn,
	    ((StgArrWords *)closure)->payload,
 	   payloadsize * sizeof(StgWord));
       pack_locn += payloadsize; 

  } else {
    // MUT_ARR_PTRS_*: pack total size, enqueue pointers
    Pack((StgWord) ((StgMutArrPtrs*)closure)->size);
    for (i=0; i<payloadsize; i++)
      QueueClosure(((StgMutArrPtrs *) closure)->payload[i]);
  }
  // this should be correct for both ARR_WORDS and MUT_ARR_*
  unpacked_size += closure_sizeW(closure);
}


/*******************************************************************
 *   unpacking a graph structure:
 *******************************************************************/

/*
  @UnpackGraph@ unpacks the graph contained in a message buffer.  It
  returns a pointer to the new graph.  

  The inPort parameter restores the unpack state when the sent graph
  is sent in more than one message.

  Formerly, we also had a globalAddr** @gamap@ parameter: set to
  point to an array of (oldGA,newGA) pairs which were created as a result
  of unpacking the buffer; and nat* @nGAs@ set to the number of GA pairs which
  were created.

  With the new per-capability allocation, we need to pass the cap.
  parameter around all the time. Could put it into a global unpacking
  state...
      
  for "pointer tagging", we assume here that all stored
  info pointers (each first word of a packed closure) also carry the
  tag found at the sender side when enqueueing it (for the first
  time!). When closures are unpacked, the tag must be added before
  inserting the result of unpacking into other closures as a pointer.
  Done by UnpackClosure(), see there.
*/

StgClosure *
UnpackGraph(rtsPackBuffer *packBuffer, 
	    STG_UNUSED Port inPort, Capability* cap) {
  StgWord *bufptr, *slotptr;
  StgClosure *closure, *graphroot;
  StgClosure *parent = NULL;
  nat bufsize, pptr = 0, pptrs = 0, pvhs = 0;
  nat unpacked_closures = 0, unpacked_thunks = 0; // stats only
#if defined(DEBUG)
  nat heapsize;
#endif

  /* to save and restore the unpack state (future use...)
  UnpackInfo *unpackState = NULL;
  */
  nat currentOffset;

  /* Initialisation */
  InitPacking(rtsTrue);      // same as in PackNearbyGraph

  /*  IF_DEBUG(sanity, // do a sanity check on the incoming packet
  	   checkPacket(packBuffer));
  */

  graphroot = (StgClosure *)NULL;

  /* Unpack the header */
  bufsize = packBuffer->size;
  IF_PAR_DEBUG(pack,
    heapsize = packBuffer->unpacked_size;
    debugBelch("Packing: Header unpacked. (bufsize=%d, heapsize=%d)\n"
	       "Unpacking closures now...\n", 
	       bufsize, heapsize));
 
  /* starting point */
  bufptr = packBuffer->buffer;

  // check if an incomplete subgraph is pending for this inport 
  // if there is any, we should unpack a PART Message here.
  
  /* TODO!
     Inport* inport = findInportByP(inPort);
     unpackState = inport->unpackState;
     inport->unpackState = NULL; // invalidate, will be deallocated!
  if (unpackState!=NULL) {
    IF_PAR_DEBUG(pack,
	   debugBelch("Unpack state found, partial message on inport %d.\n", 
		      (int)inPort.id));
    // restore data for unpacking: inverse analogon to saveUnpackState(...)
    parent = restoreUnpackState(unpackState, &graphroot, &pptr, &pptrs, &pvhs);
  } else {
    IF_PAR_DEBUG(pack,
	   debugBelch("Unpack state not found, normal message on inport %d\n.", 
		      (int)inPort.id));
    parent = (StgClosure *) NULL;
  }
   */

  do {
    /* check that we aren't at the end of the buffer, yet */
    IF_DEBUG(sanity, ASSERT(*bufptr != END_OF_BUFFER_MARKER));

    /* pointer to the type marker (Offset, PLC, or Closure) */
    slotptr = bufptr;
    currentOffset = (offsetpadding /* ...which is at least 1! */
		     + ((nat) (bufptr - (packBuffer->buffer))));

    /* this allocates heap space, checks for PLC/offset etc 
     * The "pointer tag" found at sender side is added
     * inside this routine, nothing special to do here.
     */
    closure = UnpackClosure (/*in/out*/&bufptr, cap);
    unpacked_closures++; // stats only
    unpacked_thunks += (closure_THUNK(closure)) ? 1 : 0; // stats only

    /*
     * Set parent pointer to point to chosen closure.  If we're at the top of
     * the graph (our parent is NULL), then we want to arrange to return the
     * chosen closure to our caller (possibly in place of the allocated graph
     * root.)
     */
    if (parent == NULL) 
      {
	/* we are at the root. Do not remove the tag here (old code did)! */
	// graphroot = UNTAG_CLOSURE(closure);
	graphroot = closure;
	IF_PAR_DEBUG(pack,
		     debugBelch("Graph root %p, with tag %x",
				closure, (int) GET_CLOSURE_TAG(closure)));
      }
    else
      {
	// if we restored the queue, we must update temporary blackholes
	// instead of just writing a pointer into the parent
	StgClosure* childP = (StgClosure*) ((StgPtr) parent)[HEADERSIZE + pvhs + pptr];
	if (childP && // perhaps invalid ptr
	    IsBlackhole(childP)) { 
	  // We will not hit a "tagged" childP here, it is always a BH
	  // created by us.
	  
	  // wake up blocked threads, use system tso as owner
	  updateThunk(cap, (StgTSO*) &stg_system_tso, childP, closure);
	} else { 
	  ((StgPtr) parent)[HEADERSIZE + pvhs + pptr] = (StgWord) closure; 
	}
      }
    
      // store closure for offsets, (special var. for offsets to other
      // packets)
    if (!((StgWord) *slotptr == OFFSET || (StgWord) *slotptr == PLC )) { 
      // avoid duplicate entries for static closures or indirections
      //	HEAP_ALLOCED(closure)) {
      IF_PAR_DEBUG(packet, 
		   debugBelch("---> Entry in Offset Table: (%d, %p)\n", 
			      currentOffset, closure));
      /* note that we store the address WITH TAG here! */
      insertHashTable(offsetTable, currentOffset, (void*) closure);
    }
    
    /* Locate next parent pointer */
    
    LocateNextParent(&parent, &pptr, &pptrs, &pvhs);
    
    // stop when buffer size has been reached or end of graph
  } while ((parent != NULL) && (bufsize > (nat) (bufptr-(packBuffer->buffer))));

  if (parent != NULL) {  // prepare all for next part if graph not complete
 
   barf("Graph fragmentation not supported, packet full\n" 
	"(and THIS MESSAGE SHOULD NEVER BE SEEN BY A USER)\n"
	"Try a bigger pack buffer with +RTS -qQ<size>");

  /* future use: save unpack state to support fragmented subgraphs

    StgClosure* tempBH;
    StgPtr childP;

    IF_PAR_DEBUG(packet,
		 debugBelch("bufptr - (packBuffer->buffer) = %d, bufsize = %d, parent = %p\n",
		       (nat) (bufptr-(packBuffer->buffer)), bufsize, parent);
		 debugBelch("Queue not empty, packet entirely unpacked.\n"
			    "Expecting rest on same inport.\n"));

    offsetpadding += bufsize; // to save it in unpackState...
    unpackState = saveUnpackState(graphroot, parent, pptr, pptrs, pvhs);// also saves queue and offsets/padding

    // TODO
    // inport->unpackState = unpackState; // set field in inport (implies "pendingMsg" inport state)

    // queue has been saved, now fill in temporary QueueMe closures (destroying queue and unpack state)
    while (parent != NULL) {
      // it can be that there are already QMs hanging on the queued parent closures (restored queue)
      // in this case, we do not create new QMs (possibly already blocked TSOs)
      childP = (StgPtr) ((StgPtr) parent)[HEADERSIZE + pvhs + pptr];
      if (childP == NULL) { // invalid ptr, was set to NULL when filling in a new closure
	tempBH = createBH(cap);
	((StgPtr)parent)[HEADERSIZE + pvhs + pptr] = (StgWord) tempBH;
	IF_PAR_DEBUG(packet,
		   debugBelch("Inserted temporary QueueMe child at %p in parent closure %p.\n",
			      tempBH, parent));
      }	else {
	ASSERT(IsBlackhole((StgClosure*) childP));
      }
      LocateNextParent(&parent, &pptr, &pptrs, &pvhs); 
      // until the queue is empty and the last parent filled
    } // while
  */
  } else { 
    // PARENT == NULL: whole graph has arrived, drop offset table
    freeHashTable(offsetTable, NULL);
    offsetTable = NULL;
  }

  IF_PAR_DEBUG(pack,
    debugBelch("Packing: Unpacking done. \n"));

   IF_PAR_DEBUG(pack,
  	       GraphFingerPrint(graphroot);
  	       debugBelch(">>> Fingerprint of graph rooted at %p (after unpacking <<%ld>>:\n"
			  "\t{%s}\n",
			  graphroot, (long)packBuffer->id, fingerPrintStr));

  /* check for magic end-of-buffer word (+ increment bufptr */
  IF_DEBUG(sanity, ASSERT(*(bufptr++) == END_OF_BUFFER_MARKER));

  /* we unpacked exactly as many words as there are in the buffer */
  ASSERT(bufsize == (nat) (bufptr-(packBuffer->buffer)));
  /* we filled no more heap closure than we allocated at the beginning; 
     ideally this should be a ==; 
     NB: test is only valid if we unpacked anything at all (graphroot might
         end up to be a PLC!), therefore the strange test for HEAP_ALLOCED 
  */

  /* ToDo: are we *certain* graphroot has been set??? WDP 95/07 */
  ASSERT(graphroot!=NULL);

  IF_DEBUG(sanity,
           {
	     StgPtr p;

	     /* check the unpacked graph */
	     //checkHeapChunk(graphroot,graph-sizeof(StgWord));

	     // if we do sanity checks, then wipe the pack buffer after unpacking
	     for (p=(StgPtr)packBuffer->buffer; p<(StgPtr)(packBuffer->buffer)+(packBuffer->size); )
	       *p++ = 0xdeadbeef;
            });

  return (graphroot);
}


/*
  Find the next pointer field in the parent closure, retrieve information
  about its variable header size and no. of pointers.
  If the current parent has been completely unpacked already, get the
  next closure from the global closure queue, and register the new variable
  header size and no. of pointers. 
  Example situation:

*parentP
    |
    V
  +--------------------------------------------------------------------+
  |hdr| variable hdr  | ptr1 | ptr2 | ptr3 | ... | ptrN | non-pointers |
  +--------------------------------------------------------------------+
      <--- *vhsP=2 --->                A
                                       |
	 *pptrs = N                 *pptr=3
*/
//@cindex LocateNextParent
STATIC_INLINE void
LocateNextParent(parentP, pptrP, pptrsP, pvhsP)
StgClosure **parentP;
nat *pptrP, *pptrsP, *pvhsP;
{
  nat size, nonptrs;

  /* pptr as an index into the current parent; find the next pointer field
     in the parent by increasing pptr; if that takes us off the closure
     (i.e. *pptr + 1 > *pptrs) grab a new parent from the closure queue
  */
  (*pptrP)++;
  while (*pptrP + 1 > *pptrsP) {
    /* *parentP has been constructed (all pointer set); so check it now */

    IF_DEBUG(sanity,
	     if (*parentP != (StgClosure*)NULL) // not root
	        checkClosure(*parentP));

    *parentP = DeQueueClosure();
    
    if (*parentP == NULL)
      break;
    else {
      get_closure_info(*parentP, &size, pptrsP, &nonptrs, pvhsP);
      *pptrP = 0;
    }
  }
  /* *parentP points to the new (or old) parent; */
  /* *pptr, *vhsP, and *pptrs have been updated referring to the new parent */
}


/* 
   UnpackClosure is the heart of the unpacking routine. It is called for 
   every closure found in the packBuffer. Any prefix such as GA, PLC marker
   etc has been unpacked into the *ga structure. 
   UnpackClosure does the following:
     - check for the kind of the closure (PLC, Offset, std closure)
     - copy the contents of the closure from the buffer into the heap
     - update LAGA tables (in particular if we end up with 2 closures 
       having the same GA, we make one an indirection to the other)
     - set the GAGA map in order to send back an ACK message

   At the end of this function,
   *bufptrP points to the next word in the pack buffer to be unpacked.

   "pointer tagging": 
  When unpacking, UnpackClosure() we add the tag to its return value,
  but enqueue the closure address WITHOUT A TAG, so we can access the
  unpacked closure directly by the enqueued pointer. 
  The closure WITH TAG is saved as offset value in the offset hash
  table (key=offset, value=address WITH TAG), to be filled in other
  closures as a pointer field.
  When packing, we did the reverse: saved the closure address WITH TAG
  in the queue, but stored it WITHOUT TAG in the offset table (as a
  key, value was the offset).
*/

static  StgClosure*
UnpackClosure (StgWord **bufptrP, Capability* cap) {
  StgClosure *closure;
  nat size,ptrs,nonptrs,vhs,i;
  StgInfoTable *ip;

  // remove and store the tag added to the info pointer,
  // before doing anything else!
  StgWord tag=0;

  /* Now unpack the closure body, if there is one; three cases:
     - PLC: closure is just a pointer to a static closure
     - Offset: closure has been unpacked already
     - else: copy data from packet into closure
  */
  switch( (StgWord) **bufptrP) {
    // these two cases respect the "pointer tags" by
    // design: either the tag was not removed at all (PLC case), or
    // the offset refers to an already unpacked (=> tagged) closure.
  case PLC:  
    closure = UnpackPLC(bufptrP); 
    break;
  case OFFSET:  
    closure = UnpackOffset(bufptrP); 
    break;
  case CLOSURE:

    (*bufptrP)++; // skip marker

    /* remove and store the tag added to the info pointer.
       (*bufptrP) points to a packed closure, first word has been
       tagged before packing, we read and remove the tag before
       doing anything!
    */
    tag = GET_CLOSURE_TAG((StgClosure*) **bufptrP);
    **bufptrP = UNTAG_CAST(StgWord, **bufptrP);
    IF_PAR_DEBUG(packet,
		 debugBelch("pointer tagging: removed tag %d "
			    "from info pointer %p in packet\n",
			    (int) tag, (void*) **bufptrP));

    /* *************************************************************
       The essential part: Here, we allocate heap space, fill in the
       closure and queue it to get the pointer payload filled
       later. Formerly get_req_heap_space + FillInClosure +
       QueueClosure, with an additional size check, now merged in
       order to be more flexible with the order of actions.
    */
    ASSERT(LOOKS_LIKE_INFO_PTR((StgWord)
			       ((StgClosure*)*bufptrP)->header.info));

    /*
     * Close your eyes.  You don't want to see where we're
     * looking. You can't get closure info until you've unpacked the
     * variable header, but you don't know how big it is until you've
     * got closure info.  So...we trust that the closure in the buffer
     * is organized the same way as they will be in the heap...at
     * least up through the end of the variable header.
     */
    ip = get_closure_info((StgClosure *) *bufptrP, 
			  &size, &ptrs, &nonptrs, &vhs);
  
    switch (ip->type) {
      // branch into new special routines: 
    case PAP:
    case AP:
      closure = UnpackPAP(ip, bufptrP, cap); 
      /* see below, might create/enQ some INDs */
      break;

    case ARR_WORDS:
    case MUT_ARR_PTRS_CLEAN:
    case MUT_ARR_PTRS_DIRTY:
    case MUT_ARR_PTRS_FROZEN0:
    case MUT_ARR_PTRS_FROZEN:
      // for MUT_ARR*, enqueues the closure, needs to alloc. card
      // table space after data space.
      closure = UnpackArray(ip, bufptrP, cap);
      break;

      /* other special cases will go here... */
    case AP_STACK:
      barf("Unpack: found AP_STACK");
 
    default:
      /* the pointers-first layout */

      IF_PAR_DEBUG(packet,
		   debugBelch("Allocating %d heap words for %s-closure:\n"
			      "(%d ptrs, %d non-ptrs, vhs = %d)\n"
			      , size, info_type_by_ip(ip),
			      ptrs, nonptrs, vhs));

      closure = (StgClosure*) allocate(cap, size);
      
      /* 
	 Remember, the generic closure layout is as follows:
	 +-------------------------------------------------+
	 | FIXED HEADER | VARIABLE HEADER | PTRS | NON-PRS |
	 +-------------------------------------------------+
      */
      /* Fill in the fixed header */
      for (i = 0; i < HEADERSIZE; i++)
	((StgPtr)closure)[i] = *(*bufptrP)++;
      
      /* Fill in the packed variable header */
      for (i = 0; i < vhs; i++)
	((StgPtr)closure)[HEADERSIZE + i] = *(*bufptrP)++;
      
      /* Pointers will be filled in later, but set zero here to
	 easily check if there is a temporary BH.
      */
      for (i = 0; i < ptrs; i++) 
	((StgPtr)closure)[HEADERSIZE + vhs + i] = 0;
      
      /* Fill in the packed non-pointers */
      for (i = 0; i < nonptrs; i++)
	((StgPtr)closure)[HEADERSIZE + i + vhs + ptrs] 
	  =  *(*bufptrP)++;
      
      ASSERT(HEADERSIZE+vhs+ptrs+nonptrs == size);
      
      QueueClosure(closure);
    } // switch(ip->type)


    break;
  default: 
    /* if not PLC or Offset it !must! be a (GA and then the) closure */
    barf("unpackClosure: Found invalid type marker %d.\n", (long) **bufptrP);
  }

  return TAG_CLOSURE(tag,closure);
}

static StgClosure * 
UnpackPAP(StgInfoTable *info, StgWord **bufptrP, Capability* cap) {
  StgPAP *pap;
  nat args, size, hsize, i;

#if defined(DEBUG)
  // for "manual sanity check" with debug flag "packet" only
  StgWord bitmap = 0;
#endif

  /* Unpacking a PAP/AP
   *
   * in the buffer:
   * +----------------------------------------------------------------+
   * | Header | (arity , n_args) | Tag | Arg/Tag | Tag | Arg/Tag | ...|
   * +----------------------------------------------------------------+
   *
   * Header size is 1 (normal) for PAP or 2 StgWords (Thunk Header) for AP.
   * Tag is PLC or CLOSURE constant, repeated if it is CLOSURE,
   * followed by the arg otherwise. Unpacking creates indirections and
   * inserts references to them when tag CLOSURE is found, otherwise
   * just unpacks the arg.
   *
   * should give in the heap:
   * +---------------------------------------------------------------+
   * | Header | (arity , n_args) | Fct. | Arg/&Ind1 | Arg/&Ind2 | ...|
   * +---------------------------------------------------------------+
   * followed by <= n_args indirections pointed at from the stack
   */ 
  
  /* calc./alloc. needed closure space in the heap. 
   * We are using the common macros provided.
   */
  switch (info->type) {
  case PAP: 
    hsize = HEADERSIZE + 1;
    args  = ((StgPAP*) *bufptrP)->n_args;
    size  = PAP_sizeW(args);
    break;
  case AP:
    hsize = sizeofW(StgThunkHeader) + 1;
    args  = ((StgAP*)  *bufptrP)->n_args;
    size  = AP_sizeW(args);
    break;
    /*
  case AP_STACK:
    hsize = sizeofW(StgThunkHeader)+1;
    args  = ((StgAP_STACK*) *bufptrP)->size;
    size  = AP_STACK_sizeW(args);
    break;
    */
  default: 
    barf("UnpackPAP: strange info pointer, type %d ", info->type);
  }
  IF_PAR_DEBUG(packet,
	       debugBelch("allocating %d heap words for a PAP(%d args)\n",
			  size, args));
  pap = (StgPAP *) allocate(cap, size);
  
  /* fill in sort-of-header fields (real header and (arity,n_args)) */
  for(i=0; i < hsize; i++) {
    ((StgPtr) pap)[i] = (StgWord) *(*bufptrP)++;
  }
  // enqueue to get function field filled (see get_closure_info)
  QueueClosure((StgClosure*) pap);

  // zero out the function field (for BH check in UnpackGraph) 
  pap->fun = (StgClosure*) 0;

  // unpack the stack (size == args), starting at pap[hsize]
  // make room for fct. pointer, thus start at hsize+1
  for(i=hsize+1; i < size; i++) {
    StgClosure* ind;
    switch((long) **bufptrP) {
    case PLC:
      // skip marker, unpack data into stack
      (*bufptrP)++;
      ((StgPtr) pap)[i] = (StgWord) *(*bufptrP)++;
      IF_PAR_DEBUG(packet, bitmap |= 1); // set bit in bitmap
      break;
    case CLOSURE:
      // skip 2 markers, create/enqueue indirection, put it on the stack
      (*bufptrP)+=2;
      ind = (StgClosure*) allocate(cap, sizeofW(StgInd));
      SET_HDR(ind, &stg_IND_info, CCS_SYSTEM); // ccs to be checked...
      ((StgInd*)ind)->indirectee = 0; // for BH-check in UnpackGraph
      ((StgPtr) pap)[i] = (StgWord) ind;
      QueueClosure(ind);
      break;
    default:
      barf("UnpackPAP: strange tag %d, should be %d or %d.", 
	   **bufptrP, PLC, CLOSURE);
    }
    IF_PAR_DEBUG(packet, bitmap = bitmap << 1); // shift to next bit
  }

  IF_PAR_DEBUG(packet,
	       debugBelch("unpacked a %s @ address %p, "
			  "%d args, constructed bitmap %#o.\n",
			  info_type((StgClosure*) pap),pap,
			  args, (int) bitmap));

  return (StgClosure*) pap;
}

static StgClosure* 
UnpackArray(StgInfoTable* info, StgWord **bufptrP, Capability* cap) {
  nat size;
  StgMutArrPtrs *array; // can also be StgArrWords, but fields not
			// used in this case.
  /*
   * We have to distinguish pointer arrays from word arrays. In the
   * case of pointers, we enqueue the unpacked closure for filling in
   * pointers, otherwise we just unpack the words (doing a memcpy).
   * 
   * Since GHC-6.13, ptr arrays additionally carry a "card table" for
   * generational GC (to indicate mutable/dirty elements). For
   * unpacking, allocate the card table and fill it with zero.
   *
   * With this change, probably split into two methods??
   */

  switch(info->type) {
  case ARR_WORDS:
  /*     Array layout in heap/buffer is the following:
   * +-----------------------------------------------------------+
   * | Header |size(in bytes)| word1 | word2 | ... | word_(size) |
   * +-----------------------------------------------------------+
   *
   * additional size to allocate is the 2nd word in buffer (in bytes)
   * but we read it using the selector function in ClosureMacros.h
   */
    size = sizeofW(StgArrWords) + arr_words_words((StgArrWords*) *bufptrP);
    IF_PAR_DEBUG(packet,
		 debugBelch("Unpacking word array, size %d\n", size));
    array = (StgMutArrPtrs *) allocate(cap, size);

    /* copy header and payload words in one go */
    memcpy(array, *bufptrP, size*sizeof(StgWord));
    *bufptrP += size;
    /*    for(i = 0; i < size; i++)
	  ((StgPtr) array)[i] = (StgWord) *(*bufptrP)++; */
    break;

  case MUT_ARR_PTRS_CLEAN:
  case MUT_ARR_PTRS_DIRTY:
  case MUT_ARR_PTRS_FROZEN0:
  case MUT_ARR_PTRS_FROZEN:
  /* Array layout in buffer:
   * +----------------------+......................................+
   * | Header | ptrs | size | ptr1 | ptr2 | .. | ptrN | card space |
   * +----------------------+......................................+
   *                         (added in heap when unpacking)
   * ptrs indicates how many pointers to come (N). Size field gives
   * total size for pointers and card table behind (to add).
   */
    // size = sizeofW(StgMutArrPtrs) + (StgWord) *((*bufptrP)+2);
    size = closure_sizeW((StgClosure*) *bufptrP);
    ASSERT(size ==
	   sizeofW(StgMutArrPtrs) + ((StgMutArrPtrs*) *bufptrP)->size);
    IF_PAR_DEBUG(packet,
		 debugBelch("Unpacking ptrs array, %d ptrs, size %d\n",
			    (StgWord) *((*bufptrP)+1), size));
    array = (StgMutArrPtrs *) allocate(cap, size);

    // set area 0 (Blackhole-test in unpacking and card table)
    memset(array, 0, size*sizeof(StgWord));
    /* write header and enqueue it, pointers will be filled in */
    for (size=0;size<(sizeof(StgMutArrPtrs)/sizeof(StgWord)); size++)
      ((StgPtr) array)[size] = (StgWord) *(*bufptrP)++;
    QueueClosure((StgClosure*)array);
    break;

  default:
    barf("UnpackArray: unexpected closure type %d", info->type);
  }

  IF_PAR_DEBUG(packet,
	       debugBelch(" Array created @ %p.\n",array));

  return (StgClosure*) array;
}

//@cindex UnpackPLC
STATIC_INLINE  StgClosure *
UnpackPLC(StgWord **bufptrP)
{
  StgClosure* plc;

  ASSERT((long) **bufptrP == PLC);

  (*bufptrP)++; // skip marker
  // Not much to unpack; just a static local address
  plc = (StgClosure*) **bufptrP;
  (*bufptrP)++; // skip address
  IF_PAR_DEBUG(packet,
	       debugBelch("*<^^ Unpacked PLC at %p\n", plc)); 
  return plc;
}

//@cindex UnpackOffset
STATIC_INLINE  StgClosure *
UnpackOffset(StgWord **bufptrP)
{
  StgClosure* existing;
  int offset;

  ASSERT((long) **bufptrP == OFFSET);

  (*bufptrP)++; // skip marker
  // unpack nat; find closure for this offset
  offset = (nat) **bufptrP;
  (*bufptrP)++; // skip offset

  // find this closure in an offset hashtable (we can have several packets)
  existing = (StgClosure *) lookupHashTable(offsetTable, offset);

  IF_PAR_DEBUG(packet,
	       debugBelch("*<__ Unpacked indirection to closure %p (was OFFSET %d, current padding %d)",
			  existing, offset, offsetpadding));

  // we should have found something...
  ASSERT(existing);

  return existing;
}


/* functions to save and restore the unpacking state from a 
 * saved format (including queue and offset table).
 * Format is defined as type "UnpackInfo".

static
StgClosure* restoreUnpackState(UnpackInfo* unpack,StgClosure** graphroot, 
		nat* pptr, nat* pptrs, nat* pvhs) {
  nat size, i;
  StgClosure* parent;

  IF_PAR_DEBUG(pack,
	       debugBelch("restore unpack state"));
  ASSERT(unpack != NULL);

  parent = unpack->parent;
  *pptr = unpack->pptr;
  *pptrs = unpack->pptrs;
  *pvhs = unpack->pvhs;
  *graphroot = unpack->graphroot;

  size = unpack->queue_length;
  for (i = 0; i < size; i++) // if no queue (size == 0): no action.
    QueueClosure(*(unpack->queue + i));

  // if we restore an unpack state, we use an existing hashtable:
  freeHashTable(offsetTable, NULL);
  offsetTable = (HashTable *) unpack->offsets;
  offsetpadding = unpack->offsetpadding;

  // free allocated memory:
  stgFree(unpack->queue);
  stgFree(unpack);

  IF_PAR_DEBUG(pack,
	       debugBelch("unpack state restored (graphroot: %p, current "
			  "parent: %p (ptr %d of %d, vhs= %d, offset %d).",
		     *graphroot, parent, *pptr, *pptrs, *pvhs, offsetpadding));
  return parent;
}

static
StgClosure** saveQueue(nat* size) {
  StgClosure** queue;

  // closures are saved as StgPtr in the queue...
  ASSERT(clq_pos <= clq_size); // we want to have a positive size
  *size = clq_size - clq_pos;

  if (*size == 0) return NULL; // no queue to save

  // queue to save: 
  IF_PAR_DEBUG(packet, 
	       debugBelch("saveQueue: saving ");
	       PrintClosureQueue());
  queue = (StgClosure **) stgMallocBytes(*size * sizeof(StgClosure*), 
					 "saveQueue: Queue");
  memcpy(queue, ClosureQueue+clq_pos, *size * sizeof(StgClosure*));
  IF_PAR_DEBUG(packet,
	       { nat j;
	         debugBelch("saveQueue: saved this queue:\n");
		 for (j = 0; j < *size; j++)
		   debugBelch("\tClosure %d: %p\n",*size - j,queue[j]);
	       });
  return queue;
}

static
UnpackInfo* saveUnpackState(StgClosure* graphroot, StgClosure* parent, 
			    nat pptr, nat pptrs, nat pvhs) {
  UnpackInfo* save;
  nat size;

  save = stgMallocBytes(sizeof(UnpackInfo),"saveUnpackState: UnpackInfo");
  IF_PAR_DEBUG(pack,
	       debugBelch("saving current unpack state at %p",save);
	       debugBelch("graphroot: %p, current parent: %p (ptr %d of %d, vhs= %d)",
		     graphroot, parent, pptr, pptrs, pvhs));
  // simple tasks: save numbers
  save->parent = parent;
  save->pptr = pptr;
  save->pptrs = pptrs;
  save->pvhs = pvhs;
  save->graphroot = graphroot;

  // complicated tasks: save queue and offset table
  save->queue = saveQueue(&size);
  save->queue_length = size;
  save->offsetpadding = offsetpadding; // padding for keys in offsetTable
  save->offsets = offsetTable;         // hashtable remains allocated

  IF_PAR_DEBUG(pack,
	       debugBelch("unpack state saved (offsetpadding %d in "
			  "hashtable at %p, %d closures in queue at %p).",
			  save->offsetpadding, save->offsets, 
			  save->queue_length, save->queue));

  return save;
}
 */

/* Experimental feature: serialisation into a Haskell Byte Array and
 * respective deserialisation. 
 *
 */

// pack, then copy the buffer into newly (Haskell-)allocated space
// (unless packing was blocked, in which case we return NULL 
StgClosure* PackToMemory(StgClosure* graphroot, StgTSO* tso,
			 Capability* cap) {
  rtsPackBuffer* buffer;
  StgArrWords* wordArray;

  buffer = PackNearbyGraph(graphroot, tso, 0);

  if (buffer == NULL) {
    // packing hit a black hole, return NULL to caller.
    return NULL;
  }

  /* allocate space to hold an array (size is buffer size * wordsize)
     +---------+----------+------------------------+
     |ARR_WORDS| n_bytes  | data (array of words)  |
     +---------+----------+------------------------+
   */
  wordArray = (StgArrWords*) allocate(cap, 2 + buffer->size);
  SET_HDR(wordArray, &stg_ARR_WORDS_info, CCS_SYSTEM); // ccs to be checked!
  wordArray->bytes = sizeof(StgWord) * (buffer->size);
  memcpy((void*) &(wordArray->payload),
	 (void*) (buffer->buffer),
	 (buffer->size)*sizeof(StgWord));

  return ((StgClosure*) wordArray);
}

// unpacking from a Haskell array (note we are using the pack buffer)
StgClosure* UnpackGraphWrapper(StgArrWords* packBufferArray,
			       Capability* cap) {
  Port noPort = (Port) {0,0,0};
  StgClosure* newGraph;

  InitPackBuffer(); // allocate, if not done yet

  // set the fields straight...
  globalPackBuffer->sender = globalPackBuffer->receiver = noPort;
  globalPackBuffer->id = -1;
  globalPackBuffer->size = packBufferArray->bytes / sizeof(StgWord);
  globalPackBuffer->unpacked_size = -1;
  // copy the data into the buffer
  memcpy((void*) &(globalPackBuffer->buffer),
	 (void*) packBufferArray->payload,
	 packBufferArray->bytes);
  // unpack
  newGraph = UnpackGraph(globalPackBuffer, noPort, cap);
  return newGraph;
}




// debugging functions
#if defined(DEBUG)

/*
  Generate a finger-print for a graph.  A finger-print is a string,
  with each char representing one node; depth-first traversal.
*/

/* this array has to be kept in sync with includes/ClosureTypes.h */
static char* fingerPrintChar = 
  "0ccccccccfffffffttttttt" /* INVALID CONSTRs FUNs THUNKs */
  "TBAPP___"     /* SELECTOR BCO AP PAP AP_STACK INDs */
  "RRRRRFFFF*@MM"/* RETs FRAMEs BQ BLACKHOLE MVARs */
  "aAAAAmmwppXS" /* ARRAYs MUT_VARs WEAK PRIM MUT_PRIM TSO STACK */
  "&FFFW"        /* TREC (STM-)FRAMEs WHITEHOLE*/
  ;

// recursive worker function:
static void GraphFingerPrint_(StgClosure *p);

static void GraphFingerPrint(StgClosure *p) {

  ASSERT(tmpClosureTable==NULL);

  // delete old fingerprint:
  fingerPrintStr[0]='\0';

  /* init hash table */
  tmpClosureTable = allocHashTable();

  /* now do the real work */
  GraphFingerPrint_(p);

  /* nuke hash table */
  freeHashTable(tmpClosureTable, NULL);
  tmpClosureTable = NULL;

  ASSERT(strlen(fingerPrintStr)<=MAX_FINGER_PRINT_LEN);
}

/*
  This is the actual worker functions. 
  All recursive calls should be made to this function.
*/
static void GraphFingerPrint_(StgClosure *p) {
  nat i, len, args, arity;
  const StgInfoTable *info;
  StgWord *payload;

  // first remove potential pointer tags
  p = UNTAG_CLOSURE(p);

  len = strlen(fingerPrintStr);
  ASSERT(len<=MAX_FINGER_PRINT_LEN);
  /* at most 7 chars added immediately (unchecked) for this node */
  if (len+7>=MAX_FINGER_PRINT_LEN)
    return;

  /* check whether we have met this node already to break cycles */
  if (lookupHashTable(tmpClosureTable, (StgWord)p)) { // ie. already touched
    strcat(fingerPrintStr, ".");
    return; 
  }

  /* record that we are processing this closure */
  insertHashTable(tmpClosureTable, (StgWord) p, (void *)rtsTrue/*non-NULL*/);

  ASSERT(LOOKS_LIKE_CLOSURE_PTR(p));

  info = get_itbl((StgClosure *)p);

  // append char for this node
  fingerPrintStr[len] = fingerPrintChar[info->type]; 
  fingerPrintStr[len+1] = '\0'; 
  /* the rest of this fct recursively traverses the graph */
  switch (info -> type) {

    // simple and static objects
  case CONSTR_STATIC:
  case CONSTR_NOCAF_STATIC:
  case FUN_STATIC:
  case THUNK_STATIC:
    break;
  
    /* CONSTRs, THUNKs, FUNs are written with arity */
  case THUNK_2_0:
    // append char for this node
    strcat(fingerPrintStr, "20(");
    // special treatment for thunks... extra smp header field
    GraphFingerPrint_(((StgThunk *)p)->payload[0]);
    GraphFingerPrint_(((StgThunk *)p)->payload[1]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  case FUN_2_0:
  case CONSTR_2_0:
    // append char for this node
    strcat(fingerPrintStr, "20(");
    GraphFingerPrint_(((StgClosure *)p)->payload[0]);
    GraphFingerPrint_(((StgClosure *)p)->payload[1]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  
  case THUNK_1_0:
    // append char for this node
    strcat(fingerPrintStr, "10(");
    GraphFingerPrint_(((StgThunk *)p)->payload[0]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  
  case FUN_1_0:
  case CONSTR_1_0:
    // append char for this node
    strcat(fingerPrintStr, "10(");
    GraphFingerPrint_(((StgClosure *)p)->payload[0]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  
  case THUNK_0_1:
  case FUN_0_1:
  case CONSTR_0_1:
    // append char for this node
    strcat(fingerPrintStr, "01");
    break;
  
  case THUNK_0_2:
  case FUN_0_2:
  case CONSTR_0_2:
    // append char for this node
    strcat(fingerPrintStr, "02");
    break;
  
  case THUNK_1_1:
    // append char for this node
    strcat(fingerPrintStr, "11(");
    GraphFingerPrint_(((StgThunk *)p)->payload[0]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  case FUN_1_1:
  case CONSTR_1_1:
    // append char for this node
    strcat(fingerPrintStr, "11(");
    GraphFingerPrint_(((StgClosure *)p)->payload[0]);
    if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
      strcat(fingerPrintStr, ")");
    break;
  
  case THUNK:
    {
	char str[6];
	sprintf(str,"%d?(",info->layout.payload.ptrs);
	strcat(fingerPrintStr,str); 
	for (i=0; i<info->layout.payload.ptrs; i++)
	  GraphFingerPrint_(((StgThunk *)p)->payload[i]);
	if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
	  strcat(fingerPrintStr, ")");
    }
    break;
  case FUN:
  case CONSTR:
    {
	char str[6];
	sprintf(str,"%d?(",info->layout.payload.ptrs);
	strcat(fingerPrintStr,str); 
	for (i=0; i<info->layout.payload.ptrs; i++)
	  GraphFingerPrint_(((StgClosure *)p)->payload[i]);
	if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
	  strcat(fingerPrintStr, ")");
    }
    break;
  
  case THUNK_SELECTOR:
      GraphFingerPrint_(((StgSelector *)p)->selectee);
      break;
  
  case BCO:
    break;

  case AP_STACK:
    break;
    /*
    arity = ((StgAP_STACK*)p)->size;
    args  = arity;
    payload = (StgPtr) ((StgAP_STACK*)p)->payload;
    p = ((StgAP_STACK*)p)->fun;
    goto print;
    */
  case AP:
    arity = ((StgAP*)p)->arity;
    args  = ((StgAP*)p)->n_args;
    payload = (StgPtr)((StgAP*)p)->payload;
    p = ((StgAP*)p)->fun;
    goto print;
  case PAP:
    /* note the arity (total #args) and n_args (how many supplied) */
    arity = ((StgPAP*)p)->arity;
    args  = ((StgPAP*)p)->n_args;
    payload = (StgPtr) ((StgPAP*)p)->payload;
    p = ((StgPAP*)p)->fun;
  print:
    { char str[6];
      sprintf(str,"%d/%d(",arity,args);
      strcat(fingerPrintStr,str); 
    // follow the function, and everything on the stack
      GraphFingerPrint_((StgClosure *) (p));
      if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN) {
	StgWord bitmap;
	StgFunInfoTable *funInfo = get_fun_itbl(UNTAG_CLOSURE(p));
	strcat(fingerPrintStr, "|");
	switch(funInfo->f.fun_type) {
	  /* these two use a large bitmap. We do not follow...*/
	case ARG_GEN_BIG:
	case ARG_BCO:
	  break;
	case ARG_GEN:
	  bitmap = funInfo->f.b.bitmap;
	  break;
	default:
	  bitmap = stg_arg_bitmaps[funInfo->f.fun_type];
	}
	// size = BITMAP_SIZE(bitmap);
	bitmap = BITMAP_BITS(bitmap);
	while (args > 0) {
	  if ((bitmap & 1) == 0) 
	    GraphFingerPrint_((StgClosure *)(*payload));
	  payload++;
	  if (strlen(fingerPrintStr)+2<MAX_FINGER_PRINT_LEN)
	    strcat(fingerPrintStr, ")");
	  args--;
	  bitmap = bitmap>>1;
	}
      }
    }
    break;

  case IND:
  case IND_PERM:
  case IND_STATIC:
    /* do not print the '_' for indirections */
    fingerPrintStr[len] = '\0';
    /* could also be different type StgIndStatic */
    GraphFingerPrint_(((StgInd*)p)->indirectee);
    break;

  case RET_BCO:
  case RET_SMALL:
  case RET_BIG:
  case RET_DYN:
  case RET_FUN:
  case UPDATE_FRAME:
  case CATCH_FRAME:
  case UNDERFLOW_FRAME:
  case STOP_FRAME:
  case BLOCKING_QUEUE:
  case BLACKHOLE:
    // check if this is actually an indirection. See above in packing code
    // some Blackholes are actually indirections since ghc-7.0
    switch (((StgInfoTable*)get_itbl(UNTAG_CLOSURE(((StgInd*)p)->indirectee)))->type) {
    case TSO:
    case BLOCKING_QUEUE:
      debugBelch("Woopsie! Found blackhole while doing fingerprint!\n");
      break;
    default:
      /* do not print the '_' for indirections */
      fingerPrintStr[len] = '\0';
      GraphFingerPrint_(((StgInd*)p)->indirectee);
      break;
    }
    break;
  
  case MVAR_CLEAN:
  case MVAR_DIRTY:
    if (((StgMVar *)p)->value != &stg_END_TSO_QUEUE_closure)
      GraphFingerPrint_(((StgMVar *)p)->value);
    break;

  case ARR_WORDS:
    { // record size only (contains StgWords, not pointers)
	char str[6];
	sprintf(str,"%ld",(long) arr_words_words((StgArrWords*)p));
	strcat(fingerPrintStr,str); 
    }
    break;
  case MUT_ARR_PTRS_CLEAN:
  case MUT_ARR_PTRS_DIRTY:
  case MUT_ARR_PTRS_FROZEN0:
  case MUT_ARR_PTRS_FROZEN:
    {
	char str[6];
	sprintf(str,"%ld",(long)((StgMutArrPtrs*)p)->ptrs);
	strcat(fingerPrintStr,str); 
  	nat i;
  	for (i = 0; i < ((StgMutArrPtrs*)p)->ptrs; i++) {
	  //contains closures... follow
	  GraphFingerPrint_(((StgMutArrPtrs*)p)->payload[i]);
  	}
  	break;
    }
  case MUT_VAR_CLEAN:
  case MUT_VAR_DIRTY:
    GraphFingerPrint_(((StgMutVar *)p)->var);
    break;
  
  case WEAK:
  case PRIM:
    break;
  case MUT_PRIM:
    break;
  case TSO:
    break;
  case STACK:
    break;

  case TREC_CHUNK:
  case ATOMICALLY_FRAME:
  case CATCH_RETRY_FRAME:
  case CATCH_STM_FRAME:
  case WHITEHOLE:
    break;

  default:
    barf("GraphFingerPrint_: unknown closure %d",
	 info -> type); // , info_type((StgClosure*) p)); // info_type_by_ip(info));
  }
 
}    

/* END OF DEBUG */
#endif 

#endif /* PARALLEL_HASKELL */

