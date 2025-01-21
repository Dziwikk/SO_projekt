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

static int traveled[MAX_PIDS];
static volatile int end_kasjer = 0;

/* --------------------------------------------------- *
 * Funkcja obsługująca pojedynczą linię komendy.
 * line może mieć postać "BUY 1234 27 0 fifo_pasazer_1234"
 * albo "QUIT", albo coś innego.
 * --------------------------------------------------- */
static void handle_line(char *line)
{
    // Usuwamy ewentualne znaki \r na końcu
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';

    // Sprawdzamy, czy to komenda BUY
    if (strncmp(line, "BUY", 3) == 0) {
        int pid, age, group;
        char fifo_response[128]; // nazwa FIFO do odpowiedzi
        // Parsujemy "BUY %d %d %d %s"
        int c = sscanf(line, "BUY %d %d %d %s", &pid, &age, &group, fifo_response);
        if (c < 3) {
            printf("[KASJER] Błędne: %s\n", line);
            return;
        }
        // Jeśli nie podano nazwy FIFO, ustawiamy domyślną
        if (c < 4) {
            strcpy(fifo_response, "fifo_kasjer_out");
        }

        printf("[KASJER] Pasażer %d (wiek=%d) group=%d\n", pid, age, group);

        // Wybór łodzi (boat = 1 lub 2)
        int boat =  boat = (rand() % 2) + 1;
        if (group > 0) {
            boat = 2;
        } else {
            if (age < 15 || age > 70) boat = 2;
        }

        // Wyliczamy zniżki i skip
        int discount = 0, skip = 0;
        if (pid >= 0 && pid < MAX_PIDS) {
            if (!traveled[pid]) {
                traveled[pid] = 1;
                if (age < 3) discount = 100;  // maluch za darmo
            } else {
                // Ten pid już pływał
                skip = 1;
                // dzieci < 3 wciąż 100% zniżki, reszta 50%
                if (age < 3) discount = 100;
                else discount = 50;
            }
        }

        // Otwieramy FIFO pasażera
        int fd_resp = open(fifo_response, O_WRONLY);
        if (fd_resp < 0) {
            perror("[KASJER] open fifo_response");
            return;
        }

        // Wysyłamy odpowiedź
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
    // Tworzymy lub usuwamy FIFO wg potrzeb
    mkfifo("fifo_kasjer_in", 0666);
    // Jeśli nie używamy wspólnego wyjściowego FIFO, to je usuwamy
    unlink("fifo_kasjer_out");

    // Otwieramy FIFO w trybie O_RDONLY
    int fd_in = open("fifo_kasjer_in", O_RDONLY);
    if (fd_in < 0) {
        perror("[KASJER] open fifo_kasjer_in");
        return 1;
    }

    // Otwieramy "dummy" deskryptor do fifo_kasjer_in w O_WRONLY,
    // żeby FIFO nie zamknęło się, gdy nikt nie pisze
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

    // Pętla główna
    while (!end_kasjer) {
        // Czytamy do wolnej części bufora (rbuf + rbuf_len)
        ssize_t n = read(fd_in, rbuf + rbuf_len, sizeof(rbuf) - rbuf_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // sygnał, próbujemy dalej
            }
            perror("[KASJER] read");
            break;
        }
        if (n == 0) {
            // Brak danych w FIFO (writerzy mogą być zamknięci)
            // ...ale mamy fd_dummy, więc FIFO powinno żyć
            // Zwykle czekamy chwilę albo robimy continue
            //usleep(10000); // 0.01s
            continue;
        }

        // Zwiększamy liczbę bajtów w buforze
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
                // Linia za długa, ucinamy
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
