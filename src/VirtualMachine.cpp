#include "Machine.h"
#include "VirtualMachine.h"
#include <unistd.h>
#include <vector>
#include <queue>
#include <cstring>
#include <sys/types.h>
#include <fcntl.h>

#include <iostream>
#include <iomanip>

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);
TVMStatus VMDateTime(SVMDateTimeRef curdatetime);
void ArrayCopy(const uint8_t* src, uint8_t* dest, int index, int len);
TVMStatus FileSeek(int filedescriptor, int offset, int whence, int *newoffset);
TVMStatus FileRead(int filedescriptor, void *data, int *length);

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

#pragma pack(1)
struct BPB {
    uint8_t BS_jmpBoot[3];
    char BS_OEMName[8];
    int BPB_BytsPerSec = 0;
    int BPB_SecPerClus = 0;
    int BPB_RsvdSecCnt = 0;
    int BPB_NumFATs = 0;
    int BPB_RootEntCnt = 0;
    int BPB_TotSec16 = 0;
    int BPB_Media;
    int BPB_FATSz16;
    int BPB_SecPerTrk;
    int BPB_NumHeads;
    int BPB_HiddSec;
    int BPB_TotSec32;
    int BS_DrvNum;
    int BS_Reserved1;
    int BS_BootSig;
    uint8_t BS_VolID[4];
    uint8_t BS_VolLab[11];
    uint8_t BS_FilSysType[8];

    void LoadFromSector(uint8_t *sec) {
        ArrayCopy(sec, BS_jmpBoot, 0, 3);

        for (int i = 3; i < 11; i++) {
            BS_OEMName[i - 3] = sec[i];
        }

        for (int i = 0; i < 8; i++) {
            BPB_BytsPerSec += ((sec[11] >> i) & 0x1) * (0x1 << i);
            BPB_BytsPerSec += ((sec[12] >> i) & 0x1) * (0x100 << i);

            BPB_SecPerClus += ((sec[13] >> i) & 0x1) * (0x1 << i);

            BPB_RsvdSecCnt += ((sec[14] >> i) & 0x1) * (0x1 << i);
            BPB_RsvdSecCnt += ((sec[15] >> i) & 0x1) * (0x100 << i);

            BPB_NumFATs += ((sec[16] >> i) & 0x1) * (0x1 << i);

            BPB_RootEntCnt += ((sec[17] >> i) & 0x1) * (0x1 << i);
            BPB_RootEntCnt += ((sec[18] >> i) & 0x1) * (0x100 << i);

            BPB_TotSec16 += ((sec[19] >> i) & 0x1) * (0x1 << i);
            BPB_TotSec16 += ((sec[20] >> i) & 0x1) * (0x100 << i);

            BPB_Media += ((sec[21] >> i) & 0x1) * (0x1 << i);\

            BPB_FATSz16 += ((sec[22] >> i) & 0x1) * (0x1 << i);
            BPB_FATSz16 += ((sec[23] >> i) & 0x1) * (0x100 << i);

            BPB_SecPerTrk += ((sec[24] >> i) & 0x1) * (0x1 << i);
            BPB_SecPerTrk += ((sec[25] >> i) & 0x1) * (0x100 << i);

            BPB_NumHeads += ((sec[26] >> i) & 0x1) * (0x1 << i);
            BPB_NumHeads += ((sec[27] >> i) & 0x1) * (0x100 << i);

            BPB_HiddSec += ((sec[28] >> i) & 0x1) * (0x1 << i);
            BPB_HiddSec += ((sec[29] >> i) & 0x1) * (0x100 << i);
            BPB_HiddSec += ((sec[30] >> i) & 0x1) * (0x10000 << i);
            BPB_HiddSec += ((sec[31] >> i) & 0x1) * (0x1000000 << i);

            BPB_TotSec32 += ((sec[32] >> i) & 0x1) * (0x1 << i);
            BPB_TotSec32 += ((sec[33] >> i) & 0x1) * (0x100 << i);
            BPB_TotSec32 += ((sec[34] >> i) & 0x1) * (0x10000 << i);
            BPB_TotSec32 += ((sec[35] >> i) & 0x1) * (0x1000000 << i);

            BS_DrvNum += ((sec[36] >> i) & 0x1) * (0x1 << i);

            BS_Reserved1 += ((sec[37] >> i) & 0x1) * (0x1 << i);

            BS_BootSig += ((sec[38] >> i) & 0x1) * (0x1 << i);
        }

        ArrayCopy(sec, BS_VolID, 39, 4);
        ArrayCopy(sec, BS_VolLab, 43, 11);
        ArrayCopy(sec, BS_FilSysType, 54, 8);
    }

