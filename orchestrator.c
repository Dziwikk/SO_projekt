/* File: orchestrator.c
   Kompilacja: gcc orchestrator.c -o orchestrator
   Uruchomienie: ./orchestrator
   Wpisujemy w konsoli:
     r N  -> generuj N pasażerów
     p    -> uruchom policjanta (wysyła sygnały do sternika)
     q    -> zakończ symulację
*/

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
#include <sys/types.h>
#include <sys/stat.h>


/* Ścieżki do plików wykonywalnych */
#define PATH_STERNIK   "./sternik"
#define PATH_KASJER    "./kasjer"
#define PATH_PASAZER   "./pasazer"
#define PATH_POLICJANT "./policjant"

/* Nazwane FIFO – będziemy usuwać przy końcu */
#define FIFO_STERNIK_IN  "fifo_sternik_in"
#define FIFO_STERNIK_OUT "fifo_sternik_out"
#define FIFO_KASJER_IN   "fifo_kasjer_in"
#define FIFO_KASJER_OUT  "fifo_kasjer_out"

/* Maksymalna liczba pasażerów do zapamiętania */
#define MAX_PASS 100

/* Struktura do przechowywania informacji o uruchomionym procesie:
   - pid
   - deskryptory do czytania z pipe (logi)
   - nazwa "STERNIK"/"KASJER"/"PASAZER"/"POLICJANT #"
*/
typedef struct {
    pid_t pid;
    int  pipe_read;  // deskryptor do czytania logów
    char name[32];   // np. "STERNIK", "KASJER", "PASAZER 101", ...
} ChildProc;

static ChildProc p_sternik;         // 1 sternik
static ChildProc p_kasjer;          // 1 kasjer
static ChildProc p_policjant;       // 1 policjant (ew. można wielokrotnie)
static ChildProc p_pass[MAX_PASS];  // pasażerowie
static int pass_count=0;            // ile pasażerów

static volatile sig_atomic_t end_all = 0;

/* Usuwamy FIFO */
void cleanup_fifo(void) {
    unlink(FIFO_STERNIK_IN);
    unlink(FIFO_STERNIK_OUT);
    unlink(FIFO_KASJER_IN);
    unlink(FIFO_KASJER_OUT);
}

/* Tworzymy potok do przechwytu stdout/stderr childa.
   Zwracamy deskryptor do czytania (rodzic) i wypełniamy fd[2].
*/
int create_pipe_for_child(int *fd) {
    if (pipe(fd)<0) {
        perror("[ORCH] pipe");
        return -1;
    }
    return 0;
}

