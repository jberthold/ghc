/* -----------------------------------------------------------------------------
 *
 * (c) The AQUA Project, Glasgow University, 1994-1997
 * (c) The GHC Team, 1998-2006
 *
 * Functions for parsing the argument list.
 *
 * ---------------------------------------------------------------------------*/

#include "PosixSource.h"
#include "Rts.h"

#include "RtsOpts.h"
#include "RtsUtils.h"
#include "Profiling.h"
#include "RtsFlags.h"

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#include <string.h>

// Flag Structure
RTS_FLAGS RtsFlags;

/*
 * Split argument lists
 */
int     prog_argc = 0;    /* an "int" so as to match normal "argc" */
char  **prog_argv = NULL;
int     full_prog_argc = 0;    /* an "int" so as to match normal "argc" */
char  **full_prog_argv = NULL;
char   *prog_name = NULL; /* 'basename' of prog_argv[0] */
int     rts_argc = 0;  /* ditto */
char  **rts_argv = NULL;
#if defined(mingw32_HOST_OS)
// On Windows, we want to use GetCommandLineW rather than argc/argv,
// but we need to mutate the command line arguments for withProgName and
// friends. The System.Environment module achieves that using this bit of
// shared state:
int       win32_prog_argc = 0;
wchar_t **win32_prog_argv = NULL;
#endif

/*
 * constants, used later 
 */
#define RTS 1
#define PGM 0

#if defined(PARALLEL_RTS)

static void process_par_option(int arg, int *rts_argc, char *rts_argv[], rtsBool *error);

#if defined(DEBUG)
static char *par_debug_opts_strs[] = {
  "DEBUG (-qDv, -qD1): verbose; be generally verbose with parallel related stuff.\n",
  "DEBUG (-qDc, -qD2): mpcomm; low level messages.\n",
  "DEBUG (-qDp, -qD4): pack; packing code.\n",
  "DEBUG (-qDq, -qD8): packet; verbose packing code.\n",
  "DEBUG (-qDP, -qD16): processes; process management code.\n",
  "DEBUG (-qDo, -qD32): ports; port management code (more verbose).\n"
};
/* one character codes for the available debug options */
static char par_debug_opts_flags[] = {
  'v', 'c', 'p', 'q', 'P', 'o'
};

static void set_par_debug_options(nat n);
static void help_par_debug_options(nat n);

#endif
#endif

/* -----------------------------------------------------------------------------
   Static function decls
   -------------------------------------------------------------------------- */

static void procRtsOpts      (int rts_argc0, RtsOptsEnabledEnum enabled);

static void normaliseRtsOpts (void);

static void initStatsFile    (FILE *f);

static int  openStatsFile    (char *filename, const char *FILENAME_FMT,
                              FILE **file_ret);

static StgWord64 decodeSize  (const char *flag, nat offset,
                              StgWord64 min, StgWord64 max);

static void bad_option       (const char *s);

#ifdef TRACING
static void read_trace_flags(char *arg);
#endif

static void errorUsage      (void) GNU_ATTRIBUTE(__noreturn__);

static char *  copyArg  (char *arg);
static char ** copyArgv (int argc, char *argv[]);
static void    freeArgv (int argc, char *argv[]);

/* -----------------------------------------------------------------------------
 * Command-line option parsing routines.
 * ---------------------------------------------------------------------------*/

void initRtsFlagsDefaults(void)
{
    RtsFlags.GcFlags.statsFile		= NULL;
    RtsFlags.GcFlags.giveStats		= NO_GC_STATS;

    RtsFlags.GcFlags.maxStkSize		= (8 * 1024 * 1024) / sizeof(W_);
    RtsFlags.GcFlags.initialStkSize	= 1024 / sizeof(W_);
    RtsFlags.GcFlags.stkChunkSize       = (32 * 1024) / sizeof(W_);
    RtsFlags.GcFlags.stkChunkBufferSize = (1 * 1024) / sizeof(W_);

    RtsFlags.GcFlags.minAllocAreaSize   = (512 * 1024)        / BLOCK_SIZE;
    RtsFlags.GcFlags.minOldGenSize      = (1024 * 1024)       / BLOCK_SIZE;
    RtsFlags.GcFlags.maxHeapSize	= 0;    /* off by default */
    RtsFlags.GcFlags.heapSizeSuggestion	= 0;    /* none */
    RtsFlags.GcFlags.heapSizeSuggestionAuto = rtsFalse;
    RtsFlags.GcFlags.pcFreeHeap		= 3;	/* 3% */
    RtsFlags.GcFlags.oldGenFactor       = 2;
    RtsFlags.GcFlags.generations        = 2;
    RtsFlags.GcFlags.squeezeUpdFrames	= rtsTrue;
    RtsFlags.GcFlags.compact            = rtsFalse;
    RtsFlags.GcFlags.compactThreshold   = 30.0;
    RtsFlags.GcFlags.sweep              = rtsFalse;
#ifdef RTS_GTK_FRONTPANEL
    RtsFlags.GcFlags.frontpanel         = rtsFalse;
#endif
    RtsFlags.GcFlags.idleGCDelayTime    = 300; /* millisecs */

#if osf3_HOST_OS
/* ToDo: Perhaps by adjusting this value we can make linking without
 * -static work (i.e., not generate a core-dumping executable)? */
# if SIZEOF_VOID_P == 8
    RtsFlags.GcFlags.heapBase           = 0x180000000L;
# else
#  error I have no idea where to begin the heap on a non-64-bit osf3 machine.
# endif
#else
    RtsFlags.GcFlags.heapBase           = 0;   /* means don't care */
#endif

#ifdef DEBUG
    RtsFlags.DebugFlags.scheduler	= rtsFalse;
    RtsFlags.DebugFlags.interpreter	= rtsFalse;
    RtsFlags.DebugFlags.weak		= rtsFalse;
    RtsFlags.DebugFlags.gccafs		= rtsFalse;
    RtsFlags.DebugFlags.gc		= rtsFalse;
    RtsFlags.DebugFlags.block_alloc	= rtsFalse;
    RtsFlags.DebugFlags.sanity		= rtsFalse;
    RtsFlags.DebugFlags.stable		= rtsFalse;
    RtsFlags.DebugFlags.stm             = rtsFalse;
    RtsFlags.DebugFlags.prof		= rtsFalse;
    RtsFlags.DebugFlags.apply		= rtsFalse;
    RtsFlags.DebugFlags.linker		= rtsFalse;
    RtsFlags.DebugFlags.squeeze		= rtsFalse;
    RtsFlags.DebugFlags.hpc		= rtsFalse;
    RtsFlags.DebugFlags.sparks		= rtsFalse;
#endif

#if defined(PROFILING)
    RtsFlags.CcFlags.doCostCentres	= 0;
#endif /* PROFILING */

    RtsFlags.ProfFlags.doHeapProfile      = rtsFalse;
    RtsFlags.ProfFlags.profileInterval    = 100;

#ifdef PROFILING
    RtsFlags.ProfFlags.includeTSOs        = rtsFalse;
    RtsFlags.ProfFlags.showCCSOnException = rtsFalse;
    RtsFlags.ProfFlags.maxRetainerSetSize = 8;
    RtsFlags.ProfFlags.ccsLength          = 25;
    RtsFlags.ProfFlags.modSelector        = NULL;
    RtsFlags.ProfFlags.descrSelector      = NULL;
    RtsFlags.ProfFlags.typeSelector       = NULL;
    RtsFlags.ProfFlags.ccSelector         = NULL;
    RtsFlags.ProfFlags.ccsSelector        = NULL;
    RtsFlags.ProfFlags.retainerSelector   = NULL;
    RtsFlags.ProfFlags.bioSelector        = NULL;
#endif

#ifdef TRACING
    RtsFlags.TraceFlags.tracing       = TRACE_NONE;
    RtsFlags.TraceFlags.timestamp     = rtsFalse;
    RtsFlags.TraceFlags.scheduler     = rtsFalse;
#endif

    RtsFlags.MiscFlags.tickInterval	= 20;  /* In milliseconds */
    RtsFlags.ConcFlags.ctxtSwitchTime	= 20;  /* In milliseconds */

    RtsFlags.MiscFlags.install_signal_handlers = rtsTrue;
    RtsFlags.MiscFlags.machineReadable = rtsFalse;
    RtsFlags.MiscFlags.linkerMemBase    = 0;

#ifdef THREADED_RTS
    RtsFlags.ParFlags.nNodes	        = 1;
    RtsFlags.ParFlags.migrate           = rtsTrue;
    RtsFlags.ParFlags.parGcEnabled      = 1;
    RtsFlags.ParFlags.parGcGen          = 0;
    RtsFlags.ParFlags.parGcLoadBalancingEnabled = rtsTrue;
    RtsFlags.ParFlags.parGcLoadBalancingGen = 1;
    RtsFlags.ParFlags.setAffinity       = 0;
#endif

#ifdef PARALLEL_RTS
    // common parallelism options:
    RtsFlags.ParFlags.packBufferSize	= 10485760; // 10MB, we set it...;
    RtsFlags.ParFlags.sendBufferSize	= 20; // MD this default should be tested;
    RtsFlags.ParFlags.placement         = 0; /* default: RR placement,
						including local PE*/
#endif /* PARALLEL_RTS */

#if defined(THREADED_RTS)
    RtsFlags.ParFlags.maxLocalSparks	= 4096;
#endif /* THREADED_RTS */

#ifdef TICKY_TICKY
    RtsFlags.TickyFlags.showTickyStats	 = rtsFalse;
    RtsFlags.TickyFlags.tickyFile	 = NULL;
#endif

#ifdef USE_PAPI
    /* By default no special measurements taken */
    RtsFlags.PapiFlags.eventType        = 0;
    RtsFlags.PapiFlags.numUserEvents    = 0;
#endif
}

