/*******************************************************
 * File: orchestrator.c
 *
 * Główna „centrala” uruchamiająca symulację.
 * Obsługuje komendy z stdin:
 *   - p     -> uruchom policjanta (o ile jeszcze nie działa)
 *   - q     -> zakończ całą symulację natychmiast
 *
 * Uruchamia wątek generatora pasażerów:
 *   - co kilka sekund tworzy pasażerów
 *   - *niektórzy* wracają (ten sam pid), co w kasjerze
 *     zostanie wykryte i da skip=1.
 *
 * Po TIMEOUT sekundach symulacja sama się kończy (time_killer).
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/select.h>  /* do użycia select() */

#define PATH_STERNIK   "./sternik"
#define PATH_KASJER    "./kasjer"
#define PATH_PASAZER   "./pasazer"
#define PATH_POLICJANT "./policjant"

#define FIFO_STERNIK_IN  "fifo_sternik_in"
#define FIFO_STERNIK_OUT "fifo_sternik_out"
#define FIFO_KASJER_IN   "fifo_kasjer_in"
#define FIFO_KASJER_OUT  "fifo_kasjer_out"

#define MAX_PASS 200

/* Czas całkowity symulacji (w sekundach) */
#define TIMEOUT 60

static volatile sig_atomic_t end_all = 0;

static pid_t pid_sternik = 0;
static pid_t pid_kasjer  = 0;
static pid_t pid_policjant = 0;

/* Tutaj zapisujemy PID-y uruchomionych pasażerów (procesów) */
static pid_t p_pass[MAX_PASS];
static int   pass_count = 0;

/* Wątek generatora pasażerów */
static pthread_t generator_thread;
static volatile int generator_running = 1;

/* Wątek time_killer – kończy symulację po TIMEOUT s */
static pthread_t time_killer_thread;

/* Funkcje prototypy */
void end_simulation(void);
void cleanup_fifo(void);

/* ----------------------------------------------------- */
/* Uruchamianie childa (bez przechwytywania stdout) */
static pid_t run_child(const char *cmd, char *const argv[])
{
    pid_t cpid = fork();
    if (cpid < 0) {
        perror("[ORCH] fork");
        return -1;
    }
    if (cpid == 0) {
        /* Proces potomny */
        execv(cmd, argv);
        perror("[ORCH] execv error");
        _exit(1);
    }
    /* Proces macierzysty */
    return cpid;
}

/* ----------------------------------------------------- */
/* Uruchamianie *jednego* pasażera o konkretnym pid i age */
static void run_passenger(int pid, int age)
{
    if (pass_count >= MAX_PASS) {
        printf("[ORCH] Osiągnięto MAX_PASS=%d\n", MAX_PASS);
        return;
    }
    char arg1[32], arg2[32];
    sprintf(arg1, "%d", pid);
    sprintf(arg2, "%d", age);

    char *args[] = { (char*)PATH_PASAZER, arg1, arg2, NULL };

    pid_t cpid = fork();
    if (cpid == 0) {
        execv(PATH_PASAZER, args);
        perror("[ORCH] execv pasazer");
        _exit(1);
    } else if (cpid > 0) {
        p_pass[pass_count++] = cpid;
        printf("[ORCH] Uruchomiono pasażera PID=%d (wiek=%d, procPID=%d)\n",
               pid, age, cpid);
        usleep(200000); // krótka pauza
    } else {
        perror("[ORCH] fork pasazer");
    }
}

/* ----------------------------------------------------- */
/* Wątek generatora pasażerów – generuje co kilka sekund.
   Część pasażerów jest NOWA, a część WRACA (ten sam pid). */
static void *generator_func(void *arg)
{
    /* Tablica pidów – unikalnych pasażerów */
    static int generated_pids[1000]; 
    static int unique_count = 0;
    int base_pid = 1000; /* zacznijmy od 1000 */

    srand(time(NULL) ^ getpid());

    while (!end_all && generator_running) {
        if (unique_count > 0 && (rand()%2 == 1)) {
            /* (50% szans) – WRACAJĄCY pasażer */
            int idx = rand() % unique_count; 
            int old_pid = generated_pids[idx];
            int age = rand()%80 + 1;
            printf("[GEN] Pasażer WRACA: pid=%d age=%d\n", old_pid, age);
            run_passenger(old_pid, age); 
        } else {
            /* (pozostałe ~50%) – NOWY pasażer */
            int new_pid = base_pid + unique_count;
            generated_pids[unique_count] = new_pid;
            unique_count++;

            int age = rand()%80 + 1;
            printf("[GEN] Pasażer NOWY: pid=%d age=%d\n", new_pid, age);
            run_passenger(new_pid, age);
        }

        /* przerwa 2..5 sekund, w małych kawałkach sprawdzamy end_all */
        int slp = rand()%4 + 2; // 2..5 sek
        for (int i=0; i<slp*10 && !end_all; i++) {
            usleep(100000);
        }

        if (unique_count >= 1000) {
            /* zapobiegawczo, by nie wyjść poza tablicę */
            printf("[GEN] Osiągnięto 1000 unikalnych pidów, wstrzymuję generację.\n");
            break;
        }
    }

    return NULL;
}