/* Funkcja do uruchomienia childa z przekierowaniem stdout/stderr do pipe */
pid_t run_child(const char *cmd, char *const argv[], ChildProc *out_info)
{
    int pipefd[2];
    if (pipe(pipefd)<0) {
        perror("[ORCH] pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid<0) {
        perror("[ORCH] fork");
        return -1;
    }
    if (pid==0) {
        /* Dziecko */
        close(pipefd[0]); /* zamykamy odczyt w dziecku */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Uruchamiamy program */
        execv(cmd, argv);
        perror("[ORCH] execv");
        exit(1);
    } else {
        /* Rodzic */
        close(pipefd[1]); /* zamykamy zapis */
        out_info->pid = pid;
        out_info->pipe_read = pipefd[0];
        return pid;
    }
}

/* Wątek/czytanie w pętli logów z childa i wypisywanie z prefixem */
void read_logs(ChildProc *cp)
{
    char buf[256];
    while (!end_all) {
        ssize_t n = read(cp->pipe_read, buf, sizeof(buf)-1);
        if (n<=0) {
            if (n==0) {
                /* Może child zakończył? */
                usleep(100000);
                continue;
            }
            if (errno==EAGAIN || errno==EINTR) {
                usleep(100000);
                continue;
            }
            break;
        }
        buf[n] = '\0';
        /* Wypisujemy z prefixem */
        fprintf(stdout, "[%s] %s", cp->name, buf);
        fflush(stdout);
    }
    close(cp->pipe_read);
}

/* Uruchamiamy sternika */
void start_sternik(void)
{
    if (p_sternik.pid != 0) {
        fprintf(stdout, "[ORCH] Sternik już działa (PID=%d).\n", p_sternik.pid);
        return;
    }
    char *args[] = { (char*)PATH_STERNIK, NULL };
    snprintf(p_sternik.name, sizeof(p_sternik.name), "STERNIK");
    run_child(PATH_STERNIK, args, &p_sternik);
    if (p_sternik.pid>0) {
        fprintf(stdout, "[ORCH] Uruchomiono sternika, PID=%d\n", p_sternik.pid);
    }
}

/* Uruchamiamy kasjera */
void start_kasjer(void)
{
    if (p_kasjer.pid != 0) {
        fprintf(stdout, "[ORCH] Kasjer już działa (PID=%d).\n", p_kasjer.pid);
        return;
    }
    char *args[] = { (char*)PATH_KASJER, NULL };
    snprintf(p_kasjer.name, sizeof(p_kasjer.name), "KASJER");
    run_child(PATH_KASJER, args, &p_kasjer);
    if (p_kasjer.pid>0) {
        fprintf(stdout, "[ORCH] Uruchomiono kasjera, PID=%d\n", p_kasjer.pid);
    }
}

/* Generowanie pasażerów */
void generate_passengers(int n)
{
    srand(time(NULL)^getpid());
    for (int i=0; i<n; i++) {
        if (pass_count>=MAX_PASS) {
            fprintf(stdout, "[ORCH] Osiągnięto max pasażerów.\n");
            return;
        }
        pid_t newpid=0;
        ChildProc *cp = &p_pass[pass_count];
        cp->pid=0;
        int pass_id = 1000 + pass_count;
        int age = rand()%80 + 1; // 1..80
        int disc= (rand()%3==0) ? 1:0; // ~33% discount
        char arg1[32], arg2[32], arg3[32];
        sprintf(arg1, "%d", pass_id);
        sprintf(arg2, "%d", age);
        sprintf(arg3, "%d", disc);

        char *args[] = { (char*)PATH_PASAZER, arg1, arg2, arg3, NULL };

        /* Nazwa: PASAZER 1000, np. */
        snprintf(cp->name, sizeof(cp->name), "PASAZER_%d", pass_id);
        newpid = run_child(PATH_PASAZER, args, cp);
        if (newpid>0) {
            pass_count++;
            fprintf(stdout, "[ORCH] Uruchomiono pasażera %d (PID=%d, age=%d, disc=%d).\n",
                    pass_id, newpid, age, disc);
            usleep(300000);
        }
    }
}

/* Uruchamiamy policjanta */
void start_policjant(void)
{
    if (p_sternik.pid<=0) {
        fprintf(stdout, "[ORCH] Nie ma sternika -> brak celu sygnałów.\n");
        return;
    }
    if (p_policjant.pid>0) {
        fprintf(stdout, "[ORCH] Policjant już działa (PID=%d)?\n", p_policjant.pid);
        // można pozwolić na multi, ale zostawmy tak
        return;
    }
    char arg_pid[32];
    sprintf(arg_pid, "%d", p_sternik.pid);
    char *args[] = { (char*)PATH_POLICJANT, arg_pid, NULL };

    snprintf(p_policjant.name, sizeof(p_policjant.name), "POLICJANT");
    run_child(PATH_POLICJANT, args, &p_policjant);
    if (p_policjant.pid>0) {
        fprintf(stdout, "[ORCH] Uruchomiono policjanta (PID=%d) -> sternik=%d\n",
                p_policjant.pid, p_sternik.pid);
    }
}

/* Kończenie symulacji */
void end_simulation(void)
{
    fprintf(stdout, "[ORCH] Kończę symulację...\n");
    end_all = 1;

    /* Wysyłamy QUIT do kasjera i sternika przez FIFO, ewentualnie kill() */
    if (p_kasjer.pid>0) {
        int fd = open(FIFO_KASJER_IN, O_WRONLY);
        if (fd>=0) {
            write(fd, "QUIT\n", 5);
            close(fd);
        }
    }
    if (p_sternik.pid>0) {
        int fd = open(FIFO_STERNIK_IN, O_WRONLY);
        if (fd>=0) {
            write(fd, "QUIT\n", 5);
            close(fd);
        }
    }

    /* Zabij policjanta, pasażerów, kasjera, sternika */
    if (p_policjant.pid>0) {
        kill(p_policjant.pid, SIGTERM);
    }
    for (int i=0; i<pass_count; i++) {
        if (p_pass[i].pid>0) {
            kill(p_pass[i].pid, SIGTERM);
        }
    }
    if (p_kasjer.pid>0) {
        kill(p_kasjer.pid, SIGTERM);
    }
    if (p_sternik.pid>0) {
        kill(p_sternik.pid, SIGTERM);
    }

    /* Poczekaj na zakończenie */
    if (p_policjant.pid>0) {
        waitpid(p_policjant.pid, NULL, 0);
        p_policjant.pid=0;
    }
    for (int i=0; i<pass_count; i++) {
        if (p_pass[i].pid>0) {
            waitpid(p_pass[i].pid, NULL, 0);
            p_pass[i].pid=0;
        }
    }
    if (p_kasjer.pid>0) {
        waitpid(p_kasjer.pid, NULL, 0);
        p_kasjer.pid=0;
    }
    if (p_sternik.pid>0) {
        waitpid(p_sternik.pid, NULL, 0);
        p_sternik.pid=0;
    }

    cleanup_fifo();
    fprintf(stdout, "[ORCH] Symulacja zakończona.\n");
}

/* Wątek odczytujący logi z danego childa i wypisujący je na stdout */
#include <pthread.h>

void *log_reader_thread(void *arg)
{
    ChildProc *cp = (ChildProc*)arg;
    read_logs(cp);
    return NULL;
}

int main(void)
{
    /* Usuwamy stare FIFO, tworzymy nowe */
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

    /* Tworzymy wątki do czytania logów: sternik, kasjer,
       a także pasażerowie i policjant będą tworzone dynamicznie. */
    pthread_t th_sternik, th_kasjer, th_policjant;
    th_policjant = 0;

    if (p_sternik.pid>0) {
        pthread_create(&th_sternik, NULL, log_reader_thread, &p_sternik);
    }
    if (p_kasjer.pid>0) {
        pthread_create(&th_kasjer, NULL, log_reader_thread, &p_kasjer);
    }

    /* Główny loop interfejsu */
    fprintf(stdout, "[ORCH] Dostępne komendy:\n");
    fprintf(stdout, "  r <N> - generuj N pasażerów\n");
    fprintf(stdout, "  p     - uruchom policjanta\n");
    fprintf(stdout, "  q     - zakończ symulację\n");

    char cmd[128];
    while (!end_all) {
        fprintf(stdout, "> ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin)) {
            // ctrl+d
            break;
        }
        if (cmd[0]=='q') {
            break;
        } else if (cmd[0]=='p') {
            start_policjant();
            if (p_policjant.pid>0) {
                // odpal wątek do czytania
                pthread_create(&th_policjant, NULL, log_reader_thread, &p_policjant);
            }
        } else if (cmd[0]=='r') {
            int n=1;
            if (sscanf(cmd, "r %d", &n)==1) {
                fprintf(stdout, "[ORCH] Generuję %d pasażerów...\n", n);
                generate_passengers(n);
                // Każdy pasażer ma swój log. Uruchom wątki czytania:
                for (int i=0; i<pass_count; i++) {
                    static pthread_t pass_thread[MAX_PASS];
                    if (p_pass[i].pid>0 && pass_thread[i]==0) {
                        // odpal wątek do log_reader
                        pthread_create(&pass_thread[i], NULL, log_reader_thread, &p_pass[i]);
                    }
                }
            } else {
                fprintf(stdout, "[ORCH] Błędna składnia: r <N>\n");
            }
        } else {
            fprintf(stdout, "[ORCH] Nieznana komenda.\n");
        }
    }

    /* Kończymy symulację */
    end_simulation();

    return 0;
}