static const char *
usage_text[] = {
"",
"Usage: <prog> <args> [+RTS <rtsopts> | -RTS <args>] ... --RTS <args>",
"",
"   +RTS    Indicates run time system options follow",
"   -RTS    Indicates program arguments follow",
"  --RTS    Indicates that ALL subsequent arguments will be given to the",
"           program (including any of these RTS flags)",
"",
"The following run time system options are available:",
"",
"  -?       Prints this message and exits; the program is not executed",
"  --info   Print information about the RTS used by this program",
"",
"  -K<size> Sets the maximum stack size (default 8M)  Egs: -K32k   -K512k",
"  -ki<size> Sets the initial thread stack size (default 1k)  Egs: -ki4k -ki2m",
"  -kc<size> Sets the stack chunk size (default 32k)",
"  -kb<size> Sets the stack chunk buffer size (default 1k)",
"",
"  -A<size> Sets the minimum allocation area size (default 512k) Egs: -A1m -A10k",
"  -M<size> Sets the maximum heap size (default unlimited)  Egs: -M256k -M1G",
"  -H<size> Sets the minimum heap size (default 0M)   Egs: -H24m  -H1G",
"  -m<n>    Minimum % of heap which must be available (default 3%)",
"  -G<n>    Number of generations (default: 2)",
"  -c<n>    Use in-place compaction instead of copying in the oldest generation",
"           when live data is at least <n>% of the maximum heap size set with",
"           -M (default: 30%)",
"  -c       Use in-place compaction for all oldest generation collections",
"           (the default is to use copying)",
"  -w       Use mark-region for the oldest generation (experimental)",
#if defined(THREADED_RTS)
"  -I<sec>  Perform full GC after <sec> idle time (default: 0.3, 0 == off)",
#endif
"",
"  -t[<file>] One-line GC statistics (if <file> omitted, uses stderr)",
"  -s[<file>] Summary  GC statistics (if <file> omitted, uses stderr)",
"  -S[<file>] Detailed GC statistics (if <file> omitted, uses stderr)",
#ifdef RTS_GTK_FRONTPANEL
"  -f       Display front panel (requires X11 & GTK+)",
#endif
"",
"",
"  -Z       Don't squeeze out update frames on stack overflow",
"  -B       Sound the bell at the start of each garbage collection",
#if defined(PROFILING)
"",
"  -px      Time/allocation profile (XML)  (output file <program>.prof)",
"  -p       Time/allocation profile        (output file <program>.prof)",
"  -P       More detailed Time/Allocation profile",
"  -Pa      Give information about *all* cost centres",

# if defined(PROFILING)
"",
"  -hx            Heap residency profile (XML)   (output file <program>.prof)",
"  -h<break-down> Heap residency profile (hp2ps) (output file <program>.hp)",
"     break-down: c = cost centre stack (default)",
"                 m = module",
"                 d = closure description",
"                 y = type description",
"                 r = retainer",
"                 b = biography (LAG,DRAG,VOID,USE)",
"  A subset of closures may be selected thusly:",
"    -hc<cc>,...  specific cost centre(s) (top of stack only)",
"    -hC<cc>,...  specific cost centre(s) (anywhere in stack)",
"    -hm<mod>...  all cost centres from the specified modules(s)",
"    -hd<des>,... closures with specified closure descriptions",
"    -hy<typ>...  closures with specified type descriptions",
"    -hr<cc>...   closures with specified retainers",
"    -hb<bio>...  closures with specified biographies (lag,drag,void,use)",
"",
"  -R<size>       Set the maximum retainer set size (default: 8)",
"", 
"  -L<chars>      Maximum length of a cost-centre stack in a heap profile",
"                 (default: 25)",
"",
"  -xt            Include threads (TSOs) in a heap profile",
"",
"  -xc      Show current cost centre stack on raising an exception",
# endif
#endif /* PROFILING or PAR */

#ifdef TRACING
"",
"  -l[flags]  Log events in binary format to the file <program>.eventlog",
#  ifdef DEBUG
"  -v[flags]  Log events to stderr",
#  endif
"             where [flags] can contain:",
"                s    scheduler events",
#  ifdef DEBUG
"                t    add time stamps (only useful with -v)",
#  endif
#endif

#if !defined(PROFILING)
"",
"  -hT      Heap residency profile (output file <program>.hp)",
#endif
"  -i<sec>  Time between heap samples (seconds, default: 0.1)",
"",
#if defined(TICKY_TICKY)
"  -r<file>  Produce ticky-ticky statistics (with -rstderr for stderr)",
"",
#endif
#ifdef PARALLEL_RTS
"  -N<n>     Use <n> (virtual) processors in parallel "
// -qp<n>  is still understood, but not encouraged.
#if defined(USE_PVM)
"(PVM default: all available nodes)",
/* NB: the -N<n> is implemented by the driver script!! */
#elif defined(USE_MPI)
"(MPI default: 1)",
/* NB: the -N<n> is implemented by the driver script!! */
#else // normally, the default should be as in THREADED_RTS
"(default: 1)",
#endif
#endif
"  -C<secs>  Context-switch interval in seconds.",
"            0 or no argument means switch as often as possible.",
"            Default: 0.02 sec; resolution is set by -V below.",
"  -V<secs>  Master tick interval in seconds (0 == disable timer).",
"            This sets the resolution for -C and the profile timer -i.",
"            Default: 0.02 sec.",
"",
#if defined(DEBUG)
"  -Ds  DEBUG: scheduler",
"  -Di  DEBUG: interpreter",
"  -Dw  DEBUG: weak",
"  -DG  DEBUG: gccafs",
"  -Dg  DEBUG: gc",
"  -Db  DEBUG: block",
"  -DS  DEBUG: sanity",
"  -Dt  DEBUG: stable",
"  -Dp  DEBUG: prof",
"  -De  DEBUG: event logging",
"  -Da  DEBUG: apply",
"  -Dl  DEBUG: linker",
"  -Dm  DEBUG: stm",
"  -Dz  DEBUG: stack squeezing",
"  -Dc  DEBUG: program coverage",
"  -Dr  DEBUG: sparks",
"",
"     NOTE: DEBUG events are sent to stderr by default; add -l to create a",
"     binary event log file instead.",
# if defined(PARALLEL_RTS)
"",
"  -qD  DEBUG(parallel):",
# endif
"",
#endif /* DEBUG */
#if defined(THREADED_RTS) && !defined(NOSMP)
"  -N<n>     Use <n> processors (default: 1)",
"  -N        Determine the number of processors to use automatically",
"  -qg[<n>]  Use parallel GC only for generations >= <n>",
"            (default: 0, -qg alone turns off parallel GC)",
"  -qb[<n>]  Use load-balancing in the parallel GC only for generations >= <n>",
"            (default: 1, -qb alone turns off load-balancing)",
"  -qa       Use the OS to set thread affinity (experimental)",
"  -qm       Don't automatically migrate threads between CPUs",
#endif
"  --install-signal-handlers=<yes|no>",
"            Install signal handlers (default: yes)",
#if defined(PARALLEL_RTS)
"  -qQ<size> Set pack-buffer size (default: 10MB)",
"  -qq<n>    Set MPI-send-buffer size to <n> * pack-buffer (default: 20)",
"  -qremote  Avoid placing child processes on the same PE",
"  -qrnd     Enable random process placement (i.e. not round-robin)",
/*
"  -qP       Activate parallel profiling (Eden)",
"  -qPh      include GC statistics in trace file (implies -qP)",
"  -qPm      include message traffic in trace file (implies -qP)",
*/
#endif
#if defined(THREADED_RTS)
"  -e<n>     Maximum number of outstanding local sparks (default: 4096)",
#endif
#if defined(x86_64_HOST_ARCH)
"  -xm       Base address to mmap memory in the GHCi linker",
"            (hex; must be <80000000)",
#endif
#if defined(USE_PAPI)
"  -aX       CPU performance counter measurements using PAPI",
"            (use with the -s<file> option).  X is one of:",
"",
/* "            y - cycles", */
"            1 - level 1 cache misses",
"            2 - level 2 cache misses",
"            b - branch mispredictions",
"            s - stalled cycles",
"            e - cache miss and branch misprediction events",
"            +PAPI_EVENT   - collect papi preset event PAPI_EVENT",
"            #NATIVE_EVENT - collect native event NATIVE_EVENT (in hex)",
#endif
"",
"RTS options may also be specified using the GHCRTS environment variable.",
"",
"Other RTS options may be available for programs compiled a different way.",
"The GHC User's Guide has full details.",
"",
0
};

