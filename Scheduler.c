/*
Program: Scheduler
Modified by: Ian Penrose & Lindsay Wax
Created by: Professor Mike Duren, University of Arizona
Course: CYBV 489


Description: This file acts as the scheduler for the "Operating System" within the THREADS
environment. Processes are created here, controlled by a dispatcher utilizing a round-
robin with priority system, and then exit.
*/



#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include "THREADSLib.h"
#include "Scheduler.h"
#include "Processes.h"

#define NUM_PRIORITIES (HIGHEST_PRIORITY + 1)   // +1 to account for the lowest priority being 0

Process processTable[MAX_PROCESSES];    // This table holds every currently existing process, regardless of their status
Process *runningProcess = NULL;         // The currently running process, aka the current context
Queue readyLists[NUM_PRIORITIES];       // +1 to account for priority 0; index = priority
int nextPid = 1;                        // Controls the id of the next created process
int debugFlag = 1;                      // If set for console output, the text may not appear if debugging mode is off
interrupt_handler_t* interruptVector;   // Table that contains interrupt information

/* Provided functions */
static int watchdog(char*);
static inline void disableInterrupts();
static inline void enableInterrupts();  // New function, placed here for organizational purposes
void dispatcher();
static int launch(void *);
static void check_deadlock();
static void DebugConsole(char* format, ...);

/* New functions */
static int push(Queue* target, Process* node);
static Process* pop(Queue* target);
static int boolAvailableProcesses();
static int isHighestPriorityProcess(Process* target);
static Process* getHighestPriorityProcess();
static void cleanUpChild(Process* target);
static void addJoinedProc(Process* target);
static void clock_handler(char* deviceName, uint8_t command, uint32_t status);

/* DO NOT REMOVE */
extern int SchedulerEntryPoint(void* pArgs);
int check_io_scheduler();
check_io_function check_io;


/*************************************************************************
   bootstrap()

   Purpose - This is the first function called by THREADS on startup.

             The function must setup the OS scheduler and primitive
             functionality and then spawn the first two processes.  
             
             The first two process are the watchdog process 
             and the startup process SchedulerEntryPoint.  
             
             The statup process is used to initialize additional layers
             of the OS.  It is also used for testing the scheduler 
             functions.

   Parameters - Arguments *pArgs - these arguments are unused at this time.

   Returns - The function does not return!

   Side Effects - The effects of this function is the launching of the kernel.

 *************************************************************************/
int bootstrap(void *pArgs)
{
    set_debug_level(0); // COMMENT THIS LINE OUT TO ENABLE DEBUG MESSAGES

    int result; /* value returned by call to spawn() */
    set_psr(PSR_KERNEL_MODE);

    /* set this to the scheduler version of this function.*/
    check_io = check_io_scheduler;

    /* Initialize the process table. */
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        processTable[i].pid = 0;
    }

    /* Initialize the Ready list, etc. */
    for (int i = 0; i < NUM_PRIORITIES; i++)
    {
        readyLists[i].head = NULL;
        readyLists[i].tail = NULL;
        readyLists[i].size = 0;
        readyLists[i].priority = i;
    }

    /* Initialize the clock interrupt handler */
    interruptVector = get_interrupt_handlers();
    interruptVector[THREADS_TIMER_INTERRUPT] = clock_handler;

    /* startup a watchdog process */
    result = k_spawn("watchdog", watchdog, NULL, THREADS_MIN_STACK_SIZE, LOWEST_PRIORITY); // Will always be pid = 1
    if (result < 0)
    {
        console_output(debugFlag, "Scheduler(): spawn for watchdog returned an error (%d), stopping...\n", result);
        stop(1);
    }

    /* start the test process, which is the main for each test program.  */
    result = k_spawn("Scheduler", SchedulerEntryPoint, NULL, 2 * THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY); // Will always be pid = 2
    if (result < 0)
    {
        console_output(debugFlag,"Scheduler(): spawn for SchedulerEntryPoint returned an error (%d), stopping...\n", result);
        stop(1);
    }

    /* Initialized and ready to go!! */
    dispatcher();

    /* This should never return since we are not a real process. */

    stop(-3);
    return 0;

}

