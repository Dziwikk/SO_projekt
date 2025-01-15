/* File: kasjer.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

int main(void)
{
    mkfifo("fifo_kasjer_in", 0666);
    mkfifo("fifo_kasjer_out", 0666);

    int fd_in = open("fifo_kasjer_in", O_RDONLY);
    int fd_out = open("fifo_kasjer_out", O_WRONLY);

    fprintf(stdout, "[KASJER] Start.\n");

    char buf[256];
    while (1) {
        ssize_t n = read(fd_in, buf, sizeof(buf)-1);
        if (n<=0) {
            if (n==0) {
                usleep(100000);
                continue;
            }
            break;
        }
        buf[n] = '\0';

        if (strncmp(buf, "BUY", 3)==0) {
            int pid, age, disc;
            sscanf(buf, "BUY %d %d %d", &pid, &age, &disc);
            fprintf(stdout, "[KASJER] Pasażer %d prosi o bilet (wiek=%d, disc=%d)\n",
                    pid, age, disc);
            // Prosta logika: <15 lub >70 => boat=2, else boat=1
            int boat=1;
            if (age<15 || age>70) boat=2;
            // Odpowiadamy
            dprintf(fd_out, "OK %d BOAT=%d DISC=%d\n", pid, boat, disc);
        }
        else if (strncmp(buf, "QUIT", 4)==0) {
            fprintf(stdout, "[KASJER] QUIT.\n");
            break;
        }
    }

    close(fd_in);
    close(fd_out);
    fprintf(stdout, "[KASJER] Zakończono.\n");
    return 0;
}
