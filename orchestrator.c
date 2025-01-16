/*******************************************************
 * File: orchestrator.c
 *
 * Komendy z stdin:
 *   p -> uruchom policjanta
 *   q -> zakończ symulację
 *
 * Generator pasażerów:
 *   - część to pary dziecko (<15) + rodzic (>=18) 
 *     z tym samym group_id => boat=2 w kasjerze
 *   - część to normalni pojedynczy pasażerowie
 *   - część wraca (skip=1) - stary pid
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

#define PATH_STERNIK   "./sternik"
#define PATH_KASJER    "./kasjer"
#define PATH_PASAZER   "./pasazer"
#define PATH_POLICJANT "./policjant"

#define FIFO_STERNIK_IN  "fifo_sternik_in"
#define FIFO_KASJER_IN   "fifo_kasjer_in"

#define MAX_PASS 200
#define TIMEOUT 60

static volatile sig_atomic_t end_all = 0;

static pid_t pid_sternik=0;
static pid_t pid_kasjer=0;
static pid_t pid_policjant=0;

static pid_t p_pass[MAX_PASS];
static int pass_count=0;

/* Generator i time_killer */
static pthread_t generator_thread;
static volatile int generator_running=1;

static pthread_t time_killer_thread;

/* Prototypy */
void end_simulation(void);
void cleanup_fifo(void);

/* ------------------------------- */
/* Uruchamianie childa (fork+exec) */
static pid_t run_child(const char *cmd, char *const argv[])
{
    pid_t c = fork();
    if (c<0) {
        perror("[ORCH] fork");
        return -1;
    }
    if (c==0) {
        execv(cmd, argv);
        perror("[ORCH] execv");
        _exit(1);
    }
    return c;
}

/* ------------------------------- */
/* Uruchamianie pasażera z <pid,age,group> */
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
    if (c==0) {
        execv(PATH_PASAZER, args);
        perror("[ORCH] execv pasazer");
        _exit(1);
    } else if (c>0) {
        p_pass[pass_count++] = c;
        printf("[ORCH] Passenger pid=%d age=%d group=%d -> procPID=%d\n",
               pid, age, group, c);
        usleep(200000);
    } else {
        perror("[ORCH] fork pass");
    }
}

/* ------------------------------- */
/* Wątek generatora:
   - 25%: para (dziecko <15, rodzic >=18) -> group_id
   - 25%: WRACA stary pid
   - 50%: zwykły pasażer
*/
void *generator_func(void *arg)
{
    static int used_pids[5000];  /* Podniesiony rozmiar na 5000 */
    static int used_count = 0;
    int base_pid = 1000;

    srand(time(NULL) ^ getpid());
    while (!end_all && generator_running) {
        /* 4 scenariusze: dice=0->(dziecko+rodzic), dice=1->wraca,
           dice=2,3->normal passenger, ale w większej liczbie. */
        int dice = rand() % 4;

        if (dice == 0) {
            /* para: dziecko + rodzic, group=unique_count+1000 */
            int grp = base_pid + used_count;

            // Dziecko
            int child_pid = grp;
            used_pids[used_count] = child_pid;
            used_count++;

            int child_age = rand() % 14 + 1; // 1..14
            run_passenger(child_pid, child_age, grp);

            // Rodzic
            int parent_pid = grp + 1000; 
            int parent_age = rand() % 50 + 20; // 20..69
            run_passenger(parent_pid, parent_age, grp);

        } else if (dice == 1 && used_count > 0) {
            /* wraca stary pid (skip=1 w kasjerze) */
            int idx = rand() % used_count;
            int old_pid = used_pids[idx];
            int age = rand() % 80 + 1;
            printf("[GEN] WRACA old_pid=%d age=%d\n", old_pid, age);
            run_passenger(old_pid, age, 0);

        } else {
            /* normal passengers, ale od razu 3..7 zamiast jednego */
            int how_many = rand() % 5 + 3;  // 3..7
            for (int i = 0; i < how_many; i++) {
                int new_pid = base_pid + used_count;
                used_pids[used_count] = new_pid;
                used_count++;

                int age = rand() % 80 + 1;
                run_passenger(new_pid, age, 0);

                /* Kontrola, żeby nie wyjść poza tablicę used_pids */
                if (used_count >= 5000) {
                    printf("[GEN] Osiągnięto 5000 pid, stop.\n");
                    break;
                }
            }
        }

        /* Zabezpieczenie, gdybyśmy doszli do 5000 */
        if (used_count >= 5000) {
            printf("[GEN] Wiele pid, stop.\n");
            break;
        }

        /* Skróćmy pauzę na 1..2 s, zamiast 2..4 */
        int slp = rand() % 2 + 1;  // 1..2
        for (int i = 0; i < slp * 10 && !end_all; i++) {
            usleep(100000); // co 0.1 s
        }
    }

    return NULL;
}

