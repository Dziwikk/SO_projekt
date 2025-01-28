/*******************************************************
 * File: kasjer.c
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_PIDS  5000
#define BUFSZ     4096  // Bufor do czytania z FIFO

/* Tablica, która zapamiętuje, czy dany pid już płynął: 
   traveled[pid] = 0 (nie płynął), 1 (już płynął) */
static int traveled[MAX_PIDS];

/* Flaga kończąca pętlę główną kasjera */
static volatile int end_kasjer = 0;

/* --------------------------------------------------- *
 * Funkcja obsługująca pojedynczą linię komendy.
 * line może mieć postać:
 *   "BUY 1234 27 0 fifo_pasazer_1234"
 *   "BUY 1001 10 50 fifo_inne"
 *   "QUIT"
 * itd.
 * --------------------------------------------------- */
static void handle_line(char *line)
{
    // Usuwamy ewentualne znaki \r na końcu (Windowsowe)
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';

    // Sprawdzamy, czy to komenda BUY
    if (strncmp(line, "BUY", 3) == 0) {
        int pid, age, group;
        char fifo_response[128]; // nazwa FIFO pasażera do odpowiedzi
        // Parsujemy: "BUY %d %d %d %s"
        int c = sscanf(line, "BUY %d %d %d %s", &pid, &age, &group, fifo_response);
        if (c < 3) {
            printf("[KASJER] Błędne: %s\n", line);
            return;
        }
        // Jeśli nie podano nazwy FIFO, ustawiamy domyślną (opcjonalne)
        if (c < 4) {
            strcpy(fifo_response, "fifo_kasjer_out");
        }

        printf("[KASJER] Pasażer %d (wiek=%d), group=%d\n", pid, age, group);

        // Wybór łodzi (boat = 1 lub 2) na pierwszy rejs
        // Domyślnie losujemy, ale zmienimy wg warunków:
        //   - if group>0 => boat = 2
        //   - if age<15 => boat = 2
        //   - if age>70 => boat = 2
        //   - inaczej boat = 1 lub 2 wylosowane
        int boat = (rand() % 2) + 1;

        if (group > 0) {
            // Dziecko + dorosły => łódź 2
            boat = 2;
        } else {
            // Bez grupy
            if (age < 15 || age > 70) {
                // Dzieci < 15 i seniorzy > 70 => tylko łódź 2
                boat = 2;
            }
        }

        // Sprawdzamy, czy to pierwszy rejs, czy już drugi
        // (tzn. passenger o PID-ie 'pid' już pływał?)
        int discount = 0;
        int skip = 0;  // skip=1 => pasażer omija kolejkę

        if (pid >= 0 && pid < MAX_PIDS) {
            if (!traveled[pid]) {
                // Pierwszy rejs tego pid-a
                traveled[pid] = 1;
                
                // Jeśli maluch < 3 lat => 100% zniżki
                if (age < 3) {
                    discount = 100;
                }
            } else {
                // Ten pid już pływał => to drugi (lub kolejny) rejs
                skip = 1;  // omija kolejkę
                // Ustalamy zniżkę
                if (age < 3) {
                    discount = 100;  // maluch zawsze za darmo
                } else {
                    discount = 50;   // pozostali 50% zniżki
                }

                // Zgodnie z wymaganiami "drugi rejs = dowolna łódź"
                // Więc bez względu na wiek/grupę – teraz może pójść 1 lub 2
                boat = (rand() % 2) + 1;
            }
        }

        // Otwieramy FIFO pasażera w trybie zapisu
        int fd_resp = open(fifo_response, O_WRONLY);
        if (fd_resp < 0) {
            perror("[KASJER] open fifo_response");
            return;
        }

        // Wysyłamy odpowiedź:
        // "OK <pid> BOAT=<1|2> DISC=<discount> SKIP=<0|1> GROUP=<group>"
        char resp_buf[256];
        snprintf(resp_buf, sizeof(resp_buf),
                 "OK %d BOAT=%d DISC=%d SKIP=%d GROUP=%d\n",
                 pid, boat, discount, skip, group);
        write(fd_resp, resp_buf, strlen(resp_buf));
        close(fd_resp);
    }
    else if (strncmp(line, "QUIT", 4) == 0) {
        printf("[KASJER] QUIT => end.\n");
        end_kasjer = 1;
    }
    else {
        // Być może pusta linia lub nieznana komenda
        if (line[0] != '\0') {
            printf("[KASJER] Nieznane: %s\n", line);
        }
    }
}

