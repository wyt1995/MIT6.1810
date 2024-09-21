#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
    // your code here.  you should write the secret to fd 2 using write
    // (e.g., write(2, secret, 8)

    char *base = sbrk(PGSIZE * 32);
    base = base + 8 * PGSIZE + 16;
    if (write(2, base, 8) != 8) {
        printf("write error\n");
        exit(1);
    }
    exit(0);
}
