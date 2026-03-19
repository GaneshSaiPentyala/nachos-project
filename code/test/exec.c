/* exec.c
 *	Simple program to test the exec system call.
 */

#include "syscall.h"
#define stdin 0
#define stdout 1

int main() {
    int pid;
    pid = Exec("child");
    if (pid < 0) {
        Write("Exec failed: ", 14, stdout);
        PrintNum(pid);
    } else {
        int i;
        for (i = 0; i < 10; i++) {
            Write("This is the parent thread.\n", 27, stdout);
        }
        Join(pid);
}
