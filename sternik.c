/*******************************************************
 * File: sternik.c
 *
 * Obsługa dwóch łodzi (boat1 i boat2).
 * Parametry (stałe w kodzie lub #define):
 *   - N1, T1 -> max pasażerów i czas rejsu łodzi 1
 *   - N2, T2 -> max pasażerów i czas rejsu łodzi 2
 *   - K       -> max osób jednocześnie na pomoście
 *
 * Przyjmuje argv[1] = <timeout> (np. 60 sekund) – do określenia,
 * do kiedy w ogóle można wypływać w nowy rejs.
 *
 * Obsługuje sygnały SIGUSR1 / SIGUSR2:
 *   - SIGUSR1 -> przerwanie łodzi1
 *   - SIGUSR2 -> przerwanie łodzi2
 *
 * Komunikaty z fifo_sternik_in:
 *   - QUEUE <pid> <boat> <disc>
 *   - QUEUE_SKIP <pid> <boat>
 *   - INFO
 *   - QUIT
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>

/* ---- PARAMETRY ŁODZI ---- */
#define N1 5         // maks pasażerów łódź1
#define T1 2         // czas rejsu łódź1
#define N2 6         // maks pasażerów łódź2
#define T2 2         // czas rejsu łódź2
#define K  2         // max osób naraz na pomoście
#define LOAD_TIMEOUT 2  // ile sekund czekamy na kolejnych pasażerów

/* ---- Kolejka pasażerów ---- */
#define QSIZE 50
typedef struct {
    int pid;
    int disc; // zniżka (tylko do logów)
} PassengerItem;

typedef struct {
    PassengerItem items[QSIZE];
    int front;
    int rear;
    int count;
} PassQueue;

static void initQueue(PassQueue *q) {
    q->front=0; 
    q->rear=0;  
    q->count=0;
}
static int isEmpty(PassQueue *q) { return (q->count == 0); }
static int isFull(PassQueue *q)  { return (q->count == QSIZE); }

static int enqueue(PassQueue *q, PassengerItem it) {
    if (isFull(q)) return -1;
    q->items[q->rear] = it;
    q->rear = (q->rear+1) % QSIZE;
    q->count++;
    return 0;
}
static PassengerItem dequeue(PassQueue *q) {
    PassengerItem tmp = {0,0};
    if (isEmpty(q)) return tmp;
    tmp = q->items[q->front];
    q->front = (q->front + 1) % QSIZE;
    q->count--;
    return tmp;
}

/* Kolejki: skip i normalne dla łodzi 1 i 2 */
static PassQueue queueBoat1;
static PassQueue queueBoat1_skip;
static PassQueue queueBoat2;
static PassQueue queueBoat2_skip;

/* Plik logów (opcjonalny) */
static FILE *sternikLog = NULL;

/* boatX_active -> czy łódź X ma kontynuować kursy (1 = tak, 0 = nie) */
static volatile sig_atomic_t boat1_active = 1;
static volatile sig_atomic_t boat1_inrejs = 0;

static volatile sig_atomic_t boat2_active = 1;
static volatile sig_atomic_t boat2_inrejs = 0;

/* Czas startu i czas końca (timeout z argv[1]) */
static time_t start_time;
static time_t end_time;

/* Stan pomostu: INBOUND (pasażerowie wchodzą), OUTBOUND (wychodzą), FREE */
typedef enum {INBOUND, OUTBOUND, FREE} PomostState;
static PomostState pomost_state = FREE;
static int pomost_count = 0;  // ile osób na pomoście

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_pomost_free = PTHREAD_COND_INITIALIZER;

/* Funkcja wspólnego logowania */
static void logMsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    if (sternikLog) {
        vfprintf(sternikLog, fmt, ap);
        fflush(sternikLog);
    }
    va_end(ap);
}

/* Sygnały policjanta: łódź1 -> SIGUSR1, łódź2 -> SIGUSR2 */
static void sigusr1_handler(int sig) {
    /* boat1_active=0 -> łódź1 nie popłynie więcej;
       jeśli w rejsie, zakończy rejs i potem koniec,
       jeśli w porcie -> wyładuje pasażerów od razu. */
    if (!boat1_inrejs) {
        logMsg("[BOAT1] (SIGUSR1) -> w porcie, koniec.\n");
    } else {
        logMsg("[BOAT1] (SIGUSR1) -> w rejsie, dokończę i koniec.\n");
    }
    boat1_active=0;
}
static void sigusr2_handler(int sig) {
    if (!boat2_inrejs) {
        logMsg("[BOAT2] (SIGUSR2) -> w porcie, koniec.\n");
    } else {
        logMsg("[BOAT2] (SIGUSR2) -> w rejsie, dokończę i koniec.\n");
    }
    boat2_active=0;
}

