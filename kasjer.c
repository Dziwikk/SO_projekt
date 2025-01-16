/*******************************************************
 File: kasjer.c
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_PIDS 5000
static int traveled[MAX_PIDS];

int main(void)
{
    mkfifo("fifo_kasjer_in",0666);
    mkfifo("fifo_kasjer_out",0666);

    int fd_in = open("fifo_kasjer_in", O_RDONLY|O_NONBLOCK);
    int fd_out= open("fifo_kasjer_out",O_WRONLY);

    if (fd_in<0 || fd_out<0) {
        perror("[KASJER] open FIFO");
        return 1;
    }
    for (int i=0; i<MAX_PIDS; i++) traveled[i]=0;

    printf("[KASJER] Start.\n");
    setbuf(stdout,NULL);

    char buf[256];
    while (1) {
        ssize_t n= read(fd_in, buf, sizeof(buf)-1);
        if (n<0) {
            if (errno==EAGAIN || errno==EINTR) {
                usleep(100000);
                continue;
            }
            perror("[KASJER] read");
            break;
        }
        if (n==0) {
            usleep(100000);
            continue;
        }
        buf[n]='\0';

        if (strncmp(buf,"BUY",3)==0) {
            int pid, age, group=0;
            int c= sscanf(buf,"BUY %d %d %d",&pid,&age,&group);
            if (c<2) {
                printf("[KASJER] Błędne: %s\n",buf);
                continue;
            }
            if (c<3) group=0;

            printf("[KASJER] Pasażer %d (wiek=%d) group=%d\n", pid, age, group);

            /* Wybieramy boat */
            int boat=1;
            /* jesli group !=0 => boat=2 */
            if (group>0) {
                boat=2;
            } else {
                /* normal: dziecko<15 => boat=2, wiek>70 => boat=2, otherwise boat=1 */
                if (age<15 || age>70) boat=2;
            }

            int discount=0, skip=0;
            if (pid>=0 && pid<MAX_PIDS) {
                if (!traveled[pid]) {
                    traveled[pid]=1; 
                    if (age<3) discount=100; 
                } else {
                    /* wraca */
                    skip=1;
                    if (age<3) discount=100;
                    else discount=50;
                }
            }

            /* Odpowiadamy pasażerowi */
            dprintf(fd_out,"OK %d BOAT=%d DISC=%d SKIP=%d GROUP=%d\n",
                    pid, boat, discount, skip, group);

        } else if (strncmp(buf,"QUIT",4)==0) {
            printf("[KASJER] QUIT => end.\n");
            break;
        } else {
            printf("[KASJER] Nieznane: %s\n", buf);
        }
    }

    close(fd_in);
    close(fd_out);
    printf("[KASJER] end.\n");
    return 0;
}
