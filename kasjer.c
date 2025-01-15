/*******************************************************
 * File: kasjer.c
 *
 * Zasada:
 *   - "BUY <pid> <age>"
 *   - Dzieci <3 lat -> discount=100%
 *   - Dzieci <15 lub osoby >70 -> boat=2 (zależnie od Twojego projektu)
 *   - Drugi raz (traveled[pid]==1) -> skip=1, discount=50% (o ile nie ma jeszcze
 *     silniejszego powodu, np. <3 lat = 100%).
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_PIDS 5000
static int traveled[MAX_PIDS]; // 0 - pierwszy raz, 1 - już płynął

int main(void)
{
    /* Tworzymy FIFO (kasjer_in, kasjer_out) */
    mkfifo("fifo_kasjer_in", 0666);
    mkfifo("fifo_kasjer_out", 0666);

    int fd_in = open("fifo_kasjer_in", O_RDONLY | O_NONBLOCK);
    int fd_out = open("fifo_kasjer_out", O_WRONLY);

    if (fd_in < 0 || fd_out < 0) {
        perror("[KASJER] Błąd otwarcia FIFO");
        return 1;
    }

    for (int i = 0; i < MAX_PIDS; i++) {
        traveled[i] = 0;
    }

    printf("[KASJER] Start.\n");

    char buf[256];
    while (1) {
        ssize_t n = read(fd_in, buf, sizeof(buf)-1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                usleep(100000);
                continue;
            }
            perror("[KASJER] read error");
            break;
        }
        if (n == 0) {
            usleep(100000);
            continue;
        }

        buf[n] = '\0';
        
        if (strncmp(buf, "BUY", 3) == 0) {
            int pid, age;
            sscanf(buf, "BUY %d %d", &pid, &age);
            printf("[KASJER] Pasażer %d (wiek=%d)\n", pid, age);

            /* Ustalamy łódź */
            int boat = 1;
            // np. w Twoim projekcie: jeśli <15 lub >70 -> boat=2
            if (age < 15 || age > 70) {
                boat = 2;
            }

            /* Logika zniżek */
            int discount = 0; 
            int skip = 0;

            if (pid >= 0 && pid < MAX_PIDS) {
                if (!traveled[pid]) {
                    /* pierwszy raz */
                    traveled[pid] = 1;

                    /* Dzieci <3 lat -> discount=100% (darmowe) */
                    if (age < 3) {
                        discount = 100;
                    }
                } else {
                    /* już płynął -> discount=50%, skip=1 */
                    discount = 50;
                    skip = 1;

                    /* ALE: jeśli to dziecko <3, to i tak discount=100%,
                     * można zadecydować, która reguła jest „silniejsza”. 
                     * Przykład: 
                     if (age <3) {
                         discount=100;
                     }
                     */
                }
            }

            /* Odpowiadamy pasażerowi */
            dprintf(fd_out, "OK %d BOAT=%d DISC=%d SKIP=%d\n",
                    pid, boat, discount, skip);
        }
        else if (strncmp(buf, "QUIT", 4) == 0) {
            printf("[KASJER] QUIT -> kończę.\n");
            break;
        }
        else {
            printf("[KASJER] Nieznany komunikat: %s\n", buf);
        }
    }

    close(fd_in);
    close(fd_out);
    printf("[KASJER] Zakończono.\n");
    return 0;
}
