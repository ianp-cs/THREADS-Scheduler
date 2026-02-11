/*
Modified by: Ian Penrose & Lindsay Wax
Created by: Professor Mike Duren, University of Arizona
Course: CYBV 489
*/

#pragma once

#define READY 1			// Waiting to run
#define QUIT 2			// Waiting to be cleaned up and removed
#define RUNNING 3		// Actively running; is the current running process
#define BLOCKED 11		// Waiting for a child or joined process to finish running

/*
Processes are the simulated processes created and used by the "Operating System" in the THREADS environment.
*/
typedef struct _process
{
	struct _process*        nextReadyProcess;			// Points to the next ready process in the priority queue
	struct _process*		nextSiblingProcess;			// Points to the next child belonging to this process' parent
	struct _process*		pParent;					// Points to this process' parent 
	struct _process*        pChildren;					// Points to the head child in this process' children linked list
	struct _process*		pExited[MAX_PROCESSES];		// An array of pointers to child processes waiting to be cleaned up
	struct _process*		pJoined[MAX_PROCESSES];		// An array of pointers to processes joined to this process

	char           name[MAXNAME];		// Process name 
	char           startArgs[MAXARG];	// Process arguments 
	void*		   context;				// Process's current context (i.e. READY, QUIT, BLOCKED, RUNNING) 
	short          pid;					// Process id (pid) 
	int            priority;			// The priority of the process, determines how quickly the dispatcher will run it 
	int (*entryPoint) (void*);			// The entry point (function pointer) that is called from launch 
	char*	       stack;				// Contents of the process' memory stack 
	unsigned int   stacksize;			// Size of the memory stack allocated to the process 
	int            status;				// READY, QUIT, BLOCKED, etc. 
	int			   exitCode;			// The code needed by k_wait() and is input into k_exit()
	int			   signaled;			// 1 = TRUE, 0 = FALSE
	int			   startTime;			// The time the process was created, according to the system clock
	int			   runTime;				// The start of the process' current run time. This resets everytime this process is set as the active process.
	int			   numChildren;			// The number of active children processes this process has
	int			   numExited;			// The number of exited children waiting to be cleaned up
	int			   numJoined;			// The number of processes joined to this process
	bool		   joined;				// Is the process joined to another?
	int			   endJoinCode;			// The exit code received by the last process this process was joined to

} Process;

/*
Queues are FIFO linked lists whose nodes are Processes.
*/
typedef struct _queue
{
	Process*	head;		// First process in the linked list
	Process*	tail;		// Last process in the linked list
	int			size;		// Total number of processes in queue
	int			priority;	// The priority of the processes in the queue

} Queue;