/*******************************************************
 * File: orchestrator.c
 *
 * Komendy z stdin:
 *   p -> uruchom policjanta
 *   q -> zakończ symulację
 *
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
#include <sys/select.h>

/* Ścieżki do plików wykonywalnych */
#define PATH_STERNIK   "./sternik"
#define PATH_KASJER    "./kasjer"
#define PATH_PASAZER   "./pasazer"
#define PATH_POLICJANT "./policjant"

/* Nazwane FIFO */
#define FIFO_STERNIK_IN  "fifo_sternik_in"
#define FIFO_KASJER_IN   "fifo_kasjer_in"

/* Maksymalna liczba pasażerów */
#define MAX_PASS 1000

#define MAX_PID 5000 // limit pid-ow
#define BASE_PID 1000 // pid bazowy
/* Czas symulacji – ustalany przez usera */
static int TIMEOUT;

/* Flaga zakończenia */
static volatile sig_atomic_t end_all = 0;

/* Parametry sterujące generowaniem pasażerów */
static int g_min_batch = 3;  /* minimalna liczba pasażerów (normal) w jednej turze */
static int g_max_batch = 7;  /* maksymalna liczba pasażerów (normal) w jednej turze */

/* Licznik wszystkich wygenerowanych pasażerów */
static int total_generated = 0; 

static int passenger_pids[MAX_PASS+1];
static int passenger_pids_count = 0; /// do czyszczenia fifo

/* PID-y procesów: sternik, kasjer, policjant, pasażerowie */
static pid_t pid_sternik = 0;
static pid_t pid_kasjer  = 0;
static pid_t pid_policjant = 0;
static pid_t p_pass[MAX_PASS];
static int   pass_count = 0;

/* Wątek generatora i flaga sterująca jego pracą */
static pthread_t generator_thread;
static volatile int generator_running = 1;

/* Wątek time_killer */
static pthread_t time_killer_thread;

/* Deklaracje funkcji */
void end_simulation(void);
void cleanup_fifo(void);

/* Deklaracja (prototyp) */
static pid_t run_child(const char *cmd, char *const argv[]);

/* Tutaj definicja run_child */
static pid_t run_child(const char *cmd, char *const argv[])
{
    pid_t c = fork();
    if (c < 0) {
        perror("[ORCH] fork");
        return -1;
    }
    if (c == 0) {
        execv(cmd, argv);
        perror("[ORCH] execv");
        _exit(1);
    }
    return c;
}


/* ------------------------------- */
/* Funkcja tworząca pasażera z (pid,age,group) */
static void run_passenger(int pid, int age, int group)
{
    if (pass_count >= MAX_PASS) {
        printf("[ORCH] Osiągnięto MAX_PASS.\n");
        return;
    }
    char arg1[32], arg2[32], arg3[32];
    sprintf(arg1, "%d", pid);
    sprintf(arg2, "%d", age);
    sprintf(arg3, "%d", group);

    char *args[] = { (char*)PATH_PASAZER, arg1, arg2, arg3, NULL };

    pid_t c = fork();
    if (c == 0) {
        /* Proces potomny -> pasazer */
        execv(PATH_PASAZER, args);
        perror("[ORCH] execv pasazer");
        _exit(1);
    } else if (c > 0) {
        p_pass[pass_count++] = c;
        passenger_pids[passenger_pids_count++] = pid;
        printf("[ORCH] Passenger pid=%d age=%d group=%d -> procPID=%d\n",
               pid, age, group, c);
        total_generated++;
    } else {
        perror("[ORCH] fork pass");
    }
}


