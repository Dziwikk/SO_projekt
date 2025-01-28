/*******************************************************
  File: sternik.c
*******************************************************/

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

/* Parametry łodzi i rejsów */
#define N1 10
#define T1 4   // "teoretyczny" czas rejsu (łódź1)
#define N2 11
#define T2 5   // "teoretyczny" czas rejsu (łódź2)

/* Pojemność pomostu (K<Ni) */
#define K  8

/* Po ilu sekundach (max) łódź kończy załadunek i wypływa nawet niepełna. */
#define LOAD_TIMEOUT 2

/* Rozmiar kolejek */
#define QSIZE 100000

/* Zakładamy max grupy, np. do 100000 */
#define MAX_GROUP 100000

/* Struktura pasażera w kolejce */
typedef struct {
    int  pid;         // ID pasażera
    int  disc;        // Zniżka (0 lub np. 50)
    int  group;       // ID grupy (0 - brak)
    char pass_fifo[128]; // nazwa FIFO pasażera - do wysłania "UNLOADED"
} PassengerItem;




/* Kolejka cykliczna */
typedef struct {
    PassengerItem items[QSIZE];
    int front, rear, count;
} PassQueue;

static void initQueue(PassQueue *q) {
    q->front = 0;
    q->rear  = 0;
    q->count = 0;
}
static int isEmpty(PassQueue *q){
    return (q->count==0);
}
static int isFull(PassQueue *q) {
    return (q->count==QSIZE);
}
static int enqueue(PassQueue *q, PassengerItem p){
    if(isFull(q)) return -1;
    q->items[q->rear] = p;
    q->rear = (q->rear + 1) % QSIZE;
    q->count++;
    return 0;
}
static PassengerItem dequeue(PassQueue *q){
    PassengerItem tmp = {0,0,0,""};
    if(isEmpty(q)) return tmp;
    tmp = q->items[q->front];
    q->front = (q->front + 1) % QSIZE;
    q->count--;
    return tmp;
}

/* Kolejki dla obu łodzi (normal i skip) */
static PassQueue queueBoat1, queueBoat1_skip;
static PassQueue queueBoat2, queueBoat2_skip;

/* Dla boat2 - liczenie członków grup, by sprawdzić czy 
   np. dziecko i opiekun (grupa=ta sama) dotarli */
static int groupCount[MAX_GROUP];
static int groupTarget[MAX_GROUP];

/* Stan aktywności łodzi (czy jest jeszcze dozwolona do rejsu),
   i czy łódź jest aktualnie w rejsie (boatX_inrejs=1 -> sygnał nie wymusza unload) */
static volatile sig_atomic_t boat1_active=1, boat1_inrejs=0;
static volatile sig_atomic_t boat2_active=1, boat2_inrejs=0;

/* Czas startu i końca programu (do ewent. globalnego timeoutu) */
static time_t start_time, end_time;

/* Pomost – może być w trybie: wolny/ INBOUND/ OUTBOUND */
typedef enum {INBOUND, OUTBOUND, FREE} PomostState;
static PomostState pomost_state = FREE;
static int pomost_count=0;  // ilu pasażerów aktualnie na pomoście

/* Mutex i zmienne warunkowe */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_pomost_free = PTHREAD_COND_INITIALIZER;

/* Plik do logowania zdarzeń */
// logi juz nie potrzebne, zostawiam bo mozna latwo dorobic

static void logMsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    va_end(ap);
}

/* Obsługa sygnałów – łódź1 i łódź2 kończą rejsy */
static void sigusr1_handler(int s){
    if(!boat1_inrejs){
        logMsg("[BOAT1] (SIGUSR1) w porcie => zakończ i wyładuj.\n");
    } else {
        logMsg("[BOAT1] (SIGUSR1) w rejsie => dokończę rejs normalnie.\n");
    }
    boat1_active = 0;
}
static void sigusr2_handler(int s){
    if(!boat2_inrejs){
        logMsg("[BOAT2] (SIGUSR2) w porcie => zakończ i wyładuj.\n");
    } else {
        logMsg("[BOAT2] (SIGUSR2) w rejsie => dokończę rejs normalnie.\n");
    }
    boat2_active = 0;
}
/* Struktura do pomostu -> kazda lodz posiada swoj wlasny pomost*/
typedef struct {
    PomostState state;
    int count;
} Pomost;

