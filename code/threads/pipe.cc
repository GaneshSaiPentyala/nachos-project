#include "pipe.h"

#include "main.h"
#include "synch.h"
#include "utility.h"

PipeChannel::PipeChannel() {
    head = 0;
    tail = 0;
    size = 0;
    readerCount = 1;
    writerCount = 1;
    lock = new Lock("pipe_channel_lock");
    notEmpty = new Condition("pipe_not_empty");
    notFull = new Condition("pipe_not_full");
}

PipeChannel::~PipeChannel() {
    delete notFull;
    delete notEmpty;
    delete lock;
}

int PipeChannel::Read(char *buffer, int charCount) {
    if (buffer == NULL || charCount < 0) return -1;

    lock->Acquire();

    int bytesRead = 0;
    while (bytesRead < charCount) {
        while (size == 0 && writerCount > 0) {
            notEmpty->Wait(lock);
        }

        if (size == 0 && writerCount == 0) break;

        buffer[bytesRead++] = data[head];
        head = (head + 1) % kBufferSize;
        size--;
        notFull->Signal(lock);
    }

    lock->Release();
    return bytesRead;
}

int PipeChannel::Write(const char *buffer, int charCount) {
    if (buffer == NULL || charCount < 0) return -1;

    lock->Acquire();

    if (readerCount == 0) {
        lock->Release();
        return -1;
    }

    int bytesWritten = 0;
    while (bytesWritten < charCount) {
        while (size == kBufferSize && readerCount > 0) {
            notFull->Wait(lock);
        }

        if (readerCount == 0) {
            lock->Release();
            return (bytesWritten > 0) ? bytesWritten : -1;
        }

        data[tail] = buffer[bytesWritten++];
        tail = (tail + 1) % kBufferSize;
        size++;
        notEmpty->Signal(lock);
    }

    lock->Release();
    return bytesWritten;
}

void PipeChannel::OpenReader() {
    lock->Acquire();
    readerCount++;
    lock->Release();
}

void PipeChannel::OpenWriter() {
    lock->Acquire();
    writerCount++;
    lock->Release();
}

void PipeChannel::CloseReader() {
    lock->Acquire();
    if (readerCount > 0) readerCount--;
    notFull->Broadcast(lock);
    lock->Release();
}

void PipeChannel::CloseWriter() {
    lock->Acquire();
    if (writerCount > 0) writerCount--;
    notEmpty->Broadcast(lock);
    lock->Release();
}

bool PipeChannel::CanDelete() const {
    return readerCount == 0 && writerCount == 0;
}

PipeDescriptors::PipeDescriptors() {
    lock = new Semaphore("pipe_descriptor_lock", 1);
    for (int i = 0; i < kMaxDescriptors; i++) {
        descriptors[i].inUse = false;
        descriptors[i].mode = DESCRIPTOR_FREE;
        descriptors[i].channel = NULL;
        descriptors[i].ownerPid = -1;
    }
}

PipeDescriptors::~PipeDescriptors() {
    for (int i = 0; i < kMaxDescriptors; i++) {
        PipeChannel *channel = descriptors[i].channel;
        if (channel == NULL) continue;

        bool seen = false;
        for (int j = 0; j < i; j++) {
            if (descriptors[j].channel == channel) {
                seen = true;
                break;
            }
        }

        if (!seen) delete channel;
    }

    delete lock;
}

int PipeDescriptors::FindFreeDescriptor() {
    for (int i = 0; i < kMaxDescriptors; i++) {
        if (!descriptors[i].inUse) return i;
    }
    return -1;
}

int PipeDescriptors::ToInternalIndex(int desNum) const {
    if (desNum < kDescriptorBase) return -1;
    int index = desNum - kDescriptorBase;
    if (index < 0 || index >= kMaxDescriptors) return -1;
    return index;
}

int PipeDescriptors::ToExternalDescriptor(int index) const {
    return kDescriptorBase + index;
}

bool PipeDescriptors::CanAccessDescriptor(const DescriptorEntry &entry) const {
    int currentPid = kernel->currentThread->processID;
    return entry.ownerPid == currentPid;
}

bool PipeDescriptors::IsPipeDescriptor(int desNum) const {
    return ToInternalIndex(desNum) != -1;
}