    void PrintFATInfo() {
        std::cout << "OEM Name:             " << BS_OEMName << std::endl
                  << "Bytes Per Sector:     " << BPB_BytsPerSec << std::endl
                  << "Sectors Per Cluster:  " << BPB_SecPerClus << std::endl
                  << "Reserved Sectors:     " << BPB_RsvdSecCnt << std::endl
                  << "FAT Count:            " << BPB_NumFATs << std::endl
                  << "Root Entries:         " << BPB_RootEntCnt << std::endl
                  << "Sector Count 16:      " << BPB_TotSec16 << std::endl
                  << "Media Count:          " << BPB_Media << std::endl
                  << "FAT Size:             " << BPB_FATSz16 << std::endl
                  << "Sectors Per Track:    " << BPB_SecPerTrk << std::endl
                  << "Head Count:           " << BPB_NumHeads << std::endl
                  << "Hidden Sectors:       " << BPB_HiddSec << std::endl
                  << "Sector Count 32:      " << BPB_TotSec32 << std::endl
                  << "Drive Num:            " << BS_DrvNum << std::endl
                  << "Boot Signature:       " << BS_BootSig << std::endl;
    }
};
#pragma pack()

#pragma pack(1)
struct FATFile {
    int fd;
    int FATindex;
    bool write = false;
    SVMDirectoryEntry rootEntry;
    int rootEntryByteIndex;
    int dataClusterByteIndex;
};
#pragma pack()
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
BPB* BPBcache;
int FATFd = 0;
int FATStartByte = 0;
TVMMutexID fatMutex;
TVMMutexID sharedMemMutex;
uint8_t* FATcache;
std::vector<FATFile*> filesCache;
std::vector<FATFile*> openFiles;


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
    while(1){
        //std::cout << "-idling.." << "\n";
    }
}

void ArrayCopy(const uint8_t* src, uint8_t* dest, int index, int len){
    for(int i = 0; i < len; i++){
        dest[i] = src[index+i];
    }
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
    //std::cout << "-Thread " << ((Thread*)(calldata))->tid << " filecallback\n";
    MachineSuspendSignals(&sigState);
    Thread *t = (Thread*)(calldata);
    t->state = VM_THREAD_STATE_READY;
    t->fileResult = result;
    readyThreadList.push(t);
    MachineResumeSignals(&sigState);
    threadSchedule(WAIT_FOR_PRIO);
}

// Skeleton function
void ThreadWrapper(void* param){
    Thread* t = (Thread*)(param);
    (t->entry)(t->param);
    VMThreadTerminate(t->tid);
}

