/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2009
 *
 * RTS-specific types.
 *
 * Do not #include this file directly: #include "Rts.h" instead.
 *
 * To understand the structure of the RTS headers, see the wiki:
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/SourceTree/Includes
 *
 * ---------------------------------------------------------------------------*/

#ifndef RTS_TYPES_H
#define RTS_TYPES_H

typedef unsigned int  nat;           /* at least 32 bits (like int) */
typedef unsigned long lnat;          /* at least 32 bits            */

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
   Types specific to the parallel runtime system.
*/

/* 
   Types specific to the PARALLEL_RTS runtime system.
   but types are defined in the sequential base system as well
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

#endif /* RTS_TYPES_H */