/* --------------------------------------------------- *
 * Główny program kasjera
 * --------------------------------------------------- */
int main(void)
{
    // Tworzymy FIFO do komunikacji (o ile nie istnieje)
    mkfifo("fifo_kasjer_in", 0666);
    // (Jeśli korzystamy z jednego wspólnego FIFO wyjściowego, można by tu też
    //  je utworzyć, np. mkfifo("fifo_kasjer_out", 0666);)

    // Otwieramy FIFO we/wy w odpowiednich trybach
    int fd_in = open("fifo_kasjer_in", O_RDONLY);
    if (fd_in < 0) {
        perror("[KASJER] open fifo_kasjer_in");
        return 1;
    }

    // Dummy-writer do tego samego FIFO, by zapobiec zamknięciu przy braku piszących
    int fd_dummy = open("fifo_kasjer_in", O_WRONLY);
    if (fd_dummy < 0) {
        perror("[KASJER] open fifo_kasjer_in (dummy)");
        close(fd_in);
        return 1;
    }

    // Zerujemy tablicę traveled
    for (int i = 0; i < MAX_PIDS; i++) {
        traveled[i] = 0;
    }

    printf("[KASJER] Start.\n");
    setbuf(stdout, NULL);

    // Bufor do czytania i zmienna rbuf_len - ile mamy danych w buforze
    static char rbuf[BUFSZ];
    int rbuf_len = 0;

    // Główna pętla odczytu z FIFO
    while (!end_kasjer) {
        ssize_t n = read(fd_in, rbuf + rbuf_len, sizeof(rbuf) - rbuf_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // sygnał, próbujemy dalej
            }
            perror("[KASJER] read");
            break;
        }
        if (n == 0) {
            // Brak danych (zakończenie zapisów) - ale mamy fd_dummy,
            // więc FIFO raczej się nie zamknie całkowicie
            usleep(10000); // drobna przerwa
            continue;
        }

        // Mamy n bajtów świeżo wczytanych. Zwiększamy rbuf_len
        rbuf_len += n;

        // Szukamy wszystkich linii zakończonych '\n'
        int start = 0;
        while (1) {
            char *nl = memchr(rbuf + start, '\n', rbuf_len - start);
            if (!nl) {
                // Brak kolejnego newline => przerywamy
                break;
            }
            // Pozycja newline
            int line_len = (int)(nl - (rbuf + start));
            // Kopiujemy tę linijkę do tymczasowego bufora
            char line[512];
            if (line_len >= (int)sizeof(line)) {
                // Linia za długa -> ucinamy
                line_len = (int)sizeof(line) - 1;
            }
            memcpy(line, rbuf + start, line_len);
            line[line_len] = '\0';

            // Obsługujemy linię
            handle_line(line);

            // Przesuwamy start za newline
            start += line_len + 1;
        }

        // Jeśli zostały niedokończone bajty na końcu,
        // przesuwamy je na początek bufora
        if (start < rbuf_len) {
            int leftover = rbuf_len - start;
            memmove(rbuf, rbuf + start, leftover);
            rbuf_len = leftover;
        } else {
            rbuf_len = 0;
        }
    }

    // Kończymy
    close(fd_in);
    close(fd_dummy);
    printf("[KASJER] end.\n");
    return 0;
}
