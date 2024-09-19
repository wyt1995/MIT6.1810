#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define RD 0
#define WR 1

void sieve(int[2]) __attribute__((noreturn));

void sieve(int left[2]) {
    int prime;
    close(left[WR]);

    // read the first number; if no more left, close the pipe and exit
    if (read(left[RD], &prime, sizeof(int)) <= 0) {
        close(left[RD]);
        exit(0);
    }
    // otherwise, print the number
    printf("prime %d\n", prime);

    // set up the right pipe
    int pid;
    int right[2];
    pipe(right);

    if ((pid = fork()) < 0) {
        fprintf(2, "fork failed\n");
        exit(1);
    } else if (pid == 0) {
        close(left[RD]);
        sieve(right);
    } else {
        close(right[RD]);

        // read number from the left pipe, write to right only if not divisible
        int num;
        while (read(left[RD], &num, sizeof(int)) > 0) {
            if (num % prime) {
                write(right[WR], &num, sizeof(int));
            }
        }
        close(left[RD]);
        close(right[WR]);

        wait(0);
        exit(0);
    }
}

int main(int argc, char* argv[]) {
    int pid;
    int fds[2];

    pipe(fds);
    if ((pid = fork()) < 0) {
        fprintf(2, "fork failed\n");
        exit(1);
    } else if (pid == 0) {
        sieve(fds);
    } else {
        // parent process: feed numbers into the pipe
        close(fds[RD]);
        for (int i = 2; i <= 280; i++) {
            if (write(fds[WR], &i, sizeof(int)) < 0) {
                fprintf(2, "failed to write number %d\n", i);
                exit(1);
            }
        }
        close(fds[WR]);
        wait(0);
        exit(0);
    }
    exit(0);
}