/* ------------------------------- */
/* Wątek time_killer -> po TIMEOUT end_simulation */
void *time_killer_func(void *arg)
{
    sleep(TIMEOUT);
    if (!end_all) {
        printf("[ORCH/TIME] Time out =%d -> end.\n", TIMEOUT);
        end_simulation();
    }
    return NULL;
}

/* ------------------------------- */
/* cleanup_fifo */
void cleanup_fifo(void)
{
    unlink(FIFO_STERNIK_IN);
    unlink(FIFO_KASJER_IN);
}

/* ------------------------------- */
/* end_simulation -> wysyłamy QUIT i kill */
void end_simulation(void)
{
    if (end_all) return;
    end_all = 1;
    generator_running = 0;  // jeśli mamy wątek generatora pasażerów

    printf("[ORCH] end_simulation() -> sprawdź, QUIT, kill -TERM, kill -9...\n");

    /* --- 0. Sprawdzamy, czy policjant, pasażerowie, kasjer i sternik
            już się sami nie zamknęli (np. policjant zakończył się dawno).
       Jeśli tak, to ustalamy ich pid_* = 0, by pominąć w dalszych krokach.
    */
    if (pid_policjant > 0) {
        pid_t w = waitpid(pid_policjant, NULL, WNOHANG);
        if (w == pid_policjant) {
            /* Policjant już się zakończył samodzielnie */
            pid_policjant = 0;
        }
    }
    for (int i = 0; i < pass_count; i++) {
        if (p_pass[i] > 0) {
            pid_t w = waitpid(p_pass[i], NULL, WNOHANG);
            if (w == p_pass[i]) {
                /* Ten pasażer już skończył */
                p_pass[i] = 0;
            }
        }
    }
    if (pid_kasjer > 0) {
        pid_t w = waitpid(pid_kasjer, NULL, WNOHANG);
        if (w == pid_kasjer) {
            pid_kasjer = 0;
        }
    }
    if (pid_sternik > 0) {
        pid_t w = waitpid(pid_sternik, NULL, WNOHANG);
        if (w == pid_sternik) {
            pid_sternik = 0;
        }
    }

    /* --- 1. Wysyłamy komendę QUIT do kasjera i sternika (jeśli nadal żyją) */
    if (pid_kasjer > 0) {
        int fk = open(FIFO_KASJER_IN, O_WRONLY);
        if (fk >= 0) {
            write(fk, "QUIT\n", 5);
            close(fk);
        }
        printf("[ORCH] (QUIT) kasjer pid=%d\n", pid_kasjer);
    }
    if (pid_sternik > 0) {
        int fs = open(FIFO_STERNIK_IN, O_WRONLY | O_NONBLOCK);
if (fs >= 0) {
    // sternik czyta lub jest wciąż żywy
    write(fs, "QUIT\n", 5); 
    close(fs);
} else {
    // printf("[ORCH] sternik fifo open error, pewnie sternik już dead\n");
}

        printf("[ORCH] (QUIT) sternik pid=%d\n", pid_sternik);
    }

    /* --- 2. Wysyłamy SIGTERM do policjanta, pasażerów, kasjera, sternik (jeśli >0) */
    if (pid_policjant > 0) {
        kill(pid_policjant, SIGTERM);
    }
    for (int i = 0; i < pass_count; i++) {
        if (p_pass[i] > 0) {
            kill(p_pass[i], SIGTERM);
        }
    }
    if (pid_kasjer > 0) {
        kill(pid_kasjer, SIGTERM);
    }
    if (pid_sternik > 0) {
        kill(pid_sternik, SIGTERM);
    }

    /* --- 3. Dajemy im 0.5 sek, aby zakończyli się normalnie */
    usleep(500000);

    /* --- 4. Wysyłamy SIGKILL tym, którzy wciąż żyją */
    if (pid_policjant > 0) {
        if (0 == waitpid(pid_policjant, NULL, WNOHANG)) {
            kill(pid_policjant, SIGKILL);
        }
    }
    for (int i = 0; i < pass_count; i++) {
        if (p_pass[i] > 0) {
            if (0 == waitpid(p_pass[i], NULL, WNOHANG)) {
                kill(p_pass[i], SIGKILL);
            }
        }
    }
    if (pid_kasjer > 0) {
        if (0 == waitpid(pid_kasjer, NULL, WNOHANG)) {
            kill(pid_kasjer, SIGKILL);
        }
    }
    if (pid_sternik > 0) {
        if (0 == waitpid(pid_sternik, NULL, WNOHANG)) {
            kill(pid_sternik, SIGKILL);
        }
    }

    /* --- 5. Teraz normalnie czekamy na wszystkich, żeby nie było zombie */
    if (pid_policjant > 0) {
        waitpid(pid_policjant, NULL, 0);
        pid_policjant = 0;
    }
    for (int i = 0; i < pass_count; i++) {
        if (p_pass[i] > 0) {
            waitpid(p_pass[i], NULL, 0);
            p_pass[i] = 0;
        }
    }
    if (pid_kasjer > 0) {
        waitpid(pid_kasjer, NULL, 0);
        pid_kasjer = 0;
    }
    if (pid_sternik > 0) {
        waitpid(pid_sternik, NULL, 0);
        pid_sternik = 0;
    }

    /* --- 6. cleanup FIFO i komunikat końcowy */
    cleanup_fifo();
    printf("[ORCH] end_simulation -> done.\n");
}

