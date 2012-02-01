#ifndef PEOPCODES_H
#define PEOPCODES_H

#if defined(PARALLEL_RTS)

/************************************************************************
*                         PEOpCodes.h                                   *
*									*
*	This file contains definitions for all the GUM PE Opcodes       *
*       It's based on the GRAPH for PVM version                         *
*       Phil Trinder, Glasgow University 8th December 1994              *
*									*
   RFPointon, December 1999
     - removed PP_SYSMAN_TID, introduced PP_READY
     - removed PP_MAIN_TASK, introduced PP_NEWPE
     - added PP_REVAL

   JB: introduced categories "System"/"Data"
       system messages are received with higher priority, see MPSystem.

* convention: 
* (to keep things structured, no depending functionality)
*   definitions start with system messages (MIN_PEOPS)
*   
* JB 2006: reordered tags, left old definitions intact when not used.
*
***********************************************************************/

#define	MIN_PEOPS		0x50
#define	MAX_PEOPS		0x5c

/* ************************** */
/* Generic Parallel RTS */
/*Startup + Shutdown*/
#define	PP_READY		0x50  /* sent PEs -> main PE */
#define	PP_NEWPE		0x51  /* sent (by middleware) new PE -> main PE */
#define	PP_PETIDS		0x52  /* sent main PE -> PEs */
#define	PP_FINISH		0x53  /* sent PEs & via taskExit notfiy -> main PE  */
#define PP_FAIL                 0x54  /* sent (by middleware) failing PE -> main PE */

/* remote evaluation (sortof heap data + action) */
#define PP_RFORK                0x55
/* heap data */
#define PP_CONNECT              0x56
#define PP_DATA                 0x57
#define PP_HEAD                 0x58
#define PP_CONSTR               0x59
#define PP_PART                 0x5a

/* forcing termination */
#define PP_TERMINATE            0x5b

/* packet of msg.s - buffering */ 
#define PP_PACKET               0x5c

#define	PEOP_NAMES		"Ready", "NewPE",  \
				"PETIDS","Finish", \
                                "Fail",            \
                                "RFork",           \
                                "Connect","Data",  \
                                "Head","Constr",   \
                                "Part",            \
                                "Terminate",       \
                                "Packet"

// simple validation method:
#define ISOPCODE(code) (((code) <= MAX_PEOPS) && ((code) >= MIN_PEOPS))

/* message categories, important for receiving messages: */
// system messages: handled by, or related to the scheduler
#define ISSYSCODE(code)   ((code) == PP_FINISH) // 
// other messages are data messages, concerning heap manipulation only

/*
 * getOpName returns the character-string name of any OpCode.
 */

#if defined(DEBUG)
static char *UserPEOpNames[] = { PEOP_NAMES };
static char* getOpName(int op)
{
    if (ISOPCODE(op))
	return (UserPEOpNames[op - MIN_PEOPS]);
    else
	return ("Unknown PE OpCode");
}
#endif

#endif /* PARALLEL_HASKELL */

#endif /* PEOPCODES_H */
