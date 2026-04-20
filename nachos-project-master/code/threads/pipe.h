#ifndef PIPE_H
#define PIPE_H

#include "copyright.h"

class Lock;
class Condition;
class Semaphore;

class PipeChannel {
   public:
    PipeChannel();
    ~PipeChannel();

    int Read(char *buffer, int charCount);
    int Write(const char *buffer, int charCount);
    void OpenReader();
    void OpenWriter();
    void CloseReader();
    void CloseWriter();
    bool CanDelete() const;

   private:
    static const int kBufferSize = 256;

    char data[kBufferSize];
    int head;
    int tail;
    int size;
    int readerCount;
    int writerCount;
    Lock *lock;
    Condition *notEmpty;
    Condition *notFull;
};

class PipeDescriptors {
   public:
    PipeDescriptors();
    ~PipeDescriptors();

    int createDes(int *readDes, int *writeDes, const char *name);
    int duplicateDesToProcess(int desNum, int targetPid, int *newDes);
    int readDes(int desNum, char *buffer, int charCount);
    int writeDes(int desNum, const char *buffer, int charCount);
    int closeDes(int desNum);
    void CloseAllForProcess(int pid);
    bool IsPipeDescriptor(int desNum) const;

   private:
    enum DescriptorMode {
        DESCRIPTOR_FREE = 0,
        DESCRIPTOR_READ,
        DESCRIPTOR_WRITE,
    };

    struct DescriptorEntry {
        bool inUse;
        DescriptorMode mode;
        PipeChannel *channel;
        int ownerPid;
    };

    static const int kMaxDescriptors = 128;
    static const int kDescriptorBase = 100;

    DescriptorEntry descriptors[kMaxDescriptors];
    Semaphore *lock;

    int FindFreeDescriptor();
    int ToInternalIndex(int desNum) const;
    int ToExternalDescriptor(int index) const;
    bool CanAccessDescriptor(const DescriptorEntry &entry) const;
    int CloseDescriptorByIndex(int index, bool requireOwnership);
};

#endif