STATIC_INLINE rtsBool
strequal(const char *a, const char * b)
{
    return(strcmp(a, b) == 0);
}

static void splitRtsFlags(char *s)
{
    char *c1, *c2;

    c1 = s;
    do {
	while (isspace(*c1)) { c1++; };
	c2 = c1;
	while (!isspace(*c2) && *c2 != '\0') { c2++; };
	
	if (c1 == c2) { break; }
	
        s = stgMallocBytes(c2-c1+1, "RtsFlags.c:splitRtsFlags()");
        strncpy(s, c1, c2-c1);
        s[c2-c1] = '\0';
        rts_argv[rts_argc++] = s;

	c1 = c2;
    } while (*c1 != '\0');
}
    
/* -----------------------------------------------------------------------------
   Parse the command line arguments, collecting options for the RTS.

   On return:
     - argv[] is *modified*, any RTS options have been stripped out
     - *argc  contains the new count of arguments in argv[]

     - rts_argv[]  (global) contains a copy of the collected RTS args
     - rts_argc    (global) contains the count of args in rts_argv

     - prog_argv[] (global) contains a copy of the non-RTS args (== argv)
     - prog_argc   (global) contains the count of args in prog_argv

     - prog_name   (global) contains the basename of prog_argv[0]

  -------------------------------------------------------------------------- */

void setupRtsFlags (int *argc, char *argv[])
{
    nat mode;
    nat total_arg;
    nat arg, rts_argc0;

    setProgName (argv);
    total_arg = *argc;
    arg = 1;

    *argc = 1;
    rts_argc = 0;

    rts_argv = stgCallocBytes(total_arg + 1, sizeof (char *), "setupRtsFlags");

    rts_argc0 = rts_argc;

    // process arguments from the ghc_rts_opts global variable first.
    // (arguments from the GHCRTS environment variable and the command
    // line override these).
    {
	if (ghc_rts_opts != NULL) {
            splitRtsFlags(ghc_rts_opts);
            // opts from ghc_rts_opts are always enabled:
            procRtsOpts(rts_argc0, RtsOptsAll);
            rts_argc0 = rts_argc;
        }
    }

    // process arguments from the GHCRTS environment variable next
    // (arguments from the command line override these).
    {
	char *ghc_rts = getenv("GHCRTS");

	if (ghc_rts != NULL) {
            if (rtsOptsEnabled == RtsOptsNone) {
                errorBelch("Warning: Ignoring GHCRTS variable as RTS options are disabled.\n         Link with -rtsopts to enable them.");
                // We don't actually exit, just warn
            } else {
                splitRtsFlags(ghc_rts);
                procRtsOpts(rts_argc0, rtsOptsEnabled);
                rts_argc0 = rts_argc;
            }
        }
    }

    // Split arguments (argv) into PGM (argv) and RTS (rts_argv) parts
    //   argv[0] must be PGM argument -- leave in argv

    for (mode = PGM; arg < total_arg; arg++) {
	// The '--RTS' argument disables all future +RTS ... -RTS processing.
	if (strequal("--RTS", argv[arg])) {
	    arg++;
	    break;
	}
	// The '--' argument is passed through to the program, but
	// disables all further +RTS ... -RTS processing.
	else if (strequal("--", argv[arg])) {
	    break;
	}
	else if (strequal("+RTS", argv[arg])) {
            mode = RTS;
        }
	else if (strequal("-RTS", argv[arg])) {
	    mode = PGM;
	}
        else if (mode == RTS) {
            rts_argv[rts_argc++] = copyArg(argv[arg]);
        }
        else {
            argv[(*argc)++] = argv[arg];
	}
    }
    // process remaining program arguments
    for (; arg < total_arg; arg++) {
	argv[(*argc)++] = argv[arg];
    }
    argv[*argc] = (char *) 0;
    rts_argv[rts_argc] = (char *) 0;

    procRtsOpts(rts_argc0, rtsOptsEnabled);

    normaliseRtsOpts();

    setProgArgv(*argc, argv);

    if (RtsFlags.GcFlags.statsFile != NULL) {
        initStatsFile (RtsFlags.GcFlags.statsFile);
    }
    if (RtsFlags.TickyFlags.tickyFile != NULL) {
        initStatsFile (RtsFlags.GcFlags.statsFile);
    }
}

/* -----------------------------------------------------------------------------
 * procRtsOpts: Process rts_argv between rts_argc0 and rts_argc.
 * -------------------------------------------------------------------------- */

