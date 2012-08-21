/* 
 * Generalised RTE for parallel Haskells.
 *
 * File: ghc/rts/parallel/MPSystem.h
 *
 * Purpose: provide standard interface from RTE to MP-System
 *
 * Services by this interface:
 *  - manages addressing (real) machines from (abstract) node numbers
 *  - provides startup, synchronisation, shutdown for parallel system 
 *  - provides send / receive for communication between nodes
 */


#ifndef MPSYSTEM_H
#define MPSYSTEM_H

#ifdef PARALLEL_RTS /* whole file */

/*
 * By including "Rts.h" here, we can use types GlobalTaskId, rtsBool, etc.
 * Normally, Rts.h should be included before including this file "MPSystem.h", 
 * but it is save to use here (protected from double-inclusion) and
 * brings useful other stuff (e.g. stdlib). 
 */
#include "Rts.h"

/* global constants, declared in Parallel.h:
 *
 * nPEs   - nat: number of PEs in the parallel system
 * thisPE - nat: logical address of this PE btw. 1 and nPEs
 * IAmMainThread - rtsBool: indicating main PE (thisPE == 1)
 */


// taken from GUM: a logical PE no. must fit into 8 bit
#define MAX_PES  255

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
rtsBool MP_start(int* argc, char** argv);

/* MP_sync synchronises all nodes in a parallel computation:
 *  sets global var.: 
 *    thisPE - GlobalTaskId: node's own task Id 
 *             (logical node address for messages)
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_sync(void);

/* MP_quit disconnects current node from MP-System:
 * Parameters:
 *     IN isError - error number, 0 if normal exit
 * Returns: Bool: success (1) or failure (0)
 */
rtsBool MP_quit(int isError);


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

rtsBool MP_send(int node, OpCode tag, long *data, int length);

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
int MP_recv(int maxlength, long *destination, // IN
	     OpCode *code, nat *sender);       // OUT

/* - a non-blocking probe operation 
 * (unspecified sender, no receive buffers any more) 
 */
rtsBool MP_probe(void);

#endif /* PARALLEL_RTS */

#endif /* MPSYSTEM_H */