// Returns byte index of next free root entry in cache
int FindFreeRootEntry(){
    int offset;
    uint8_t tempEntry[32];
    int len = 32;
    FileSeek(FATFd, (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512, SEEK_SET , &offset);
    for(int i = 0; i < BPBcache->BPB_RootEntCnt; i++){
        FileRead(FATFd, tempEntry, &len);
        if(tempEntry[0] == 0x00 || tempEntry[0] == 0xE5){
            return (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512 + i*32;
        }
    }
    return -1;
}

// Returns byte index of next free FAT entry in cache
int FindFreeFATEntry(){
    int offset;
    uint16_t tempEntry[2];
    int len = 2;
    FileSeek(FATFd, BPBcache->BPB_RsvdSecCnt*512, SEEK_SET , &offset);
    for(int i = 0; i < BPBcache->BPB_FATSz16*512; i+=2){
        FileRead(FATFd, tempEntry, &len);
        if(tempEntry[0] == 0 && tempEntry[1] == 0){
            return  BPBcache->BPB_RsvdSecCnt*512 + i;
        }
    }
    return -1;
}
//=============================================================

// FILE OPERATIONS
//=====================================================================================================
TVMStatus FileOpen(const char *filename, int flags, int mode, int *filedescriptor){
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

TVMStatus FileClose(int filedescriptor){
    MachineFileClose(filedescriptor, &FileCallback, runningThread);

    threadSchedule(WAIT_FOR_FILE);

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus FileRead(int filedescriptor, void *data, int *length){
    if(data == NULL || length == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    void* mem = NULL;
    for(int i = *length; i > 0; i -= 512){
        if(!sharedMem->memChunks.empty()){
            mem = sharedMem->memChunks.back();
            sharedMem->memChunks.pop_back();
        }
        MachineFileRead(filedescriptor, mem, *length, &FileCallback, runningThread);
        threadSchedule(WAIT_FOR_FILE);
        memcpy(data, mem, *length*sizeof(char));
        sharedMem->memChunks.push_back(mem);
    }

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *length = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus FileWrite(int filedescriptor, void *data, int *length){
    if(data == NULL || length == NULL)
        return VM_STATUS_ERROR_INVALID_PARAMETER;

    VMMutexAcquire(sharedMemMutex, VM_TIMEOUT_INFINITE);
    MachineSuspendSignals(&sigState);
    void* mem = NULL;
    for(int i = *length; i > 0; i -= 512){
        if(!sharedMem->memChunks.empty()){
            mem = sharedMem->memChunks.back();
            sharedMem->memChunks.pop_back();
        }

        int len = (i < 512) ? i : 512;

        memcpy(mem, data, len);
        MachineFileWrite(filedescriptor, mem, len, &FileCallback, runningThread);

        MachineResumeSignals(&sigState);
        threadSchedule(WAIT_FOR_FILE);
        sharedMem->memChunks.push_back(mem);
    }
    VMMutexRelease(sharedMemMutex);

    //std::cout << "-back to thread " << runningThread->tid << "\n";

    if(runningThread->fileResult < 0)
        return VM_STATUS_FAILURE;
    else{
        *length = runningThread->fileResult;
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus FileSeek(int filedescriptor, int offset, int whence, int *newoffset){
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



// BASIC OPERATIONS
//=====================================================================================================
TVMStatus VMStart(int tickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[]){
    TVMMainEntry main = VMLoadModule(argv[0]);
    tickMS = tickms;

    sharedMem = new SharedMem();
    VMMutexCreate(&sharedMemMutex);
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

    // FAT file related code ----------------------------
    // BPB & Init
    VMMutexCreate(&fatMutex);
    BPBcache = new BPB();
    uint8_t tmpBPB[512];
    int len = 512;
    FileOpen(mount, O_RDWR, 0600, &FATFd);;
    FileRead(FATFd, tmpBPB, &len);
    BPBcache->LoadFromSector(tmpBPB);
    BPBcache->PrintFATInfo();

    int offset;

    // Load FAT cache
    FATStartByte = BPBcache->BPB_RsvdSecCnt*512;
    len = BPBcache->BPB_RsvdSecCnt*512;
    uint8_t tmpFAT[len];
    for(int i = 0; i < len; i++){
        FileSeek(FATFd, len, SEEK_SET, &offset);
        FileRead(FATFd, tmpFAT, &len);
    }
    FATcache = tmpFAT;

    /*
    len = 2;
    FileSeek(fatFd, BPBcache->BPB_RsvdSecCnt*512, SEEK_SET , &offset);
    uint8_t tmpEntry[2];
    for(int i = 0; i < BPBcache->BPB_FATSz16*512; i+=2){
        FileRead(fatFd, tmpEntry, &len);
        std::cout << (int)tmpEntry[1] << "|" << (int)tmpEntry[0] << " ";
    }
    std::cout << "\n";
    std::cout << FindFreeFATEntry() << "\n";
     */

    // Load root cache
    /*
    len = BPBcache->BPB_RootEntCnt*32;
    uint8_t tmpRootCache[len];
    FileSeek(fatFd, (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512, SEEK_SET , &offset);
    FileRead(fatFd, tmpRootCache, &len);
    */

    uint8_t tmpRootEntry[32];
    len = 32;
    FileSeek(FATFd, (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512, SEEK_SET , &offset);
    for(int i = 0; i < BPBcache->BPB_RootEntCnt; i++){
        FileRead(FATFd, tmpRootEntry, &len);
        if(tmpRootEntry[0] == 0x00)
            break;
        else if((tmpRootEntry[11] == 0xf) || (tmpRootEntry[11] == 0x10) ||  (tmpRootEntry[0] == 0xE5))
            continue;
        else{
            FATFile* ff = new FATFile();
            ff->FATindex = tmpRootEntry[26] + (tmpRootEntry[27] * 16);
            for(int i = 0; i < 11; i++){
                if((char)tmpRootEntry[i] != ' ')
                    ff->rootEntry.DShortFileName[i] = (char)tmpRootEntry[i];
            }
            filesCache.push_back(ff);
        }
    }


    /*
    uint8_t testtmp[256];
    len = 256;
    FileSeek(FATFd, (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512  + (BPBcache->BPB_RootEntCnt * 32) + (filesCache.back()->FATindex-2)*BPBcache->BPB_BytsPerSec, SEEK_SET , &offset);
    FileRead(FATFd, testtmp, &len);
    std::cout << (char*)testtmp << "\n";
    std::cout << filesCache.back()->rootEntry.DShortFileName << " ====\n";
*/
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

// FAT FILE OPERATIONS
//=====================================================================================================
TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
    for(auto it = filesCache.begin(); it != filesCache.end(); ++it){
        if(strcmp((*it)->rootEntry.DShortFileName, filename) == 0){
            *filedescriptor = openFiles.size() + 3;
            (*it)->fd = openFiles.size() + 3;
            openFiles.push_back(*it);
            std::cout << "-opening " << (*it)->rootEntry.DShortFileName << " success\n";
            return VM_STATUS_SUCCESS;
        }
    }
    return VM_STATUS_ERROR_INVALID_PARAMETER;
}

TVMStatus VMFileClose(int filedescriptor){
    if(filedescriptor < 3){
        return FileClose(filedescriptor);
    }
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
    if(filedescriptor < 3){
        return FileRead(filedescriptor, data, length);
    }
    else{
        FATFile ff;
        for(auto it = openFiles.begin(); it != openFiles.end(); ++it){
            if((*it)->fd == filedescriptor){
                int offset;

                FileSeek(FATFd, (BPBcache->BPB_RsvdSecCnt + (BPBcache->BPB_NumFATs * BPBcache->BPB_FATSz16))*512  + (BPBcache->BPB_RootEntCnt * 32) + ((*it)->FATindex-2)*BPBcache->BPB_BytsPerSec, SEEK_SET , &offset);

                FileRead(FATFd, data, length);
                //std::cout << &data << "sfsdf\n";
                return VM_STATUS_SUCCESS;
            }
        }
    }

    return VM_STATUS_ERROR_INVALID_PARAMETER;
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length){
    if(filedescriptor < 3){
        return FileWrite(filedescriptor, data, length);
    }
    else{

    }
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
    if(filedescriptor < 3){
        return FileSeek(filedescriptor, offset, whence, newoffset);
    }
    else{

    }
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