static void procRtsOpts (int rts_argc0, RtsOptsEnabledEnum enabled)
{
    rtsBool error = rtsFalse;
    int arg;

    // Process RTS (rts_argv) part: mainly to determine statsfile
    for (arg = rts_argc0; arg < rts_argc; arg++) {
        if (rts_argv[arg][0] != '-') {
	    fflush(stdout);
	    errorBelch("unexpected RTS argument: %s", rts_argv[arg]);
	    error = rtsTrue;

        } else {

            if (enabled == RtsOptsNone) {
                errorBelch("RTS options are disabled. Link with -rtsopts to enable them.");
                stg_exit(EXIT_FAILURE);
            }

            switch(rts_argv[arg][1]) {
            case '-':
                if (strequal("info", &rts_argv[arg][2])) {
                    printRtsInfo();
                    stg_exit(0);
                }
                break;
            default:
                break;
            }

            if (enabled == RtsOptsSafeOnly) {
                errorBelch("Most RTS options are disabled. Link with -rtsopts to enable them.");
                stg_exit(EXIT_FAILURE);
            }

            switch(rts_argv[arg][1]) {

	      /* process: general args, then PROFILING-only ones, then
		 CONCURRENT-only, TICKY-only (same order as defined in
		 RtsFlags.lh); within those groups, mostly in
		 case-insensitive alphabetical order.  Final group is
		 x*, which allows for more options.
	      */

#ifdef TICKY_TICKY
# define TICKY_BUILD_ONLY(x) x
#else
# define TICKY_BUILD_ONLY(x) \
errorBelch("the flag %s requires the program to be built with -ticky", rts_argv[arg]); \
error = rtsTrue;
#endif

#ifdef PROFILING
# define PROFILING_BUILD_ONLY(x)   x
#else
# define PROFILING_BUILD_ONLY(x) \
errorBelch("the flag %s requires the program to be built with -prof", rts_argv[arg]); \
error = rtsTrue;
#endif

#ifdef TRACING
# define TRACING_BUILD_ONLY(x)   x
#else
# define TRACING_BUILD_ONLY(x) \
errorBelch("the flag %s requires the program to be built with -eventlog or -debug", rts_argv[arg]); \
error = rtsTrue;
#endif

#ifdef PARALLEL_RTS
# define PARALLEL_RTS_BUILD_ONLY(x)      x
# define THREADED_OR_PARALLEL_BUILD_ONLY(x)    x
#else
# define THREADED_OR_PARALLEL_BUILD_ONLY(x) THREADED_BUILD_ONLY(x)
# define PARALLEL_RTS_BUILD_ONLY(x) \
errorBelch("the flag %s requires a parallel build", rts_argv[arg]); \
error = rtsTrue;
#endif

#ifdef THREADED_RTS
# define THREADED_BUILD_ONLY(x)      x
#else
# define THREADED_BUILD_ONLY(x) \
errorBelch("the flag %s requires the program to be built with -threaded", rts_argv[arg]); \
error = rtsTrue;
#endif

#ifdef DEBUG
# define DEBUG_BUILD_ONLY(x) x
#else
# define DEBUG_BUILD_ONLY(x) \
errorBelch("the flag %s requires the program to be built with -debug", rts_argv[arg]); \
error = rtsTrue;
#endif

	      /* =========== GENERAL ========================== */
	      case '?':
		error = rtsTrue;
		break;

              /* This isn't going to allow us to keep related options
                 together as we add more --* flags. We really need a
                 proper options parser. */
	      case '-':
                  if (strequal("install-signal-handlers=yes",
                               &rts_argv[arg][2])) {
                      RtsFlags.MiscFlags.install_signal_handlers = rtsTrue;
                  }
                  else if (strequal("install-signal-handlers=no",
                               &rts_argv[arg][2])) {
                      RtsFlags.MiscFlags.install_signal_handlers = rtsFalse;
                  }
                  else if (strequal("machine-readable",
                               &rts_argv[arg][2])) {
                      RtsFlags.MiscFlags.machineReadable = rtsTrue;
                  }
                  else if (strequal("info",
                               &rts_argv[arg][2])) {
                      printRtsInfo();
                      stg_exit(0);
                  }
                  else {
		      errorBelch("unknown RTS option: %s",rts_argv[arg]);
		      error = rtsTrue;
                  }
		  break;
	      case 'A':
                  RtsFlags.GcFlags.minAllocAreaSize
                      = decodeSize(rts_argv[arg], 2, BLOCK_SIZE, HS_INT_MAX)
                           / BLOCK_SIZE;
                  break;

#ifdef USE_PAPI
	      case 'a':
		switch(rts_argv[arg][2]) {
		case '1':
		  RtsFlags.PapiFlags.eventType = PAPI_FLAG_CACHE_L1;
		  break;
		case '2':
		  RtsFlags.PapiFlags.eventType = PAPI_FLAG_CACHE_L2;
		  break;
		case 'b':
		  RtsFlags.PapiFlags.eventType = PAPI_FLAG_BRANCH;
		  break;
		case 's':
		  RtsFlags.PapiFlags.eventType = PAPI_FLAG_STALLS;
		  break;
		case 'e':
		  RtsFlags.PapiFlags.eventType = PAPI_FLAG_CB_EVENTS;
		  break;
                case '+':
                case '#':
                  if (RtsFlags.PapiFlags.numUserEvents >= MAX_PAPI_USER_EVENTS) {
                      errorBelch("maximum number of PAPI events reached");
                      stg_exit(EXIT_FAILURE);
                  }
                  nat eventNum  = RtsFlags.PapiFlags.numUserEvents++;
                  char kind     = rts_argv[arg][2];
                  nat eventKind = kind == '+' ? PAPI_PRESET_EVENT_KIND : PAPI_NATIVE_EVENT_KIND;

                  RtsFlags.PapiFlags.userEvents[eventNum] = rts_argv[arg] + 3;
                  RtsFlags.PapiFlags.eventType = PAPI_USER_EVENTS;
                  RtsFlags.PapiFlags.userEventsKind[eventNum] = eventKind;
                  break;
		default:
		  bad_option( rts_argv[arg] );
		}
		break;
#endif

	      case 'B':
		RtsFlags.GcFlags.ringBell = rtsTrue;
		break;

	      case 'c':
		  if (rts_argv[arg][2] != '\0') {
		      RtsFlags.GcFlags.compactThreshold =
			  atof(rts_argv[arg]+2);
		  } else {
		      RtsFlags.GcFlags.compact = rtsTrue;
		  }
		  break;

              case 'w':
		RtsFlags.GcFlags.sweep = rtsTrue;
		break;

	      case 'F':
	        RtsFlags.GcFlags.oldGenFactor = atof(rts_argv[arg]+2);
	      
		if (RtsFlags.GcFlags.oldGenFactor < 0)
		  bad_option( rts_argv[arg] );
		break;
	      
	      case 'D':
              DEBUG_BUILD_ONLY(
	      { 
		  char *c;

		  for (c  = rts_argv[arg] + 2; *c != '\0'; c++) {
		      switch (*c) {
		      case 's':
			  RtsFlags.DebugFlags.scheduler = rtsTrue;
			  break;
		      case 'i':
			  RtsFlags.DebugFlags.interpreter = rtsTrue;
			  break;
		      case 'w':
			  RtsFlags.DebugFlags.weak = rtsTrue;
			  break;
		      case 'G':
			  RtsFlags.DebugFlags.gccafs = rtsTrue;
			  break;
		      case 'g':
			  RtsFlags.DebugFlags.gc = rtsTrue;
			  break;
		      case 'b':
			  RtsFlags.DebugFlags.block_alloc = rtsTrue;
			  break;
		      case 'S':
			  RtsFlags.DebugFlags.sanity = rtsTrue;
			  break;
		      case 't':
			  RtsFlags.DebugFlags.stable = rtsTrue;
			  break;
		      case 'p':
			  RtsFlags.DebugFlags.prof = rtsTrue;
			  break;
		      case 'l':
			  RtsFlags.DebugFlags.linker = rtsTrue;
			  break;
		      case 'a':
			  RtsFlags.DebugFlags.apply = rtsTrue;
			  break;
		      case 'm':
			  RtsFlags.DebugFlags.stm = rtsTrue;
			  break;
		      case 'z':
			  RtsFlags.DebugFlags.squeeze = rtsTrue;
			  break;
		      case 'c':
			  RtsFlags.DebugFlags.hpc = rtsTrue;
			  break;
		      case 'r':
			  RtsFlags.DebugFlags.sparks = rtsTrue;
			  break;
		      default:
			  bad_option( rts_argv[arg] );
		      }
		  }
                  // -Dx also turns on -v.  Use -l to direct trace
                  // events to the .eventlog file instead.
                  RtsFlags.TraceFlags.tracing = TRACE_STDERR;
	      })
              break;

	      case 'K':
                  RtsFlags.GcFlags.maxStkSize =
                      decodeSize(rts_argv[arg], 2, sizeof(W_), HS_WORD_MAX) / sizeof(W_);
                  break;

	      case 'k':
		switch(rts_argv[arg][2]) {
                case 'c':
                  RtsFlags.GcFlags.stkChunkSize =
                      decodeSize(rts_argv[arg], 3, sizeof(W_), HS_WORD_MAX) / sizeof(W_);
                  break;
                case 'b':
                  RtsFlags.GcFlags.stkChunkBufferSize =
                      decodeSize(rts_argv[arg], 3, sizeof(W_), HS_WORD_MAX) / sizeof(W_);
                  break;
                case 'i':
                  RtsFlags.GcFlags.initialStkSize =
                      decodeSize(rts_argv[arg], 3, sizeof(W_), HS_WORD_MAX) / sizeof(W_);
                  break;
                default:
                  RtsFlags.GcFlags.initialStkSize =
                      decodeSize(rts_argv[arg], 2, sizeof(W_), HS_WORD_MAX) / sizeof(W_);
                  break;
                }
                break;

              case 'M':
                  RtsFlags.GcFlags.maxHeapSize =
                      decodeSize(rts_argv[arg], 2, BLOCK_SIZE, HS_WORD_MAX) / BLOCK_SIZE;
                  /* user give size in *bytes* but "maxHeapSize" is in *blocks* */
                  break;

	      case 'm':
                  RtsFlags.GcFlags.pcFreeHeap = atof(rts_argv[arg]+2);

                  if (RtsFlags.GcFlags.pcFreeHeap < 0 ||
                      RtsFlags.GcFlags.pcFreeHeap > 100)
                      bad_option( rts_argv[arg] );
                  break;

	      case 'G':
                  RtsFlags.GcFlags.generations =
                      decodeSize(rts_argv[arg], 2, 1, HS_INT_MAX);
                  break;

	      case 'H':
                  if (rts_argv[arg][2] == '\0') {
                      RtsFlags.GcFlags.heapSizeSuggestionAuto = rtsTrue;
                  } else {
                      RtsFlags.GcFlags.heapSizeSuggestion =
                          (nat)(decodeSize(rts_argv[arg], 2, BLOCK_SIZE, HS_WORD_MAX) / BLOCK_SIZE);
                  }
                  break;

#ifdef RTS_GTK_FRONTPANEL
	      case 'f':
		  RtsFlags.GcFlags.frontpanel = rtsTrue;
		  break;
#endif

    	      case 'I':	/* idle GC delay */
		if (rts_argv[arg][2] == '\0') {
		  /* use default */
		} else {
		    I_ cst; /* tmp */

		    /* Convert to millisecs */
		    cst = (I_) ((atof(rts_argv[arg]+2) * 1000));
		    RtsFlags.GcFlags.idleGCDelayTime = cst;
		}
		break;

	      case 'S':
		  RtsFlags.GcFlags.giveStats = VERBOSE_GC_STATS;
		  goto stats;

	      case 's':
		  RtsFlags.GcFlags.giveStats = SUMMARY_GC_STATS;
		  goto stats;

	      case 't':
		  RtsFlags.GcFlags.giveStats = ONELINE_GC_STATS;
		  goto stats;

	    stats:
		{ 
		    int r;
                    r = openStatsFile(rts_argv[arg]+2, NULL,
                                      &RtsFlags.GcFlags.statsFile);
		    if (r == -1) { error = rtsTrue; }
		}
                break;

	      case 'Z':
		RtsFlags.GcFlags.squeezeUpdFrames = rtsFalse;
		break;

	      /* =========== PROFILING ========================== */

	      case 'P': /* detailed cost centre profiling (time/alloc) */
	      case 'p': /* cost centre profiling (time/alloc) */
		PROFILING_BUILD_ONLY(
		switch (rts_argv[arg][2]) {
		  case 'x':
		    RtsFlags.CcFlags.doCostCentres = COST_CENTRES_XML;
		    break;
		  case 'a':
		    RtsFlags.CcFlags.doCostCentres = COST_CENTRES_ALL;
		    break;
		  default:
		      if (rts_argv[arg][1] == 'P') {
			  RtsFlags.CcFlags.doCostCentres =
			      COST_CENTRES_VERBOSE;
		      } else {
			  RtsFlags.CcFlags.doCostCentres =
			      COST_CENTRES_SUMMARY;
		      }
		      break;
		}
		) break;

	      case 'R':
		  PROFILING_BUILD_ONLY(
		      RtsFlags.ProfFlags.maxRetainerSetSize = atof(rts_argv[arg]+2);
  	          ) break;
	      case 'L':
		  PROFILING_BUILD_ONLY(
		      RtsFlags.ProfFlags.ccsLength = atof(rts_argv[arg]+2);
                      if(RtsFlags.ProfFlags.ccsLength <= 0) {
			bad_option(rts_argv[arg]);
                      }
		  ) break;
	      case 'h': /* serial heap profile */
#if !defined(PROFILING)
		switch (rts_argv[arg][2]) {
		  case '\0':
		  case 'T':
		    RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_CLOSURE_TYPE;
		    break;
		  default:
		    errorBelch("invalid heap profile option: %s",rts_argv[arg]);
		    error = rtsTrue;
		}
#else
		PROFILING_BUILD_ONLY(
		switch (rts_argv[arg][2]) {
		case '\0':
		case 'C':
		case 'c':
		case 'M':
		case 'm':
		case 'D':
		case 'd':
		case 'Y':
		case 'y':
		case 'R':
		case 'r':
		case 'B':
		case 'b':
		    if (rts_argv[arg][2] != '\0' && rts_argv[arg][3] != '\0') {
			{
			    char *left  = strchr(rts_argv[arg], '{');
			    char *right = strrchr(rts_argv[arg], '}');

			    // curly braces are optional, for
			    // backwards compat.
			    if (left)
				left = left+1;
			    else
				left = rts_argv[arg] + 3;

			    if (!right)
				right = rts_argv[arg] + strlen(rts_argv[arg]);

			    *right = '\0';

			    switch (rts_argv[arg][2]) {
			    case 'c': // cost centre label select
				RtsFlags.ProfFlags.ccSelector = left;
				break;
			    case 'C':
				RtsFlags.ProfFlags.ccsSelector = left;
				break;
			    case 'M':
			    case 'm': // cost centre module select
				RtsFlags.ProfFlags.modSelector = left;
				break;
			    case 'D':
			    case 'd': // closure descr select 
				RtsFlags.ProfFlags.descrSelector = left;
				break;
			    case 'Y':
			    case 'y': // closure type select
				RtsFlags.ProfFlags.typeSelector = left;
				break;
			    case 'R':
			    case 'r': // retainer select
				RtsFlags.ProfFlags.retainerSelector = left;
				break;
			    case 'B':
			    case 'b': // biography select
				RtsFlags.ProfFlags.bioSelector = left;
				break;
			    }
			}
			break;
		    }

		    if (RtsFlags.ProfFlags.doHeapProfile != 0) {
			errorBelch("multiple heap profile options");
			error = rtsTrue;
			break;
		    }

		    switch (rts_argv[arg][2]) {
		    case '\0':
		    case 'C':
		    case 'c':
			RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_CCS;
			break;
		    case 'M':
		    case 'm':
			  RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_MOD;
			  break;
		    case 'D':
		    case 'd':
			  RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_DESCR;
			  break;
		    case 'Y':
		    case 'y':
			  RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_TYPE;
			  break;
		    case 'R':
		    case 'r':
			  RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_RETAINER;
			  break;
		    case 'B':
		    case 'b':
			  RtsFlags.ProfFlags.doHeapProfile = HEAP_BY_LDV;
			  break;
		    }
		    break;
		      
		default:
		    errorBelch("invalid heap profile option: %s",rts_argv[arg]);
		    error = rtsTrue;
		}
		) 
#endif /* PROFILING */
    	    	break;

    	      case 'i':	/* heap sample interval */
		if (rts_argv[arg][2] == '\0') {
		  /* use default */
		} else {
		    I_ cst; /* tmp */

		    /* Convert to milliseconds */
		    cst = (I_) ((atof(rts_argv[arg]+2) * 1000));
		    RtsFlags.ProfFlags.profileInterval = cst;
		}
		break;

	      /* =========== CONCURRENT ========================= */
    	      case 'C':	/* context switch interval */
		if (rts_argv[arg][2] == '\0')
    	    	    RtsFlags.ConcFlags.ctxtSwitchTime = 0;
		else {
		    I_ cst; /* tmp */

		    /* Convert to milliseconds */
		    cst = (I_) ((atof(rts_argv[arg]+2) * 1000));
		    RtsFlags.ConcFlags.ctxtSwitchTime = cst;
		}
    	    	break;

              case 'V': /* master tick interval */
                if (rts_argv[arg][2] == '\0') {
                    // turns off ticks completely
                    RtsFlags.MiscFlags.tickInterval = 0;
                } else {
                    I_ cst; /* tmp */

                    /* Convert to milliseconds */
                    cst = (I_) ((atof(rts_argv[arg]+2) * 1000));
                    RtsFlags.MiscFlags.tickInterval = cst;
                }
                break;

#if !defined(NOSMP)
	      case 'N':
		THREADED_BUILD_ONLY(
		// could be THREADED_OR_PARALLEL_BUILD_ONLY()
                // PARALLEL_RTS uses this flag in a different way (for now)
		if (rts_argv[arg][2] == '\0') {
#if defined(PROFILING)
		    RtsFlags.ParFlags.nNodes = 1;
#else
                    RtsFlags.ParFlags.nNodes = getNumberOfProcessors();
#endif
		} else {
		    RtsFlags.ParFlags.nNodes
		      = strtol(rts_argv[arg]+2, (char **) NULL, 10);
		    if (RtsFlags.ParFlags.nNodes <= 0) {
		      errorBelch("bad value for -N");
		      error = rtsTrue;
		    }
#if defined(PROFILING)
                    if (RtsFlags.ParFlags.nNodes > 1) {
                        errorBelch("bad option %s: only -N1 is supported with profiling", rts_argv[arg]);
		      error = rtsTrue;
                    }
#endif
		}
		) break;

	      case 'g':
		THREADED_BUILD_ONLY(
		    switch (rts_argv[arg][2]) {
                    case '1':
                        // backwards compat only
                        RtsFlags.ParFlags.parGcEnabled = rtsFalse;
                        break;
		    default:
			errorBelch("unknown RTS option: %s",rts_argv[arg]);
			error = rtsTrue;
			break;
                    }
                    ) break;

	      case 'q':
#ifdef PARALLEL_RTS
		      // historically, "-q<option>" were for parallel Haskell.
		      // taken for threaded RTS: a,b,g,m,w below.
		      // When parallel: branch into separate option processing
                        process_par_option(arg, rts_argc, rts_argv, &error);
                        break;
#endif
                THREADED_BUILD_ONLY(
		    switch (rts_argv[arg][2]) {
		    case '\0':
			errorBelch("incomplete RTS option: %s",rts_argv[arg]);
			error = rtsTrue;
			break;
                    case 'g':
                        if (rts_argv[arg][3] == '\0') {
                            RtsFlags.ParFlags.parGcEnabled = rtsFalse;
                        } else {
                            RtsFlags.ParFlags.parGcEnabled = rtsTrue;
                            RtsFlags.ParFlags.parGcGen
                                = strtol(rts_argv[arg]+3, (char **) NULL, 10);
                        }
                        break;
		    case 'b':
                        if (rts_argv[arg][3] == '\0') {
                            RtsFlags.ParFlags.parGcLoadBalancingEnabled = rtsFalse;
                        }
                        else {
                            RtsFlags.ParFlags.parGcLoadBalancingEnabled = rtsTrue;
                            RtsFlags.ParFlags.parGcLoadBalancingGen
                                = strtol(rts_argv[arg]+3, (char **) NULL, 10);
                        }
                        break;
		    case 'a':
			RtsFlags.ParFlags.setAffinity = rtsTrue;
			break;
		    case 'm':
			RtsFlags.ParFlags.migrate = rtsFalse;
			break;
                    case 'w':
                        // -qw was removed; accepted for backwards compat
                        break;
                    default:
			errorBelch("unknown RTS option: %s",rts_argv[arg]);
			error = rtsTrue;
			break;
		    }
                    ) break;
#endif
	      /* =========== PARALLEL =========================== */
	      case 'e':
		THREADED_BUILD_ONLY(
		if (rts_argv[arg][2] != '\0') {
		    RtsFlags.ParFlags.maxLocalSparks
		      = strtol(rts_argv[arg]+2, (char **) NULL, 10);
		    if (RtsFlags.ParFlags.maxLocalSparks <= 0) {
		      errorBelch("bad value for -e");
		      error = rtsTrue;
		    }
		}
		) break;

	      /* =========== TICKY ============================== */

	      case 'r': /* Basic profiling stats */
		TICKY_BUILD_ONLY(

		RtsFlags.TickyFlags.showTickyStats = rtsTrue;

		{ 
		    int r;
                    r = openStatsFile(rts_argv[arg]+2,
                                      TICKY_FILENAME_FMT,
                                      &RtsFlags.TickyFlags.tickyFile);
		    if (r == -1) { error = rtsTrue; }
		}
	        ) break;

	      /* =========== TRACING ---------=================== */

              case 'l':
                  TRACING_BUILD_ONLY(
                      RtsFlags.TraceFlags.tracing = TRACE_EVENTLOG;
                      read_trace_flags(&rts_argv[arg][2]);
                      );
                  break;

	      case 'v':
                  DEBUG_BUILD_ONLY(
                      RtsFlags.TraceFlags.tracing = TRACE_STDERR;
                      read_trace_flags(&rts_argv[arg][2]);
                      );
                  break;

	      /* =========== EXTENDED OPTIONS =================== */

              case 'x': /* Extend the argument space */
                switch(rts_argv[arg][2]) {
                  case '\0':
		    errorBelch("incomplete RTS option: %s",rts_argv[arg]);
		    error = rtsTrue;
		    break;

                case 'b': /* heapBase in hex; undocumented */
                    if (rts_argv[arg][3] != '\0') {
                        RtsFlags.GcFlags.heapBase
                            = strtol(rts_argv[arg]+3, (char **) NULL, 16);
                    } else {
                        errorBelch("-xb: requires argument");
                        error = rtsTrue;
                    }
                    break;

#if defined(x86_64_HOST_ARCH)
                case 'm': /* linkerMemBase */
                    if (rts_argv[arg][3] != '\0') {
                        RtsFlags.MiscFlags.linkerMemBase
                            = strtol(rts_argv[arg]+3, (char **) NULL, 16);
                        if (RtsFlags.MiscFlags.linkerMemBase > 0x80000000) {
                            errorBelch("-xm: value must be <80000000");
                            error = rtsTrue;
                        }
                    } else {
                        RtsFlags.MiscFlags.linkerMemBase = 0;
                    }
                    break;
#endif

                case 'c': /* Debugging tool: show current cost centre on an exception */
                    PROFILING_BUILD_ONLY(
			RtsFlags.ProfFlags.showCCSOnException = rtsTrue;
			);
		    break;

		case 't':  /* Include memory used by TSOs in a heap profile */
		    PROFILING_BUILD_ONLY(
			RtsFlags.ProfFlags.includeTSOs = rtsTrue;
			);
		    break;

                  /* The option prefix '-xx' is reserved for future extension.  KSW 1999-11. */

	          default:
		    errorBelch("unknown RTS option: %s",rts_argv[arg]);
		    error = rtsTrue;
		    break;
                }
                break;  /* defensive programming */

	      /* =========== OH DEAR ============================ */
	      default:
		errorBelch("unknown RTS option: %s",rts_argv[arg]);
		error = rtsTrue;
		break;
	    }
	}
    }

    if (error) errorUsage();
}