/* Pomost – wejście (pasażer próbuje wejść na pomost w trybie INBOUND) */
static int enter_pomost(void)
{
    /* Możemy wejść, jeśli:
       - stan pomostu = FREE lub INBOUND
       - i nie przekroczymy limitu K
    */
    if (pomost_state == FREE) {
        pomost_state = INBOUND;
    } else if (pomost_state != INBOUND) {
        return 0; // ruch w przeciwną stronę
    }
    if (pomost_count >= K) {
        return 0; // brak miejsca
    }
    pomost_count++;
    return 1;
}

/* Pomost – pasażer skończył wchodzić */
static void leave_pomost_in(void)
{
    pomost_count--;
    if (pomost_count == 0) {
        pomost_state = FREE;
        pthread_cond_broadcast(&cond_pomost_free);
    }
}

/* Start OUTBOUND (wyładunek pasażerów) */
static void start_outbound(void)
{
    /* Czekamy, aż pomost będzie FREE */
    while (pomost_state != FREE) {
        pthread_cond_wait(&cond_pomost_free, &mutex);
    }
    pomost_state = OUTBOUND;
}

/* Koniec OUTBOUND */
static void end_outbound(void)
{
    pomost_state = FREE;
    pthread_cond_broadcast(&cond_pomost_free);
}