/* ------------------------------- */
/* Wątek generatora pasażerów */
void *generator_func(void *arg)
{
    static int used_pids[5000];
    static int used_count = 0;
    int base_pid = BASE_PID;

    srand(time(NULL) ^ getpid());

    while (!end_all && generator_running) {
        /* sprawdzenie, czy mamy już MAX_PASS */
        if (pass_count >= MAX_PASS) {
            printf("[ORCH] Osiągnięto MAX_PASS, kończę generowanie pasażerów.\n");
            break;
        }

        int dice = rand() % 4;

        if (dice == 0) {
            /* Scenariusz: para dziecko + rodzic */
            int grp = base_pid + used_count;
            // dziecko
            int child_pid = grp;
            used_pids[used_count] = child_pid;
            used_count++;

            int child_age = rand() % 14 + 1; // 1..14
            run_passenger(child_pid, child_age, grp);

            // rodzic
            int parent_pid = grp + 1000;
            int parent_age = rand() % 50 + 20; // 20..69
            run_passenger(parent_pid, parent_age, grp);

        } else if (dice == 1 && used_count > 0) {
            /* Scenariusz: wraca stary pid (skip=1 w kasjerze) */
            int idx = rand() % used_count;
            int old_pid = used_pids[idx];
            int age = rand() % 80 + 1;
            printf("[GEN] WRACA old_pid=%d age=%d\n", old_pid, age);
            run_passenger(old_pid, age, 0);

        } else {
            /* Scenariusz: "normal" - 3..7 pasażerów naraz.
               ALE jeśli wylosuje się child (<15), to generujemy parę child+rodzic 
               (żeby dziecko nie zostało bez opiekuna).
            */
            int how_many = rand() % (g_max_batch - g_min_batch + 1) + g_min_batch;  
            for (int i = 0; i < how_many; i++) {
                int age = rand() % 80 + 1;
                if (age < 15) {
                    int grp = base_pid + used_count; // nowa grupa
                    // Child
                    int child_pid = grp;
                    used_pids[used_count] = child_pid;
                    used_count++;
                    run_passenger(child_pid, age, grp);

                    // Rodzic
                    int parent_pid = grp + 1000;
                    int parent_age = rand() % 50 + 20; // 20..69
                    used_pids[used_count] = parent_pid;
                    used_count++;
                    run_passenger(parent_pid, parent_age, grp);

                } else {
                    int new_pid = base_pid + used_count;
                    used_pids[used_count] = new_pid;
                    used_count++;
                    run_passenger(new_pid, age, 0);
                }
                if (pass_count >= MAX_PASS) {
                    printf("[ORCH] Osiągnięto MAX_PASS, kończę generowanie pasażerów.\n");
                    break;
                }
            }
        }

        if (used_count >= MAX_PID) {
            printf("[GEN] Osiągnięto MAX_PID pid, stop.\n");
            break;
        }

        /* Pauza 1..2 sekundy */
        int slp = rand() % 2 + 1;
        for (int i = 0; i < slp * 10 && !end_all; i++) {
            //usleep(100000); // co 0.1 sek, łącznie ~1..2 s
        }
    }

    return NULL;
}

/* ------------------------------- */
/* Wątek time_killer -> kończy symulację po TIMEOUT sek. */
void *time_killer_func(void *arg)
{
    sleep(TIMEOUT); // musi byc, nie ma udzial w sumilacji tylko ja konczy

    if (!end_all) {
        printf("\033[1;31m[ORCH/TIME] Time out =%d -> end.\033[0m\n", TIMEOUT);
        end_simulation();
    }
    return NULL;
}

/* ------------------------------- */
void cleanup_fifo(void)
{
    unlink(FIFO_STERNIK_IN);
    unlink(FIFO_KASJER_IN);
}

/* ------------------------------- */
/* end_simulation -> QUIT do kasjera/sternika, potem kill, czekamy, sprzątamy */
/* Funkcja do usuwania wszystkich FIFO pasażerów */
void cleanup_passenger_fifos(void)
{
    for(int i = BASE_PID - 1; i < MAX_PID + 1; i++) {

        //int pid = passenger_pids[passenger_pids_count];
        char fifo_name[64];
        snprintf(fifo_name, sizeof(fifo_name), "fifo_pasazer_%d", i);
        //printf("czyszcze fifo"); //do potestowania
        if(unlink(fifo_name) == 0){
            //printf("[ORCH] Usunięto FIFO: %s\n", fifo_name);
        }
        else{
            if(errno != ENOENT){
                perror("[ORCH] unlink");
            }
            // Jeśli FIFO nie istnieje (ENOENT), to nic nie robimy
        }
    }
}