static Pomost pomost1 = {FREE, 0};
static Pomost pomost2 = {FREE, 0};

/* Funkcje do obsługi pomostu */
static int enter_pomost(Pomost *pomost)
{
    /* Jeżeli wolny lub INBOUND, to można wchodzić w tym kierunku.
       Tylko jeśli pomost_count < K. */
    if(pomost_state==FREE){
        pomost_state = INBOUND;
    } else if(pomost_state!=INBOUND){
        return 0;
    }
    if(pomost_count >= K) return 0;

    pomost_count++;
    return 1;
}
static void leave_pomost_in(Pomost *pomost){
    /* Zmniejsza liczbę na pomoście.
       Jeśli zrobi się zero, to pomost może przejść w stan FREE. */
    pomost_count--;
    if(pomost_count==0){
        pomost_state = FREE;
        pthread_cond_broadcast(&cond_pomost_free);
    }
}
static void start_outbound(Pomost *pomost){
    /* Łódź chce rozpocząć wyładunek (OUTBOUND).
       Musi poczekać, aż pomost jest FREE. */
    while(pomost_state != FREE){
        pthread_cond_wait(&cond_pomost_free, &mutex);
    }
    pomost_state = OUTBOUND;
}
static void end_outbound(Pomost *pomost){
    pomost_state = FREE;
    pthread_cond_broadcast(&cond_pomost_free);
}



/* Prototypy wątków dla łodzi */
void *boat1_thread(void *arg);
void *boat2_thread(void *arg);

