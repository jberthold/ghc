/* --------------------------------------------------------------------------

   Initialising the parallel RTS

   An extension based on Kevin Hammond's GRAPH for PVM version
   P. Trinder, January 17th 1995.
   Adapted for the new RTS
   P. Trinder, July 1997.
   H-W. Loidl, November 1999.

   rewrite for Eden-6.x, Jost Berthold, 2006
   adapted to Eden-6.11, August 2009

   ------------------------------------------------------------------------ */

#include "Rts.h"
#include "RtsUtils.h"

#include "MPSystem.h" /* wraps middleware usage */

#include "Trace.h"
#include <string.h>
#include "Stats.h"

#include "ZipFile.h"

#include <sys/time.h>

#ifndef PARALLEL_RTS
/* provide constants nPE and thisPe for foreign import */
PEId nPEs   = 1;
PEId thisPE = 1;
#endif

#if defined(PARALLEL_RTS) /* whole rest of the file */
#if defined(TRACING)
StgWord64 startupTicks;
char *argvsave;
char *pareventsName;
struct timeval startupTime;
struct timezone startupTimeZone;
PEId pes; // remember nPEs after shutdown
#endif //TRACING
/* For flag handling see RtsFlags.h */

void
shutdownParallelSystem(StgInt n)
{
  IF_PAR_DEBUG(verbose,
      if (n==0)
        debugBelch("==== entered shutdownParallelSystem ...\n");
      else
        debugBelch("==== entered shutdownParallelSystem (ERROR %d)...\n",
                   (int) n);
               );

  // JB 11/2006: write stop event, close trace file. Done here to
  // avoid a race condition if trace files merged by main node
  // automatically.
  //edentrace: traceKillMachine
  traceKillMachine(thisPE);

  MP_quit(n);

  // free allocated space (send/receive buffers)
  freePackBuffer();
  freeRecvBuffer();
  /*
  freeMoreBuffers();
  */

  // and runtime tables
  freeRTT();

}

/*
 * SynchroniseSystem synchronises the reduction task with the system
 * manager, and initialises global structures: receive buffer for
 * communication, process table, and in GUM the Global address tables
 * (LAGA & GALA)
 */

//@cindex synchroniseSystem
void
synchroniseSystem(void)
{
  MP_sync();

  // all kinds of initialisation we can do now...
  // Don't buffer standard channels...
  setbuf(stdout,NULL);
  setbuf(stderr,NULL);

  // initialise runtime tables
  initRTT();

  // allocate global buffer in DataComms
  initPackBuffer();

  // initialise "system tso" which owns blackholes and stores blocking queues
  SET_HDR(&stg_system_tso, &stg_TSO_info, CCS_SYSTEM);
  stg_system_tso.indirectee = (StgClosure*) END_TSO_QUEUE;

}


void emitStartupEvents(void){
  //edentrace: traceCreateMachine
  //startupTicks was fetched earlier, CreateMachine has
  //to be the first Event writen to keep the order of the
  //timestamps in the buffers valid
  traceCreateMachine(thisPE,((startupTime.tv_sec) * 100000000 +
                             (startupTime.tv_usec) * 100),startupTicks);

  //edentrace:  traceVersion
  traceVersion(ProjectVersion);
  //edentrace:  traceProgramInvocation
  traceProgramInvocation(argvsave);

#if defined(TRACING)
  pes = nPEs; // and remember nPEs (shutdown will zero it)
#endif
}

/* zipTraceFiles creates a zip file (using a utility module)
 * containing the eventlog files of all PEs (for their names see
 * EventLog.c). argvsave is used as a comment string for the archive,
 * so it can be identified by unzip -l.
 *
 * The archive name will be <argv>.parevents, overwriting an
 * older file of the same name if there was one.
 *
 * This method should be used by all parallel ways which do not use a
 * run script. It must be called after shutdownParallelSystem and
 * endTracing (see RtsStartup). shutDownParallelSystem sets nPEs to 0,
 * so the value is remembered in variable "pes" for this method.
 */
