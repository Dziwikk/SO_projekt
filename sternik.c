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


/* ---- KONFIGURACJA PARAMETRÓW ---- */
#define N1 5
#define T1 3
#define N2 6
#define T2 4
#define K  2

#define LOAD_TIMEOUT 5  

/* ---- KOLEJKA PASAŻERÓW ---- */
#define QSIZE 50
typedef struct {
    int pid;
    int disc;
} PassengerItem;

typedef struct {
    PassengerItem items[QSIZE];
    int front;
    int rear;
    int count;
} PassQueue;

void initQueue(PassQueue *q) {
    q->front=0; 
    q->rear=0;  
    q->count=0;
}
int isEmpty(PassQueue *q) { return (q->count == 0); }
int isFull(PassQueue *q)  { return (q->count == QSIZE); }

int enqueue(PassQueue *q, PassengerItem it) {
    if (isFull(q)) return -1;
    q->items[q->rear] = it;
    q->rear = (q->rear+1) % QSIZE;
    q->count++;
    return 0;
}
PassengerItem dequeue(PassQueue *q) {
    PassengerItem tmp = {0,0};
    if (isEmpty(q)) return tmp;
    tmp = q->items[q->front];
    q->front = (q->front + 1) % QSIZE;
    q->count--;
    return tmp;
}

/* ---- GLOBALNE KOLEJKI: łódź1 i łódź2 ---- */
static PassQueue queueBoat1;
static PassQueue queueBoat2;

/* ---- LOG FILE (opcjonalne, aby zapisać każdą akcję) ---- */
static FILE *sternikLog = NULL;

/* ---- FLAGA SYGNAŁOWA (POLICJA) ---- */
static volatile sig_atomic_t boat1_active = 1;
static volatile sig_atomic_t boat1_inrejs = 0;

static volatile sig_atomic_t boat2_active = 1;
static volatile sig_atomic_t boat2_inrejs = 0;

/* ---- STAN POMOSTU ---- */
typedef enum {INBOUND, OUTBOUND, FREE} PomostState;
static PomostState pomost_state = FREE;
static int pomost_count = 0;

/* MUTEX, warunek */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_pomost_free = PTHREAD_COND_INITIALIZER;


/* 
   --------------- OBSŁUGA SYGNAŁÓW OD POLICJANTA ---------------
   - Jeżeli łódź w rejsie -> dokończy, a potem wyładunek i koniec
   - Jeżeli jeszcze nie wypłynęła -> OUTBOUND (jeśli jacyś pasażerowie na pokładzie?), 
     komunikat, i koniec (boatX_active=0).
*/

/* Wspólna funkcja do logowania (wypisywanie jednocześnie do stdout i do pliku) */
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

/* Handler dla łodzi 1 */
void sigusr1_handler(int sig) {
    if (!boat1_inrejs) {
        // Jeszcze nie wypłynęła
        logMsg("[BOAT1] (SIGUSR1) -> w przystani, ewakuacja i koniec.\n");
        boat1_active = 0;
    } else {
        // W rejsie
        logMsg("[BOAT1] (SIGUSR1) -> w rejsie, dokończę i koniec.\n");
        boat1_active = 0;
    }
}

/* Handler dla łodzi 2 */
void sigusr2_handler(int sig) {
    if (!boat2_inrejs) {
        logMsg("[BOAT2] (SIGUSR2) -> w przystani, ewakuacja i koniec.\n");
        boat2_active = 0;
    } else {
        logMsg("[BOAT2] (SIGUSR2) -> w rejsie, dokończę i koniec.\n");
        boat2_active = 0;
    }
}

/* Pomocnicze do pomostu */
static int enter_pomost(void)
{
    if (pomost_state == FREE) {
        pomost_state = INBOUND;
    } else if (pomost_state != INBOUND) {
        return 0;
    }
    if (pomost_count >= K) {
        return 0;
    }
    pomost_count++;
    return 1;
}
static void leave_pomost_in(void)
{
    pomost_count--;
    if (pomost_count == 0) {
        pomost_state = FREE;
        pthread_cond_broadcast(&cond_pomost_free);
    }
}
static void start_outbound(void)
{
    while (pomost_state != FREE) {
        pthread_cond_wait(&cond_pomost_free, &mutex);
    }
    pomost_state = OUTBOUND;
}
static void end_outbound(void)
{
    pomost_state = FREE;
    pthread_cond_broadcast(&cond_pomost_free);
}

