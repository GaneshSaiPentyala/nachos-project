#include "syscall.h"

int main() {
    int readFd = GetPD(0);
    char buffer[16];
    int count;

    if (readFd < 0) {
        PrintString("GetPD failed\n");
        Exit(-1);
    }

    count = PipeRead(readFd, buffer, 15);
    if (count < 0) {
        PrintString("PipeRead failed\n");
        Exit(-1);
    }

    if (count > 0) {
        Write(buffer, count, _ConsoleOutput);
    }
    Close(readFd);
    Exit(0);
}
