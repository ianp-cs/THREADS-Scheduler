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
void display_process_table();
static int getParentPid(int index);    // New function, placed here for organizational purposes
static char* getStatusStr(int index);  // New function, placed here for organizational purposes
static int getTotalCPUTime(int index); // New function, placed here for organizational purposes

/* New functions */
static int readyListPush(Queue* target, Process* node);
static Process* readyListPop(Queue* target);
static int boolAvailableProcesses();
static int isHighestPriorityProcess(Process* target);
static Process* getHighestPriorityProcess();
static void cleanUpChild(Process* target);
static void addJoinedProc(Process* target);
static void clock_handler(char* deviceName, uint8_t command, uint32_t status);
static void time_slice();
static bool isJoined(Process* target);
static bool hasChild(Process* target);
static bool hasActiveChild(Process* target);
static void freeJoinedProcesses(Process* target);
bool checkKernelMode();
static bool isScheduler();
static int exitedListPush(Process* target, Process* child);
static Process* exitedListPop(Process* target);

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

    if (!checkKernelMode())
    {
        console_output(false, "spawn(): Not in Kernel Mode.\n");
        stop(1);
    }

    DebugConsole("spawn(): creating process %s\n", name);

    disableInterrupts();

    /* Validate all of the parameters*/
    if (name == NULL)
    {
        console_output(false, "spawn(): Name value is NULL.\n");
        enableInterrupts();
        return -1;
    }
    if (strlen(name) >= (MAXNAME - 1))
    {
        console_output(false, "spawn(): Process name is too long.  Halting...\n");
        stop(1);
    }
    if (stacksize < THREADS_MIN_STACK_SIZE)
    {
        console_output(false, "spawn(): Stack size is too small.\n");
        enableInterrupts();
        return -2;
    }
    if (priority < LOWEST_PRIORITY || priority > HIGHEST_PRIORITY)
    {
        console_output(false, "spawn(): Invalid priority, priority out of range.\n");
        enableInterrupts();
        return -3;
    }

    /* Find an empty slot in the process table and save the index*/
    for (int i = 0; i < MAX_PROCESSES + 1; i++)
    {
        if (i == MAX_PROCESSES)
        {
            console_output(false, "spawn(): Failed to create process, the process table is full.\n");
            enableInterrupts();
            return -4;
        }
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
    nextPid++;  
    pNewProc->entryPoint = entryPoint;
    pNewProc->priority = priority;
    pNewProc->stacksize = stacksize;
    pNewProc->status = READY;
    pNewProc->exitCode = 0;
    pNewProc->signaled = 0;
    pNewProc->accumulatedTime = 0;
    pNewProc->runTime = 0;
    pNewProc->numChildren = 0;

    // Some processes don't have args, so we need to account for NULL
    if (arg != NULL)
    {
        strcpy(pNewProc->startArgs, arg);
    }

    
    // If there is a parent process, link the parent and this process to each other, and 
    // increment parent's children counter.
    if (runningProcess != NULL)
    {
        if (!hasChild(runningProcess)) 
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
        (pNewProc->pParent->numChildren)++;
    }

    /* Add the process to the ready list. */
    if (readyListPush(&readyLists[pNewProc->priority], pNewProc) == 0)
    {
        return -5;
    }

    /* 
    Initialize context for this process, but use launch function pointer for
    the initial value of the process's program counter (PC)
    */
    pNewProc->context = context_initialize(launch, stacksize, arg);

    // Set the proccess start time
    pNewProc->startTime = read_clock();

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
    if (!checkKernelMode())
    {
        console_output(false, "k_wait(): Not in Kernel Mode.\n");
        stop(1);
    }
    
    int result = 0;
    Process* child;

    // Case 1: Process has no children
    // Return error code
    if (runningProcess->pChildren == 0) 
    {
        return -1;
    }

    // Case 2: a child has already exited
    // Gather exit information from child, clean up the child, and return
    if (runningProcess->numExited > 0)
    {
        child = exitedListPop(runningProcess);
        result = child->pid;
        *code = child->exitCode;
        cleanUpChild(child);
        return result;
    }

    // Case 3: no child has exited yet and this process must wait
    // Block and wait for signal from a child, then
    // Gather exit information from child, clean up the child, check to see if signaled, and return

    block(BLOCKED); // Block the parent and wait for control to be returned

    if (runningProcess->numExited > 0)
    {
        child = exitedListPop(runningProcess);
        result = child->pid;
        *code = child->exitCode;
        cleanUpChild(child);
    }

    if (signaled()) // Signaled while waiting, return error code -5
    {
        return -5;
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
    if (!checkKernelMode())
    {
        console_output(false, "k_exit(): Not in Kernel Mode.\n");
        stop(1);
    }

    // Exiting process should not have children, halt program if it does
    if (hasActiveChild(runningProcess)) 
    {
        console_output(false, "k_exit(): Exiting process has active children, closing program.\n");
        stop(1);
    }

    // Check to see if this process has been signaled and set its exit code accordingly
    if (signaled()) 
    {
        runningProcess->exitCode = -5;
    }
    else
    {
        runningProcess->exitCode = code;
    }

    // Signal to parent that this process should no longer be considered active and it needs to be cleaned up
    if (runningProcess->pParent != NULL)
    {
        (runningProcess->pParent->numChildren)--;
        runningProcess->status = QUIT;
        exitedListPush(runningProcess->pParent, runningProcess);
    }

    // If the process has a blocked and unjoined parent, then unblock it
    // If this process has joined processees, the parent will free them
    if (runningProcess->pParent != NULL && 
        runningProcess->pParent->status >= BLOCKED &&
        !isJoined(runningProcess->pParent)) 
    {
        unblock(runningProcess->pParent->pid);
    }

    // If the process has joined processes, free and unblock them
    if (runningProcess->numJoined > 0)
    {
        freeJoinedProcesses(runningProcess);
    }

    // If this process is the scheduler, change status to quit
    if (isScheduler())
    {
        runningProcess->status = QUIT;
    }

    // Surrender control and wait to be cleaned up
    dispatcher();
}

/**************************************************************************
   Name - k_kill

   Purpose - Signals a process with the specified signal

   Parameters - Signal to send

   Returns - 0 if successful or 1 if signaled
*************************************************************************/
int k_kill(int pid, int signal)
{
    if (!checkKernelMode())
    {
        console_output(false, "k_kill(): Not in Kernel Mode.\n");
        stop(1);
    }

    Process* target = NULL;

    // Input validation
    if (signal != SIG_TERM)
    {
        console_output(debugFlag, "k_kill(): Invalid signal code, closing program.\n");
        stop(1);
    }

    // Find targeted process
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    // Pid does not match an existing process, halt program
    if (target == NULL)
    {
        console_output(debugFlag, "k_kill(): Attempted to signal non-existant process, closing program.\n");
        stop(1);
    }

    // Targeted process has already been signaled
    if (target->signaled)
    {
        return 1;
    }
    
    target->signaled = 1;
    return 0;
}

/**************************************************************************
   Name - k_getpid

   Purpose - Returns the pid of the currently active process

   Parameters - None

   Returns - Returns the pid of the currently active process
*************************************************************************/
int k_getpid()
{
    if (!checkKernelMode())
    {
        console_output(false, "k_getpid(): Not in Kernel Mode.\n");
        stop(1);
    }

    return runningProcess->pid;
}

/**************************************************************************
   Name - k_join

   Purpose - The function waits for the specified process to terminate and retrieves its exit code.

   Parameters - The pid of the target process the active process will join with and the exit code from 
                the target process after the join has ended. 

   Returns - 0 if successful or -5 if the process was signaled while waiting
***************************************************************************/
int k_join(int pid, int* pChildExitCode)
{
    if (!checkKernelMode())
    {
        console_output(false, "k_join(): Not in Kernel Mode.\n");
        stop(1);
    }

    Process* target = NULL;

    // Input Validation
    if (k_getpid() == pid)
    {
        console_output(false, "k_join(): Attempted to join process to itself, closing program.\n");
        stop(1);
    }

    
    if (runningProcess->pParent->pid == pid)
    {
        console_output(false, "k_join(): Attempted to join process to its parent, closing program.\n");
        stop(2);
    }

    // Find targeted process
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    // Pid does not match an existing process, halt program
    if (target == NULL)
    {
        console_output(false, "k_join(): Attempted to join process to a non-existant process, closing program.\n");
        stop(3);
    }

    // Join Processes
    addJoinedProc(target);
    runningProcess->joined = true;
    block(BLOCKED);

    // Retrieve joined process' exit code
    *pChildExitCode = runningProcess->endJoinCode;

    if (signaled())
    {
        return -5;
    }

    return 0;
}

/**************************************************************************
   Name - unblock

   Purpose - The unblock function returns a blocked process to the ready to run state before handing
             control back to the dispatcher

   Parameters - The pid of the target process to be unblocked

   Returns - Returns 0 if successful or -1 if the target process is not blocked or target pid is invalid
*************************************************************************/
int unblock(int pid)
{
    if (!checkKernelMode())
    {
        console_output(false, "unblock(): Not in Kernel Mode.\n");
        stop(1);
    }

    Process* target = NULL;

    // Find targeted process
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == pid)
        {
            target = &processTable[i];
            break;
        }
    }

    // Pid does not match an existing process or the process isn't blocked
    if (target == NULL || target->status < BLOCKED) 
    {
        return -1;
    }

    // Unblock Process
    target->status = READY;
    readyListPush(&readyLists[target->priority], target);

    dispatcher();

    return 0;
}

