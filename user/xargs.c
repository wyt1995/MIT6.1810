#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

int read_line(char* arguments[], int index) {
    char buf[1024];
    int n = 0;
    while (read(0, buf + n, 1) > 0) {
        if (buf[n] == '\n') {
            break;
        }
        n += 1;
        if (n == 1023) {
            fprintf(2, "argument is too long\n");
            exit(1);
        }
    }

    // nothing is read
    if (n == 0) {
        return 0;
    }

    int offset = 0;
    while (offset < n) {
        int start = offset;
        while (offset < n && buf[offset] != ' ') {
            offset += 1;
        }
        buf[offset++] = 0;
        arguments[index] = malloc(offset - start);
        memcpy(arguments[index], buf + start, offset - start);
        index += 1;
        while (offset < n && buf[offset] == ' ') {
            offset += 1;
        }
    }
    return index;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(2, "Usage: xargs command (argv ...)\n");
        exit(1);
    }

    char* command = malloc(strlen(argv[1]) + 1);
    strcpy(command, argv[1]);

    char* arguments[MAXARG];
    for (int i = 0; i < argc - 1; i++) {
        arguments[i] = malloc(strlen(argv[i + 1]) + 1);
        strcpy(arguments[i], argv[i + 1]);
    }

    int end = 0;
    while ((end = read_line(arguments, argc - 1)) > 0) {
        arguments[end] = 0;
        if (fork() == 0) {
            exec(command, arguments);
            fprintf(2, "exec error\n");
            exit(1);
        }
        wait(0);
        for (int i = argc - 1; i < end; i++) {
            free(arguments[i]);
        }
    }

    free(command);
    for (int i = 0; i < argc - 1; i++) {
        free(arguments[i]);
    }
    exit(0);
}