int PipeDescriptors::createDes(int *readDes, int *writeDes, const char *name) {
    (void)name;
    if (readDes == NULL || writeDes == NULL) return -1;

    lock->P();

    int readIndex = FindFreeDescriptor();
    if (readIndex == -1) {
        lock->V();
        return -1;
    }

    descriptors[readIndex].inUse = true;

    int writeIndex = FindFreeDescriptor();
    if (writeIndex == -1) {
        descriptors[readIndex].inUse = false;
        lock->V();
        return -1;
    }

    PipeChannel *channel = new PipeChannel();
    if (channel == NULL) {
        descriptors[readIndex].inUse = false;
        lock->V();
        return -1;
    }
    descriptors[readIndex].mode = DESCRIPTOR_READ;
    descriptors[readIndex].channel = channel;
    descriptors[readIndex].ownerPid = kernel->currentThread->processID;

    descriptors[writeIndex].inUse = true;
    descriptors[writeIndex].mode = DESCRIPTOR_WRITE;
    descriptors[writeIndex].channel = channel;
    descriptors[writeIndex].ownerPid = kernel->currentThread->processID;

    *readDes = ToExternalDescriptor(readIndex);
    *writeDes = ToExternalDescriptor(writeIndex);

    lock->V();
    return 0;
}

int PipeDescriptors::duplicateDesToProcess(int desNum, int targetPid,
                                           int *newDes) {
    if (newDes == NULL) return -1;

    int sourceIndex = ToInternalIndex(desNum);
    if (sourceIndex == -1) return -1;

    lock->P();

    if (!descriptors[sourceIndex].inUse || descriptors[sourceIndex].channel == NULL ||
        !CanAccessDescriptor(descriptors[sourceIndex])) {
        lock->V();
        return -1;
    }

    int newIndex = FindFreeDescriptor();
    if (newIndex == -1) {
        lock->V();
        return -1;
    }

    PipeChannel *channel = descriptors[sourceIndex].channel;
    DescriptorMode mode = descriptors[sourceIndex].mode;
    descriptors[newIndex].inUse = true;
    descriptors[newIndex].mode = mode;
    descriptors[newIndex].channel = channel;
    descriptors[newIndex].ownerPid = targetPid;
    *newDes = ToExternalDescriptor(newIndex);

    if (mode == DESCRIPTOR_READ)
        channel->OpenReader();
    else if (mode == DESCRIPTOR_WRITE)
        channel->OpenWriter();

    lock->V();
    return 0;
}

int PipeDescriptors::readDes(int desNum, char *buffer, int charCount) {
    int index = ToInternalIndex(desNum);
    if (index == -1) return -1;

    lock->P();
    bool valid = descriptors[index].inUse &&
                 descriptors[index].mode == DESCRIPTOR_READ &&
                 descriptors[index].channel != NULL &&
                 CanAccessDescriptor(descriptors[index]);
    PipeChannel *channel = descriptors[index].channel;
    lock->V();

    if (!valid) return -1;
    return channel->Read(buffer, charCount);
}

int PipeDescriptors::writeDes(int desNum, const char *buffer, int charCount) {
    int index = ToInternalIndex(desNum);
    if (index == -1) return -1;

    lock->P();
    bool valid = descriptors[index].inUse &&
                 descriptors[index].mode == DESCRIPTOR_WRITE &&
                 descriptors[index].channel != NULL &&
                 CanAccessDescriptor(descriptors[index]);
    PipeChannel *channel = descriptors[index].channel;
    lock->V();

    if (!valid) return -1;
    return channel->Write(buffer, charCount);
}

int PipeDescriptors::CloseDescriptorByIndex(int index, bool requireOwnership) {
    lock->P();

    if (index < 0 || index >= kMaxDescriptors || !descriptors[index].inUse ||
        (requireOwnership && !CanAccessDescriptor(descriptors[index])) ||
        descriptors[index].channel == NULL) {
        lock->V();
        return -1;
    }

    PipeChannel *channel = descriptors[index].channel;
    DescriptorMode mode = descriptors[index].mode;

    descriptors[index].inUse = false;
    descriptors[index].mode = DESCRIPTOR_FREE;
    descriptors[index].channel = NULL;
    descriptors[index].ownerPid = -1;

    lock->V();

    if (mode == DESCRIPTOR_READ)
        channel->CloseReader();
    else if (mode == DESCRIPTOR_WRITE)
        channel->CloseWriter();

    if (channel->CanDelete()) delete channel;
    return 0;
}

int PipeDescriptors::closeDes(int desNum) {
    int index = ToInternalIndex(desNum);
    if (index == -1) return -1;
    return CloseDescriptorByIndex(index, true);
}

void PipeDescriptors::CloseAllForProcess(int pid) {
    lock->P();
    for (int i = 0; i < kMaxDescriptors; i++) {
        if (descriptors[i].inUse && descriptors[i].ownerPid == pid) {
            lock->V();
            CloseDescriptorByIndex(i, false);
            lock->P();
        }
    }
    lock->V();
}