/*************************************************************************
   Name - block

   Purpose - This function blocks the calling process and sets the processes status in the process table 
             to the value specified in block_status. The value of block_status must be greater than 10. 
             The values of 0-10 are reserved for internal kernel process states.

   Parameters - The block status to be assigned to the blocked process

   Returns - Returns 0 if successful or -5 if the process was signaled while blocked
*************************************************************************/
int block(int newStatus)
{
    if (!checkKernelMode())
    {
        console_output(false, "block(): Not in Kernel Mode.\n");
        stop(1);
    }

    // Input validation
    if (newStatus < 10)
    {
        console_output(false, "block(): Attempting to block with a reserved status value, closing program.\n");
        stop(1);
    }

    // Set new status and hand off control to dispatcher
    runningProcess->status = newStatus;
    dispatcher();

    // The process is now unblocked and continues running
    if (signaled())
    {
        return -5;
    }

    return 0;
}

/*************************************************************************
   Name - signaled

   Purpose - Checks if the calling process has been signaled.

   Parameters - None

   Returns - Returns 1 if the process has been signaled, otherwise returns 0
*************************************************************************/
int signaled()
{
    if (!checkKernelMode())
    {
        console_output(false, "signaled(): Not in Kernel Mode.\n");
        stop(1);
    }

    return runningProcess->signaled;
}
/*************************************************************************
   Name - readtime

   Purpose - The read_time function returns the amount of time in milliseconds that the current 
             process has spent on the CPU. 

   Parameters - None

   Returns - Returns the calling process' CPU time
*************************************************************************/
int read_time()
{
    if (!checkKernelMode())
    {
        console_output(false, "read_time(): Not in Kernel Mode.\n");
        stop(1);
    }

    if (runningProcess != NULL)
    {
        // Divide by 1000 to convert from microseconds to milliseconds
        return (read_clock() - runningProcess->runTime) / 1000; 
    }

    return 0;
}