/*************************************************************************
   k_spawn()

   Purpose - spawns a new process.
   
             Finds an empty entry in the process table and initializes
             information of the process.  Updates information in the
             parent process to reflect this child process creation.

   Parameters - the process's entry point function, the stack size, and
                the process's priority.

   Returns - The Process ID (pid) of the new child process 
             The function must return if the process cannot be created.

************************************************************************ */
int k_spawn(char* name, int (*entryPoint)(void *), void* arg, int stacksize, int priority)
{
    int proc_slot;
    struct _process* pNewProc;

    /*
    if (get_psr() != PSR_KERNEL_MODE)
    {
        console_output(debugFlag, "spawn(): Not in Kernel Mode.\n");
        return -3;
    }
    */

    // Documentation says to check for Kernel Mode, but it doesn't say where Kernel mode should be activated?
    // This will likely need to be fixed while working on future milestones when we have more information!
    set_psr(PSR_KERNEL_MODE); 

    DebugConsole("spawn(): creating process %s\n", name);

    disableInterrupts();

    /* Validate all of the parameters*/
    if (name == NULL)
    {
        console_output(debugFlag, "spawn(): Name value is NULL.\n");
        return -1;
    }
    if (strlen(name) >= (MAXNAME - 1))
    {
        console_output(debugFlag, "spawn(): Process name is too long.  Halting...\n");
        stop( 1);
    }
    if (stacksize < THREADS_MIN_STACK_SIZE)
    {
        console_output(debugFlag, "spawn(): Stack size is too small.\n");
        return -4;
    }
    if (priority < LOWEST_PRIORITY || priority > HIGHEST_PRIORITY)
    {
        console_output(debugFlag, "spawn(): Invalid priority.\n");
        return -5;
    }

    /* Find an empty slot in the process table and save the index*/
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == 0)
        {
            proc_slot = i;
            break;
        }
    }
    
    // Point to memory location for the new procedure
    pNewProc = &processTable[proc_slot]; 

    /* Setup the entry in the process table. */
    strcpy(pNewProc->name, name);
    pNewProc->pid = nextPid;
    nextPid++;  // Increment Pid counter
    pNewProc->entryPoint = entryPoint;
    pNewProc->priority = priority;
    pNewProc->stacksize = stacksize;
    pNewProc->status = READY;
    pNewProc->exitCode = 0;
    pNewProc->signaled = 0;

    // Some processes don't have args, so we need to account for NULL
    if (arg != NULL)
    {
        strcpy(pNewProc->startArgs, arg);
    }

    
    // If there is a parent process, link the parent and this process to each other.
    if (runningProcess != NULL)
    {
        if (runningProcess->pChildren == NULL)
        {
            runningProcess->pChildren = pNewProc;
        }
        else
        {
            Process* child = runningProcess->pChildren;

            while (child->nextSiblingProcess != NULL)
            {
                child = child->nextSiblingProcess;
            }

            child->nextSiblingProcess = pNewProc;
        }

        pNewProc->pParent = runningProcess;
    }

    /* Add the process to the ready list. */
    if (push(&readyLists[pNewProc->priority], pNewProc) == 0)
    {
        return -2;
    }

    /* 
    Initialize context for this process, but use launch function pointer for
    the initial value of the process's program counter (PC)
    */
    pNewProc->context = context_initialize(launch, stacksize, arg);

    // Skip this function call for Watchdog and Scheduler, we need to finish initializing
    if (pNewProc->pid > 2) 
    {
        dispatcher();
    }

    return pNewProc->pid;

} /* spawn */

/**************************************************************************
   Name - launch

   Purpose - Utility function that makes sure the environment is ready,
             such as enabling interrupts, for the new process.  

   Parameters - none

   Returns - nothing
*************************************************************************/
static int launch(void *args)
{
    int result;

    DebugConsole("launch(): started: %s\n", runningProcess->name);

    /* Enable interrupts */
    enableInterrupts();

    /* Call the function passed to spawn and capture its return value */
    result = runningProcess->entryPoint(runningProcess->startArgs);

    DebugConsole("Process %d returned to launch\n", k_getpid());

    /* Stop the process gracefully */
    k_exit(result);

    return 0;
} 