/* -----------------------------------------------------------------------------
 * normaliseRtsOpts: Set some derived values, and make sure things are
 * within sensible ranges.
 * -------------------------------------------------------------------------- */

static void normaliseRtsOpts (void)
{
    if (RtsFlags.MiscFlags.tickInterval < 0) {
        RtsFlags.MiscFlags.tickInterval = 50;
    }

    // If the master timer is disabled, turn off the other timers.
    if (RtsFlags.MiscFlags.tickInterval == 0) {
        RtsFlags.ConcFlags.ctxtSwitchTime  = 0;
        RtsFlags.GcFlags.idleGCDelayTime   = 0;
        RtsFlags.ProfFlags.profileInterval = 0;
    }

    // Determine what tick interval we should use for the RTS timer
    // by taking the shortest of the various intervals that we need to
    // monitor.
    if (RtsFlags.ConcFlags.ctxtSwitchTime > 0) {
        RtsFlags.MiscFlags.tickInterval =
            stg_min(RtsFlags.ConcFlags.ctxtSwitchTime,
                    RtsFlags.MiscFlags.tickInterval);
    }

    if (RtsFlags.GcFlags.idleGCDelayTime > 0) {
        RtsFlags.MiscFlags.tickInterval =
            stg_min(RtsFlags.GcFlags.idleGCDelayTime,
                    RtsFlags.MiscFlags.tickInterval);
    }

    if (RtsFlags.ProfFlags.profileInterval > 0) {
        RtsFlags.MiscFlags.tickInterval =
            stg_min(RtsFlags.ProfFlags.profileInterval,
                    RtsFlags.MiscFlags.tickInterval);
    }

    if (RtsFlags.ConcFlags.ctxtSwitchTime > 0) {
        RtsFlags.ConcFlags.ctxtSwitchTicks =
            RtsFlags.ConcFlags.ctxtSwitchTime /
            RtsFlags.MiscFlags.tickInterval;
    } else {
        RtsFlags.ConcFlags.ctxtSwitchTicks = 0;
    }

    if (RtsFlags.ProfFlags.profileInterval > 0) {
        RtsFlags.ProfFlags.profileIntervalTicks =
            RtsFlags.ProfFlags.profileInterval / 
            RtsFlags.MiscFlags.tickInterval;
    } else {
        RtsFlags.ProfFlags.profileIntervalTicks = 0;
    }

    if (RtsFlags.GcFlags.stkChunkBufferSize >
        RtsFlags.GcFlags.stkChunkSize / 2) {
        errorBelch("stack chunk buffer size (-kb) must be less than 50%% of the stack chunk size (-kc)");
        errorUsage();
    }
}

