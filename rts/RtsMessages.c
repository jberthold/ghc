/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2004
 *
 * General utility functions used in the RTS.
 *
 * ---------------------------------------------------------------------------*/

#include "PosixSource.h"
#include "Rts.h"

#include "eventlog/EventLog.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

/* -----------------------------------------------------------------------------
   General message generation functions

   All messages should go through here.  We can't guarantee that
   stdout/stderr will be available - e.g. in a Windows program there
   is no console for generating messages, so they have to either go to
   to the debug console, or pop up message boxes.
   -------------------------------------------------------------------------- */

#if defined(PARALLEL_RTS)
/* Special versions of message functions, prepending the PE number and
   giving the Eden URL for bug reports. */
RtsMsgFunction *fatalInternalErrorFn = edenFatalInternalErrorFn;
RtsMsgFunction *debugMsgFn           = parDebugMsgFn;
RtsMsgFunction *errorMsgFn           = parErrorMsgFn;
RtsMsgFunction *sysErrorMsgFn        = parSysErrorMsgFn;
#else
// Default to the stdio implementation of these hooks.
RtsMsgFunction *fatalInternalErrorFn = rtsFatalInternalErrorFn;
RtsMsgFunction *debugMsgFn           = rtsDebugMsgFn;
RtsMsgFunction *errorMsgFn           = rtsErrorMsgFn;
RtsMsgFunction *sysErrorMsgFn        = rtsSysErrorMsgFn;
#endif

void
barf(const char*s, ...)
{
  va_list ap;
  va_start(ap,s);
  (*fatalInternalErrorFn)(s,ap);
  stg_exit(EXIT_INTERNAL_ERROR); // just in case fatalInternalErrorFn() returns
  va_end(ap);
}

void
vbarf(const char*s, va_list ap)
{
  (*fatalInternalErrorFn)(s,ap);
  stg_exit(EXIT_INTERNAL_ERROR); // just in case fatalInternalErrorFn() returns
}

void
_assertFail(const char*filename, unsigned int linenum)
{
    barf("ASSERTION FAILED: file %s, line %u\n", filename, linenum);
}

void
errorBelch(const char*s, ...)
{
  va_list ap;
  va_start(ap,s);
  (*errorMsgFn)(s,ap);
  va_end(ap);
}

void
verrorBelch(const char*s, va_list ap)
{
  (*errorMsgFn)(s,ap);
}

void
sysErrorBelch(const char*s, ...)
{
  va_list ap;
  va_start(ap,s);
  (*sysErrorMsgFn)(s,ap);
  va_end(ap);
}

void
vsysErrorBelch(const char*s, va_list ap)
{
  (*sysErrorMsgFn)(s,ap);
}

void
debugBelch(const char*s, ...)
{
  va_list ap;
  va_start(ap,s);
  (*debugMsgFn)(s,ap);
  va_end(ap);
}

void
vdebugBelch(const char*s, va_list ap)
{
  (*debugMsgFn)(s,ap);
}

/* -----------------------------------------------------------------------------
   stdio versions of the message functions
   -------------------------------------------------------------------------- */

#define BUFSIZE 512

#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
static int
isGUIApp(void)
{
  PIMAGE_DOS_HEADER pDOSHeader;
  PIMAGE_NT_HEADERS pPEHeader;

  pDOSHeader = (PIMAGE_DOS_HEADER) GetModuleHandleA(NULL);
  if (pDOSHeader->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;

  pPEHeader = (PIMAGE_NT_HEADERS) ((char *)pDOSHeader + pDOSHeader->e_lfanew);
  if (pPEHeader->Signature != IMAGE_NT_SIGNATURE)
    return 0;

  return (pPEHeader->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);
}
#endif

#define xstr(s) str(s)
#define str(s) #s

void GNU_ATTRIBUTE(__noreturn__)
rtsFatalInternalErrorFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char title[BUFSIZE], message[BUFSIZE];

     snprintf(title,   BUFSIZE, "%s: internal error", prog_name);
     vsnprintf(message, BUFSIZE, s, ap);

     MessageBox(NULL /* hWnd */,
                message,
                title,
                MB_OK | MB_ICONERROR | MB_TASKMODAL
               );
  }
  else