/**************************************************************************
   Name - k_wait

   Purpose - Wait for a child process to quit.  Return right away if
             a child has already quit.

   Parameters - Output parameter for the child's exit code. 

   Returns - the pid of the quitting child, or
        -1 if the process has no children
        -5 if the process was signaled in the join

************************************************************************ */
int k_wait(int* code)
{
    int result = 0;
    Process* child = runningProcess->pChildren;

    // Case: Process has no children
    if (child == NULL) 
    {
        return -1;
    }

    // Case: a child has already exited
    while (child != NULL) 
    {
        if (child->status == QUIT) // Find the child and clean it up
        {
            *code = child->exitCode;
            result = child->pid;
            cleanUpChild(child);
            return result;
        }

        child = child->nextSiblingProcess; // Set new head of children linked list for the parent
    }

    // Case: no child has exited yet and this process must wait

    child = runningProcess->pChildren; // Reset to head of children linked list

    block(BLOCKED); // Block the parent and wait for control to be returned
    dispatcher();

    if (signaled()) // Signaled while waiting, return error code -5
    {
        return -5;
    }

    while (child != NULL) // Find exited child
    {
        if (child->status == QUIT) // Find the child and clean it up
        {
            *code = child->exitCode;
            result = child->pid;
            cleanUpChild(child);
            break;
        }

        child = child->nextSiblingProcess; // Set new head of children linked list for the parent
    }

    return result;

} 

/**************************************************************************
   Name - k_exit

   Purpose - Exits a process and coordinates with the parent for cleanup 
             and return of the exit code.

   Parameters - the code to return to the grieving parent

   Returns - nothing
   
*************************************************************************/
void k_exit(int code)
{
    // Exiting process should not have children, halt program if it does
    if (runningProcess->pChildren != NULL) 
    {
        console_output(debugFlag, "k_exit(): Exiting process has active children, closing program.\n");
        stop(1);
    }

    // If the process has a blocked parent, unblock it
    if (runningProcess->pParent != NULL && runningProcess->pParent->status == BLOCKED) 
    {
        unblock(runningProcess->pParent->pid);
    }
    
    // Signal to parent that this process needs to be cleaned up
    runningProcess->status = QUIT;

    if (signaled()) // Signaled while exiting, overwrite error code -5
    {
        runningProcess->exitCode = -5;
    }
    else
    {
        runningProcess->exitCode = code;
    }

    // Surrender control and wait to be cleaned up
    dispatcher();
}

/**************************************************************************
   Name - k_kill

   Purpose - Signals a process with the specified signal

   Parameters - Signal to send

   Returns -
*************************************************************************/
int k_kill(int pid, int signal)
{
    Process* target;

    if (signal != SIG_TERM)
    {
        console_output(debugFlag, "k_kill(): Invalid signal code, closing program.\n");
        stop(1);
    }

    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    if (target == NULL)
    {
        console_output(debugFlag, "k_kill(): Attempted to signal non-existant process, closing program.\n");
        stop(1);
    }

    if (target->signaled)
    {
        return 1;
    }
    
    target->signaled = 1;
    return 0;
}

/**************************************************************************
   Name - k_getpid
*************************************************************************/
int k_getpid()
{
    return runningProcess->pid;
}

/**************************************************************************
   Name - k_join
***************************************************************************/
int k_join(int pid, int* pChildExitCode)
{
    // Input Validation
    Process* target;

    if (getpid() == pid)
    {
        console_output(debugFlag, "k_join(): Attempted to join process to itself, closing program.\n");
        stop(1);
    }

    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    if (target == NULL)
    {
        console_output(debugFlag, "k_join(): Attempted to join process to a non-existant process, closing program.\n");
        stop(1);
    }

    // Join Processes
    addJoinedProc(target);
    block(BLOCKED);
    dispatcher();

    if (signaled())
    {
        return -5;
    }

    return 0;
}

/**************************************************************************
   Name - unblock
*************************************************************************/
int unblock(int pid)
{
    // Input validation
    Process* target;

    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    // This will need to be changed if there ends up being more than 1 BLOCKED status code
    if (target == NULL || target->status != BLOCKED) 
    {
        return -1;
    }

    // Unblock Process
    target->status = READY;
    push(&readyLists[target->priority], target);

    return 0;
}

/*************************************************************************
   Name - block
*************************************************************************/
int block(int newStatus)
{
    if (newStatus > 10)
    {
        console_output(debugFlag, "block(): Blocking status code is NOT greater than 10, closing program.\n");
        stop(1);
    }

    runningProcess->status = newStatus;
    dispatcher();

    if (signaled())
    {
        return -5;
    }

    return 0;
}