/*************************************************************************
   Name - readClock

   Purpose - This function returns the current value of the system clock using the THREADS 
             system_clock function

   Parameters - None

   Returns - The function returns the value of system_clock. 
*************************************************************************/
DWORD read_clock()
{
    return system_clock();
}

/*************************************************************************
   Name - display_process_table

   Purpose - This function displays all non-empty processes in the process table. The displayed 
             information includes the process Id (PID), parent PID, Priority, status, number of 
             children,  cpu time, and process name.

   Parameters - None

   Returns - None
*************************************************************************/
void display_process_table()
{
    if (!checkKernelMode())
    {
        console_output(false, "display_process_table(): Not in Kernel Mode.\n");
        stop(1);
    }

    console_output(0, "\nPID    Parent    Priority    Status        # Kids       CPUtime         Name\n");
    console_output(0, "------------------------------------------------------------------------------\n");

    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid > 0)
        {
            console_output(0, "%-7d%-10d%-12d%-14s%-13d%-16d%s\n",
                processTable[i].pid, getParentPid(i), processTable[i].priority, getStatusStr(i),
                processTable[i].numChildren, processTable[i].accumulatedTime, processTable[i].name);
        }
    }

    console_output(0, "\n");
}

/*************************************************************************
   Name - getParentPid

   Purpose - This helper function returns the pid of the target process' parent

   Parameters - The index of the target process within the process table

   Returns - The pid of the parent process or -1 if the target process has no parent
*************************************************************************/
static int getParentPid(int index)
{
    if (processTable[index].pParent == NULL)
    {
        return -1;
    }
    else
    {
        return processTable[index].pParent->pid;
    }
}