#endif
  {
     /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
     if (prog_argv != NULL && prog_name != NULL) {
       fprintf(stderr, "%s: internal error: ", prog_name);
     } else {
       fprintf(stderr, "internal error: ");
     }
     vfprintf(stderr, s, ap);
     fprintf(stderr, "\n");
     fprintf(stderr, "    (GHC version %s for %s)\n", ProjectVersion, xstr(HostPlatform_TYPE));
     fprintf(stderr, "    Please report this as a GHC bug:  http://www.haskell.org/ghc/reportabug\n");
     fflush(stderr);
  }

#ifdef TRACING
  if (RtsFlags.TraceFlags.tracing == TRACE_EVENTLOG) endEventLogging();
#endif

  abort();
  // stg_exit(EXIT_INTERNAL_ERROR);
}

void
rtsErrorMsgFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char buf[BUFSIZE];
     int r;

         r = vsnprintf(buf, BUFSIZE, s, ap);
         if (r > 0 && r < BUFSIZE) {
                MessageBox(NULL /* hWnd */,
              buf,
              prog_name,
              MB_OK | MB_ICONERROR | MB_TASKMODAL
              );
     }
  }
  else
#endif
  {
     /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
     if (prog_name != NULL) {
       fprintf(stderr, "%s: ", prog_name);
     }
     vfprintf(stderr, s, ap);
     fprintf(stderr, "\n");
  }
}

void
rtsSysErrorMsgFn(const char *s, va_list ap)
{
    char *syserr;

#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
        (LPTSTR) &syserr,
        0,
        NULL );

    if (isGUIApp())
    {
        char buf[BUFSIZE];
        int r;

        r = vsnprintf(buf, BUFSIZE, s, ap);
        if (r > 0 && r < BUFSIZE) {
            r = vsnprintf(buf+r, BUFSIZE-r, ": %s", syserr);
            MessageBox(NULL /* hWnd */,
                       buf,
                       prog_name,
                       MB_OK | MB_ICONERROR | MB_TASKMODAL
                );
        }
    }
    else
#else
    syserr = strerror(errno);
    // ToDo: use strerror_r() if available
#endif
    {
        /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
        if (prog_argv != NULL && prog_name != NULL) {
            fprintf(stderr, "%s: ", prog_name);
        }
        vfprintf(stderr, s, ap);
        if (syserr) {
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
            // Win32 error messages have a terminating \n
            fprintf(stderr, ": %s", syserr);
#else
            fprintf(stderr, ": %s\n", syserr);
#endif
        } else {
            fprintf(stderr, "\n");
        }
    }

#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
    if (syserr) LocalFree(syserr);
#endif
}

void
rtsDebugMsgFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char buf[BUFSIZE];
         int r;

         r = vsnprintf(buf, BUFSIZE, s, ap);
         if (r > 0 && r < BUFSIZE) {
       OutputDebugString(buf);
     }
  }
  else
#endif
  {
     /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
     vfprintf(stderr, s, ap);
     fflush(stderr);
  }
}

#if defined(PARALLEL_RTS)
/* *****************************
 * Parallel RTS error message functions.
 *
 * These functions are mostly copies from the above "rts..."
 * functions. Differences:
 *
 * - in case of error exit: shut down the system
 * - give the Eden website for bug reports
 * - prepend the PE number (thisPE) to debug messages
 * - try a clean shutdown upon fatal internal errors
 *
 * Functions kept separate because the different print format (thisPE)
 * would make an ifdef'ed solution very ugly.
 */

void
edenFatalInternalErrorFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char title[BUFSIZE], message[BUFSIZE];

     snprintf(title, BUFSIZE, "%s [PE %d]: internal error",
              prog_name, thisPE);
     vsnprintf(message, BUFSIZE, s, ap);

     MessageBox(NULL /* hWnd */,
                message,
                title,
                MB_OK | MB_ICONERROR | MB_TASKMODAL
                );
  }
  else