/*************************************************************************
   Name - signaled
*************************************************************************/
int signaled()
{
    return runningProcess->signaled;
}
/*************************************************************************
   Name - readtime
*************************************************************************/
int read_time()
{
    return 0;
}

/*************************************************************************
   Name - readClock
*************************************************************************/
DWORD read_clock()
{
    return system_clock();
}

void display_process_table()
{

}

/**************************************************************************
   Name - dispatcher

   Purpose - This is where context changes to the next process to run.

   Parameters - none

   Returns - nothing

*************************************************************************/
void dispatcher()
{
    Process* nextProcess = NULL; // Points to the next process that should run

    // No process was running
    if (runningProcess == NULL) 
    {
        nextProcess = getHighestPriorityProcess();
    }
    // Current process has been blocked by another process or has exited, do not add to readyLists
    else if (runningProcess->status == BLOCKED || runningProcess->status == QUIT)
    {
        nextProcess = getHighestPriorityProcess();
    }
    // Current process is still the higest priority
    else if (isHighestPriorityProcess(runningProcess)) 
    {
        return;
    }
    // Otherwise, reassess which process should run after adding current process back into readyLists
    else 
    {
        Process* previousProcess = runningProcess;
        runningProcess = NULL;

        // Add previous process back into the ready lists
        previousProcess->status = READY;
        push(&readyLists[previousProcess->priority], previousProcess);

        nextProcess = getHighestPriorityProcess();
    }

    if (nextProcess == NULL)
    {
        console_output(debugFlag, "Dispatcher(): Attempting to switch context to NULL process, closing program...\n");
        stop(1);
    }

    // Give control to the next process
    runningProcess = nextProcess;
    context_switch(runningProcess->context);
}


/**************************************************************************
   Name - watchdog

   Purpose - The watchdoog keeps the system going when all other
         processes are blocked.  It can be used to detect when the system
         is shutting down as well as when a deadlock condition arises.

   Parameters - none

   Returns - nothing
   *************************************************************************/
static int watchdog(char* dummy)
{
    DebugConsole("watchdog(): called\n");
    while (1)
    {
        check_deadlock();
    }
    return 0;
} 

/* check to determine if deadlock has occurred... */
static void check_deadlock()
{
    if (check_io() == 1)
    {
        return;
    }

    if (boolAvailableProcesses())
    {
        stop(1);
    }
    else
    {
        console_output(false, "All processes completed.\n");
        stop(0);
    }
}

/*
 * Disables the interrupts.
 */
static inline void disableInterrupts()
{

    /* We ARE in kernel mode */


    int psr = get_psr();

    psr = psr & ~PSR_INTERRUPTS;

    set_psr(psr);

} /* disableInterrupts */

/*
 * Enabless the interrupts.
 */
static inline void enableInterrupts()
{
    int psr = get_psr();

    psr = psr & PSR_INTERRUPTS;

    set_psr(psr);

} /* enableInterrupts */

/**************************************************************************
   Name - DebugConsole
   Purpose - Prints  the message to the console_output if in debug mode
   Parameters - format string and va args
   Returns - nothing
   Side Effects -
*************************************************************************/
static void DebugConsole(char* format, ...)
{
    char buffer[2048];
    va_list argptr;

    if (debugFlag)
    {
        va_start(argptr, format);
        vsprintf(buffer, format, argptr);
        console_output(TRUE, buffer);
        va_end(argptr);

    }
}


/* there is no I/O yet, so return false. */
int check_io_scheduler()
{
    return false;
}



/* NEW FUNCTIONS */

/**************************************************************************
   Name - push

   Purpose - The Process node is added to the end of the priority queue
        pointed to by target.

   Parameters - target, a pointer to a priority Queue
                node, a pointed to a node that is to be added to target

   Returns - -1 if an error occurs, otherwise returns the new size of target
   *************************************************************************/
static int push(Queue* target, Process* node)
{
    if (target->size == (MAX_PROCESSES - 1))
    {
        console_output(TRUE, "Could not add Process '%s' to Queue %d, the Queue is full.\n", node->name, target->priority);
        return -1;
    }
    else if (target->priority != node->priority)
    {
        console_output(TRUE, "Could not add Process '%s' to Queue %d, Process and Queue priorities do not match.\n", node->name, target->priority);
        return -1;
    }
    else if (target->size == 0)
    {
        target->head = node;
        target->tail = node;
        target->size += 1;
        return target->size;
    }
    else
    {
        target->tail->nextReadyProcess = node;
        target->tail = node;
        target->size += 1;
        return target->size;
    }
}