/*************************************************************************
   Name - getStatusStr

   Purpose - This helper function converts the target process' status code into readable text and
             returns it.

   Parameters - The index of the target process within the process table

   Returns - The text equivalent of the target process' status code
*************************************************************************/
static char* getStatusStr(int index)
{
    if (processTable[index].status == READY)
    {
        return "READY";
    }
    else if (processTable[index].status == QUIT)
    {
        return "QUIT";
    }
    else if (processTable[index].status == RUNNING)
    {
        return "RUNNING";
    }
    else if (processTable[index].status >= BLOCKED)
    {
        return "BLOCKED";
    }
    else
    {
        return "ERROR";
    }
}

/**************************************************************************
   Name - dispatcher

   Purpose - This is where context changes to the next process to run.

   Parameters - none

   Returns - nothing

*************************************************************************/
void dispatcher()
{
    if (!checkKernelMode())
    {
        console_output(false, "dispatcher(): Not in Kernel Mode.\n");
        stop(1);
    }

    // Add to accumulated time for the process
    if (runningProcess != NULL)
    {
        runningProcess->accumulatedTime += read_time();
    }

    // Points to the next process that should run
    Process* nextProcess = NULL; 

    // No process was running
    if (runningProcess == NULL) 
    {
        nextProcess = getHighestPriorityProcess();
    }
    // Current process has been blocked by another process or has exited, do not add to readyLists
    else if (runningProcess->status >= BLOCKED || runningProcess->status == QUIT)
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
        readyListPush(&readyLists[previousProcess->priority], previousProcess);

        nextProcess = getHighestPriorityProcess();
    }

    if (nextProcess == NULL)
    {
        console_output(debugFlag, "Dispatcher(): Attempting to switch context to NULL process, closing program...\n");
        stop(1);
    }

    // Give control to the next process and set its run time 
    runningProcess = nextProcess;
    runningProcess->runTime = read_clock();
    runningProcess->status = RUNNING;

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

/**************************************************************************
   Name - check_deadlock

   Purpose - This functions checks for deadlock, and if none is found, it ends the program 
             gracefully. If there is no deadlock, but there are active functions running
             besides watchdog, the program is halted with an error.

   Parameters - none

   Returns - nothing
   *************************************************************************/
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

/**************************************************************************
   Name - disableInterrupts

   Purpose - This functions edits the psr to temporarily disable interrupts, which is necessary when
             working in critical regions.

   Parameters - none

   Returns - nothing
   *************************************************************************/
static inline void disableInterrupts()
{
    int psr = get_psr();

    psr = psr & ~PSR_INTERRUPTS;

    set_psr(psr);

} 

/**************************************************************************
   Name - enableInterrupts

   Purpose - This functions edits the psr to turn interrupts back on, called after finishing work
             within critical regions.

   Parameters - none

   Returns - nothing
   *************************************************************************/
static inline void enableInterrupts()
{
    int psr = get_psr();

    psr = psr | PSR_INTERRUPTS;

    set_psr(psr);

} 

/**************************************************************************
   Name - DebugConsole

   Purpose - Prints the message to the console_output if in debug mode

   Parameters - format string and va args

   Returns - nothing
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
    if (!checkKernelMode())
    {
        console_output(false, "check_io_scheduler(): Not in Kernel Mode.\n");
        stop(1);
    }

    return false;
}

/* NEW FUNCTIONS */

/**************************************************************************
   Name - readyListPush

   Purpose - The Process node is added to the end of the priority queue
             pointed to by target.

   Parameters - target, a pointer to a priority Queue
                node, a pointed to a node that is to be added to target

   Returns - -1 if an error occurs, otherwise returns the new size of target
   *************************************************************************/