static void errorUsage (void)
{
    const char **p;

    fflush(stdout);
    for (p = usage_text; *p; p++)
        errorBelch("%s", *p);
#if defined(PARALLEL_RTS) && defined(DEBUG)
    errorBelch("Parallel debugging:");
    help_par_debug_options(MAX_PAR_DEBUG_MASK);
#endif
    stg_exit(EXIT_FAILURE);
}


#ifdef PARALLEL_RTS

static void
process_par_option(int arg, int *rts_argc, char *rts_argv[], rtsBool *error)
{

  // the -q prefix is shared with THREADED_RTS.
  // Currently taken by THREADED_RTS: a,b,g,m,w.
  // Currently accepted here: D,Q,r,W

  /* Communication and task creation cost parameters */
  switch(rts_argv[arg][2]) {
 
  // alphabetical order:
  case 'q': /* -qq<n> ... set send buffer size to <n> * packbuffer */
    if (rts_argv[arg][3] != '\0') {
      RtsFlags.ParFlags.sendBufferSize 
        = strtol(rts_argv[arg]+3, (char **) NULL, 10);
    }
    if (RtsFlags.ParFlags.sendBufferSize <= 0) {
      errorBelch("bad value for -qq");
      *error = rtsTrue;
    }
    break;
  case 'Q': /* -qQ<n> ... set pack buffer size to <n> bytes*/
    if (rts_argv[arg][3] != '\0') {
      RtsFlags.ParFlags.packBufferSize = 
	decodeSize(rts_argv[arg], 3, 1024, HS_INT_MAX);
    } else {
      errorBelch("missing size of PackBuffer (for -qQ)\n");
      *error = rtsTrue;
    }
    IF_PAR_DEBUG(verbose,
		 debugBelch("-qQ<n>: pack buffer size set to %d bytes\n", 
		       RtsFlags.ParFlags.packBufferSize));
    break;
  case 'r':  // -Qr... : something about placement  
    switch(rts_argv[arg][3]) {
    case 'n': // will be "random placement"
      if (!strncmp(rts_argv[arg],"-qrnd",5)) {
	RtsFlags.ParFlags.placement 
	  = (1 | RtsFlags.ParFlags.placement); // set first bit 
	IF_PAR_DEBUG(verbose,
	     debugBelch("-qrnd: random process placement\n"));
      }
      break;
    case 'e': // will be -Qremote: create children only on remote PEs
      if (!strncmp(rts_argv[arg],"-qremote",8)) {
	RtsFlags.ParFlags.placement 
	  = (2 | RtsFlags.ParFlags.placement); // set 2nd bit
	IF_PAR_DEBUG(verbose,
	     debugBelch("-qremote: only remote process creation.\n"));
      }
      break;
    default: 
      doNothing();
    }
    break; // finish "case 'r'"

    /*
  case 'B': // timeout for buffered messaging
    RtsFlags.ParFlags.BufferTime
      = strtol(rts_argv[arg]+3, (char **) NULL, 10);
    IF_PAR_DEBUG(verbose,
		 debugBelch("%s: Timeout for sending message buffers: %d\n",
		       rts_argv[arg],RtsFlags.ParFlags.BufferTime));
    break;
  case 'P': // -qP for writing a log file 
    //RtsFlags.ParFlags.ParStats.Full = rtsFalse;

    // JB 11/2006: borrowed NewLogfile and Heap flags for Eden tracing
    // (pablo sddf format). NewLogfile (for now) always on, Heap can
    // be used as an optional extension (was commented out before).
    RtsFlags.ParFlags.ParStats.NewLogfile = rtsTrue;

    // JB 03/2007: option -qPm for "Full" => include messages

    switch(rts_argv[arg][3]) {
    case '\0': // JB 03/07... RtsFlags.ParFlags.ParStats.Full = rtsTrue;
      // "Full" for Eden tracing: trace messages as well
      break; // nothing special, just an ordinary profile
    case 'm': RtsFlags.ParFlags.ParStats.Full = rtsTrue;
      // "Full" for Eden tracing: trace messages as well
      break;
    case '0': RtsFlags.ParFlags.ParStats.Suppressed = rtsTrue;
	RtsFlags.ParFlags.ParStats.Full = rtsFalse;
      break;
    case 'b': RtsFlags.ParFlags.ParStats.Binary = rtsTrue;
      break;
    case 's': RtsFlags.ParFlags.ParStats.Sparks = rtsTrue;
      break;
    case 'h': RtsFlags.ParFlags.ParStats.Heap = rtsTrue;
      break;
    case 'n': RtsFlags.ParFlags.ParStats.NewLogfile = rtsTrue;
      break;
    default: barf("Unknown option -qP%c", rts_argv[arg][2]);
    }
    IF_PAR_DEBUG(verbose,
		 debugBelch("(-qP) writing to log-file (%s)",
			    (RtsFlags.ParFlags.ParStats.NewLogfile ? 
			     "SDDF" : 
			     "not implemented")));
    break;
  
    default:
      errorBelch("setupRtsFlags: unknown parallelism option %s ",rts_argv[arg]);
      *error = rtsTrue;
    }
    break;
    */  
# if defined(DEBUG)  
  case 'W':
    if (rts_argv[arg][3] != '\0') {
      RtsFlags.ParFlags.wait
	= strtol(rts_argv[arg]+3, (char **) NULL, 10);
    } else {
      RtsFlags.ParFlags.wait = 1000;
    }
    IF_PAR_DEBUG(verbose,
		 debugBelch("-qW<n>: length of wait loop after "
                            "synchr before reduction: %ld\n", 
			    RtsFlags.ParFlags.wait));
    break;

  case 'D':  /* -qD ... all the debugging options */
    /* if not specified further: switch on "verbosity" (bit 0,1) */
    if (rts_argv[arg][3]== 0) {
      set_par_debug_options(1);
      break;
    }
    if (isdigit(rts_argv[arg][3])) {/* Set all debugging options in one */
      /* hack warning: interpret the flags as a binary number */
      nat n = (nat) strtol(rts_argv[arg]+3, (char **) NULL, 10);
      set_par_debug_options(n);
    } else {
      nat i;
      for (i=0; i<=MAX_PAR_DEBUG_OPTION; i++) 
	if (rts_argv[arg][3] == par_debug_opts_flags[i])
	  break;
	
      if (i > MAX_PAR_DEBUG_OPTION) {
	errorBelch("Valid parallel debug options are:\n");
	help_par_debug_options(MAX_PAR_DEBUG_MASK);
	bad_option( rts_argv[arg] );
      } else { // flag found; now set it
	set_par_debug_options(PAR_DEBUG_MASK(i));  // 2^i
      }
    }
    break;
# endif
  default:
    errorBelch("Unknown option -q%c (%d opts in total)", 
	  rts_argv[arg][2], *rts_argc);
    *error = rtsTrue;
    break;
  } /* switch */
}