/* ------------------------------- */
/* start sternik (TIMEOUT -> argv[1]) */
static void start_sternik(void)
{
    if (pid_sternik>0) {
        printf("[ORCH] sternik already.\n");
        return;
    }
    char arg[32];
    sprintf(arg,"%d", TIMEOUT);
    char *args[]={(char*)PATH_STERNIK,arg,NULL};
    pid_t c= run_child(PATH_STERNIK, args);
    if (c>0) {
        pid_sternik=c;
        printf("[ORCH] sternik pid=%d.\n", c);
    }
}

/* start kasjer */
static void start_kasjer(void)
{
    if (pid_kasjer>0) {
        printf("[ORCH] kasjer already.\n");
        return;
    }
    char *args[]={(char*)PATH_KASJER,NULL};
    pid_t c= run_child(PATH_KASJER, args);
    if (c>0) {
        pid_kasjer=c;
        printf("[ORCH] kasjer pid=%d.\n", c);
    }
}

/* start policjant */
static void start_policjant(void)
{
    if (pid_sternik<=0) {
        printf("[ORCH] No sternik -> policeman no signals.\n");
        return;
    }
    if (pid_policjant>0) {
        printf("[ORCH] policeman already.\n");
        return;
    }
    char arg[32];
    sprintf(arg,"%d", pid_sternik);
    char *args[]={(char*)PATH_POLICJANT,arg,NULL};
    pid_t c= run_child(PATH_POLICJANT, args);
    if (c>0) {
        pid_policjant=c;
        printf("[ORCH] policeman pid=%d, sternik=%d.\n", c, pid_sternik);
    }
}

/* ------------------------------- */
/* main */
int main(void)
{
    setbuf(stdout,NULL);

    cleanup_fifo();
    mkfifo(FIFO_STERNIK_IN,0666);
    mkfifo(FIFO_KASJER_IN,0666);

    start_sternik();
    sleep(1);
    start_kasjer();
    sleep(1);

    /* wątek generatora */
    pthread_create(&generator_thread,NULL,generator_func,NULL);

    /* wątek time_killer */
    pthread_create(&time_killer_thread,NULL,time_killer_func,NULL);

    printf("[ORCH] Komendy: p->policjant, q->end\n");

    char cmd[128];
    while (!end_all) {
        /* check sternik */
        if (pid_sternik>0) {
            int st;
            pid_t w= waitpid(pid_sternik,&st,WNOHANG);
            if (w==pid_sternik) {
                printf("[ORCH] sternik ended -> end.\n");
                end_simulation();
                break;
            }
        }

        fflush(stdout);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO,&rfds);

        struct timeval tv;
        tv.tv_sec=0; tv.tv_usec=100000;
        int ret= select(STDIN_FILENO+1,&rfds,NULL,NULL,&tv);
        if (ret<0) {
            if (errno==EINTR) continue;
            perror("[ORCH] select");
            break;
        }
        if (ret==0) continue;

        if (FD_ISSET(STDIN_FILENO,&rfds)) {
            if (!fgets(cmd,sizeof(cmd),stdin)) break;
            if (cmd[0]=='q') {
                end_simulation();
                break;
            } else if (cmd[0]=='p') {
                start_policjant();
            } else {
                printf("[ORCH] Nieznana komenda.\n");
            }
        }
    }

    if (!end_all) {
        end_simulation();
    }
    pthread_join(generator_thread,NULL);
    pthread_join(time_killer_thread,NULL);
    return 0;
}