static int readyListPush(Queue* target, Process* node)
{
    disableInterrupts();

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

    enableInterrupts();
}

/**************************************************************************
   Name - readyListPop

   Purpose - Retrieves the first node of target priority Queue, removes it
        from the Queue, and returns it.

   Parameters - target, a pointer to a priority Queue

   Returns - NULL if an error occurs, otherwise returns the Process readyListPopped
        from target
   *************************************************************************/
static Process* readyListPop(Queue* target)
{
    disableInterrupts();

    Process* node = NULL;

    if (target->size == 0)
    {
        console_output(TRUE, "Could not readyListPop from Queue %d, the Queue is empty.\n", target->priority);
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

    enableInterrupts();

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
    // Skip lowest priority
    for (int i = 1; i < NUM_PRIORITIES; i++) 
    {
        if (readyLists[i].size > 0)
        {
            return true;
        }
    }

    // Watchdog should be running, so don't count it
    if (readyLists[0].size > 1) 
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
        readyListPops it from its Queue, and returns it.

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
            return readyListPop(&readyLists[i]);
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
    // Remove from parent, if it is the head of the children list
    if (runningProcess->pChildren->pid == target->pid)
    {
        runningProcess->pChildren = target->nextSiblingProcess;
    }
    // Remove from sibling chain if it isn't
    else
    {
        Process* child = runningProcess->pChildren;

        while (child->nextSiblingProcess->pid != target->pid)
        {
            child = child->nextSiblingProcess;
        }

        child->nextSiblingProcess = target->nextSiblingProcess;
    }

    // Free any joined processes the child may still have
    if (target->numJoined > 0)
    {
        freeJoinedProcesses(target);
    }

    // Clear child from the process table
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processTable[i].pid == target->pid)
        {
            processTable[i].nextReadyProcess = NULL;
            processTable[i].nextSiblingProcess = NULL;
            processTable[i].pParent = NULL;
            processTable[i].pChildren = NULL;

            strcpy(processTable[i].name, "");
            strcpy(processTable[i].startArgs, "");;
            processTable[i].context = NULL;
            processTable[i].pid = 0;
            processTable[i].priority = 0;
            processTable[i].entryPoint = NULL;
            processTable[i].stack = '\0';
            processTable[i].stacksize = 0;
            processTable[i].status = 0;
            processTable[i].exitCode = 0;
            processTable[i].signaled = 0;
            processTable[i].startTime = 0;
            processTable[i].runTime = read_clock();
            processTable[i].numChildren = 0;
        }
    }
}

/**************************************************************************
   Name - addJoinedProc

   Purpose - Adds the currently active process to target's list of joined processes

   Parameters - The target process who the active process joins with

   Returns - none

   Note - This functionality works under the assumption that a process will only free its joined
          processes once, specifically when it is exiting or being cleaned up by its parent. This
          functionality may break if a process can free its joined processes multiple times in its
          lifetime. 
   *************************************************************************/
static void addJoinedProc(Process* target)
{
    // Add the current Process' to target's joined list and increment joined count
    target->pJoined[target->numJoined] = runningProcess;
    (target->numJoined)++;
}

/**************************************************************************
   Name - clock_handler

   Purpose - This function is used by the interrupt handler to create time slices for processes
             and facilitate the round-robin with priority system for CPU time.

   Note - Not sure exactly what the parameters are used for, since a lot of the interrupt handler is
          hidden within libraries we can't access. Assume the parameters are needed, even if they
          aren't used within the function.
   *************************************************************************/
static void clock_handler(char* deviceName, uint8_t command, uint32_t status)
{
    time_slice();
}

/**************************************************************************
   Name - time_slice

   Purpose - This function checks to see if the active running process has used up its alloted CPU
             time. If it has, the dispatcher is called, otherwise, the process continues running.

   Parameters - none

   Returns - none
   *************************************************************************/
void time_slice()
{
    if (read_time() >= 80)
    {
        dispatcher();
    }
}

/**************************************************************************
   Name - isJoined

   Purpose - This function checks if the target process has been joined with another

   Parameters - the target process to check

   Returns - true if target is joined, otherwise returns false
   *************************************************************************/
