/*******************************************************
 * File: pasazer.c
 *
 * Uruchamiany z parametrami:
 *   ./pasazer <id> <age>
 *
 * Łączy się z kasjerem przez FIFO (IN/OUT),
 * wysyła polecenie BUY i czeka na odpowiedź OK.
 * Następnie ustawia się w kolejce do łodzi (QUEUE lub QUEUE_SKIP)
 * przez fifo_sternik_in.
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    
    if (argc<3) {
        fprintf(stdout, "[PASAZER] Użycie: %s <id> <age>\n", argv[0]);
        return 1;
    }
    int pid = atoi(argv[1]);
    int age = atoi(argv[2]);

    /* Otwieramy FIFO kasjera */
    int fd_ki = open("fifo_kasjer_in", O_WRONLY);
    int fd_ko = open("fifo_kasjer_out", O_RDONLY | O_NONBLOCK);

    if (fd_ki<0 || fd_ko<0) {
        perror("[PASAZER] Błąd otwarcia FIFO kasjera");
        return 1;
    }

    /* Wysyłamy polecenie BUY */
    char buf[256];
    snprintf(buf, sizeof(buf), "BUY %d %d\n", pid, age);
    write(fd_ki, buf, strlen(buf));

    /* Czekamy na odpowiedź OK */
    int tries=0;
    int boat=0, disc=0, skip=0;
    int ok=0;
    while (tries<50) { // do ~5 sekund (50 * 0.1s)
        ssize_t n = read(fd_ko, buf, sizeof(buf)-1);
        if (n>0) {
            buf[n]='\0';
            if (strncmp(buf, "OK", 2)==0) {
                int got_id;
                sscanf(buf, "OK %d BOAT=%d DISC=%d SKIP=%d",
                       &got_id, &boat, &disc, &skip);
                if (got_id==pid) {
                    printf("[PASAZER %d] bilet: boat=%d, disc=%d, skip=%d\n",
                           pid, boat, disc, skip);
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
        /* Kasjer nie odpowiedział – rezygnujemy */
        printf("[PASAZER %d] kasjer nie odpowiedział.\n", pid);
        return 1;
    }

    /* Teraz do sternika -> QUEUE / QUEUE_SKIP */
    int fd_st = open("fifo_sternik_in", O_WRONLY);
    if (fd_st<0) {
        printf("[PASAZER %d] Nie mogę otworzyć fifo_sternik_in.\n", pid);
        return 1;
    }

    /* Jeśli skip=1 -> QUEUE_SKIP, inaczej -> QUEUE */
    if (skip==1) {
        snprintf(buf, sizeof(buf), "QUEUE_SKIP %d %d\n", pid, boat);
    } else {
        snprintf(buf, sizeof(buf), "QUEUE %d %d %d\n", pid, boat, disc);
    }
    write(fd_st, buf, strlen(buf));
    close(fd_st);

    //printf("[PASAZER %d] Koniec.\n", pid);
    return 0;
}