/* ----------------------------------------------------- */
/* Wątek time_killer – po TIMEOUT s kończy symulację */
static void *time_killer_func(void *arg)
{
    sleep(TIMEOUT);
    if (!end_all) {
        printf("[ORCH/TIME] Minęło %d s -> kończę symulację.\n", TIMEOUT);
        end_simulation();
    }
    return NULL;
}

/* ----------------------------------------------------- */
/* Usuwanie starych plików FIFO */
void cleanup_fifo(void) {
    unlink(FIFO_STERNIK_IN);
    unlink(FIFO_STERNIK_OUT);
    unlink(FIFO_KASJER_IN);
    unlink(FIFO_KASJER_OUT);
}

/* ----------------------------------------------------- */
/* Koniec symulacji – wysyłamy QUIT do kasjera i sternika, zabijamy procesy */
void end_simulation(void)
{
    if (end_all) return;
    end_all = 1;

    printf("[ORCH] end_simulation() -> QUIT + kill...\n");

    /* Zatrzymujemy generator pasażerów */
    generator_running = 0;
    printf("[ORCH] Zatrzymujemy generator pasażerów\n");

    /* Wysyłamy QUIT do kasjera */
    if (pid_kasjer>0) {
        int fdk = open(FIFO_KASJER_IN, O_WRONLY);
        if (fdk>=0) {
            write(fdk, "QUIT\n", 5);
            close(fdk);
        }
        printf("[ORCH] Wysłano QUIT do kasjera (PID=%d)\n", pid_kasjer);
    }

    /* Wysyłamy QUIT do sternika */
    if (pid_sternik>0) {
        int fds = open(FIFO_STERNIK_IN, O_WRONLY);
        if (fds>=0) {
            write(fds, "QUIT\n", 5);
            close(fds);
        }
        printf("[ORCH] Wysłano QUIT do sternika (PID=%d)\n", pid_sternik);
    }

    /* Zabijamy policjanta, pasażerów, kasjera, sternika (jeśli jeszcze żyją) */
    if (pid_policjant>0) kill(pid_policjant, SIGTERM);

    for (int i=0; i<pass_count; i++) {
        if (p_pass[i]>0) {
            kill(p_pass[i], SIGTERM);
        }
    }

    if (pid_kasjer>0)   kill(pid_kasjer, SIGTERM);
    if (pid_sternik>0)  kill(pid_sternik, SIGTERM);

    printf("[ORCH] Czekamy na wszystkich (żeby uniknąć zombie).\n");

    /* Czekamy na wszystkich */
    if (pid_policjant>0) {
        waitpid(pid_policjant, NULL, 0);
        pid_policjant=0;
    }
    for (int i=0; i<pass_count; i++) {
        if (p_pass[i]>0) {
            waitpid(p_pass[i], NULL, 0);
            p_pass[i]=0;
        }
    }
    if (pid_kasjer>0) {
        waitpid(pid_kasjer, NULL, 0);
        pid_kasjer=0;
    }
    if (pid_sternik>0) {
        waitpid(pid_sternik, NULL, 0);
        pid_sternik=0;
    }

    cleanup_fifo();
    printf("[ORCH] Symulacja zakończona.\n");
}

/* ----------------------------------------------------- */
/* Start sternika: przekazujemy TIMEOUT jako argument (w sekundach) */
static void start_sternik(void)
{
    if (pid_sternik!=0) {
        printf("[ORCH] Sternik już działa (PID=%d).\n", pid_sternik);
        return;
    }
    char arg_timeout[32];
    sprintf(arg_timeout, "%d", TIMEOUT);
    char *args[] = { (char*)PATH_STERNIK, arg_timeout, NULL };

    pid_t cpid = run_child(PATH_STERNIK, args);
    if (cpid>0) {
        pid_sternik = cpid;
        printf("[ORCH] Uruchomiono sternika, PID=%d, TIMEOUT=%d.\n", cpid, TIMEOUT);
    }
}