/* ------------------------------------------------------
   boat1_thread
   - obsługuje pasażerów dla łodzi1
   - sygnał w porcie => force unload
   - sygnał w rejsie => dokończenie rejsu
------------------------------------------------------ */
void *boat1_thread(void *arg)
{
    logMsg("[BOAT1] start max=%d T1=%ds.\n", N1, T1);

    Pomost *pomost = &pomost1;

    while(1){
        pthread_mutex_lock(&mutex);

        /* Sprawdzamy, czy łódź już nieaktywna. */
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] boat1_active=0, koniec.\n");
            break;
        }

        /* Czy w kolejce cokolwiek jest? Jeśli nie, czekamy. */
        if(isEmpty(&queueBoat1_skip) && isEmpty(&queueBoat1)){
            pthread_mutex_unlock(&mutex);
            // Można tutaj dać drobny sleep, aby nie mielić CPU
            //usleep(100000);
            continue;
        }

        logMsg("[BOAT1] Załadunek...\n");
        int loaded=0;
        time_t load_start = time(NULL);

        /* rejsList – pasażerowie, którzy załadowali się na łódź */
        PassengerItem rejsList[N1];
        int rejsCount=0;

        while(rejsCount < N1 && boat1_active){
            /* Wybieramy najpierw z kolejki skip, potem normal */
            PassQueue *q = NULL;
            if(!isEmpty(&queueBoat1_skip)){
                q = &queueBoat1_skip;
            } else if(!isEmpty(&queueBoat1)){
                q = &queueBoat1;
            } else {
                /* brak pasażerów – sprawdzamy timeout LOAD_TIMEOUT */
                if(LOAD_TIMEOUT>0 && time(NULL)-load_start >= LOAD_TIMEOUT){
                    break;
                }
                /* zwalniamy na moment mutex, żeby inni mogli coś wstawić do kolejki */
                pthread_mutex_unlock(&mutex);
                //usleep(100000);
                pthread_mutex_lock(&mutex);
                if(!boat1_active) break;
                continue;
            }

            /* Sprawdzamy, czy pomost jest dostępny (INBOUND) i <K osób na nim */
            if(pomost_state==FREE || pomost_state==INBOUND){
                if(pomost_count < K){
                    PassengerItem p = q->items[q->front];

                    /* dequeue z kolejki */
                    dequeue(q);

                    /* wejdź na pomost */
                    if(enter_pomost(pomost)){
                        leave_pomost_in(pomost); // od razu zszedł i wsiadł na łódź
                        rejsList[rejsCount++] = p;
                        loaded++;
                        logMsg("[BOAT1] pasażer %d(disc=%d) wsiada (%d/%d)\n",
                               p.pid, p.disc, loaded, N1);
                    }
                } else {
                    /* pomost_count == K -> czekamy na zwolnienie */
                    pthread_mutex_unlock(&mutex);
                    //usleep(50000);
                    pthread_mutex_lock(&mutex);
                }
            } else {
                /* pomost w trybie OUTBOUND, nie można wsiadać */
                pthread_mutex_unlock(&mutex);
                //usleep(50000);
                pthread_mutex_lock(&mutex);
            }

            /* Force unload, jeśli sygnał przyszedł w trakcie załadunku */
            if(!boat1_active){
                /* Jeśli nie w rejsie => force unload */
                if(!boat1_inrejs && rejsCount>0){
                    logMsg("[BOAT1] Force unload (sygnał w porcie)...\n");
                    for(int i=0; i<rejsCount; i++){
                        PassengerItem pp = rejsList[i];
                        if(pp.pid>0 && pp.pass_fifo[0]){
                            int fd_p = open(pp.pass_fifo, O_WRONLY);
                            if(fd_p>=0){
                                char tmp[64];
                                snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                                write(fd_p, tmp, strlen(tmp));
                                close(fd_p);
                                logMsg("[BOAT1] (force) UNLOADED -> pasażer %d\n", pp.pid);
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&mutex);
                logMsg("[BOAT1] sygnał w trakcie załadunku.\n");
                return NULL; 
            }

            if(loaded == N1) break;
            if(LOAD_TIMEOUT>0 && time(NULL)-load_start >= LOAD_TIMEOUT) break;
        }

        /* Drugi check: czy w trakcie tego czekania nie wyłączono łodzi */
        if(!boat1_active){
            if(!boat1_inrejs && rejsCount>0){
                logMsg("[BOAT1] Force unload (sygnał w porcie, tuż przed rejs).\n");
                for(int i=0; i<rejsCount; i++){
                    PassengerItem pp = rejsList[i];
                    if(pp.pid>0 && pp.pass_fifo[0]){
                        int fd_p = open(pp.pass_fifo, O_WRONLY);
                        if(fd_p>=0){
                            char tmp[64];
                            snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                            write(fd_p, tmp, strlen(tmp));
                            close(fd_p);
                            logMsg("[BOAT1] (force) UNLOADED -> pasażer %d\n", pp.pid);
                        }
                    }
                }
            }
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] sygnał w trakcie załadunku.\n");
            break;
        }

        if(loaded==0){
            /* Nikogo nie załadowano, wracamy do pętli głównej */
            pthread_mutex_unlock(&mutex);
            continue;
        }

        /* Teraz czekamy, aż pomost będzie wolny (pomost_count=0, stan=FREE).
           Nie możemy wypłynąć, jeśli ktoś jeszcze wchodzi! */
        while((pomost_state==INBOUND || pomost_count>0) && boat1_active){
            pthread_cond_wait(&cond_pomost_free, &mutex);
        }
        if(!boat1_active){
            /* Force unload */
            if(!boat1_inrejs && rejsCount>0){
                logMsg("[BOAT1] Force unload (sygnał w porcie, tuż przed REJSEM).\n");
                for(int i=0; i<rejsCount; i++){
                    PassengerItem pp = rejsList[i];
                    if(pp.pid>0 && pp.pass_fifo[0]){
                        int fd_p= open(pp.pass_fifo, O_WRONLY);
                        if(fd_p>=0){
                            char tmp[64];
                            snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                            write(fd_p, tmp, strlen(tmp));
                            close(fd_p);
                            logMsg("[BOAT1] (force) UNLOADED -> pasażer %d\n", pp.pid);
                        }
                    }
                }
            }
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] przerwanie przed rejsem.\n");
            break;
        }
         time_t now = time(NULL);
        if(now+T1+T1*0.9 > end_time){
            logMsg("[BOAT1] brak czasu na rejs.\n");
            /* wyładuj */
            start_outbound(pomost);
            logMsg("[BOAT1] %d pasażerów zeszło (koniec czasu).\n",rejsCount);
            for(int i=0;i<rejsCount;i++){
                PassengerItem pp= rejsList[i];
                if(pp.group>0) groupCount[pp.group]--;
            }
            end_outbound(pomost);
            pthread_mutex_unlock(&mutex);
            break;
        }


        /* Teraz łódź1 wyrusza w rejs */
        boat1_inrejs = 1;
        logMsg("[BOAT1] Wypływam z %d pasażerami (rejs logicznie %ds).\n", rejsCount, T1);
        pthread_mutex_unlock(&mutex);

        // Tu brak realnego sleep, można ewentualnie dać "usleep(1000*T1)".
        //usleep(1000*T1);

        /* Koniec rejsu -> zaczynamy wyładunek (OUTBOUND) */
        pthread_mutex_lock(&mutex);
        boat1_inrejs=0;
        logMsg("[BOAT1] Rejs koniec -> OUTBOUND.\n");
        start_outbound(pomost);

        /* "UNLOAD" -> wysyłamy do każdego pasażera 'UNLOADED <pid>' */
        for(int i=0; i<rejsCount; i++){
            PassengerItem pp = rejsList[i];
            if(pp.pid>0 && pp.pass_fifo[0]){
                int fd_p= open(pp.pass_fifo, O_WRONLY);
                if(fd_p>=0){
                    char tmp[64];
                    snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                    write(fd_p, tmp, strlen(tmp));
                    close(fd_p);
                    logMsg("[BOAT1] UNLOADED -> pasażer %d\n", pp.pid);
                }
            }
        }
        logMsg("[BOAT1] pasażerowie wyszli.\n");

        /* Zwolnij pomost z OUTBOUND i wróć do FREE */
        end_outbound(pomost);
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] sygnał w trakcie/po wyład.\n");
            break;
        }

        pthread_mutex_unlock(&mutex);
    }

    logMsg("[BOAT1] koniec wątku.\n");
    return NULL;
}