void zipTraceFiles(void) {

#if defined(TRACING)

  char **files, *prog;
  int i;
  bool res;

  if (!IAmMainThread || RtsFlags.TraceFlags.tracing != TRACE_EVENTLOG) {
    return;
  }

  // see rts/eventlog/EventLog.c, must match naming convention there
    prog = stgMallocBytes(strlen(prog_name) + 1, "initEventLogging");
    strcpy(prog, prog_name);
#if defined(mingw32_HOST_OS)
    // on Windows, drop the .exe suffix if there is one
    {
        char *suff;
        suff = strrchr(prog,'.');
        if (suff != NULL && !strcmp(suff,".exe")) {
            *suff = '\0';
        }
    }
#endif

  files = stgMallocBytes(sizeof(char*) * pes, "file array");

  if (pes == 1) {
    // mv $prog#1.eventlog $prog.parevents; return;
    files[0] = stgMallocBytes(strlen(prog) + 10 + 1 + 1, "fname");
    sprintf(files[0], "%s#1.eventlog", prog);
    if (rename(files[0], pareventsName) != 0) {
      sysErrorBelch("Failed to rename trace file");
      errorBelch("(trying to rename %s to %s)", files[0], pareventsName);
    }
    free(files[0]);
  } else {
    for (i=0; i < (int)pes; i++) {
      FILE *f;
      // file[i] = "$prog#$i.eventlog". Assume $i requires < 5 char
      files[i] = stgMallocBytes(strlen(prog) + 5 + 10 + 1, "fname");
      sprintf(files[i], "%s#%d.eventlog", prog, i+1);
      // check that file actually exists. This method should be portable
      if ((f = fopen(files[i], "r")) == NULL) {
        free(files[i]); i--; pes--; // error, skip this file
      } else {
        fclose(f);
      }
    }
    // did we end up having only one trace file (or none)?
    if (pes <= 1) {
      if (rename(files[0], pareventsName) != 0) {
        sysErrorBelch("Failed to rename trace file");
        errorBelch("(trying to rename %s to %s)", files[0], pareventsName);
      }
    } else {
      res = compressFiles(pareventsName, pes, files, argvsave);
      
      // and remove the files if this worked
      for (i=0; i < (int)pes; i++) {
        if (res == true) {
          if (remove(files[i]) < 0) {
            sysErrorBelch("Failed to remove file");
            errorBelch("(when removing file %s)\n", files[i]);
          }
        }
        free(files[i]);
      }
    }
  }
  free(files);
#endif
}


/*
  Do the startup stuff (middleware-dependencies wrapped in MPSystem.h
  Global vars held in MPSystem:  IAmMainThread, thisPE, nPEs
  Called at the beginning of RtsStartup.startupHaskell
*/

void
startupParallelSystem(int* argc, char **argv[]) {

  //  getStartTime(); // init start time (in RtsUtils.*)

  // write Event for machine startup here, before
  // communication is set up (might take a while)

  // JB 11/2006: thisPE is still 0 at this moment, we cannot name the
  // trace file here => startup time is in reality sync time.
//MD/TH 03/2010: workaround: store timestamp here, use it in synchroniseSystem
#if defined(TRACING)
  startupTicks = stat_getElapsedTime(); // see Stats.c, elapsed time from init
  gettimeofday(&startupTime,&startupTimeZone);
  //MD: copy argument list to string for traceProgramInvocation
  int len = 0;
  int i=0;
  while (i < *argc){
    len+=strlen((*argv)[i])+1;
    i++;
  }
  len--;
  argvsave = stgMallocBytes( len + 1, "argvsave");
  pareventsName = stgMallocBytes( len + 10 + 1, "pareventsName");


    strcat(argvsave,(*argv)[0]);
    strcat(pareventsName,(*argv)[0]);
#if defined(mingw32_HOST_OS)
    // on Windows, drop the .exe suffix if there is one
    {
        char *suff;
        suff = strrchr(pareventsName,'.');
        if (suff != NULL && !strcmp(suff,".exe")) {
            *suff = '\0';
        }
    }
#endif

  i=1;
  while (i < *argc){
    strcat(argvsave," ");
    strcat(pareventsName,"_");
    strcat(argvsave,(*argv)[i]);
    strcat(pareventsName,(*argv)[i]);
    i++;
  }
  strcat(pareventsName,".parevents");
#endif //TRACING
  // possibly starts other PEs (first argv is number)
  // sets IAmMainThread, nPEs.
  // strips argv of first argument, adjusts argc
  MP_start(argc, argv);

  if (IAmMainThread){
    /* Only in debug mode? */
    fprintf(stderr, "==== Starting parallel execution on %d processors ...\n",
            nPEs);
  }
}
#endif /* PARALLEL_RTS -- whole file */