/* ---------------- BOAT1 THREAD ---------------- */
void *boat1_thread(void *arg)
{
    logMsg("[BOAT1] Start wątku - automatyczny, pomost K=%d.\n", K);

    while (1) {
        pthread_mutex_lock(&mutex);

        // Sprawdzamy aktywność
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] Kończę (boat1_active=0) w przystani.\n");
            break;
        }

        // Czy są pasażerowie w kolejce
        if (isEmpty(&queueBoat1)) {
            pthread_mutex_unlock(&mutex);
            usleep(1000000);
            continue;
        }

        logMsg("[BOAT1] Załadunek (INBOUND) - czekam do %d pasażerów.\n", N1);
        int loaded = 0;
        time_t tstart = time(NULL);

        while (loaded < N1 && boat1_active) {
            if (isEmpty(&queueBoat1)) {
                pthread_mutex_unlock(&mutex);
                usleep(300000);
                pthread_mutex_lock(&mutex);

                if (difftime(time(NULL), tstart) >= LOAD_TIMEOUT) break;
                if (!boat1_active) break;
            } else {
                PassengerItem p = queueBoat1.items[queueBoat1.front];
                if (pomost_state==FREE || pomost_state==INBOUND) {
                    if (pomost_count < K) {
                        dequeue(&queueBoat1);
                        if (enter_pomost()) {
                            leave_pomost_in();
                            loaded++;
                            logMsg("[BOAT1] Pasażer %d(disc=%d) wsiada (%d/%d)\n",
                                    p.pid, p.disc, loaded, N1);
                        }
                    } else {
                        pthread_mutex_unlock(&mutex);
                        usleep(200000);
                        pthread_mutex_lock(&mutex);
                    }
                } else {
                    // pomost OUTBOUND
                    pthread_mutex_unlock(&mutex);
                    usleep(200000);
                    pthread_mutex_lock(&mutex);
                }
            }

            if (!boat1_active) break;
            if (loaded == N1) break;
            if (difftime(time(NULL), tstart) >= LOAD_TIMEOUT) break;
        }

        // Po załadunku sprawdzamy sygnał
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] Sygnał w trakcie załadunku -> kończę.\n");
            break;
        }

        // **TU** ZABEZPIECZENIE PRZED PUSTYM REJSEM
        if (loaded == 0) {
            // nikt nie wsiadł – nie płyń
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] Brak pasażerów -> czekam.\n");
            continue; 
        }

        // czekamy, aż pomost się zwolni
        while (pomost_state==INBOUND || pomost_count>0) {
            pthread_cond_wait(&cond_pomost_free, &mutex);
            if (!boat1_active) break;
        }
        if (!boat1_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] Sygnał -> rezygnuję z wypłynięcia.\n");
            break;
        }

        boat1_inrejs = 1;
        logMsg("[BOAT1] Wypływam z %d pasażerami.\n", loaded);
        pthread_mutex_unlock(&mutex);

        sleep(T1);

        pthread_mutex_lock(&mutex);
        boat1_inrejs = 0;
        logMsg("[BOAT1] Rejs zakończony -> OUTBOUND.\n");
        start_outbound();
        logMsg("[BOAT1] Pasażerowie wysiedli.\n");
        end_outbound();
        if (!boat1_active) {
            logMsg("[BOAT1] Sygnał w trakcie rejsu -> koniec pracy.\n");
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        usleep(200000);
    }

    logMsg("[BOAT1] Wyłączam wątek.\n");
    return NULL;
}

/* ---------------- BOAT2 THREAD ---------------- */
void *boat2_thread(void *arg)
{
    logMsg("[BOAT2] Start wątku - automatyczny, pomost K=%d.\n", K);

    while (1) {
        pthread_mutex_lock(&mutex);
        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] Kończę (boat2_active=0) w przystani.\n");
            break;
        }

        if (isEmpty(&queueBoat2)) {
            pthread_mutex_unlock(&mutex);
            usleep(1000000);
            continue;
        }

        logMsg("[BOAT2] Załadunek (INBOUND) do %d pasażerów.\n", N2);
        int loaded=0;
        time_t tstart=time(NULL);

        while (loaded < N2 && boat2_active) {
            if (isEmpty(&queueBoat2)) {
                pthread_mutex_unlock(&mutex);
                usleep(300000);
                pthread_mutex_lock(&mutex);

                if (difftime(time(NULL), tstart)>=LOAD_TIMEOUT) break;
                if (!boat2_active) break;
            } else {
                PassengerItem p = queueBoat2.items[queueBoat2.front];
                if (pomost_state==FREE || pomost_state==INBOUND) {
                    if (pomost_count < K) {
                        dequeue(&queueBoat2);
                        if (enter_pomost()) {
                            leave_pomost_in();
                            loaded++;
                            logMsg("[BOAT2] Pasażer %d(disc=%d) wsiada (%d/%d)\n",
                                   p.pid, p.disc, loaded, N2);
                        }
                    } else {
                        pthread_mutex_unlock(&mutex);
                        usleep(200000);
                        pthread_mutex_lock(&mutex);
                    }
                } else {
                    pthread_mutex_unlock(&mutex);
                    usleep(200000);
                    pthread_mutex_lock(&mutex);
                }
            }

            if (!boat2_active) break;
            if (loaded==N2) break;
            if (difftime(time(NULL), tstart)>=LOAD_TIMEOUT) break;
        }

        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] Sygnał w trakcie załadunku -> kończę.\n");
            break;
        }

        // **ZABEZPIECZENIE** - nie płyń pusto
        if (loaded==0) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] Brak pasażerów -> czekam.\n");
            continue;
        }

        while (pomost_state==INBOUND || pomost_count>0) {
            pthread_cond_wait(&cond_pomost_free, &mutex);
            if (!boat2_active) break;
        }
        if (!boat2_active) {
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] Rezygnuję z wypłynięcia.\n");
            break;
        }

        boat2_inrejs = 1;
        logMsg("[BOAT2] Wypływam z %d pasażerami.\n", loaded);
        pthread_mutex_unlock(&mutex);

        sleep(T2);

        pthread_mutex_lock(&mutex);
        boat2_inrejs = 0;
        logMsg("[BOAT2] Rejs zakończony -> OUTBOUND.\n");
        start_outbound();
        logMsg("[BOAT2] Pasażerowie wysiedli.\n");
        end_outbound();
        if (!boat2_active) {
            logMsg("[BOAT2] Sygnał w trakcie rejsu -> koniec pracy.\n");
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        usleep(200000);
    }

    logMsg("[BOAT2] Wyłączam wątek.\n");
    return NULL;
}

