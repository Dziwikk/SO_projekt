/* File: pasazer.c
   Uruchamiany jako: ./pasazer <pass_id> <age> <discount>
   Kupuje bilet u kasjera, potem wysyła "QUEUE" do sternika i "START" by wymusić rejs
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    if (argc<4) {
        fprintf(stdout, "[PASAZER] Użycie: %s <id> <age> <disc>\n", argv[0]);
        return 1;
    }
    int pid = atoi(argv[1]);
    int age = atoi(argv[2]);
    int disc= atoi(argv[3]);

    int fd_ki = open("fifo_kasjer_in", O_WRONLY);
    int fd_ko = open("fifo_kasjer_out", O_RDONLY);

    if (fd_ki<0 || fd_ko<0) {
        perror("[PASAZER] Błąd otwarcia FIFO kasjera");
        return 1;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "BUY %d %d %d\n", pid, age, disc);
    write(fd_ki, buf, strlen(buf));

    int n = read(fd_ko, buf, sizeof(buf)-1);
    if (n<=0) {
        fprintf(stdout, "[PASAZER %d] Kasjer nie odpowiada.\n", pid);
        close(fd_ki);
        close(fd_ko);
        return 1;
    }
    buf[n] = '\0';
    if (strncmp(buf, "OK", 2)==0) {
        int got_id, boat, d;
        sscanf(buf, "OK %d BOAT=%d DISC=%d", &got_id, &boat, &d);
        fprintf(stdout, "[PASAZER %d] Kupiłem bilet: łódź=%d disc=%d\n", pid, boat, d);

        int fd_st = open("fifo_sternik_in", O_WRONLY);
        if (fd_st>=0) {
            snprintf(buf, sizeof(buf), "QUEUE %d %d %d\n", pid, boat, d);
            write(fd_st, buf, strlen(buf));

            // Wymuszamy start
            sleep(1);
            snprintf(buf, sizeof(buf), "START %d\n", boat);
            write(fd_st, buf, strlen(buf));

            close(fd_st);
        }
    } else {
        fprintf(stdout, "[PASAZER %d] Bilet odrzucony.\n", pid);
    }

    close(fd_ki);
    close(fd_ko);

    // Pasażer kończy
    return 0;
}