/* ------------------------------------------------------
   boat2_thread
   - analogicznie do boat1, z dodatkową obsługą grup
------------------------------------------------------ */
void *boat2_thread(void *arg)
{
    logMsg("[BOAT2] start max=%d T2=%ds.\n", N2, T2);

    /* Zerowanie grup */
    memset(groupCount,  0, sizeof(groupCount));
    memset(groupTarget, 0, sizeof(groupTarget));

    Pomost *pomost = &pomost2;

    while(1){
        pthread_mutex_lock(&mutex);

        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] boat2_active=0, koniec.\n");
            break;
        }

        /* Czy są pasażerowie w kolejkach? */
        if(isEmpty(&queueBoat2_skip) && isEmpty(&queueBoat2)){
            pthread_mutex_unlock(&mutex);
            //usleep(200000);
            continue;
        }

        logMsg("[BOAT2] Załadunek...\n");
        int loaded=0;
        time_t load_start = time(NULL);

        PassengerItem rejsList[N2];
        int rejsCount=0;

        while(rejsCount < N2 && boat2_active){
            PassQueue *q=NULL;
            if(!isEmpty(&queueBoat2_skip)) {
                q = &queueBoat2_skip;
            } else if(!isEmpty(&queueBoat2)) {
                q = &queueBoat2;
            } else {
                if(LOAD_TIMEOUT>0 && time(NULL)-load_start >= LOAD_TIMEOUT) {
                    break;
                }
                pthread_mutex_unlock(&mutex);
                //usleep(100000);
                pthread_mutex_lock(&mutex);
                if(!boat2_active) break;
                continue;
            }

            if(pomost_state==FREE || pomost_state==INBOUND){
                if(pomost_count< K){
                    PassengerItem p = q->items[q->front];
                    dequeue(q);
                    if(enter_pomost(pomost)){
                        leave_pomost_in(pomost);
                        /* W boat2: jeżeli group>0 i groupTarget[group]==0,
                           to ustawiamy groupTarget=2 (sygnalizuje, że ma płynąć
                           łącznie 2 osoby - np. dziecko+opiekun).
                        */
                        if(p.group>0 && groupTarget[p.group]==0){
                            groupTarget[p.group] = 2; 
                        }
                        groupCount[p.group]++;

                        rejsList[rejsCount++] = p;
                        loaded++;
                        logMsg("[BOAT2] pasażer %d(disc=%d,grp=%d) wsiada (%d/%d)\n",
                               p.pid, p.disc, p.group, loaded, N2);
                    }
                } else {
                    pthread_mutex_unlock(&mutex);
                    //usleep(50000);
                    pthread_mutex_lock(&mutex);
                }
            } else {
                pthread_mutex_unlock(&mutex);
                //usleep(50000);
                pthread_mutex_lock(&mutex);
            }

            /* Force unload w porcie, jeśli sygnał nadszedł */
            if(!boat2_active){
                if(!boat2_inrejs && rejsCount>0){
                    logMsg("[BOAT2] Force unload (sygnał w porcie)...\n");
                    for(int i=0; i<rejsCount; i++){
                        PassengerItem pp= rejsList[i];
                        if(pp.pid>0 && pp.pass_fifo[0]){
                            int fd_p= open(pp.pass_fifo, O_WRONLY);
                            if(fd_p>=0){
                                char tmp[64];
                                snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                                write(fd_p, tmp, strlen(tmp));
                                close(fd_p);
                                logMsg("[BOAT2] (force) UNLOADED -> pasażer %d\n", pp.pid);
                            }
                        }
                        if(pp.group>0) groupCount[pp.group]--;
                    }
                }
                pthread_mutex_unlock(&mutex);
                logMsg("[BOAT2] sygnał w trakcie załadunku.\n");
                return NULL;
            }

            if(loaded==N2) break;
            if(LOAD_TIMEOUT>0 && time(NULL)-load_start >= LOAD_TIMEOUT) break;
        }

        if(!boat2_active){
            if(!boat2_inrejs && rejsCount>0){
                logMsg("[BOAT2] Force unload (sygnał w porcie, tuż przed rejs).\n");
                for(int i=0; i<rejsCount; i++){
                    PassengerItem pp= rejsList[i];
                    if(pp.pid>0 && pp.pass_fifo[0]){
                        int fd_p= open(pp.pass_fifo, O_WRONLY);
                        if(fd_p>=0){
                            char tmp[64];
                            snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                            write(fd_p, tmp, strlen(tmp));
                            close(fd_p);
                            logMsg("[BOAT2] (force) UNLOADED -> pasażer %d\n", pp.pid);
                        }
                    }
                    if(pp.group>0) groupCount[pp.group]--;
                }
            }
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] sygnał w trakcie załadunku.\n");
            break;
        }

        if(loaded==0){
            pthread_mutex_unlock(&mutex);
            continue;
        }

        /* Czekamy aż pomost będzie wolny, bo sternik nie może wypłynąć, 
           gdy ktoś jeszcze wchodzi. */
        while((pomost_state==INBOUND || pomost_count>0) && boat2_active){
            pthread_cond_wait(&cond_pomost_free,&mutex);
        }
        if(!boat2_active){
            if(!boat2_inrejs && rejsCount>0){
                logMsg("[BOAT2] Force unload (sygnał w porcie, tuż przed rejs).\n");
                for(int i=0; i<rejsCount; i++){
                    PassengerItem pp= rejsList[i];
                    if(pp.pid>0 && pp.pass_fifo[0]){
                        int fd_p= open(pp.pass_fifo, O_WRONLY);
                        if(fd_p>=0){
                            char tmp[64];
                            snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                            write(fd_p, tmp, strlen(tmp));
                            close(fd_p);
                            logMsg("[BOAT2] (force) UNLOADED -> pasażer %d\n", pp.pid);
                        }
                    }
                    if(pp.group>0) groupCount[pp.group]--;
                }
            }
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie przed rejs.\n");
            break;
        }

        /* Sprawdzamy, czy wszystkie grupy mają komplet (np. min. 2 osoby). 
           Jeżeli np. jest dziecko (group>0) i brakuje opiekuna => rejs odwołany. */
        int allGroupsOk=1;
        for(int i=0; i<rejsCount; i++){
            PassengerItem pp= rejsList[i];
            if(pp.group>0 && groupCount[pp.group] < 2){
                // np. jest 1, a powinno być 2
                allGroupsOk=0;
                break;
            }
        }
        if(!allGroupsOk){
            logMsg("[BOAT2] brakuje partnera z group -> rezygnuję z rejsu.\n");
            start_outbound(pomost);
            logMsg("[BOAT2] %d pasażerów zeszło (niedokończona grupa).\n", rejsCount);

            /* Ci pasażerowie schodzą, groupCount-- */
            for(int i=0; i<rejsCount; i++){
                PassengerItem pp= rejsList[i];
                if(pp.group>0) groupCount[pp.group]--;
            }
            end_outbound(pomost);
            pthread_mutex_unlock(&mutex);
            continue;
        }

         /* sprawdzamy czas */
        time_t now= time(NULL);
        if(now+T2+T2*0.9> end_time){
            logMsg("[BOAT2] brak czasu na rejs.\n");
            /* wyładuj */
            start_outbound(pomost);
            logMsg("[BOAT2] %d pasażerów zeszło (koniec czasu).\n",rejsCount);
            for(int i=0;i<rejsCount;i++){
                PassengerItem pp= rejsList[i];
                if(pp.group>0) groupCount[pp.group]--;
            }
            end_outbound(pomost);
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Start rejsu */
        boat2_inrejs=1;
        logMsg("[BOAT2] Wypływam z %d pasażerami (rejs logicznie %ds).\n", rejsCount, T2);
        pthread_mutex_unlock(&mutex);

        //sleep(T2);//komentujemy do sprawdzenia - odkomentowac w celu realnej symulacji

        pthread_mutex_lock(&mutex);
        boat2_inrejs=0;
        logMsg("[BOAT2] Rejs koniec -> OUTBOUND.\n");
        start_outbound(pomost);

        /* Wyładunek normalny -> "UNLOADED <pid>" */
        for(int i=0; i<rejsCount; i++){
            PassengerItem pp= rejsList[i];
            if(pp.pid>0 && pp.pass_fifo[0]){
                int fd_p= open(pp.pass_fifo, O_WRONLY);
                if(fd_p>=0){
                    char tmp[64];
                    snprintf(tmp,sizeof(tmp),"UNLOADED %d\n", pp.pid);
                    write(fd_p, tmp, strlen(tmp));
                    close(fd_p);
                    logMsg("[BOAT2] UNLOADED -> pasażer %d\n", pp.pid);
                }
            }
        }

        logMsg("[BOAT2] pasażerowie wyszli.\n");
        end_outbound(pomost);

        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] sygnał po wyład.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);
    }

    logMsg("[BOAT2] koniec wątku.\n");
    return NULL;
}