/* ----------------------------------------------------- */
/* Start kasjera */
static void start_kasjer(void)
{
    if (pid_kasjer!=0) {
        printf("[ORCH] Kasjer już działa (PID=%d).\n", pid_kasjer);
        return;
    }
    char *args[] = { (char*)PATH_KASJER, NULL };
    pid_t cpid = run_child(PATH_KASJER, args);
    if (cpid>0) {
        pid_kasjer=cpid;
        printf("[ORCH] Uruchomiono kasjera, PID=%d.\n", cpid);
    }
}

/* ----------------------------------------------------- */
/* Start policjanta */
static void start_policjant(void)
{
    if (pid_sternik<=0) {
        printf("[ORCH] Nie ma sternika -> policjant nie ma do kogo wysłać sygnałów.\n");
        return;
    }
    if (pid_policjant!=0) {
        printf("[ORCH] Policjant już działa (PID=%d)\n", pid_policjant);
        return;
    }
    char arg_pid[32];
    sprintf(arg_pid, "%d", pid_sternik);
    char *args[] = { (char*)PATH_POLICJANT, arg_pid, NULL };

    pid_t cpid = run_child(PATH_POLICJANT, args);
    if (cpid>0) {
        pid_policjant=cpid;
        printf("[ORCH] Uruchomiono policjanta, PID=%d -> sternik=%d.\n", cpid, pid_sternik);
    }
}

/* ----------------------------------------------------- */
/* main – pętla główna z select() + timeout (100ms),
   by nie blokować się na fgets(). Komendy:
   - p -> start policjanta
   - q -> end_simulation
   (komenda r <N> jest zakomentowana, jeśli chcesz, odkomentuj). */
int main(void)
{
    setbuf(stdout, NULL);

    /* Czyścimy i tworzymy FIFO */
    cleanup_fifo();
    mkfifo(FIFO_STERNIK_IN, 0666);
    mkfifo(FIFO_STERNIK_OUT, 0666);
    mkfifo(FIFO_KASJER_IN, 0666);
    mkfifo(FIFO_KASJER_OUT, 0666);

    /* Uruchamiamy sternika i kasjera */
    start_sternik();
    sleep(1);
    start_kasjer();
    sleep(1);

    /* Wątek generatora pasażerów */
    pthread_create(&generator_thread, NULL, generator_func, NULL);

    /* Wątek time_killer -> po TIMEOUT sek kończy symulację */
    pthread_create(&time_killer_thread, NULL, time_killer_func, NULL);

    printf("[ORCH] Dostępne komendy:\n");
    //printf("  r <N> -> generuj N pasażerów (ręcznie)\n");
    printf("  p     -> uruchom policjanta\n");
    printf("  q     -> zakończ symulację\n");
    printf("Czas maksymalny: %d sekund.\n", TIMEOUT);

    char cmd[128];

    while (!end_all) {
        /* 1. Sprawdzamy, czy sternik się zakończył */
        if (pid_sternik > 0) {
            int status;
            pid_t w = waitpid(pid_sternik, &status, WNOHANG);
            if (w == pid_sternik) {
                printf("[ORCH] Sternik zakończył pracę -> end_simulation.\n");
                end_simulation();
                break;
            }
        }

        /* 2. Wyświetlamy prompt (opcjonalnie) */
        //printf("> ");
        fflush(stdout);

        /* 3. select() z timeout=100ms, by nie blokować się na fgets() */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 0.1 s

        int ret = select(STDIN_FILENO+1, &readfds, NULL, NULL, &tv);
        if (ret<0) {
            if (errno==EINTR) continue;
            perror("[ORCH] select error");
            break;
        }
        if (ret==0) {
            /* timeout upłynął, brak danych na stdin */
            continue;
        }

        /* 4. Jest coś na stdin */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(cmd, sizeof(cmd), stdin)) {
                /* np. ctrl+D */
                break;
            }

            /* 5. Parsujemy komendę */
            if (cmd[0]=='q') {
                end_simulation();
                break;
            } else if (cmd[0]=='p') {
                start_policjant();
            } 
            /*
            else if (cmd[0]=='r') {
                int n;
                if (sscanf(cmd, "r %d", &n)==1) {
                    printf("[ORCH] Generuję %d pasażerów...\n", n);
                    for (int i=0; i<n; i++) {
                        // można wywołać run_passenger z unikalnym pid
                        // lub prosto generate_passengers(1)
                        generate_passengers(1);
                    }
                } else {
                    printf("[ORCH] Błędna składnia: r <N>\n");
                }
            } 
            */
            else {
                printf("[ORCH] Nieznana komenda.\n");
            }
        }
    }

    if (!end_all) {
        end_simulation();
    }

    /* Czekamy na wątki generatora i time_killera */
    pthread_join(generator_thread, NULL);
    pthread_join(time_killer_thread, NULL);

    return 0;
}
