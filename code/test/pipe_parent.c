#include "syscall.h"

int main() {
    int readFd = -1;
    int writeFd = -1;
    int pid;
    char message[] = "pipe works";

    if (Pipe(&readFd, &writeFd) < 0) {
        PrintString("Pipe create failed\n");
        Exit(-1);
    }

    pid = ExecP("pipe_child", readFd);
    if (pid < 0) {
        PrintString("ExecP failed\n");
        Exit(-1);
    }

    if (PipeWrite(writeFd, message, 10) < 0) {
        PrintString("PipeWrite failed\n");
        Exit(-1);
    }

    Close(writeFd);
    Join(pid);
    Exit(0);
}
