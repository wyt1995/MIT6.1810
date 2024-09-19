#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char* argv[]) {
    int pid;
    int fds[2];

    pipe(fds);
    if ((pid = fork()) < 0) {
        fprintf(2, "fork process failed\n");
        exit(1);
    } else if (pid == 0) {
        char buffer;
        if (read(fds[0], &buffer, 1) != 1) {
            fprintf(2, "read byte failed\n");
            exit(1);
        }
        fprintf(1, "%d: received ping\n", getpid());
        close(fds[0]);
        close(fds[1]);
    } else {
        char byte = 'a';
        if (write(fds[1], &byte, 1) != 1) {
            fprintf(2, "write byte failed\n");
            exit(1);
        }
        wait(0);
        fprintf(1, "%d: received pong\n", getpid());
        close(fds[0]);
        close(fds[1]);
    }

    exit(0);
}
