#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    int fd_a1 = open("/proc/sykom/a1stma", O_WRONLY);
    int fd_a2 = open("/proc/sykom/a2stma", O_WRONLY);
    int fd_ctrl = open("/proc/sykom/ctstma", O_WRONLY);
    int fd_stat = open("/proc/sykom/ststma", O_RDONLY);
    int fd_res = open("/proc/sykom/restma", O_RDONLY);

    write(fd_a1, "2.5", 3);
    write(fd_a2, "4.0", 3);
    write(fd_ctrl, "1", 1);

    char buf[64];
    int n;
    do {
        lseek(fd_stat, 0, SEEK_SET);
        n = read(fd_stat, buf, sizeof(buf)-1);
        buf[n] = '\0';
    } while (strncmp(buf, "done", 4) != 0);

    lseek(fd_res, 0, SEEK_SET);
    n = read(fd_res, buf, sizeof(buf)-1);
    buf[n] = '\0';
    printf("Result: %s\n", buf);

    close(fd_a1); close(fd_a2); close(fd_ctrl); close(fd_stat); close(fd_res);
    return 0;
}