#ifdef DEBUG
/*
  Interpret n as a binary number masking Par debug options and set the 
  correxponding option. See par_debug_opts_strs for explanations of the flags.
*/
static void
set_par_debug_options(nat n) {
  nat i;

  for (i=0; i<=MAX_PAR_DEBUG_OPTION; i++) 
    if ((n>>i)&1) {
      debugBelch("%s", par_debug_opts_strs[i]);
      switch (i) {
        case 0: RtsFlags.ParFlags.Debug.verbose       = rtsTrue;  break;
        case 1: RtsFlags.ParFlags.Debug.mpcomm        = rtsTrue;  break;
        case 2: RtsFlags.ParFlags.Debug.pack          = rtsTrue;  break;
        case 3: RtsFlags.ParFlags.Debug.packet        = rtsTrue;  break;
        case 4: RtsFlags.ParFlags.Debug.procs         = rtsTrue;  break;
        case 5: RtsFlags.ParFlags.Debug.ports         = rtsTrue;  break;
        default: barf("set_par_debug_options: only %d debug options expected",
		      MAX_PAR_DEBUG_OPTION);
      } /* switch */
    } /* if */
}

/*
  Print one line explanation for each of the GranSim debug options specified
  in the bitmask n.
*/
static void
help_par_debug_options(nat n) {
  nat i;

  for (i=0; i<=MAX_PAR_DEBUG_OPTION; i++) 
    if ((n>>i)&1) 
      debugBelch("%s", par_debug_opts_strs[i]);
}

#endif /* DEBUG */
#endif /* PARALLEL_RTS */

static void
stats_fprintf(FILE *f, char *s, ...)
{
    va_list ap;
    va_start(ap,s);
    if (f == NULL) {
	vdebugBelch(s, ap);
    } else {
	vfprintf(f, s, ap);
    }
    va_end(ap);
}

/* -----------------------------------------------------------------------------
 * openStatsFile: open a file in which to put some runtime stats
 * -------------------------------------------------------------------------- */

