/*******************************************************
 File: pasazer.c
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    setbuf(stdout,NULL);
    if (argc<4) {
        fprintf(stdout,"Użycie: %s <id> <age> <group>\n",argv[0]);
        return 1;
    }
    int pid= atoi(argv[1]);
    int age= atoi(argv[2]);
    int grp= atoi(argv[3]);

    int fd_ki = open("fifo_kasjer_in", O_WRONLY);
    int fd_ko = open("fifo_kasjer_out",O_RDONLY|O_NONBLOCK);
    if (fd_ki<0 || fd_ko<0) {
        perror("[PASAZER] open kasjer FIFO");
        return 1;
    }

    /* Wysyłamy BUY pid age group */
    char buf[256];
    snprintf(buf,sizeof(buf),"BUY %d %d %d\n", pid, age, grp);
    write(fd_ki, buf, strlen(buf));

    int tries=0;
    int boat=0, disc=0, skip=0, groupBack=0;
    int ok=0;
    while (tries<50) {
        ssize_t n= read(fd_ko, buf, sizeof(buf)-1);
        if (n>0) {
            buf[n]='\0';
            if (!strncmp(buf,"OK",2)) {
                int got_pid;
                sscanf(buf,"OK %d BOAT=%d DISC=%d SKIP=%d GROUP=%d",&got_pid,&boat,&disc,&skip,&groupBack);
                if (got_pid==pid) {
                    printf("[PASAZER %d] OK boat=%d disc=%d skip=%d group=%d\n",
                           pid, boat, disc, skip, groupBack);
                    ok=1;
                }
                break;
            }
        }
        usleep(100000);
        tries++;
    }
    close(fd_ki);
    close(fd_ko);

    if (!ok) {
        printf("[PASAZER %d] Kasjer nie odpowiedział.\n", pid);
        return 1;
    }

    /* teraz sternik -> QUEUE or QUEUE_SKIP */
    int fd_st= open("fifo_sternik_in", O_WRONLY);
    if (fd_st<0) {
        printf("[PASAZER %d] brak fifo_sternik_in.\n",pid);
        return 1;
    }

    if (skip==1) {
        snprintf(buf,sizeof(buf),"QUEUE_SKIP %d %d\n", pid, boat);
    } else {
        snprintf(buf,sizeof(buf),"QUEUE %d %d %d\n", pid, boat, disc);
    }
    write(fd_st, buf, strlen(buf));
    close(fd_st);

    return 0;
}
