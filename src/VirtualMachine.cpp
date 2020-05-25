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
//=============================================================

// VARIABLES & CONTAINERS
//=============================================================
volatile unsigned int g_tick;
unsigned int tickMS;
TMachineSignalState sigState;
int threadCnt = 0;
void* sharedMem;

Thread* runningThread;
std::vector<Thread*> threadList;
//std::priority_queue<TCB*, std::vector<TCB*>, TCBCompare> deadThreadList;
std::priority_queue<Thread*, std::vector<Thread*>, TCBCompareTimeup> waitingThreadList;
std::priority_queue<Thread*, std::vector<Thread*>, TCBComparePrio> readyThreadList;
//=============================================================

// HELPER FUNCTIONS
//=============================================================
void EmptyMain(void* param){
}

void IdleMain(void* param){
    MachineEnableSignals();
    //std::cout << "-Idling..." << "\n";
    while(1);
}

void threadSchedule(TVMThreadState prevThreadState){
    //MachineSuspendSignals(&sigState);
    //Gets rid of dead threads from ready list
    while(readyThreadList.top()->state == VM_THREAD_STATE_DEAD){
        readyThreadList.pop();
    }

    if(prevThreadState == VM_THREAD_STATE_READY){
        if(runningThread->prio < readyThreadList.top()->prio){
            Thread* prev = runningThread;
            Thread* next = readyThreadList.top();
            readyThreadList.pop();
            prev->state = VM_THREAD_STATE_READY;
            readyThreadList.push(prev);
            runningThread = next;
            runningThread->state = VM_THREAD_STATE_RUNNING;
            MachineResumeSignals(&sigState);
            MachineContextSwitch(&prev->cntx, &next->cntx);
        }
    }
    else if(prevThreadState == VM_THREAD_STATE_WAITING){
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
    else if(prevThreadState == VM_THREAD_STATE_DEAD){
        Thread* prev = runningThread;
        runningThread = readyThreadList.top();
        readyThreadList.pop();
        runningThread->state = VM_THREAD_STATE_RUNNING;
        MachineResumeSignals(&sigState);
        MachineContextSwitch(&prev->cntx, &runningThread->cntx);
    }
    MachineResumeSignals(&sigState);
}

void AlarmCallback(void* calldata){
    //std::cout << "-ALARM" << "\n";
    g_tick++;
    MachineSuspendSignals(&sigState);
    if(waitingThreadList.size() != 0){
        if(waitingThreadList.top()->timeup <= g_tick){
            Thread* prev = runningThread;
            Thread* next = waitingThreadList.top();

            waitingThreadList.pop();
            prev->state = VM_THREAD_STATE_READY;
            next->state = VM_THREAD_STATE_RUNNING;
            readyThreadList.push(prev);

            runningThread = next;

            MachineResumeSignals(&sigState);
            MachineContextSwitch(&prev->cntx, &next->cntx);
        }
    }
}

void FileCallback(void* calldata, int result){
    MachineSuspendSignals(&sigState);
    Thread *t = (Thread*)(calldata);
    t->state = VM_THREAD_STATE_READY;
    t->fileResult = result;
    readyThreadList.push(t);
    MachineResumeSignals(&sigState);
    threadSchedule(VM_THREAD_STATE_READY);
}

// Skeleton function
void ThreadWrapper(void* param){
    Thread* t = (Thread*)(param);
    (t->entry)(t->param);

    //if(tcb->tid != 0)
    t->state = VM_THREAD_STATE_DEAD;
    VMThreadTerminate(t->tid);
}
//=============================================================


// BASIC OPERATIONS
//=====================================================================================================
TVMStatus VMStart(int tickms, int argc, char *argv[]){
    TVMMainEntry main = VMLoadModule(argv[0]);
    tickMS = tickms;
    MachineInitialize();
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
    MachineSuspendSignals(&sigState);
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state != VM_THREAD_STATE_DEAD){
                MachineResumeSignals(&sigState);
               return VM_STATUS_ERROR_INVALID_STATE;
            }
            else{
                threadList.erase(it);
                threadSchedule(VM_THREAD_STATE_READY);
                MachineResumeSignals(&sigState);
                return VM_STATUS_SUCCESS;
            }
        }
    }
    MachineResumeSignals(&sigState);
    return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadActivate(TVMThreadID threadID){
    MachineSuspendSignals(&sigState);
    Thread *t = NULL;
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state != VM_THREAD_STATE_DEAD){
                MachineResumeSignals(&sigState);
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

    t->state = VM_THREAD_STATE_READY;
    MachineContextCreate(&(t->cntx), &ThreadWrapper, t, t->stackAdr, t->stackSize);
    readyThreadList.push(t);

    if(threadID > 1){
        MachineResumeSignals(&sigState);
        threadSchedule(VM_THREAD_STATE_READY);
    }

    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID threadID){
    MachineSuspendSignals(&sigState);
    for(auto it = threadList.begin(); it != threadList.end(); ++it){
        if((*it)->tid == threadID){
            if((*it)->state != VM_THREAD_STATE_DEAD){
                MachineResumeSignals(&sigState);
                return VM_STATUS_ERROR_INVALID_STATE;
            }
            else{
                //std::cout << "-terminating thread " << (*it)->tid << "\n";
                if(threadID == runningThread->tid){
                    //std::cout << "-terminating thread " << (*it)->tid << "\n";
                    threadSchedule(VM_THREAD_STATE_DEAD);
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_SUCCESS;
                }
                else{
                    threadSchedule(VM_THREAD_STATE_READY);
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_SUCCESS;
                }
            }
        }
    }
    MachineResumeSignals(&sigState);
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

    MachineSuspendSignals(&sigState);
    runningThread->timeup = g_tick + tick;
    threadSchedule(VM_THREAD_STATE_WAITING);
    MachineResumeSignals(&sigState);
    return VM_STATUS_SUCCESS;
}
//=====================================================================================================



// FILE OPERATIONS
//=====================================================================================================
TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
    if(filename == NULL || filedescriptor == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineFileOpen(filename, flags, mode, &FileCallback, runningThread);

    threadSchedule(VM_THREAD_STATE_DEAD);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *filedescriptor = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileClose(int filedescriptor){
    MachineFileClose(filedescriptor, &FileCallback, runningThread);

    threadSchedule(VM_THREAD_STATE_DEAD);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
    if(data == NULL || length == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    MachineFileRead(filedescriptor, data, *length, &FileCallback, runningThread);

    threadSchedule(VM_THREAD_STATE_DEAD);

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

    MachineFileWrite(filedescriptor, data, *length, &FileCallback, runningThread);

    threadSchedule(VM_THREAD_STATE_DEAD);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *length = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
    MachineFileSeek(filedescriptor, offset, whence, &FileCallback, runningThread);

    threadSchedule(VM_THREAD_STATE_DEAD);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *newoffset = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}
//=====================================================================================================

}