/*******************************************************
 * File: pasazer.c
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>


int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);

    if (argc < 4) {
        fprintf(stdout, "Użycie: %s <id> <age> <group>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]); 
    int age = atoi(argv[2]); 
    int grp = atoi(argv[3]); 

    /* 1) Tworzenie unikalnego FIFO do komunikacji (z kasjerem i sternikiem). */
    char fifo_response[64];
    snprintf(fifo_response, sizeof(fifo_response), "fifo_pasazer_%d", pid);
    unlink(fifo_response);  // na wszelki wypadek usuwamy ślad starego

    if (mkfifo(fifo_response, 0666) == -1) {
        perror("[PASAZER] mkfifo");
        return 1;
    }

    /* 2) Wysyłamy polecenie BUY do kasjera_in, podając nazwę swojego FIFO */
    int fd_ki = open("fifo_kasjer_in", O_WRONLY);
    if (fd_ki < 0) {
        perror("[PASAZER] open fifo_kasjer_in");
        unlink(fifo_response);
        return 1;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "BUY %d %d %d %s\n", pid, age, grp, fifo_response);

    if (write(fd_ki, buf, strlen(buf)) == -1) {
        perror("[PASAZER] write to fifo_kasjer_in");
        close(fd_ki);
        unlink(fifo_response);
        return 1;
    }
    close(fd_ki);

    /* 3) Odbieramy odpowiedź OK (lub błąd) od kasjera przez nasze fifo_pasazer_<pid>. */
    int fd_resp = open(fifo_response, O_RDONLY);
    if (fd_resp < 0) {
        perror("[PASAZER] open fifo_pasazer_ (kasjer)");
        unlink(fifo_response);
        return 1;
    }

    int tries = 0;
    int boat = 0, disc = 0, skip = 0, groupBack = 0;
    int ok = 0;

    while (1) {
        ssize_t n = read(fd_resp, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* Sprawdzamy, czy zaczyna się od "OK " */
            if (strncmp(buf, "OK", 2) == 0) {
                /*
                  Przykładowy format:
                  "OK 1234 BOAT=1 DISC=0 SKIP=0 GROUP=0\n"
                */
                sscanf(buf, "OK %*d BOAT=%d DISC=%d SKIP=%d GROUP=%d",
                       &boat, &disc, &skip, &groupBack);

                printf("[PASAZER %d] Dostalem od kasjera: %s", pid, buf);
                ok = 1;
                break;
            } else {
                printf("[PASAZER %d] (kasjer) Nieznana odp: %s\n", pid, buf);
            }
        } else if (n == 0) {
            // Kasjer zamknął FIFO — może brak odpowiedzi
            break;
        } else {
            // n < 0 => błąd odczytu
            if (errno != EINTR) {
                perror("[PASAZER] read (kasjer)");
                break;
            }
        }
        //usleep(100000); // co 0.1 s
    
    }
    close(fd_resp);

    if (!ok) {
        printf("[PASAZER %d] Kasjer nie odpowiedział poprawnie. Konczę.\n", pid);
        unlink(fifo_response);
        return 0;
    }

    /*
     * Mamy boat i disc, a także skip=0/1, które decyduje, czy wysłać do sternika
     * "QUEUE" czy "QUEUE_SKIP". 
     * Teraz pasażer "zgłasza się" do sternika, by stanąć w kolejce na łódź.
     */

    /* 4) Wysyłamy do sternika "QUEUE" lub "QUEUE_SKIP" z naszą nazwą FIFO. */
    int fd_st = open("fifo_sternik_in", O_WRONLY);
    if (fd_st < 0) {
        perror("[PASAZER] open fifo_sternik_in");
        // Zamiast wychodzić, można ewentualnie spróbować ponowić itp.
        unlink(fifo_response);
        return 1;
    }

    if (skip == 1) {
        // FORMAT: QUEUE_SKIP <pid> <boat> <disc> <fifo_pasazer_pid>
        snprintf(buf, sizeof(buf), "QUEUE_SKIP %d %d %d %s\n",
                 pid, boat, disc, fifo_response);
    } else {
        // FORMAT: QUEUE <pid> <boat> <disc> <fifo_pasazer_pid>
        snprintf(buf, sizeof(buf), "QUEUE %d %d %d %s\n",
                 pid, boat, disc, fifo_response);
    }

    if (write(fd_st, buf, strlen(buf)) == -1) {
        perror("[PASAZER] write to fifo_sternik_in");
        close(fd_st);
        unlink(fifo_response);
        return 1;
    }
    close(fd_st);

    /* 5) Teraz ponownie otwieramy własne fifo_pasazer_<pid> i 
     *    czekamy na wiadomość "UNLOADED <pid>" od sternika.
     *    Będzie to oznaczać zakończenie rejsu (lub 'force unload').
     */
    fd_resp = open(fifo_response, O_RDONLY);
    if (fd_resp < 0) {
        perror("[PASAZER] open fifo_pasazer_ (sternik)");
        unlink(fifo_response);
        return 1;
    }

    int got_unloaded = 0;
    while (1) {
        ssize_t n = read(fd_resp, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';

            /* Przykładowo sternik wysyła "UNLOADED 1234\n" */
            if (strncmp(buf, "UNLOADED", 8) == 0) {
                int who = -1;
                sscanf(buf, "UNLOADED %d", &who);
                if (who == pid) {
                    printf("[PASAZER %d] Otrzymałem UNLOADED -> kończę.\n", pid);
                    got_unloaded = 1;
                    break;
                } else {
                    // Może akurat była inna linia dla kogoś innego 
                    // (mało prawdopodobne, ale w multi-FIFO też się zdarza)
                    printf("[PASAZER %d] Otrzymałem UNLOADED %d (nie moje?)\n", pid, who);
                }
            } else {
                printf("[PASAZER %d] (sternik) Nieznane: %s\n", pid, buf);
            }
        }
        else if (n == 0) {
            // Koniec pliku — writer (sternik) zamknął FIFO. 
            // Jeśli nie dostaliśmy "UNLOADED", to trudno, kończymy.
            //break;
        }
        else {
            if (errno != EINTR) {
                perror("[PASAZER] read (sternik)");
                break;
            }
        }
    }

    close(fd_resp);
    unlink(fifo_response);

    if (!got_unloaded) {
        printf("[PASAZER %d] Nie doczekałem się 'UNLOADED'. Koniec.\n", pid);
          close(fd_resp);
    unlink(fifo_response);
    }

    return 0;
}