static bool isJoined(Process* target)
{
    return target->joined;
}

/**************************************************************************
   Name - hasChild

   Purpose - This function checks if the target process has any children, including ones waiting
             to be cleaned up.

   Parameters - the target process to check

   Returns - true if target has children, otherwise returns false

   Note - This looks similar to hasActiveChild, but IT IS different!
   *************************************************************************/
static bool hasChild(Process* target)
{
    if (target->pChildren != NULL)
    {
        return true;
    }

    return false;
}

/**************************************************************************
   Name - hasActiveChild

   Purpose - This function checks if the target process has any children who are not waiting to be
             cleaned up.

   Parameters - the target process to check

   Returns - true if target is joined, otherwise returns false

   Note - This looks similar to hasChild, but IT IS different!
   *************************************************************************/
static bool hasActiveChild(Process* target)
{
    if (target->numChildren > 0)
    {
        return true;
    }

    return false;
}

/**************************************************************************
   Name - isJoined

   Purpose - This function checks if the target process has been joined with another

   Parameters - the target process to check

   Returns - true if target is joined, otherwise returns false
   *************************************************************************/
static void freeJoinedProcesses(Process* target)
{
    for (int i = 0; i < target->numJoined; i++)
    {
        Process* joinee = target->pJoined[i];
        joinee->joined = false;
        joinee->endJoinCode = target->exitCode;
        unblock(joinee->pid);
    }
}

/**************************************************************************
   Name - checkKernelMode

   Purpose - This function checks the psr to ensure the OS is in kernel mode and not user mode

   Parameters - None

   Returns - true if in kernel mode, otherwise returns false
   *************************************************************************/
bool checkKernelMode()
{
    int psr = get_psr() & PSR_KERNEL_MODE;

    if (psr != PSR_KERNEL_MODE)
    {
        return false;
    }

    return true;
}

/**************************************************************************
   Name - isScheduler

   Purpose - This function checks if the currently active process is the Scheduler

   Parameters - None

   Returns - true if the current process is the Scheduler, otherwise returns false
   *************************************************************************/
static bool isScheduler()
{
    // 2 = Scheduler pid
    if (runningProcess->pid == 2)
    {
        return true;
    }

    return false;
}

/**************************************************************************
   Name - exitedListPush

   Purpose - This function adds the child process to the end of target process' list of exiting 
             children. The list is a FIFO list.

   Parameters - the child process that is exiting and the target process that is the parent of child

   Returns - returns 1 if successful or 0 if an error occurs
   *************************************************************************/
static int exitedListPush(Process* target, Process* child)
{
    disableInterrupts();

    // Input validation
    if (target == NULL)
    {
        console_output(false, "exitedListPush(): Target points to NULL.\n");
        return 0;
    }

    if (child == NULL)
    {
        console_output(false, "exitedListPush(): Child points to NULL.\n");
        return 0;
    }

    if (target->numExited == MAX_PROCESSES)
    {
        console_output(false, "exitedListPush(): Exited list for this process is full.\n");
        return 0;
    }

    // Add child to list and increment exited children counter
    target->pExited[target->numExited] = child;
    (target->numExited)++;

    enableInterrupts();

    return 1;
}

/**************************************************************************
   Name - exitedListPop

   Purpose - This function removes the first exiting process in target's exiting children list and
             returns it. The list is a FIFO list.

   Parameters - the target process to remove an exiting child from

   Returns - a pointer to the exiting child process or NULL if an error occurred
   *************************************************************************/
static Process* exitedListPop(Process* target)
{
    disableInterrupts();

    Process* child = NULL;

    // Input validation
    if (target == NULL)
    {
        console_output(false, "exitedListPop(): Target points to NULL.\n");
        return NULL;
    }

    if (target->numExited == 0)
    {
        console_output(false, "exitedListPop(): Target has no children to pop.\n");
        return NULL;
    }

    // Get first child in list
    child = target->pExited[0];

    // Shift list to the left and decrement exited children counter
    for (int i = 0; i < (target->numExited) - 1; i++)
    {
        target->pExited[i] = target->pExited[i + 1];
    }

    target->pExited[(target->numExited) - 1] = NULL;
    (target->numExited)--;

    enableInterrupts();

    return child;
}