/* Wątek łodzi1 */
void *boat1_thread(void *arg)
{
    logMsg("[BOAT1] Start wątku (max=%d, T1=%ds).\n", N1, T1);

    while (1) {
        pthread_mutex_lock(&mutex);

        /* Czy łódź jeszcze aktywna? */
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] boat1_active=0 -> kończę.\n");
            break;
        }

        /* Sprawdź, czy w kolejce są pasażerowie (skip lub normal) */
        if (isEmpty(&queueBoat1_skip) && isEmpty(&queueBoat1)) {
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }

        /* Załadunek pasażerów */
        logMsg("[BOAT1] Załadunek...\n");
        int loaded=0;
        time_t load_start = time(NULL);

        while (loaded < N1 && boat1_active) {
            /* Wybieramy w pierwszej kolejności skip, potem normal */
            PassQueue *q = NULL;
            if (!isEmpty(&queueBoat1_skip)) {
                q = &queueBoat1_skip;
            } else if (!isEmpty(&queueBoat1)) {
                q = &queueBoat1;
            } else {
                /* brak pasażerów w kolejce, czekamy do LOAD_TIMEOUT */
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
                if (!boat1_active) break;
                if (difftime(time(NULL), load_start)>=LOAD_TIMEOUT) break;
                continue;
            }

            PassengerItem p = q->items[q->front];
            /* Próba wejścia na pomost */
            if (pomost_state==FREE || pomost_state==INBOUND) {
                if (pomost_count < K) {
                    dequeue(q);
                    if (enter_pomost()) {
                        /* Od razu zwalniamy pomost (symulacja wsiadania) */
                        leave_pomost_in();
                        loaded++;
                        logMsg("[BOAT1] Pasażer %d(disc=%d) wsiada (%d/%d)\n",
                               p.pid, p.disc, loaded, N1);
                    }
                } else {
                    /* Pomost pełny */
                    pthread_mutex_unlock(&mutex);
                    usleep(50000);
                    pthread_mutex_lock(&mutex);
                }
            } else {
                /* pomost OUTBOUND -> czekamy */
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
            }

            /* Sprawdzamy warunki wyjścia z pętli załadunku */
            if (!boat1_active) break;
            if (loaded == N1) break;
            if (difftime(time(NULL), load_start)>=LOAD_TIMEOUT) break;
        }

        /* Jeśli łódź przestała być aktywna w trakcie załadunku – wyładuj i koniec */
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] przerwanie w trakcie załadunku -> wyładunek.\n");
            if (loaded>0) {
                /* Pasażerowie, którzy zdążyli wsiąść – muszą zejść (bez rejsu) */
                logMsg("[BOAT1] OUTBOUND -> wyładowuję %d pasażerów (sygnał/koniec).\n", loaded);
                pthread_mutex_lock(&mutex);
                start_outbound();
                logMsg("[BOAT1] Pasażerowie zeszli.\n");
                end_outbound();
                pthread_mutex_unlock(&mutex);
            }
            break;
        }

        /* Jeśli nikt nie wsiadł, to czekamy krótką chwilę i spróbujemy ponownie */
        if (loaded == 0) {
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }

        /* czekamy, aż pomost się opróżni (INBOUND->FREE) przed wypłynięciem */
        while ((pomost_state==INBOUND || pomost_count>0) && boat1_active) {
            pthread_cond_wait(&cond_pomost_free, &mutex);
        }
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] przerwanie przed wypłynięciem -> wyładunek.\n");
            if (loaded>0) {
                /* Wyładuj pasażerów */
                pthread_mutex_lock(&mutex);
                start_outbound();
                logMsg("[BOAT1] Pasażerowie zeszli.\n");
                end_outbound();
                pthread_mutex_unlock(&mutex);
            }
            break;
        }

        /* Sprawdzamy, czy mamy dość czasu na rejs */
        time_t now = time(NULL);
        if (now + T1 > end_time) {
            /* Brak czasu na rejs – wyładuj i koniec */
            logMsg("[BOAT1] Brakuje czasu na nowy rejs -> nie wypływamy.\n");
            if (loaded > 0) {
                logMsg("[BOAT1] OUTBOUND (bez rejsu) -> wyładowuję %d pasażerów.\n", loaded);
                start_outbound();
                logMsg("[BOAT1] Pasażerowie wysiedli.\n");
                end_outbound();
            }
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Wypływamy */
        boat1_inrejs=1;
        logMsg("[BOAT1] Wypływam z %d pasażerami.\n", loaded);
        pthread_mutex_unlock(&mutex);

        /* Rejs T1 sekund */
        sleep(T1);

        /* Wracamy do portu – wyładunek */
        pthread_mutex_lock(&mutex);
        boat1_inrejs=0;
        logMsg("[BOAT1] Rejs zakończony -> OUTBOUND.\n");
        start_outbound();
        logMsg("[BOAT1] Pasażerowie wysiedli.\n");
        end_outbound();

        /* Jeśli łódź została wyłączona (sygnał) w trakcie rejsu – to już nie pływamy dalej */
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] przerwanie po zakończeniu rejsu.\n");
            break;
        }

        pthread_mutex_unlock(&mutex);
        usleep(50000);
    }

    logMsg("[BOAT1] Koniec wątku.\n");
    return NULL;
}

