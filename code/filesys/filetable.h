#ifndef FILETABLE_H
#define FILETABLE_H
#include "openfile.h"
#include "sysdep.h"

#define FILE_MAX 10
#define CONSOLE_IN 0
#define CONSOLE_OUT 1
#define MODE_READWRITE 0
#define MODE_READ 1
#define MODE_WRITE 2

class FileTable {
   private:
    enum DescriptorType {
        FD_FREE = 0,
        FD_FILE,
        FD_PIPE_READ,
        FD_PIPE_WRITE,
    };

    class PipeBuffer {
       public:
        PipeBuffer() {
            readOpenCount = 1;
            writeOpenCount = 1;
            head = 0;
            tail = 0;
            size = 0;
        }
        ~PipeBuffer() {}

        int Read(char *buffer, int charCount) {
            if (charCount < 0) return -1;

            int bytesRead = 0;
            while (bytesRead < charCount) {
                if (size == 0) break;

                buffer[bytesRead++] = data[head];
                head = (head + 1) % PIPE_BUFFER_SIZE;
                size--;
            }
            return bytesRead;
        }

        int Write(char *buffer, int charCount) {
            if (charCount < 0) return -1;
            if (readOpenCount == 0) return -1;

            int bytesWritten = 0;
            while (bytesWritten < charCount && size < PIPE_BUFFER_SIZE) {

                data[tail] = buffer[bytesWritten++];
                tail = (tail + 1) % PIPE_BUFFER_SIZE;
                size++;
            }
            return bytesWritten;
        }

        void CloseReader() {
            if (readOpenCount > 0) readOpenCount--;
        }

        void CloseWriter() {
            if (writeOpenCount > 0) writeOpenCount--;
        }

        bool CanDelete() {
            return readOpenCount == 0 && writeOpenCount == 0;
        }

       private:
        static const int PIPE_BUFFER_SIZE = 256;

        char data[PIPE_BUFFER_SIZE];
        int readOpenCount;
        int writeOpenCount;
        int head;
        int tail;
        int size;
    };

    OpenFile** openFile;
    int* fileOpenMode;
    DescriptorType *descriptorType;
    PipeBuffer **pipeBuffer;

   public:
    FileTable() {
        openFile = new OpenFile*[FILE_MAX];
        fileOpenMode = new int[FILE_MAX];
        descriptorType = new DescriptorType[FILE_MAX];
        pipeBuffer = new PipeBuffer *[FILE_MAX];

        for (int i = 0; i < FILE_MAX; i++) {
            openFile[i] = NULL;
            fileOpenMode[i] = MODE_READWRITE;
            descriptorType[i] = FD_FREE;
            pipeBuffer[i] = NULL;
        }

        fileOpenMode[CONSOLE_IN] = MODE_READ;
        fileOpenMode[CONSOLE_OUT] = MODE_WRITE;
        descriptorType[CONSOLE_IN] = FD_FILE;
        descriptorType[CONSOLE_OUT] = FD_FILE;
    }

    int Insert(char* fileName, int openMode) {
        int freeIndex = -1;
        int fileDescriptor = -1;
        for (int i = 2; i < FILE_MAX; i++) {
            if (openFile[i] == NULL) {
                freeIndex = i;
                break;
            }
        }

        if (freeIndex == -1) {
            return -1;
        }

        if (openMode == MODE_READWRITE)
            fileDescriptor = OpenForReadWrite(fileName, FALSE);
        if (openMode == MODE_READ)
            fileDescriptor = OpenForRead(fileName, FALSE);

        if (fileDescriptor == -1) return -1;
        openFile[freeIndex] = new OpenFile(fileDescriptor);
        fileOpenMode[freeIndex] = openMode;
        descriptorType[freeIndex] = FD_FILE;

        return freeIndex;
    }