/* ---- MAIN ---- */
int main(void)
{
    // Otwieramy plik do logowania:
    sternikLog = fopen("sternik.log", "w");
    if (!sternikLog) {
        perror("[STERNIK] Nie mogę otworzyć sternik.log");
        exit(1);
    }

    // Rejestracja sygnałów
    struct sigaction sa1, sa2;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa1, NULL);

    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa2, NULL);

    // Inicjalizacja
    initQueue(&queueBoat1);
    initQueue(&queueBoat2);

    pomost_state = FREE;
    pomost_count = 0;

    // Tworzymy FIFO
    unlink("fifo_sternik_in");
    mkfifo("fifo_sternik_in", 0666);
    int fd_in = open("fifo_sternik_in", O_RDONLY | O_NONBLOCK);
    if (fd_in < 0) {
        perror("[STERNIK] open fifo_sternik_in");
        fprintf(sternikLog, "[STERNIK] Błąd otwarcia fifo_sternik_in!\n");
        fclose(sternikLog);
        return 1;
    }

    logMsg("[STERNIK] Start - K=%d, automatyczne rejsy.\n", K);
    logMsg("[STERNIK] SIGUSR1->boat1, SIGUSR2->boat2.\n");

    // Wątki łodzi
    pthread_t tid_b1, tid_b2;
    pthread_create(&tid_b1, NULL, boat1_thread, NULL);
    pthread_create(&tid_b2, NULL, boat2_thread, NULL);

    // Pętla odbioru komend
    char buf[256];
    while (1) {
        ssize_t n = read(fd_in, buf, sizeof(buf)-1);
        if (n>0) {
            buf[n] = '\0';
            if (strncmp(buf, "QUEUE", 5)==0) {
                int pid=0, bno=0, disc=0;
                sscanf(buf, "QUEUE %d %d %d", &pid, &bno, &disc);

                pthread_mutex_lock(&mutex);
                PassengerItem pi = { pid, disc };
                if (bno==1 && boat1_active) {
                    if (!isFull(&queueBoat1)) {
                        enqueue(&queueBoat1, pi);
                        logMsg("[STERNIK] Pasażer %d -> queueBoat1 (size=%d)\n", pid, queueBoat1.count);
                    } else {
                        logMsg("[STERNIK] queueBoat1 full -> odrzucono %d\n", pid);
                    }
                }
                else if (bno==2 && boat2_active) {
                    if (!isFull(&queueBoat2)) {
                        enqueue(&queueBoat2, pi);
                        logMsg("[STERNIK] Pasażer %d -> queueBoat2 (size=%d)\n", pid, queueBoat2.count);
                    } else {
                        logMsg("[STERNIK] queueBoat2 full -> odrzucono %d\n", pid);
                    }
                }
                else {
                    logMsg("[STERNIK] Łódź %d nieaktywna - odrzucono pasażera %d\n", bno, pid);
                }
                pthread_mutex_unlock(&mutex);
            }
            else if (strncmp(buf, "INFO", 4)==0) {
                pthread_mutex_lock(&mutex);
                const char *st = (pomost_state==FREE) ? "FREE"
                                : (pomost_state==INBOUND) ? "INBOUND" : "OUTBOUND";
                logMsg("[INFO] boat1_active=%d, boat2_active=%d, queue1=%d, queue2=%d, pomost_count=%d, state=%s\n",
                       boat1_active, boat2_active, queueBoat1.count, queueBoat2.count,
                       pomost_count, st);
                pthread_mutex_unlock(&mutex);
            }
            else if (strncmp(buf, "QUIT", 4)==0) {
                logMsg("[STERNIK] Otrzymano QUIT, kończę.\n");
                break;
            }
        }
        usleep(100000);
    }

    // Kończymy
    pthread_mutex_lock(&mutex);
    boat1_active = 0;
    boat2_active = 0;
    pthread_mutex_unlock(&mutex);

    pthread_join(tid_b1, NULL);
    pthread_join(tid_b2, NULL);

    close(fd_in);
    logMsg("[STERNIK] Koniec.\n");

    fclose(sternikLog);
    return 0;
}