/* Wątek łodzi2 – analogicznie do łodzi1 */
void *boat2_thread(void *arg)
{
    logMsg("[BOAT2] Start wątku (max=%d, T2=%ds).\n", N2, T2);

    while (1) {
        pthread_mutex_lock(&mutex);

        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] boat2_active=0 -> kończę.\n");
            break;
        }

        if (isEmpty(&queueBoat2_skip) && isEmpty(&queueBoat2)) {
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }

        logMsg("[BOAT2] Załadunek...\n");
        int loaded = 0;
        time_t load_start = time(NULL);

        while (loaded < N2 && boat2_active) {
            PassQueue *q = NULL;
            if (!isEmpty(&queueBoat2_skip)) {
                q = &queueBoat2_skip;
            } else if (!isEmpty(&queueBoat2)) {
                q = &queueBoat2;
            } else {
                /* brak pasażerów – czekamy do LOAD_TIMEOUT */
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
                if (!boat2_active) break;
                if (difftime(time(NULL), load_start)>=LOAD_TIMEOUT) break;
                continue;
            }

            PassengerItem p = q->items[q->front];
            if (pomost_state==FREE || pomost_state==INBOUND) {
                if (pomost_count < K) {
                    dequeue(q);
                    if (enter_pomost()) {
                        leave_pomost_in();
                        loaded++;
                        logMsg("[BOAT2] Pasażer %d(disc=%d) wsiada (%d/%d)\n",
                               p.pid, p.disc, loaded, N2);
                    }
                } else {
                    pthread_mutex_unlock(&mutex);
                    usleep(50000);
                    pthread_mutex_lock(&mutex);
                }
            } else {
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
            }

            if (!boat2_active) break;
            if (loaded==N2) break;
            if (difftime(time(NULL), load_start)>=LOAD_TIMEOUT) break;
        }

        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie w trakcie załadunku -> wyładunek.\n");
            if (loaded>0) {
                logMsg("[BOAT2] OUTBOUND -> wyładowuję %d pasażerów (sygnał/koniec).\n", loaded);
                pthread_mutex_lock(&mutex);
                start_outbound();
                logMsg("[BOAT2] Pasażerowie zeszli.\n");
                end_outbound();
                pthread_mutex_unlock(&mutex);
            }
            break;
        }

        if (loaded==0) {
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }

        while ((pomost_state==INBOUND || pomost_count>0) && boat2_active) {
            pthread_cond_wait(&cond_pomost_free, &mutex);
        }
        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie przed wypłynięciem -> wyładunek.\n");
            if (loaded>0) {
                pthread_mutex_lock(&mutex);
                start_outbound();
                logMsg("[BOAT2] Pasażerowie zeszli.\n");
                end_outbound();
                pthread_mutex_unlock(&mutex);
            }
            break;
        }

        time_t now = time(NULL);
        if (now + T2 > end_time) {
            logMsg("[BOAT2] Brakuje czasu na nowy rejs -> nie wypływamy.\n");
            if (loaded>0) {
                logMsg("[BOAT2] OUTBOUND (bez rejsu) -> wyładowuję %d pasażerów.\n", loaded);
                start_outbound();
                logMsg("[BOAT2] Pasażerowie wysiedli.\n");
                end_outbound();
            }
            pthread_mutex_unlock(&mutex);
            break;
        }

        boat2_inrejs=1;
        logMsg("[BOAT2] Wypływam z %d pasażerami.\n", loaded);
        pthread_mutex_unlock(&mutex);

        sleep(T2);

        pthread_mutex_lock(&mutex);
        boat2_inrejs=0;
        logMsg("[BOAT2] Rejs zakończony -> OUTBOUND.\n");
        start_outbound();
        logMsg("[BOAT2] Pasażerowie wysiedli.\n");
        end_outbound();

        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie po zakończeniu rejsu.\n");
            break;
        }

        pthread_mutex_unlock(&mutex);
        usleep(50000);
    }

    logMsg("[BOAT2] Koniec wątku.\n");
    return NULL;
}