/**************************************************************************
   Name - pop

   Purpose - Retrieves the first node of target priority Queue, removes it
        from the Queue, and returns it.

   Parameters - target, a pointer to a priority Queue

   Returns - NULL if an error occurs, otherwise returns the Process popped
        from target
   *************************************************************************/
static Process* pop(Queue* target)
{
    Process* node = NULL;

    if (target->size == 0)
    {
        console_output(TRUE, "Could not pop from Queue %d, the Queue is empty.\n", target->priority);
    }
    else if (target->size == 1)
    {
        node = target->head;
        target->head = NULL;
        target->tail = NULL;
        target->size -= 1;
    }
    else
    {
        node = target->head;
        target->head = node->nextReadyProcess;
        node->nextReadyProcess = NULL;
        target->size -= 1;
    }

    return node;
}

/**************************************************************************
   Name - boolAvailableProcesses

   Purpose - Determines if there are any processes available, excluding 
        Watchdog, and returns a boolean based on the determination.

   Parameters - none

   Returns - true if there are processes available (besides Watchdog), otherwise
        returns false
   *************************************************************************/
static int boolAvailableProcesses()
{
    for (int i = 1; i < NUM_PRIORITIES; i++) // Skip lowest priority
    {
        if (readyLists[i].size > 0)
        {
            return true;
        }
    }
    if (readyLists[0].size > 1) // Watchdog should be running
    {
        return true;
    }

    return false;
}

/**************************************************************************
   Name - isHighestPriorityProcess

   Purpose - Each priority Queue is searched to find any Process that is 
        a higher or equal priority compared to the target's, and returns a 
        boolean reflecting if there is one or not.

   Parameters - target, a pointer to a Process

   Returns - true if no Process is found with a higher or equal priority,
        otherwise returns false
   *************************************************************************/
static int isHighestPriorityProcess(Process* target)
{
    for (int i = HIGHEST_PRIORITY; i >= target->priority; i--)
    {
        if (readyLists[i].size > 0)
        {
            return false;
        }
    }

    return true;
}

/**************************************************************************
   Name - getHighestPriorityProcess

   Purpose - Each priority Queue is searched for the highest priority Process,
        pops it from its Queue, and returns it.

   Parameters - none

   Returns - Null if there are no Processes (which is an error), otherwise
        returns a pointer to the highest priority Process
   *************************************************************************/
static Process* getHighestPriorityProcess()
{
    for (int i = HIGHEST_PRIORITY; i >= 0; i--)
    {
        if (readyLists[i].size > 0)
        {
            return pop(&readyLists[i]);
        }
    }

    return NULL; // This line should never run!
}

/**************************************************************************
   Name - cleanUpChild

   Purpose - This helper function is called by a parent Process to clean up
        and remove a child Process pointed to by target. All pointers to
        it from other Processes cleared or changed, and the child is
        removed from the Process table.

   Parameters - target, a pointer to a Process

   Returns - none
   *************************************************************************/
static void cleanUpChild(Process* target)
{
    Process blankProcess;

    // Remove from parent, if it is the head of the children list
    if (runningProcess->pChildren->pid == target->pid)
    {
        runningProcess->pChildren = target->nextSiblingProcess;
    }
    // Remove from sibling chain if it isn't
    else
    {
        Process* child = runningProcess->pChildren;

        while (child->nextSiblingProcess->pid != target->pid);
        {
            child = child->nextSiblingProcess;
        }

        child->nextSiblingProcess = target->nextSiblingProcess;
    }

    // Clear child from the process table
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == target->pid)
        {
            blankProcess.pid = 0;
            processTable[i] = blankProcess;
        }
    }
}


static void addJoinedProc(Process* target)
{
    // Add target to the current Process' list
    Process* joinedPtr = runningProcess->pJoined;

    if (joinedPtr == NULL)
    {
        joinedPtr = target;
    }
    else
    {
        while (joinedPtr->pJoined != NULL)
        {
            joinedPtr = joinedPtr->pJoined;
        }

        joinedPtr->pJoined = target;
    }
}


static void clock_handler(char* deviceName, uint8_t command, uint32_t status)
{

}