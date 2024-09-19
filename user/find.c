#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"

// see ls.c for reference
void find(char* const path, char* const file) {
    char buf[512], *ptr;
    int fd;
    struct stat st;
    struct dirent de;

    // open directory
    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    // check file status
    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    // only find file in a directory
    if (st.type != T_DIR) {
        close(fd);
        return;
    }

    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        printf("find: path too long");
        close(fd);
        return;
    }
    strcpy(buf, path);
    ptr = buf + strlen(buf);
    *ptr++ = '/';

    // read the current directory, which consists of a sequence of dirent structs
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        // don't recurse into "." and ".."
        if (de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
            continue;
        }
        memmove(ptr, de.name, DIRSIZ);
        ptr[DIRSIZ] = 0;

        // current reading file status
        if (stat(buf, &st) < 0) {
            printf("find: cannot stat %s\n", buf);
            continue;
        }

        // recurse if it is a directory, compare name if it is a file
        if (st.type == T_DIR) {
            find(buf, file);
        } else if (st.type == T_FILE && strcmp(de.name, file) == 0) {
            printf("%s\n", buf);
        }
    }
    close(fd);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(2, "Usage: find directory filename\n");
        exit(1);
    }
    find(argv[1], argv[2]);
    exit(0);
}
