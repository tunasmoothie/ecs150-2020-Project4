#include "Machine.h"
#include "VirtualMachine.h"
#include <unistd.h>
#include <vector>
#include <queue>
#include <cstring>

#include <iostream>

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);

// OBJECTS
//=============================================================
struct Thread{
    TVMThreadID tid;
    TVMThreadState state;
    TVMThreadPriority prio;
    SMachineContext cntx;
    TVMThreadEntry entry;
    void *param;
    void *stackAdr;
    size_t stackSize;
    TVMTick timeup;
    int fileResult;
};

struct TCBComparePrio {
    bool operator()(Thread *lhs, Thread *rhs) {
        return lhs->prio < rhs->prio;
    }
};

struct TCBCompareTimeup {
    bool operator()(Thread *lhs, Thread *rhs) {
        return lhs->timeup > rhs->timeup;
    }
};

struct SharedMem{
    std::vector<void*> memChunks;

    void Initialize(void* baseAdr, TVMMemorySize size){
        int i = 0;
        while(size > 0){
            memChunks.push_back((char*)baseAdr + (i*512));
            size -= 512;
            i++;
        }
    }
};

struct Mutex{
    TVMMutexID mid;
    TVMThreadID owner;
    bool locked;
    std::priority_queue<Thread*, std::vector<Thread*>, TCBComparePrio> waitlist;
};
//=============================================================

// VARIABLES & CONTAINERS
//=============================================================
volatile unsigned int g_tick;
unsigned int tickMS;
TMachineSignalState sigState;
int threadCnt = 0;
int mutexCnt = 0;

SharedMem* sharedMem;
Thread* runningThread;

std::vector<Thread*> threadList;
std::vector<Mutex*> mutexList;
std::priority_queue<Thread*, std::vector<Thread*>, TCBCompareTimeup> waitingThreadList;
std::priority_queue<Thread*, std::vector<Thread*>, TCBComparePrio> readyThreadList;
//=============== ==============================================

// HELPER FUNCTIONS
//=============================================================
//Used as placeholder for main thread
void EmptyMain(void* param){
}

void IdleMain(void* param){
    while(1);
}

#define WAIT_FOR_PRIO        0
#define WAIT_FOR_SLEEP       1
#define WAIT_FOR_FILE        2
#define WAIT_FOR_MUTEX       3
#define THREAD_TERMINATED    4

void threadSchedule(int scheduleType){

    //Gets rid of dead threads from ready list
    while(readyThreadList.top()->state == VM_THREAD_STATE_DEAD){
        readyThreadList.pop();
    }

    if(scheduleType == WAIT_FOR_PRIO){
        if(runningThread->prio < readyThreadList.top()->prio){
            MachineSuspendSignals(&sigState);
            Thread* prev = runningThread;
            Thread* next = readyThreadList.top();
            //std::cout << "-switching from thread " << prev->tid << " to " << next->tid << "\n";
            readyThreadList.pop();
            prev->state = VM_THREAD_STATE_READY;
            readyThreadList.push(prev);
            runningThread = next;
            runningThread->state = VM_THREAD_STATE_RUNNING;
            MachineResumeSignals(&sigState);
            MachineContextSwitch(&prev->cntx, &next->cntx);
        }
    }
    else if(scheduleType == WAIT_FOR_SLEEP){
        MachineSuspendSignals(&sigState);
        Thread* prev = runningThread;
        Thread* next = readyThreadList.top();
        readyThreadList.pop();
        prev->state = VM_THREAD_STATE_READY;
        waitingThreadList.push(prev);
        runningThread = next;
        runningThread->state = VM_THREAD_STATE_RUNNING;
        MachineResumeSignals(&sigState);
        MachineContextSwitch(&prev->cntx, &next->cntx);
    }
    else if(scheduleType == WAIT_FOR_FILE || scheduleType ==  WAIT_FOR_MUTEX){
        MachineSuspendSignals(&sigState);
        Thread* prev = runningThread;
        Thread* next = readyThreadList.top();
        //std::cout << "-switching from thread " << prev->tid << " to " << next->tid << "\n";
        readyThreadList.pop();
        prev->state = VM_THREAD_STATE_WAITING;
        runningThread = next;
        runningThread->state = VM_THREAD_STATE_RUNNING;
        MachineResumeSignals(&sigState);
        MachineContextSwitch(&prev->cntx, &next->cntx);
    }
    else if(scheduleType == THREAD_TERMINATED){
        MachineSuspendSignals(&sigState);
        runningThread = readyThreadList.top();
        readyThreadList.pop();
        runningThread->state = VM_THREAD_STATE_RUNNING;
        SMachineContext tmp;
        MachineResumeSignals(&sigState);
        MachineContextSwitch(&tmp, &runningThread->cntx);
    }
}

