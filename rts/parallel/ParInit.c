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
#include "LLC.h"
#include "HLC.h"

#include "MPSystem.h" /* wraps middleware usage */

#include "Trace.h"
#include <string.h>
#include "Stats.h"


#include <sys/time.h>

#ifndef PARALLEL_RTS
/* provide constants nPE and thisPe for foreign import */
nat nPEs   = 1;
nat thisPE = 1;
#endif

#ifdef PARALLEL_RTS /* whole rest of the file */
#ifdef TRACING
StgWord64 startupTicks;
char *argvsave;
struct timeval startupTime;
struct timezone startupTimeZone;
#endif //TRACING
/* For flag handling see RtsFlags.h */

void
shutdownParallelSystem(StgInt n)
{
  IF_PAR_DEBUG(verbose,
	       if (n==0)
  	         debugBelch("==== entered shutdownParallelSystem ...\n");
               else
  	         debugBelch("==== entered shutdownParallelSystem (ERROR %d)...\n", (int) n);
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

  // care not to GC any CAFs. Incoming packets might refer to them
  // This variable lives in Storage.c
  keepCAFs = rtsTrue;

  // initialise "system tso" which owns blackholes and stores blocking queues
  SET_HDR(&stg_system_tso, &stg_TSO_info, CCS_SYSTEM);
  stg_system_tso.indirectee = (StgClosure*) END_TSO_QUEUE;

}


void emitStartupEvents(void){
  //edentrace: traceCreateMachine
  //startupTicks was fetched earlier, CreateMachine has 
  //to be the first Event writen to keep the order of the 
  //timestamps in the buffers valid
  traceCreateMachine(thisPE,((startupTime.tv_sec) * 100000000 + (startupTime.tv_usec) * 100),startupTicks); 

  //edentrace:  traceVersion
  traceVersion(ProjectVersion);
  //edentrace:  traceProgramInvocation
  traceProgramInvocation(argvsave);
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
//MD/TH 03/2010: workaround: store timestamp here and use it in synchroniseSystem
#ifdef TRACING
  startupTicks = stat_getElapsedTime(); // see Stats.c, elapsed time from init
  gettimeofday(&startupTime,&startupTimeZone);
  //MD: copy argument list to string for traceProgramInvocation
  int len = 0;
  int i=0;
  while (i < *argc){
    len+=strlen((*argv)[i])+1;
    i++;
  }
  argvsave = (char *)calloc(len + 1, sizeof(char));
  i=0;
  while (i < *argc){
  strcat(argvsave,(*argv)[i]);
  strcat(argvsave," ");
    i++;
  }
#endif //TRACING
  // possibly starts other PEs (first argv is number) 
  // sets IAmMainThread, nPEs
  MP_start(argc, *argv); 
  
  (*argv)[1] = (*argv)[0];   /* ignore the nPEs argument */
  (*argv)++; (*argc)--;
  if (IAmMainThread){
	/* Only in debug mode? */
	fprintf(stderr, "==== Starting parallel execution on %d processors ...\n", 
		nPEs);
  }
}
#endif /* PARALLEL_RTS -- whole file */