#endif
    {
  /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
  if (prog_argv != NULL && prog_name != NULL) {
    fprintf(stderr, "%s [PE %d]: internal error: ", prog_name, thisPE);
  } else {
    fprintf(stderr, "[PE %d]: internal error: ", thisPE);
  }
  vfprintf(stderr, s, ap);
  fprintf(stderr, "\n");
  fprintf(stderr, "    (Eden compiler %s for %s)\n",
          ProjectVersion, xstr(HostPlatform_TYPE));
  fprintf(stderr, "    Please report this as a bug: "
          "http://www.mathematik.uni-marburg.de/~eden\n");
  fflush(stderr);

  // The sequential system uses abort(); but we would like to shut down the
  // entire system cleanly, using stg_exit.
  stg_exit(EXIT_INTERNAL_ERROR);
    }
}

void
parErrorMsgFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char buf[BUFSIZE];
     char title[BUFSIZE];
     int r;

     r = snprintf(title, BUFSIZE, "%s [PE %d]", prog_name, thisPE);
     r = vsnprintf(buf, BUFSIZE, s, ap);
     if (r > 0 && r < BUFSIZE) {
       MessageBox(NULL /* hWnd */,
                  buf,
                  title,
                  MB_OK | MB_ICONERROR | MB_TASKMODAL
                  );
     }
  }
  else
#endif
  {
    /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
    if (prog_name != NULL) {
      fprintf(stderr, "%s [PE %d]: ", prog_name, thisPE);
    } else {
      fprintf(stderr, "[PE %d]: ", thisPE);
    }
    vfprintf(stderr, s, ap);
    fprintf(stderr, "\n");
  }
}

void
parSysErrorMsgFn(const char *s, va_list ap)
{
  char *syserr;

#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
        (LPTSTR) &syserr,
        0,
        NULL );

  if (isGUIApp())
  {
      char buf[BUFSIZE], title[BUFSIZE];
      int r;

      r = snprintf(title, BUFSIZE, "%s [PE %d]", prog_name, thisPE);
      r = vsnprintf(buf, BUFSIZE, s, ap);
      if (r > 0 && r < BUFSIZE) {
          r = vsnprintf(buf+r, BUFSIZE-r, ": %s", syserr);
          MessageBox(NULL /* hWnd */,
                     buf,
                     title,
                     MB_OK | MB_ICONERROR | MB_TASKMODAL
                     );
      }
    }
    else
#else
    syserr = strerror(errno);
    // ToDo: use strerror_r() if available
#endif
    {
        /* don't fflush(stdout); WORKAROUND bug in Linux glibc */
        if (prog_argv != NULL && prog_name != NULL) {
            fprintf(stderr, "%s [PE %d]: ", prog_name, thisPE);
        } else {
            fprintf(stderr, "[PE %d]: ", thisPE);
        }
        vfprintf(stderr, s, ap);
        if (syserr) {
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
            // Win32 error messages have a terminating \n
            fprintf(stderr, ": %s", syserr);
#else
            fprintf(stderr, ": %s\n", syserr);
#endif
        } else {
            fprintf(stderr, "\n");
        }
    }

#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
    if (syserr) LocalFree(syserr);
#endif
}

void
parDebugMsgFn(const char *s, va_list ap)
{
#if defined(cygwin32_HOST_OS) || defined (mingw32_HOST_OS)
  if (isGUIApp())
  {
     char buf[BUFSIZE];
     int r;

     r = vsnprintf(buf, BUFSIZE, s, ap);
     if (r > 0 && r < BUFSIZE) {
         OutputDebugString(buf);
     }
  }
  else
#endif
  {
    fprintf(stderr, "[PE %d]", thisPE);
    vfprintf(stderr, s, ap);
    fflush(stderr);
  }
}
#endif
