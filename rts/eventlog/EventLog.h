/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 2008-2009
 *
 * Support for fast binary event logging.
 *
 * ---------------------------------------------------------------------------*/

#ifndef EVENTLOG_H
#define EVENTLOG_H

#include "rts/EventLogFormat.h"
#include "Capability.h"

#ifdef PARALLEL_RTS
#include "Rts.h"
#endif //PARALLEL_RTS

#include "BeginPrivate.h"

#ifdef TRACING

/*
 * Descriptions of EventTags for events.
 */
extern char *EventTagDesc[];

void initEventLogging(void);
void endEventLogging(void);
void freeEventLogging(void);
void abortEventLogging(void); // #4512 - after fork child needs to abort
void flushEventLog(void);     // event log inherited from parent

/* 
 * Post a scheduler event to the capability's event buffer (an event
 * that has an associated thread).
 */
void postSchedEvent(Capability *cap, EventTypeNum tag, 
                    StgThreadID id, StgWord info1, StgWord info2);

/*
 * Post a nullary event.
 */
void postEvent(Capability *cap, EventTypeNum tag);

void postMsg(char *msg, va_list ap);

void postUserMsg(Capability *cap, char *msg, va_list ap);

void postCapMsg(Capability *cap, char *msg, va_list ap);

void postVersion(char *version);

void postProgramInvocation(char *commandline);

#ifdef PARALLEL_RTS
void postProcessEvent(EventProcessID pid, EventTypeNum tag);

void postAssignThreadToProcessEvent(Capability *cap, EventThreadID tid, EventProcessID pid);

void postCreateMachineEvent(EventMachineID pe, StgWord64 time, StgWord64 ticks, EventTypeNum tag);

void postKillMachineEvent(EventMachineID pe, EventTypeNum tag);

void postSendMessageEvent(OpCode msgtag, rtsPackBuffer* buf);
                          
void postReceiveMessageEvent(Capability *cap, OpCode msgtag, rtsPackBuffer* buf);

#endif //PARALLEL_RTS

#else /* !TRACING */

INLINE_HEADER void postSchedEvent (Capability *cap  STG_UNUSED,
                                   EventTypeNum tag STG_UNUSED,
                                   StgThreadID id   STG_UNUSED,
                                   StgWord info1    STG_UNUSED,
                                   StgWord info2    STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postEvent (Capability *cap  STG_UNUSED,
                              EventTypeNum tag STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postMsg (char *msg STG_UNUSED, 
                            va_list ap STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postCapMsg (Capability *cap STG_UNUSED,
                               char *msg STG_UNUSED, 
                               va_list ap STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postVersion(char *version STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postProgramInvocation(char *commandline STG_UNUSED)
{ /* nothing */ }

#if defined(PARALLEL_RTS)
INLINE_HEADER void postProcessEvent(EventProcessID pid  STG_UNUSED,
                                    EventTypeNum tag    STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postAssignThreadToProcessEvent(Capability *cap    STG_UNUSED,
                                                  EventThreadID tid  STG_UNUSED, 
                                                  EventProcessID pid STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postCreateMachineEvent(EventProcessID pid STG_UNUSED,
                                    StgWord64 time    STG_UNUSED,
                                    StgWord64 ticks    STG_UNUSED,
                                    EventTypeNum tag    STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postKillMachineEvent(EventProcessID pid STG_UNUSED,
                                    EventTypeNum tag    STG_UNUSED)
{ /* nothing */ }

INLINE_HEADER void postSendMessageEvent(OpCode msgtag              STG_UNUSED, 
                                        rtsPackBuffer *buf         STG_UNUSED)
{ /* nothing */ }
                          
INLINE_HEADER void postReceiveMessageEvent(Capability *cap            STG_UNUSED, 
                                           OpCode msgtag              STG_UNUSED, 
                                           rtsPackBuffer *buf         STG_UNUSED)

{ /* nothing */ }

//INLINE_HEADER inline StgWord64 time_ns(void STG_UNUSED){return 0; /* nothing */ }
#endif // PARALLEL_RTS

#endif

#include "EndPrivate.h"

#endif /* TRACING_H */
