
/*
 *
 * casDGOS.c
 * $Id$
 *
 *
 * $Log$
 * Revision 1.1.1.1  1996/06/20 00:28:06  jhill
 * ca server installation
 *
 *
 */

//
// CA server
// 
#include <taskLib.h> // vxWorks

#include <server.h>
#include <casClientIL.h> // casClient inline func
#include <task_params.h> // EPICS task priorities

//
// casDGOS::eventSignal()
//
void casDGOS::eventSignal()
{
	STATUS	st;

	st = semGive(this->eventSignalSem);
	assert (st==OK);
}

//
// casDGOS::eventFlush()
//
void casDGOS::eventFlush()
{
	this->flush();
}


//
// casDGOS::casDGOS()
//
casDGOS::casDGOS(caServerI &cas) : 
	eventSignalSem(NULL),
	casDGClient(cas),
	clientTId(ERROR),
	eventTId(ERROR)
{
}

//
// casDGOS::init()
//
caStatus casDGOS::init()
{
	caStatus status;

	this->eventSignalSem = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
	if (this->eventSignalSem == NULL) {
		return S_cas_noMemory;
	}

	//
	// init the base classes
	//
	status = this->casDGClient::init();

	return status;
}


//
// casDGOS::~casDGOS()
//
casDGOS::~casDGOS()
{
	if (taskIdVerify(this->clientTId)==OK) {
		taskDelete(this->clientTId);
	}
	if (taskIdVerify(this->eventTId)==OK) {
		taskDelete(this->eventTId);
	}
	if (this->eventSignalSem) {
		semDelete(this->eventSignalSem);
	}
}

//
// casDGOS::show()
//
void casDGOS::show(unsigned level)
{
	this->casDGClient::show(level);
	printf ("casDGOS at %x\n", (unsigned) this);
	if (taskIdVerify(this->clientTId) == OK) {
		taskShow(this->clientTId, level);
	}
	if (taskIdVerify(this->eventTId) == OK) {
		taskShow(this->eventTId, level);
	}
	if (this->eventSignalSem) {
		semShow(this->eventSignalSem, level);
	}
}


/*
 * casClientStart ()
 */
caStatus casDGOS::start()
{
	//
	// no (void *) vxWorks task arg
	//
	assert (sizeof(int) >= sizeof(this));

	this->clientTId = taskSpawn(
			CAST_SRVR_NAME,
                  	CAST_SRVR_PRI,
                  	CAST_SRVR_OPT,
                  	CAST_SRVR_STACK,
			(FUNCPTR) casDGServer, // get your act together wrs 
			(int) this,  // get your act together wrs
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);
	if (this->clientTId==ERROR) {
		return S_cas_noMemory;
	}
	this->eventTId = taskSpawn(
			CA_EVENT_NAME,
                  	CA_CLIENT_PRI,
                  	CA_CLIENT_OPT,
                  	CAST_SRVR_STACK,
			(FUNCPTR) casDGEvent, // get your act together wrs 
			(int) this,  // get your act together wrs
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL);
	if (this->eventTId==ERROR) {
		return S_cas_noMemory;
	}
	return S_cas_success;
}


/*
 * casDGOS::processInput ()
 * - a noop
 */
casProcCond casDGOS::processInput ()
{
	return casProcOk;
}

//
// casDGServer()
//
int casDGServer (casDGOS *pDGOS)
{
	caStatus status;

	//
	// block for the next DG until the connection closes
	//
	while (TRUE) {
		status = pDGOS->processInput();
		if (status) {
			errMessage(status, "casDGServer (casDGOS *pDGOS)");
		}
	}
}

//
// casDGEvent()
//
int casDGEvent (casDGOS *pDGOS)
{
	STATUS status;
	casProcCond cond;

	//
	// Wait for event queue entry
	//
	while (TRUE) {
		status = semTake(pDGOS->eventSignalSem, WAIT_FOREVER);
		assert (status!=OK);

		cond = pDGOS->casEventSys::process();
		if (cond != casProcOk) {
			printf("DG event sys process failed\n");
		}
	}
}

