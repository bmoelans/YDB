!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!								!
! Copyright (c) 2001-2017 Fidelity National Information		!
! Services, Inc. and/or its subsidiaries. All rights reserved.	!
!								!
! Copyright (c) 2017-2018 YottaDB LLC. and/or its subsidiaries.	!
! All rights reserved.						!
!								!
!	This source code contains the intellectual property	!
!	of its copyright holder(s), and is made available	!
!	under a license.  If you do not know the terms of	!
!	the license, please stop and do not read further.	!
!								!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

! Code in this module is based on merrors.msg and hence has an
! FIS copyright even though this module was not created by FIS.

	.FACILITY	YDB,256/PREFIX=ERR_
	.TITLE		YDBERRORS Error Messages for YottaDB
!
!  List of messages that are (currently) only used in Golang Wrapper code
!    STRUCTNOTALLOCD
!    NODEEND
!    INVLNPAIRLIST
!
!  List of known undocumented messages follows (along with a comment) (do not change "known undocumented" phrase)
! ----- Buffer to introduce new undocumented error messages without affecting UNUSEDMSGnnn match with corresponding line numbers.
!
!
! Note: This file is similar to sr_port/merrors.msg in layout.
!
QUERY2		<Invalid second argument to $QUERY. Must be -1 or 1.>/error/fao=0
MIXIMAGE	<Cannot load !AD image on process that already has !AD image loaded>/error/fao=4
LIBYOTTAMISMTCH	<$ydb_dist/libyottadb.so (!AD) does not match the shared library path (!AD)>/error/fao=4
READONLYNOSTATS	<Read-only and Statistics sharing cannot both be enabled on database>/error/fao=0
READONLYLKFAIL	<Failed to get !AD lock on READ_ONLY database file !AD>/error/fao=4
INVVARNAME	<Invalid local/global/ISV variable name supplied to API call>/error/fao=0
PARAMINVALID	<!AD parameter specified in !AD call>/error/fao=4
INSUFFSUBS	<Return subscript array for !AD call too small - needs at least !UL entries for this call>/error/fao=3
MINNRSUBSCRIPTS	<Number of subscripts cannot be a negative number>/error/fao=0
SUBSARRAYNULL	<Non-zero number of subscripts [!UL] specified but subscript array parameter is NULL in !AD call>/error/fao=3
FATALERROR1	<Fatal error raised. Generating core and terminating process. Error: !AD>/fatal/fao=2
NAMECOUNT2HI	<Number of varnames (namecount parameter in a !AD call) exceeds maximum (!UL)>/error/fao=3
INVNAMECOUNT	<Number of varnames (namecount parameter in a !AD call) cannot be less than zero>/error/fao=2
FATALERROR2	<Fatal error raised. Bypassing core generation and terminating process. Error: !AD>/fatal/fao=2
TIME2LONG	<Specified time value [0x!16@XQ] exceeds supported maximum [0x!16@XQ]>/error/fao=2
VARNAME2LONG	<Variable name length exceeds maximum allowed (!UL)>/error/fao=1
SIMPLEAPINEST	<Attempt to nest call of !AD with a call to !AD - nesting calls is not permitted in the Simple API>/error/fao=4
CALLINTCOMMIT	<TCOMMIT at call-in-level=!UL not allowed as corresponding TSTART was done at lower call-in-level=!UL>/error/fao=2
CALLINTROLLBACK	<TROLLBACK at call-in-level=!UL not allowed as corresponding TSTART was done at lower call-in-level=!UL>/error/fao=2
TCPCONNTIMEOUT	<Connection wait timeout (!UL seconds) has expired>/error/fao=2
STDERRALREADYOPEN	<STDERR deviceparameter specifies an already open device !AD>/error/fao=2
SETENVFAIL	<VIEW "SETENV":"!AD" failed in setenv() system call>/error/fao=2
UNSETENVFAIL	<VIEW "UNSETENV":"!AD" failed in unsetenv() system call>/error/fao=2
UNKNOWNSYSERR	<An unknown system error has occurred: !AD>/error/fao=2
STRUCTNOTALLOCD	<Structure not previously called with Alloc() method>/error/fao=0
PARMOFLOW	<Parameter overflow attempting to call variadic plist routine>/error/fao=0
NODEEND		<End of list of nodes/subscripts>/error/fao=0
INVLNPAIRLIST	<Invalid lockname/subscript pair list (uneven number of lockname/subscript parameters)>/error/fao=0
INVTPTRANS	<Invalid TP transaction - either invalid TP token or transaction not in progress>/error/fao=0
INVAPIMODE	<Process not allowed to first use a(n) !AD API then switch to using a(n) !AD API>/error/fao=4
NOTSUPSTAPI	<Calls to !AD are not supported if threaded Simple API functions are in use>/error/fao=2
!
! If there are UNUSEDMSG* lines unused for more than one year and at least two non-patch releases, use them before adding new lines.
!
	.end