/* main */
int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);

    if (argc < 2) {
        fprintf(stderr, "[STERNIK] Użycie: %s <timeout_s>\n", argv[0]);
        return 1;
    }
    int timeout_value = atoi(argv[1]);
    start_time = time(NULL);
    end_time   = start_time + timeout_value;

    /* Otwieramy plik logów (opcjonalnie) */
    sternikLog = fopen("sternik.log", "w");
    if (!sternikLog) {
        perror("[STERNIK] Nie można otworzyć sternik.log (logi tylko stdout)");
    }

    /* Rejestracja sygnałów policjanta (SIGUSR1 -> łódź1, SIGUSR2 -> łódź2) */
    struct sigaction sa1, sa2;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa1, NULL);

    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa2, NULL);

    /* Inicjalizacja kolejek */
    initQueue(&queueBoat1);
    initQueue(&queueBoat1_skip);
    initQueue(&queueBoat2);
    initQueue(&queueBoat2_skip);

    pomost_state = FREE;
    pomost_count = 0;

    /* Tworzymy FIFO do komunikacji z orchestrator (komendy) */
    unlink("fifo_sternik_in");
    mkfifo("fifo_sternik_in", 0666);
    int fd_in = open("fifo_sternik_in", O_RDONLY | O_NONBLOCK);
    if (fd_in<0) {
        perror("[STERNIK] open fifo_sternik_in");
        if (sternikLog) fclose(sternikLog);
        return 1;
    }

    /* Wątki łodzi */
    pthread_t tid_b1, tid_b2;
    pthread_create(&tid_b1, NULL, boat1_thread, NULL);
    pthread_create(&tid_b2, NULL, boat2_thread, NULL);

    logMsg("[STERNIK] Start sternika (timeout=%d s).\n", timeout_value);

    /* Pętla odbierająca komendy z FIFO */
    char buf[256];
    while (1) {
        ssize_t n = read(fd_in, buf, sizeof(buf)-1);
        if (n>0) {
            buf[n] = '\0';
            if (strncmp(buf, "QUEUE_SKIP", 10)==0) {
                int pid=0, bno=0;
                sscanf(buf, "QUEUE_SKIP %d %d", &pid, &bno);
                pthread_mutex_lock(&mutex);
                PassengerItem pi = { pid, 50 }; // disc=50 (powtórka)
                if (bno==1 && boat1_active) {
                    if (!isFull(&queueBoat1_skip)) {
                        enqueue(&queueBoat1_skip, pi);
                        logMsg("[STERNIK] [SKIP] Pasażer %d -> queueBoat1_skip\n", pid);
                    } else {
                        logMsg("[STERNIK] queueBoat1_skip full -> odrzucono %d\n", pid);
                    }
                }
                else if (bno==2 && boat2_active) {
                    if (!isFull(&queueBoat2_skip)) {
                        enqueue(&queueBoat2_skip, pi);
                        logMsg("[STERNIK] [SKIP] Pasażer %d -> queueBoat2_skip\n", pid);
                    } else {
                        logMsg("[STERNIK] queueBoat2_skip full -> odrzucono %d\n", pid);
                    }
                }
                else {
                    logMsg("[STERNIK] Łódź %d nieaktywna->odrzucono %d\n", bno, pid);
                }
                pthread_mutex_unlock(&mutex);
            }
            else if (strncmp(buf, "QUEUE", 5)==0) {
                int pid=0, bno=0, disc=0;
                sscanf(buf, "QUEUE %d %d %d", &pid, &bno, &disc);
                pthread_mutex_lock(&mutex);
                PassengerItem pi = { pid, disc };
                if (bno==1 && boat1_active) {
                    if (!isFull(&queueBoat1)) {
                        enqueue(&queueBoat1, pi);
                        logMsg("[STERNIK] Pasażer %d -> queueBoat1\n", pid);
                    } else {
                        logMsg("[STERNIK] queueBoat1 full->odrzucono %d\n", pid);
                    }
                }
                else if (bno==2 && boat2_active) {
                    if (!isFull(&queueBoat2)) {
                        enqueue(&queueBoat2, pi);
                        logMsg("[STERNIK] Pasażer %d -> queueBoat2\n", pid);
                    } else {
                        logMsg("[STERNIK] queueBoat2 full->odrzucono %d\n", pid);
                    }
                }
                else {
                    logMsg("[STERNIK] Łódź %d nieaktywna->odrzucono %d\n", bno, pid);
                }
                pthread_mutex_unlock(&mutex);
            }
            else if (strncmp(buf, "INFO", 4)==0) {
                pthread_mutex_lock(&mutex);
                const char *st = (pomost_state==FREE) ? "FREE"
                              : (pomost_state==INBOUND) ? "INBOUND" : "OUTBOUND";
                logMsg("[INFO] boat1_active=%d(inrejs=%d), boat2_active=%d(inrejs=%d), q1=%d, q1_skip=%d, q2=%d, q2_skip=%d, pomost_count=%d, state=%s\n",
                       boat1_active, boat1_inrejs,
                       boat2_active, boat2_inrejs,
                       queueBoat1.count, queueBoat1_skip.count,
                       queueBoat2.count, queueBoat2_skip.count,
                       pomost_count, st);
                pthread_mutex_unlock(&mutex);
            }
            else if (strncmp(buf, "QUIT", 4)==0) {
                logMsg("[STERNIK] Otrzymano QUIT -> kończę.\n");
                break;
            }
        }

        /* Sprawdzamy, czy obie łodzie nieaktywne -> koniec automatyczny */
        pthread_mutex_lock(&mutex);
        if (!boat1_active && !boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[STERNIK] Obie łodzie nieaktywne -> kończę sternika.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);

        usleep(50000);
    }

    /* Kończymy łodzie (jeśli jeszcze żyją) */
    pthread_mutex_lock(&mutex);
    boat1_active=0;
    boat2_active=0;
    pthread_mutex_unlock(&mutex);

    pthread_join(tid_b1, NULL);
    pthread_join(tid_b2, NULL);

    close(fd_in);
    logMsg("[STERNIK] Zakończyłem działanie.\n");

    if (sternikLog) fclose(sternikLog);
    return 0;
}