static int // return -1 on error
openStatsFile (char *filename,           // filename, or NULL
               const char *filename_fmt, // if filename == NULL, use
                                         // this fmt with sprintf to
                                         // generate the filename.  %s
                                         // expands to the program name.
               FILE **file_ret)          // return the FILE*
{
    FILE *f = NULL;

    if (strequal(filename, "stderr")
        || (filename_fmt == NULL && *filename == '\0')) {
        f = NULL; /* NULL means use debugBelch */
    } else {
        if (*filename != '\0') {  /* stats file specified */
            f = fopen(filename,"w");
        } else {
            char stats_filename[STATS_FILENAME_MAXLEN]; /* default <program>.<ext> */
            sprintf(stats_filename, filename_fmt, prog_name);
            f = fopen(stats_filename,"w");
        }
	if (f == NULL) {
            errorBelch("Can't open stats file %s\n", filename);
	    return -1;
	}
    }
    *file_ret = f;

    return 0;
}

/* -----------------------------------------------------------------------------
 * initStatsFile: write a line to the file containing the program name
 * and the arguments it was invoked with.
-------------------------------------------------------------------------- */

static void initStatsFile (FILE *f)
{
    /* Write prog_argv and rts_argv into start of stats file */
    int count;
    for (count = 0; count < prog_argc; count++) {
        stats_fprintf(f, "%s ", prog_argv[count]);
    }
    stats_fprintf(f, "+RTS ");
    for (count = 0; count < rts_argc; count++)
        stats_fprintf(f, "%s ", rts_argv[count]);
    stats_fprintf(f, "\n");
}

/* -----------------------------------------------------------------------------
 * decodeSize: parse a string containing a size, like 300K or 1.2M
-------------------------------------------------------------------------- */

static StgWord64
decodeSize(const char *flag, nat offset, StgWord64 min, StgWord64 max)
{
    char c;
    const char *s;
    StgDouble m;
    StgWord64 val;

    s = flag + offset;

    if (!*s)
    {
        m = 0;
    }
    else
    {
        m = atof(s);
        c = s[strlen(s)-1];

        if (c == 'g' || c == 'G') 
            m *= 1024*1024*1024;
        else if (c == 'm' || c == 'M')
            m *= 1024*1024;
        else if (c == 'k' || c == 'K')
            m *= 1024;
        else if (c == 'w' || c == 'W')
            m *= sizeof(W_);
    }

    val = (StgWord64)m;

    if (m < 0 || val < min || val > max) {
        // printf doesn't like 64-bit format specs on Windows
        // apparently, so fall back to unsigned long.
        errorBelch("error in RTS option %s: size outside allowed range (%lu - %lu)", flag, (lnat)min, (lnat)max);
        stg_exit(EXIT_FAILURE);
    }

    return val;
}

#if defined(TRACING)
static void read_trace_flags(char *arg)
{
    char *c;

    for (c  = arg; *c != '\0'; c++) {
        switch(*c) {
        case '\0':
            break;
        case 's':
            RtsFlags.TraceFlags.scheduler = rtsTrue;
            break;
        case 't':
            RtsFlags.TraceFlags.timestamp = rtsTrue;
            break;
        case 'g':
            // ignored for backwards-compat
            break;
        default:
            errorBelch("unknown trace option: %c",*c);
            break;
        }
    }
}
#endif

static void GNU_ATTRIBUTE(__noreturn__)
bad_option(const char *s)
{
  errorBelch("bad RTS option: %s", s);
  stg_exit(EXIT_FAILURE);
}

/* ----------------------------------------------------------------------------
   Copying and freeing argc/argv
   ------------------------------------------------------------------------- */

static char * copyArg(char *arg)
{
    char *new_arg = stgMallocBytes(strlen(arg) + 1, "copyArg");
    strcpy(new_arg, arg);
    return new_arg;
}

static char ** copyArgv(int argc, char *argv[])
{
    int i;
    char **new_argv;

    new_argv = stgCallocBytes(argc + 1, sizeof (char *), "copyArgv 1");
    for (i = 0; i < argc; i++) {
        new_argv[i] = copyArg(argv[i]);
    }
    new_argv[argc] = NULL;
    return new_argv;
}

static void freeArgv(int argc, char *argv[])
{
    int i;
    if (argv != NULL) {
        for (i = 0; i < argc; i++) {
            stgFree(argv[i]);
        }
        stgFree(argv);
    }
}

/* -----------------------------------------------------------------------------
   Getting/Setting the program's arguments.

   These are used by System.Environment, and parts of the RTS.
   -------------------------------------------------------------------------- */

void
setProgName(char *argv[])
{
    /* Remove directory from argv[0] -- default files in current directory */
#if !defined(mingw32_HOST_OS)
    char *last_slash;
    if ( (last_slash = (char *) strrchr(argv[0], '/')) != NULL ) {
	prog_name = last_slash+1;
   } else {
	prog_name = argv[0];
   }
#else
    char* last_slash = argv[0] + (strlen(argv[0]) - 1);
    while ( last_slash > argv[0] ) {
	if ( *last_slash == '/' || *last_slash == '\\' ) {
	    prog_name = last_slash+1;
	    return;
	}
	last_slash--;
    }
    prog_name = argv[0];
#endif
}

void
getProgArgv(int *argc, char **argv[])
{
    if (argc) { *argc = prog_argc; }
    if (argv) { *argv = prog_argv; }
}

void
setProgArgv(int argc, char *argv[])
{
    prog_argc = argc;
    prog_argv = copyArgv(argc,argv);
    setProgName(prog_argv);
}

static void
freeProgArgv(void)
{
    freeArgv(prog_argc,prog_argv);
    prog_argc = 0;
    prog_argv = NULL;
}

/* ----------------------------------------------------------------------------
   The full argv - a copy of the original program's argc/argv
   ------------------------------------------------------------------------- */

void
setFullProgArgv(int argc, char *argv[])
{
    full_prog_argc = argc;
    full_prog_argv = copyArgv(argc,argv);
}

/* These functions record and recall the full arguments, including the
   +RTS ... -RTS options. The reason for adding them was so that the
   ghc-inplace program can pass /all/ the arguments on to the real ghc. */
void
getFullProgArgv(int *argc, char **argv[])
{
    if (argc) { *argc = full_prog_argc; }
    if (argv) { *argv = full_prog_argv; }
}

void
freeFullProgArgv (void)
{
    freeArgv(full_prog_argc, full_prog_argv);
    full_prog_argc = 0;
    full_prog_argv = NULL;
}

/* ----------------------------------------------------------------------------
   The Win32 argv
   ------------------------------------------------------------------------- */

#if defined(mingw32_HOST_OS)
void freeWin32ProgArgv (void);

void
freeWin32ProgArgv (void)
{
    int i;

    if (win32_prog_argv != NULL) {
        for (i = 0; i < win32_prog_argc; i++) {
            stgFree(win32_prog_argv[i]);
        }
        stgFree(win32_prog_argv);
    }

    win32_prog_argc = 0;
    win32_prog_argv = NULL;
}

void
getWin32ProgArgv(int *argc, wchar_t **argv[])
{
    *argc = win32_prog_argc;
    *argv = win32_prog_argv;
}

void
setWin32ProgArgv(int argc, wchar_t *argv[])
{
	int i;
    
	freeWin32ProgArgv();

    win32_prog_argc = argc;
	if (argv == NULL) {
		win32_prog_argv = NULL;
		return;
	}
	
    win32_prog_argv = stgCallocBytes(argc + 1, sizeof (wchar_t *),
                                    "setWin32ProgArgv 1");
    for (i = 0; i < argc; i++) {
        win32_prog_argv[i] = stgMallocBytes((wcslen(argv[i]) + 1) * sizeof(wchar_t),
                                           "setWin32ProgArgv 2");
        wcscpy(win32_prog_argv[i], argv[i]);
    }
    win32_prog_argv[argc] = NULL;
}
#endif

/* ----------------------------------------------------------------------------
   The RTS argv
   ------------------------------------------------------------------------- */

static void
freeRtsArgv(void)
{
    freeArgv(rts_argc,rts_argv);
    rts_argc = 0;
    rts_argv = NULL;
}

/* ----------------------------------------------------------------------------
   All argvs
   ------------------------------------------------------------------------- */

void freeRtsArgs(void)
{
#if defined(mingw32_HOST_OS)
    freeWin32ProgArgv();
#endif
    freeFullProgArgv();
    freeProgArgv();
    freeRtsArgv();
}