void end_simulation(void)
{
    if (end_all) return;
    end_all = 1;
    generator_running = 0;  

    printf("[ORCH] end_simulation() -> sprawdź, QUIT, kill -TERM, kill -9...\n");
    //printf("[ORCH] W sumie wygenerowano %d pasażerów.\n", total_generated);

    /* 0) sprawdzamy, kto już nie żyje */
    if (pid_policjant > 0) {
        pid_t w = waitpid(pid_policjant, NULL, WNOHANG);
        if (w == pid_policjant) pid_policjant = 0;
    }
    for(int i=0; i<pass_count; i++){
        if(p_pass[i] > 0){
            pid_t w = waitpid(p_pass[i], NULL, WNOHANG);
            if(w == p_pass[i]) p_pass[i] = 0;
        }
    }
    if(pid_kasjer > 0){
        pid_t w = waitpid(pid_kasjer, NULL, WNOHANG);
        if(w == pid_kasjer) pid_kasjer = 0;
    }
    if(pid_sternik > 0){
        pid_t w = waitpid(pid_sternik, NULL, WNOHANG);
        if(w == pid_sternik) pid_sternik = 0;
    }

    /* 1) QUIT */
    if(pid_kasjer > 0){
        int fk = open(FIFO_KASJER_IN, O_WRONLY);
        if(fk >= 0){
            write(fk, "QUIT\n", 5);
            close(fk);
        }
        printf("\033[1;32m[ORCH] (QUIT) kasjer pid=%d\033[0m\n", pid_kasjer);
    }
    if(pid_sternik > 0){
        int fs = open(FIFO_STERNIK_IN, O_WRONLY | O_NONBLOCK);
        if(fs >= 0){
            write(fs, "QUIT\n", 5);
            close(fs);
        }
        printf("\033[1;32m[ORCH] (QUIT) sternik pid=%d\033[0m\n", pid_sternik);
    }

    /* 2) SIGTERM */
    if(pid_policjant > 0) kill(pid_policjant, SIGTERM);
    for(int i=0; i<pass_count; i++){
        if(p_pass[i] > 0){
            kill(p_pass[i], SIGTERM);
        }
    }
    if(pid_kasjer > 0) kill(pid_kasjer, SIGTERM);
    if(pid_sternik > 0) kill(pid_sternik, SIGTERM);

    usleep(500000); // pozwala na zamkniecie procesow

    /* 4) SIGKILL */
    if(pid_policjant > 0){
        if(0 == waitpid(pid_policjant, NULL, WNOHANG)){
            kill(pid_policjant, SIGKILL);
        }
    }
    for(int i=0; i<pass_count; i++){
        if(p_pass[i] > 0){
            if(0 == waitpid(p_pass[i], NULL, WNOHANG)){
                kill(p_pass[i], SIGKILL);
            }
        }
    }
    if(pid_kasjer > 0){
        if(0 == waitpid(pid_kasjer, NULL, WNOHANG)){
            kill(pid_kasjer, SIGKILL);
        }
    }
    if(pid_sternik > 0){
        if(0 == waitpid(pid_sternik, NULL, WNOHANG)){
            kill(pid_sternik, SIGKILL);
        }
    }

    /* 5) czekamy */
    if(pid_policjant > 0){
        waitpid(pid_policjant, NULL, 0);
        pid_policjant = 0;
    }
    for(int i=0; i<pass_count; i++){
        if(p_pass[i] > 0){
            waitpid(p_pass[i], NULL, 0);
            p_pass[i] = 0;
        }
    }
    if(pid_kasjer > 0){
        waitpid(pid_kasjer, NULL, 0);
        pid_kasjer = 0;
    }
    if(pid_sternik > 0){
        waitpid(pid_sternik, NULL, 0);
        pid_sternik = 0;
    }
    cleanup_passenger_fifos();
    cleanup_fifo();
    printf("\033[1;32m[ORCH] end_simulation -> done.\033[0m\n");
}