void AlarmCallback(void* calldata){
    //std::cout << "-AlARM" << "\n";
    MachineSuspendSignals(&sigState);
    g_tick++;
    if(!waitingThreadList.empty()){
        if(waitingThreadList.top()->timeup <= g_tick){
            Thread* t = waitingThreadList.top();
            waitingThreadList.pop();
            t->state = VM_THREAD_STATE_READY;
            readyThreadList.push(t);
        }
    }
    MachineResumeSignals(&sigState);
    threadSchedule(WAIT_FOR_PRIO);
}

void FileCallback(void* calldata, int result){
    MachineSuspendSignals(&sigState);
    Thread *t = (Thread*)(calldata);
    t->state = VM_THREAD_STATE_READY;
    t->fileResult = result;
    readyThreadList.push(t);
    std::cout << "-Thread " << ((Thread*)(calldata))->tid << " filecallback\n";
    MachineResumeSignals(&sigState);
    threadSchedule(WAIT_FOR_PRIO);
}

// Skeleton function
void ThreadWrapper(void* param){
    Thread* t = (Thread*)(param);
    (t->entry)(t->param);
    VMThreadTerminate(t->tid);
}
//=============================================================


// BASIC OPERATIONS
//=====================================================================================================
TVMStatus VMStart(int tickms, TVMMemorySize sharedsize, int argc, char *argv[]){
    TVMMainEntry main = VMLoadModule(argv[0]);
    tickMS = tickms;

    sharedMem = new SharedMem();
    void* memPtr = MachineInitialize(sharedsize);
    sharedMem->Initialize(memPtr, sharedsize);

    //test = MachineInitialize(sharedsize);
    MachineEnableSignals();
    MachineRequestAlarm(useconds_t(1000 * tickMS), &AlarmCallback, &tickMS);

    //Create main and idle threads, with IDs idle = 0, main = 1, set current thread to 1
    TVMThreadID id0 = 0, id1 = 1;
    VMThreadCreate(IdleMain, NULL, 0x100000, 0, &id0);
    VMThreadCreate(EmptyMain, NULL, 0x100000, VM_THREAD_PRIORITY_NORMAL, &id1);
    VMThreadActivate(id0);
    VMThreadActivate(id1);

    runningThread = readyThreadList.top();
    readyThreadList.pop();
    runningThread->state = VM_THREAD_STATE_RUNNING;

    main(argc, argv);
    MachineTerminate();
    VMUnloadModule();
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int *tickmsref){
    if(tickmsref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    *tickmsref = tickMS;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref){
    if(tickref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    *tickref = g_tick;
    return VM_STATUS_SUCCESS;
}
//=====================================================================================================


// THREAD OPERATIONS
//=====================================================================================================
TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tidRef){
    if (entry == NULL || tidRef == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineSuspendSignals(&sigState);
    //Thread creation
    Thread *t = new Thread();
    t->tid = threadCnt;
    t->state = VM_THREAD_STATE_DEAD;
    t->prio = prio;
    t->entry = entry;
    t->param = param;
    t->stackSize = memsize;
    t->stackAdr = (void *) new uint8_t[memsize];
    threadList.push_back(t);
    *tidRef = threadCnt;
    threadCnt++;

    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadDelete(TVMThreadID threadID){
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state != VM_THREAD_STATE_DEAD){
                return VM_STATUS_ERROR_INVALID_STATE;
            }
            else{
                MachineSuspendSignals(&sigState);
                threadList.erase(it);
                MachineResumeSignals(&sigState);
                threadSchedule(WAIT_FOR_PRIO);
                return VM_STATUS_SUCCESS;
            }
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadActivate(TVMThreadID threadID){
    Thread *t = NULL;
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state != VM_THREAD_STATE_DEAD){
                return VM_STATUS_ERROR_INVALID_STATE;
            }
            else
                t = (*it);
        }
    }
    if(t == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }

    MachineSuspendSignals(&sigState);
    t->state = VM_THREAD_STATE_READY;
    MachineContextCreate(&(t->cntx), &ThreadWrapper, t, t->stackAdr, t->stackSize);
    readyThreadList.push(t);
    MachineResumeSignals(&sigState);

    if(threadID > 1)
        threadSchedule(WAIT_FOR_PRIO);

    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID threadID){
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state == VM_THREAD_STATE_DEAD)
                return VM_STATUS_ERROR_INVALID_STATE;
            else{
                runningThread->state = VM_THREAD_STATE_DEAD;
                threadSchedule(THREAD_TERMINATED);
                return VM_STATUS_SUCCESS;
            }
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref){
    if(threadref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    *threadref = runningThread->tid;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID threadID, TVMThreadStateRef stateref){
    if(stateref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            *stateref = (*it)->state;
            return VM_STATUS_SUCCESS;
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadSleep(TVMTick tick){
    if(tick == VM_TIMEOUT_INFINITE){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    if(tick == VM_TIMEOUT_IMMEDIATE){
        threadSchedule(WAIT_FOR_PRIO);
    }
    else{
        MachineSuspendSignals(&sigState);
        runningThread->timeup = g_tick + tick;
        MachineResumeSignals(&sigState);
        threadSchedule(WAIT_FOR_SLEEP);
    }

    return VM_STATUS_SUCCESS;
}
//=====================================================================================================


// FILE OPERATIONS
//=====================================================================================================
TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
    if(filename == NULL || filedescriptor == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineFileOpen(filename, flags, mode, &FileCallback, runningThread);

    threadSchedule(WAIT_FOR_FILE);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *filedescriptor = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileClose(int filedescriptor){
    MachineFileClose(filedescriptor, &FileCallback, runningThread);

    threadSchedule(WAIT_FOR_FILE);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
    if(data == NULL || length == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    void* mem = NULL;
    if(!sharedMem->memChunks.empty()){
        mem = sharedMem->memChunks.back();
        sharedMem->memChunks.pop_back();
    }
    MachineFileRead(filedescriptor, mem, *length, &FileCallback, runningThread);
    threadSchedule(WAIT_FOR_FILE);
    memcpy(data, mem, *length*sizeof(char));   //TEMP FIX FOR STACK SMASHING, CAUSED BY MEMORY CHUNK < 512

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *length = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length){
    if(data == NULL || length == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineSuspendSignals(&sigState);
    void* mem = NULL;
    if(!sharedMem->memChunks.empty()){
        mem = sharedMem->memChunks.back();
        sharedMem->memChunks.pop_back();
    }
    memcpy(mem, data, *length);
    MachineFileWrite(filedescriptor, mem, *length, &FileCallback, runningThread);

    MachineResumeSignals(&sigState);
    threadSchedule(WAIT_FOR_FILE);
    //std::cout << "-back to thread " << runningThread->tid << "\n";

    sharedMem->memChunks.push_back(mem);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *length = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
    MachineFileSeek(filedescriptor, offset, whence, &FileCallback, runningThread);

    threadSchedule(WAIT_FOR_FILE);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *newoffset = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}
//=====================================================================================================


// MUTEX OPERATIONS
//=====================================================================================================
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref){
    if(mutexref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    Mutex* m = new Mutex();
    m->mid = mutexCnt;
    m->owner = 0;
    m->locked = false;
    mutexList.push_back(m);

    *mutexref = mutexCnt;
    mutexCnt++;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutexID){
    for(auto it = mutexList.begin(); it != mutexList.end(); ++it){
        if((*it)->mid == mutexID){
            if((*it)->locked)
                return VM_STATUS_ERROR_INVALID_STATE;
            else{
                mutexList.erase(it);
                return VM_STATUS_SUCCESS;
            }
        }
    }

    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMMutexQuery(TVMMutexID mutexID, TVMThreadIDRef ownerref){
    if(ownerref == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    for(auto it = mutexList.begin(); it != mutexList.end(); ++it){
        if((*it)->mid == mutexID){
            *ownerref = (*it)->locked ? (*it)->owner : VM_THREAD_ID_INVALID;
            return VM_STATUS_SUCCESS;
        }
    }

    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMMutexAcquire(TVMMutexID mutexID, TVMTick timeout){
    for(auto it = mutexList.begin(); it != mutexList.end(); ++it){
        if((*it)->mid == mutexID){
            unsigned int timeup = g_tick + timeout;
            while(1){
                //Mutex not already locked
                if(!(*it)->locked){
                    MachineSuspendSignals(&sigState);
                    (*it)->owner = runningThread->tid;
                    (*it)->locked = true;
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_SUCCESS;
                }
                    //Mutex already locked
                else{
                    MachineSuspendSignals(&sigState);
                    (*it)->waitlist.push(runningThread);
                    MachineResumeSignals(&sigState);
                    threadSchedule(WAIT_FOR_MUTEX);
                    if(timeup != VM_TIMEOUT_INFINITE && g_tick > timeup){
                        return VM_STATUS_FAILURE;
                    }
                    else
                        continue;
                }
            }
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}


TVMStatus VMMutexRelease(TVMMutexID mutexID){
    for(auto it = mutexList.begin(); it != mutexList.end(); ++it){
        if((*it)->mid == mutexID){
            if((*it)->owner != runningThread->tid){
                return VM_STATUS_ERROR_INVALID_STATE;
            }

            MachineSuspendSignals(&sigState);
            (*it)->locked = false;
            (*it)->owner = 0;
            MachineResumeSignals(&sigState);

            if((*it)->waitlist.empty())
                return VM_STATUS_SUCCESS;

            else if((*it)->waitlist.top()->prio > runningThread->prio){
                MachineSuspendSignals(&sigState);
                readyThreadList.push((*it)->waitlist.top());
                (*it)->waitlist.pop();
                MachineResumeSignals(&sigState);
                threadSchedule(WAIT_FOR_PRIO);
                return VM_STATUS_SUCCESS;
            }
            else {
                MachineSuspendSignals(&sigState);
                readyThreadList.push((*it)->waitlist.top());
                (*it)->waitlist.pop();
                MachineResumeSignals(&sigState);
                return VM_STATUS_SUCCESS;
            }
        }
    }
    return VM_STATUS_ERROR_INVALID_ID;
}
//=====================================================================================================
}