    int CreatePipe(int fds[2]) {
        int first = -1;
        int second = -1;

        for (int i = 2; i < FILE_MAX; i++) {
            if (descriptorType[i] == FD_FREE) {
                if (first == -1)
                    first = i;
                else {
                    second = i;
                    break;
                }
            }
        }

        if (first == -1 || second == -1) return -1;

        PipeBuffer *pipe = new PipeBuffer();
        pipeBuffer[first] = pipe;
        pipeBuffer[second] = pipe;
        descriptorType[first] = FD_PIPE_READ;
        descriptorType[second] = FD_PIPE_WRITE;
        fileOpenMode[first] = MODE_READ;
        fileOpenMode[second] = MODE_WRITE;
        fds[0] = first;
        fds[1] = second;
        return 0;
    }

    int Remove(int index) {
        if (index < 2 || index >= FILE_MAX) return -1;
        if (descriptorType[index] == FD_FILE && openFile[index]) {
            delete openFile[index];
            openFile[index] = NULL;
            descriptorType[index] = FD_FREE;
            return 0;
        }
        if (descriptorType[index] == FD_PIPE_READ ||
            descriptorType[index] == FD_PIPE_WRITE) {
            PipeBuffer *pipe = pipeBuffer[index];
            if (pipe == NULL) return -1;

            if (descriptorType[index] == FD_PIPE_READ)
                pipe->CloseReader();
            else
                pipe->CloseWriter();

            descriptorType[index] = FD_FREE;
            pipeBuffer[index] = NULL;

            if (pipe->CanDelete()) delete pipe;
            return 0;
        }
        return -1;
    }

    int Read(char* buffer, int charCount, int index) {
        if (index >= FILE_MAX) return -1;
        if (descriptorType[index] == FD_PIPE_READ) {
            if (pipeBuffer[index] == NULL) return -1;
            return pipeBuffer[index]->Read(buffer, charCount);
        }
        if (descriptorType[index] != FD_FILE) return -1;
        if (openFile[index] == NULL) return -1;
        int result = openFile[index]->Read(buffer, charCount);
        // if we cannot read enough bytes, we should return -2
        if (result != charCount) return -2;
        return result;
    }

    int Write(char* buffer, int charCount, int index) {
        if (index >= FILE_MAX) return -1;
        if (descriptorType[index] == FD_PIPE_WRITE) {
            if (pipeBuffer[index] == NULL) return -1;
            return pipeBuffer[index]->Write(buffer, charCount);
        }
        if (descriptorType[index] != FD_FILE) return -1;
        if (openFile[index] == NULL || fileOpenMode[index] == MODE_READ)
            return -1;
        return openFile[index]->Write(buffer, charCount);
    }

    int Seek(int pos, int index) {
        if (index <= 1 || index >= FILE_MAX) return -1;
        if (descriptorType[index] == FD_PIPE_READ ||
            descriptorType[index] == FD_PIPE_WRITE)
            return -1;
        if (openFile[index] == NULL) return -1;
        // use seek(-1) to move to the end of file
        if (pos == -1) pos = openFile[index]->Length();
        if (pos < 0 || pos > openFile[index]->Length()) return -1;
        return openFile[index]->Seek(pos);
    }

    ~FileTable() {
        for (int i = 0; i < FILE_MAX; i++) {
            if (openFile[i]) delete openFile[i];
            if (descriptorType[i] == FD_PIPE_READ && pipeBuffer[i]) {
                pipeBuffer[i]->CloseReader();
                if (pipeBuffer[i]->CanDelete()) delete pipeBuffer[i];
            } else if (descriptorType[i] == FD_PIPE_WRITE && pipeBuffer[i]) {
                pipeBuffer[i]->CloseWriter();
                if (pipeBuffer[i]->CanDelete()) delete pipeBuffer[i];
            }
        }
        delete[] openFile;
        delete[] fileOpenMode;
        delete[] descriptorType;
        delete[] pipeBuffer;
    }
};

#endif
