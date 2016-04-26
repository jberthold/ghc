/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2009
 *
 * RTS-specific types.
 *
 * Do not #include this file directly: #include "Rts.h" instead.
 *
 * To understand the structure of the RTS headers, see the wiki:
 *   http://ghc.haskell.org/trac/ghc/wiki/Commentary/SourceTree/Includes
 *
 * ---------------------------------------------------------------------------*/

#ifndef RTS_TYPES_H
#define RTS_TYPES_H

#include <stddef.h>

typedef unsigned int     nat;           /* at least 32 bits (like int) */

// Deprecated; just use StgWord instead
typedef StgWord lnat;

/* ullong (64|128-bit) type: only include if needed (not ANSI) */
#if defined(__GNUC__) 
#define LL(x) (x##LL)
#else
#define LL(x) (x##L)
#endif
  
typedef enum { 
    rtsFalse = 0, 
    rtsTrue 
} rtsBool;

typedef struct StgClosure_   StgClosure;
typedef struct StgInfoTable_ StgInfoTable;
typedef struct StgTSO_       StgTSO;

/* 
   Types specific to the parallel runtime system,
   but defined in the sequential base system as well.
*/

// aliases
typedef int           OpCode;

// a port type, stands for an inport (pe, proc,inport->id), an outport
// (pe,proc,tso->id) and processes (pe, proc, NULL)
typedef struct Port_ {
  nat machine;
  StgWord process;
  StgWord id;
} Port;
typedef Port Proc;

// Pack Buffer for constructing messages between PEs
typedef struct rtsPackBuffer_ {
    // Eden channel communication
    Port                 sender;
    Port                 receiver;
    // for data messages only,
    StgInt /* nat */     id;            // currently unused
    StgInt /* nat */     size;          // payload size in units of StgWord
    StgInt /* nat */     unpacked_size; // currently unused
    StgWord              buffer[];      // payload
} rtsPackBuffer;

#endif /* RTS_TYPES_H */