/* ------------------------------- */
/* start sternik */
static void start_sternik(void)
{
    if(pid_sternik > 0){
        printf("[ORCH] sternik already.\n");
        return;
    }
    char arg[32];
    sprintf(arg, "%d", TIMEOUT);
    char *args[] = { (char*)PATH_STERNIK, arg, NULL };
    pid_t c = run_child(PATH_STERNIK, args);
    if(c > 0){
        pid_sternik = c;
        printf("[ORCH] sternik pid=%d.\n", c);
    }
}

/* start kasjer */
static void start_kasjer(void)
{
    if(pid_kasjer > 0){
        printf("[ORCH] kasjer already.\n");
        return;
    }
    char *args[] = { (char*)PATH_KASJER, NULL };
    pid_t c = run_child(PATH_KASJER, args);
    if(c > 0){
        pid_kasjer = c;
        printf("[ORCH] kasjer pid=%d.\n", c);
    }
}

/* start policjant */
static void start_policjant(void)
{
    if(pid_sternik <= 0){
        printf("[ORCH] No sternik -> policeman no signals.\n");
        return;
    }
    if(pid_policjant > 0){
        printf("[ORCH] policeman already.\n");
        return;
    }
    char arg[32];
    sprintf(arg, "%d", pid_sternik);
    char *args[] = { (char*)PATH_POLICJANT, arg, NULL };
    pid_t c = run_child(PATH_POLICJANT, args);
    if(c > 0){
        pid_policjant = c;
        printf("\033[1;32m[ORCH] policeman pid=%d, sternik=%d.\033[0m\n", c, pid_sternik);
    }
}

/* ------------------------------- */
/* main */
int main(void)
{
    setbuf(stdout, NULL);

    int user_time = 0;
    while(1){
        printf("\033[1;34m[ORCH] Podaj czas symulacji (s, >0): \033[0m");
        fflush(stdout);

        if(scanf("%d", &user_time) != 1){
            while(getchar() != '\n');
            printf("[ORCH] Błędny format, spróbuj jeszcze raz.\n");
            continue;
        }
        if(user_time <= 0){
            printf("[ORCH] Czas musi być > 0.\n");
            continue;
        }
        break;
    }
    TIMEOUT = user_time;
    while(getchar() != '\n'); // wczytanie ewentualnego Enter

    cleanup_fifo();
    mkfifo(FIFO_STERNIK_IN, 0666);
    mkfifo(FIFO_KASJER_IN, 0666);

    start_sternik();
    sleep(1);
    start_kasjer();
    sleep(1);

    /* wątek generatora */
    pthread_create(&generator_thread, NULL, generator_func, NULL);

    /* wątek time_killer */
    pthread_create(&time_killer_thread, NULL, time_killer_func, NULL);

    printf("[ORCH] Komendy: p->policjant, q->end\n");

    char cmd[128];
    while(!end_all){
        /* sprawdzamy, czy sternik się skończył */
        if(pid_sternik > 0){
            int st;
            pid_t w = waitpid(pid_sternik, &st, WNOHANG);
            if(w == pid_sternik){
                printf("[ORCH] sternik ended-> end.\n");
                end_simulation();
                break;
            }
        }

        fflush(stdout);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv;
        tv.tv_sec = 0; 
        tv.tv_usec = 100000; // 0.1 sek
        int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if(ret < 0){
            if(errno == EINTR) continue;
            perror("[ORCH] select");
            break;
        }
        if(ret == 0){
            // nic nie wpisano
        } else {
            // jest coś na stdin
            if(FD_ISSET(STDIN_FILENO, &rfds)){
                if(!fgets(cmd, sizeof(cmd), stdin)) break;
                if(cmd[0] == 'q'){
                    end_simulation();
                    break;
                } else if(cmd[0] == 'p'){
                    start_policjant();
                } else {
                    printf("[ORCH] Nieznana komenda.\n");
                }
            }
        }
    }

    if(!end_all){
        end_simulation();
    }

    pthread_join(generator_thread, NULL);
    pthread_join(time_killer_thread, NULL);

    return 0;
}