/* MAIN sternik */
int main(int argc, char* argv[])
{
    setbuf(stdout,NULL);

    if(argc<2){
        fprintf(stderr,"Użycie: %s <timeout_s>\n",argv[0]);
        return 1;
    }
    int timeout_value= atoi(argv[1]);
    start_time = time(NULL);
    end_time   = start_time + timeout_value;

    /*sternikLog= fopen("sternik.log","w");
    if(!sternikLog){
        perror("[STERNIK] sternik.log");
    }*/

    /* Ustawienie handlerów sygnałów SIGUSR1 i SIGUSR2 */
    struct sigaction sa1, sa2;
    memset(&sa1, 0, sizeof(sa1));
    sa1.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa1, NULL);

    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa2, NULL);

    /* Inicjujemy kolejki */
    initQueue(&queueBoat1);
    initQueue(&queueBoat1_skip);
    initQueue(&queueBoat2);
    initQueue(&queueBoat2_skip);

    pomost_state = FREE;
    pomost_count = 0;

    /* Otwieramy fifo_sternik_in (przyjmujemy, że mkfifo wykonuje orchestrator lub my) */
    int fd_in= open("fifo_sternik_in", O_RDONLY | O_NONBLOCK);
    if(fd_in<0){
        perror("[STERNIK] open(fifo_sternik_in)");
        //if(sternikLog) fclose(sternikLog);
        return 1;
    }

    /* Tworzymy wątki łodzi */
    pthread_t t1, t2;
    pthread_create(&t1, NULL, boat1_thread, NULL);
    pthread_create(&t2, NULL, boat2_thread, NULL);

    logMsg("[STERNIK] start (timeout=%d).\n", timeout_value);

    char readbuf[1024];
    ssize_t rb_len = 0;

    /* Pętla główna sternika – wczytuje komendy z fifo_sternik_in */
    while(1){
        ssize_t n= read(fd_in, readbuf + rb_len, sizeof(readbuf)-1 - rb_len);
        if(n>0){
            rb_len += n;
            readbuf[rb_len] = '\0';

            /* Rozbijamy na linie po '\n' */
            char *start = readbuf;
            while(1) {
                char *nl = strchr(start, '\n');
                if(!nl) break;
                *nl = '\0'; // zamieniamy '\n' na '\0'

                char line[256];
                strncpy(line, start, sizeof(line));
                line[sizeof(line)-1] = '\0';
                start = nl + 1;

                /* Parsujemy komendę */
                if(!strncmp(line, "QUEUE_SKIP", 10)){
                    /* Format: QUEUE_SKIP pid boat disc pass_fifo */
                    int pid=0, bno=0, disc=0;
                    char p_fifo[128]={0};

                    int c = sscanf(line,"QUEUE_SKIP %d %d %d %s", &pid, &bno, &disc, p_fifo);
                    if(c < 4){
                        logMsg("[STERNIK] Błędne skip: %s\n", line);
                        continue;
                    }
                    PassengerItem pi;
                    pi.pid   = pid;
                    pi.disc  = disc;
                    pi.group = 0;
                    strncpy(pi.pass_fifo, p_fifo, sizeof(pi.pass_fifo));

                    pthread_mutex_lock(&mutex);
                    if(bno==1 && boat1_active){
                        if(!isFull(&queueBoat1_skip)){
                            enqueue(&queueBoat1_skip, pi);
                            logMsg("[STERNIK] skip pass %d -> boat1_skip (disc=%d)\n",
                                   pid, pi.disc);
                        } else {
                            logMsg("[STERNIK] queueBoat1_skip full -> odrzucam %d\n", pid);
                        }
                    }
                    else if(bno==2 && boat2_active){
                        if(!isFull(&queueBoat2_skip)){
                            enqueue(&queueBoat2_skip, pi);
                            logMsg("[STERNIK] skip pass %d -> boat2_skip (disc=%d)\n",
                                   pid, pi.disc);
                        } else {
                            logMsg("[STERNIK] queueBoat2_skip full -> odrzucam %d\n", pid);
                        }
                    }
                    else {
                        logMsg("[STERNIK] boat %d inactive => %d odrzucony\n", bno,pid);
                    }
                    pthread_mutex_unlock(&mutex);
                }
                else if(!strncmp(line, "QUEUE", 5)){
                    /* Format: QUEUE pid boat disc pass_fifo */
                    int pid=0, bno=0, disc=0;
                    char p_fifo[128]={0};

                    int c= sscanf(line,"QUEUE %d %d %d %s", &pid,&bno,&disc,p_fifo);
                    if(c < 4){
                        logMsg("[STERNIK] Błędne queue: %s\n", line);
                        continue;
                    }
                    PassengerItem pi;
                    pi.pid   = pid;
                    pi.disc  = disc;
                    pi.group = 0;
                    strncpy(pi.pass_fifo, p_fifo, sizeof(pi.pass_fifo));

                    pthread_mutex_lock(&mutex);
                    if(bno==1 && boat1_active){
                        if(!isFull(&queueBoat1)){
                            enqueue(&queueBoat1, pi);
                            logMsg("[STERNIK] pass %d->boat1 disc=%d\n", pid, disc);
                        } else {
                            logMsg("[STERNIK] queueBoat1 full => odrzucono %d\n", pid);
                        }
                    }
                    else if(bno==2 && boat2_active){
                        if(!isFull(&queueBoat2)){
                            enqueue(&queueBoat2, pi);
                            logMsg("[STERNIK] pass %d->boat2 disc=%d\n", pid, disc);
                        } else {
                            logMsg("[STERNIK] queueBoat2 full => odrzucono %d\n", pid);
                        }
                    }
                    else {
                        logMsg("[STERNIK] boat %d inactive => %d odrzucony\n", bno,pid);
                    }
                    pthread_mutex_unlock(&mutex);
                }
                else if(!strncmp(line, "INFO", 4)){
                    /* Informacja diagnostyczna */
                    pthread_mutex_lock(&mutex);
                    const char *st = (pomost_state==FREE)?"FREE":
                                     (pomost_state==INBOUND)?"INBOUND":"OUTBOUND";
                    logMsg("[INFO] b1_act=%d rejs=%d, b2_act=%d rejs=%d, "
                           "q1=%d skip=%d, q2=%d skip=%d, p_count=%d, st=%s\n",
                           boat1_active, boat1_inrejs,
                           boat2_active, boat2_inrejs,
                           queueBoat1.count, queueBoat1_skip.count,
                           queueBoat2.count, queueBoat2_skip.count,
                           pomost_count, st);
                    pthread_mutex_unlock(&mutex);
                }
                else if(!strncmp(line, "QUIT", 4)){
                    logMsg("[STERNIK] QUIT => end.\n");
                    goto finish;
                }
                else {
                    logMsg("[STERNIK] Nieznane: %s\n", line);
                }
            }
            /* Przenosimy ewentualną pozostałą część bufora (bez zakończonej linii) */
            ssize_t rem = rb_len - (start - readbuf);
            if(rem>0) {
                memmove(readbuf, start, rem);
            }
            rb_len = rem;
        }
        else if(n<0) {
            if(errno!=EAGAIN && errno!=EINTR){
                perror("[STERNIK] read");
                break;
            }
        }

        /* Timeout globalny? */
        if(time(NULL) >= end_time){
            logMsg("[STERNIK] Czas się skończył => end.\n");
            break;
        }

        /* Czy obie łodzie nieaktywne? */
        pthread_mutex_lock(&mutex);
        if(!boat1_active && !boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[STERNIK] Obie łodzie inactive -> end.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);
    }

finish:
    pthread_mutex_lock(&mutex);
    boat1_active = 0;
    boat2_active = 0;
    pthread_mutex_unlock(&mutex);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(fd_in);
    logMsg("[STERNIK] end.\n");
    //if(sternikLog) fclose(sternikLog);
    return 0;
}
