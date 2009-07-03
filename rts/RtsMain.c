/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team 1998-2000
 *
 * Main function for a standalone Haskell program.
 *
 * ---------------------------------------------------------------------------*/

#define COMPILING_RTS_MAIN

#include "PosixSource.h"
#include "Rts.h"
#include "RtsAPI.h"
#include "SchedAPI.h"
#include "RtsFlags.h"
#include "RtsUtils.h"
#include "RtsMain.h"
#include "Prelude.h"
#include "Task.h"
#if defined(mingw32_HOST_OS)
#include "win32/seh_excn.h"
#endif
#include <stdlib.h>

#ifdef DEBUG
# include "Printer.h"   /* for printing        */
#endif

#ifdef HAVE_WINDOWS_H
# include <windows.h>
#endif

extern void __stginit_ZCMain(void);

/* Annoying global vars for passing parameters to real_main() below
 * This is to get around problem with Windows SEH, see hs_main(). */
static int progargc;
static char **progargv;
static void (*progmain_init)(void);   /* This will be __stginit_ZCMain */
static StgClosure *progmain_closure;  /* This will be ZCMain_main_closure */

/* Hack: we assume that we're building a batch-mode system unless 
 * INTERPRETER is set
 */
#ifndef INTERPRETER /* Hack */
static void real_main(void)
{
    int exit_status;
    SchedulerStatus status;
    /* all GranSim/GUM init is done in startupHaskell; sets IAmMainThread! */

    startupHaskell(progargc,progargv,progmain_init);

    /* kick off the computation by creating the main thread with a pointer
       to mainIO_closure representing the computation of the overall program;
       then enter the scheduler with this thread and off we go;
      
       the same for GranSim (we have only one instance of this code)

       in a parallel setup, where we have many instances of this code
       running on different PEs, we should do this only for the main PE
       (IAmMainThread is set in startupHaskell) 
    */

    /* ToDo: want to start with a larger stack size */
    { 
	Capability *cap = rts_lock();
	cap = rts_evalLazyIO(cap,progmain_closure, NULL);
	status = rts_getSchedStatus(cap);
	taskTimeStamp(myTask());
	rts_unlock(cap);
    }

    /* check the status of the entire Haskell computation */
    switch (status) {
    case Killed:
      errorBelch("main thread exited (uncaught exception)");
      exit_status = EXIT_KILLED;
      break;
    case Interrupted:
      errorBelch("interrupted");
      exit_status = EXIT_INTERRUPTED;
      break;
    case HeapExhausted:
      exit_status = EXIT_HEAPOVERFLOW;
      break;
    case Success:
      exit_status = EXIT_SUCCESS;
      break;
    default:
      barf("main thread completed with invalid status");
    }
    shutdownHaskellAndExit(exit_status);
}

/* The rts entry point from a compiled program using a Haskell main function.
 * This gets called from a tiny main function which gets linked into each
 * compiled Haskell program that uses a Haskell main function.
 *
 * We expect the caller to pass __stginit_ZCMain for main_init and
 * ZCMain_main_closure for main_closure. The reason we cannot refer to
 * these symbols directly is because we're inside the rts and we do not know
 * for sure that we'll be using a Haskell main function.
 */
int hs_main(int argc, char *argv[], void (*main_init)(void), StgClosure *main_closure)
{
    /* We do this dance with argc and argv as otherwise the SEH exception
       stuff (the BEGIN/END CATCH below) on Windows gets confused */
    progargc = argc;
    progargv = argv;
    progmain_init    = main_init;
    progmain_closure = main_closure;

#if defined(mingw32_HOST_OS)
    BEGIN_CATCH
#endif
    real_main();
#if defined(mingw32_HOST_OS)
    END_CATCH
#endif
    return 0; /* not reached, but keeps gcc -Wall happy */
}
# endif /* BATCH_MODE */