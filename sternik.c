/*******************************************************
  File: sternik.c
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

#define N1 5
#define T1 4
#define N2 6
#define T2 5
#define K  2
#define LOAD_TIMEOUT 2

#define QSIZE 50

typedef struct {
    int pid;
    int disc;
    int group; /* <--- nowy! */
} PassengerItem;

typedef struct {
    PassengerItem items[QSIZE];
    int front, rear, count;
} PassQueue;

static void initQueue(PassQueue *q) {
    q->front=0; q->rear=0; q->count=0;
}
static int isEmpty(PassQueue *q){return q->count==0;}
static int isFull(PassQueue *q) {return q->count==QSIZE;}
static int enqueue(PassQueue *q, PassengerItem p){
    if(isFull(q)) return -1;
    q->items[q->rear]=p;
    q->rear=(q->rear+1)%QSIZE; q->count++;
    return 0;
}
static PassengerItem dequeue(PassQueue *q){
    PassengerItem tmp={0,0,0};
    if(isEmpty(q)) return tmp;
    tmp=q->items[q->front];
    q->front=(q->front+1)%QSIZE; q->count--;
    return tmp;
}

/* Kolejki: skip i normalne */
static PassQueue queueBoat1, queueBoat1_skip;
static PassQueue queueBoat2, queueBoat2_skip;

static FILE *sternikLog=NULL;

/* active=0 => łódź już nie pływa */
static volatile sig_atomic_t boat1_active=1, boat1_inrejs=0;
static volatile sig_atomic_t boat2_active=1, boat2_inrejs=0;

/* time limit */
static time_t start_time;
static time_t end_time;

/* pomost */
typedef enum {INBOUND, OUTBOUND, FREE} PomostState;
static PomostState pomost_state=FREE;
static int pomost_count=0;

static pthread_mutex_t mutex= PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_pomost_free= PTHREAD_COND_INITIALIZER;

static void logMsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout,fmt,ap);
    fflush(stdout);
    if(sternikLog){
        vfprintf(sternikLog,fmt,ap);
        fflush(sternikLog);
    }
    va_end(ap);
}

/* sygnały */
static void sigusr1_handler(int s){
    if(!boat1_inrejs){
        logMsg("[BOAT1] (SIGUSR1) w porcie, koniec.\n");
    } else {
        logMsg("[BOAT1] (SIGUSR1) w rejsie, dokończę.\n");
    }
    boat1_active=0;
}
static void sigusr2_handler(int s){
    if(!boat2_inrejs){
        logMsg("[BOAT2] (SIGUSR2) w porcie, koniec.\n");
    } else {
        logMsg("[BOAT2] (SIGUSR2) w rejsie, dokończę.\n");
    }
    boat2_active=0;
}

/* Pomost */
static int enter_pomost(void)
{
    if(pomost_state==FREE){
        pomost_state=INBOUND;
    } else if(pomost_state!=INBOUND){
        return 0;
    }
    if(pomost_count>=K) return 0;
    pomost_count++;
    return 1;
}
static void leave_pomost_in(void){
    pomost_count--;
    if(pomost_count==0){
        pomost_state=FREE;
        pthread_cond_broadcast(&cond_pomost_free);
    }
}
static void start_outbound(void){
    while(pomost_state!=FREE){
        pthread_cond_wait(&cond_pomost_free,&mutex);
    }
    pomost_state=OUTBOUND;
}
static void end_outbound(void){
    pomost_state=FREE;
    pthread_cond_broadcast(&cond_pomost_free);
}

/* W wątku boat2: przechowujemy info o tym, ile osób w danej group. 
   Załóżmy prosto, że group liczy max 2. */
#define MAX_GROUP 100000
static int groupCount[MAX_GROUP];   /* ile osób z danej grupy załadowaliśmy */
static int groupTarget[MAX_GROUP];  /* docelowo 2, jeśli w ogóle group>0 */

void *boat1_thread(void *arg)
{
    logMsg("[BOAT1] start max=%d T1=%ds.\n",N1,T1);
    while(1){
        pthread_mutex_lock(&mutex);
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] kończę.\n");
            break;
        }
        /* ... identyczna logika jak w oryginale ... */
        /* -- POMIJAMY: boat1 nie obsługuje logicznie group? 
           ewentualnie docinamy kod tak, by boat1 
           nie musiał nic specjalnego z group. 
        */
        if(isEmpty(&queueBoat1_skip) && isEmpty(&queueBoat1)){
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }
        logMsg("[BOAT1] Załadunek...\n");
        int loaded=0;
        time_t load_start=time(NULL);

        while(loaded<N1 && boat1_active){
            PassQueue *q=NULL;
            if(!isEmpty(&queueBoat1_skip)) q=&queueBoat1_skip;
            else if(!isEmpty(&queueBoat1)) q=&queueBoat1;
            else {
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
                if(!boat1_active) break;
                if(difftime(time(NULL),load_start)>=LOAD_TIMEOUT) break;
                continue;
            }
            PassengerItem p= q->items[q->front];
            if(pomost_state==FREE || pomost_state==INBOUND){
                if(pomost_count<K){
                    dequeue(q);
                    if(enter_pomost()){
                        leave_pomost_in();
                        loaded++;
                        logMsg("[BOAT1] pasażer %d(disc=%d) wsiada (%d/%d)\n",
                               p.pid,p.disc,loaded,N1);
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
            if(!boat1_active) break;
            if(loaded==N1) break;
            if(difftime(time(NULL),load_start)>=LOAD_TIMEOUT) break;
        }
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] sygnał w trakcie załadunku.\n");
            break;
        }
        if(loaded==0){
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }
        while((pomost_state==INBOUND|| pomost_count>0)&&boat1_active){
            pthread_cond_wait(&cond_pomost_free,&mutex);
        }
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] przerwanie przed rejs.\n");
            break;
        }
        time_t now=time(NULL);
        if(now+T1> end_time){
            logMsg("[BOAT1] brak czasu na rejs.\n");
            pthread_mutex_unlock(&mutex);
            break;
        }
        boat1_inrejs=1;
        logMsg("[BOAT1] Wypływam z %d.\n",loaded);
        pthread_mutex_unlock(&mutex);
        sleep(T1);

        pthread_mutex_lock(&mutex);
        boat1_inrejs=0;
        logMsg("[BOAT1] Rejs koniec->outbound.\n");
        start_outbound();
        logMsg("[BOAT1] pasażerowie wyszli.\n");
        end_outbound();
        if(!boat1_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT1] sygnał w trakcie wyład.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);
        usleep(50000);
    }
    logMsg("[BOAT1] koniec wątku.\n");
    return NULL;
}

/* BOAT2: 
   - trzymamy groupCount[] i groupTarget[], 
   - groupTarget[g] = 2 (domyślnie), 
   - ładujemy pasażerów i jak mamy dwie osoby z group g, 
     to wiemy, że komplet. Nie wypływamy, dopóki 
     w kabinie nie mamy pełnej grupy.
*/
void *boat2_thread(void *arg)
{
    logMsg("[BOAT2] start max=%d T2=%ds.\n",N2,T2);

    /* Inicjalizujemy tablice groupCount i groupTarget */
    memset(groupCount, 0,sizeof(groupCount));
    memset(groupTarget,0,sizeof(groupTarget));

    while(1){
        pthread_mutex_lock(&mutex);
        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] boat2_active=0, koniec.\n");
            break;
        }
        if(isEmpty(&queueBoat2_skip) && isEmpty(&queueBoat2)){
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }
        logMsg("[BOAT2] Załadunek...\n");
        int loaded=0;
        time_t load_start=time(NULL);

        /* Tymczasowa lista pasażerów do rejsu */
        PassengerItem rejsList[N2];
        int rejsCount=0;

        while(rejsCount<N2 && boat2_active){
            PassQueue *q=NULL;
            if(!isEmpty(&queueBoat2_skip)) q=&queueBoat2_skip;
            else if(!isEmpty(&queueBoat2)) q=&queueBoat2;
            else {
                pthread_mutex_unlock(&mutex);
                usleep(50000);
                pthread_mutex_lock(&mutex);
                if(!boat2_active) break;
                if(difftime(time(NULL),load_start)>=LOAD_TIMEOUT) break;
                continue;
            }
            PassengerItem p= q->items[q->front];
            /* wsiadanie */
            if(pomost_state==FREE|| pomost_state==INBOUND){
                if(pomost_count<K){
                    dequeue(q);
                    if(enter_pomost()){
                        leave_pomost_in();
                        /* Obsługa group */
                        if(p.group>0 && groupTarget[p.group]==0){
                            groupTarget[p.group]=2; 
                        }
                        groupCount[p.group]++; 
                        rejsList[rejsCount++]=p;
                        loaded++;
                        logMsg("[BOAT2] pasażer %d(disc=%d,grp=%d) wsiada (%d/%d)\n",
                               p.pid,p.disc,p.group,loaded,N2);
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
            if(!boat2_active) break;
            if(loaded==N2) break;
            if(difftime(time(NULL),load_start)>=LOAD_TIMEOUT) break;
        }

        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] sygnał w trakcie załadunku.\n");
            break;
        }
        if(loaded==0){
            pthread_mutex_unlock(&mutex);
            usleep(50000);
            continue;
        }

        /* czekamy, aż pomost się zwolni */
        while((pomost_state==INBOUND || pomost_count>0) && boat2_active){
            pthread_cond_wait(&cond_pomost_free,&mutex);
        }
        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie przed rejs.\n");
            break;
        }

        /* sprawdzamy group -> czy mamy wszystkie osoby z danej grupy? 
           Jeżeli jest w rejsList ktoś z group>0 i groupCount[group]<2 => 
           brakuje partnera => czekamy?
           Dla uproszczenia: 
           - sprawdzamy, czy dla wszystkich w rejsList group>0 => groupCount[group]>=2
           - jeśli nie, to czekamy jeszcze chwilę i próbujemy wczytać brakującą osobę
        */
        int allGroupsOk=1;
        for(int i=0; i<rejsCount; i++){
            PassengerItem p= rejsList[i];
            if(p.group>0 && groupCount[p.group]< groupTarget[p.group]){
                allGroupsOk=0;
                break;
            }
        }
        if(!allGroupsOk){
            logMsg("[BOAT2] brakuje partnera z group -> rezygnuję z rejsu i wyładowuję.\n");
            /* wyładuj tymczasowo wszystkich */
            start_outbound();
            logMsg("[BOAT2] %d pasażerów zeszło (niedokończona grupa).\n", rejsCount);
            for(int i=0; i<rejsCount;i++){
                PassengerItem pp= rejsList[i];
                if(pp.group>0) {
                    groupCount[pp.group]--; 
                }
            }
            end_outbound();
            pthread_mutex_unlock(&mutex);
            usleep(500000); /* dłuższa pauza, by dać czas wczytać brakującą osobę */
            continue;
        }

        /* sprawdzamy czas */
        time_t now= time(NULL);
        if(now+T2> end_time){
            logMsg("[BOAT2] brak czasu na rejs.\n");
            /* wyładuj */
            start_outbound();
            logMsg("[BOAT2] %d pasażerów zeszło (koniec czasu).\n",rejsCount);
            for(int i=0;i<rejsCount;i++){
                PassengerItem pp= rejsList[i];
                if(pp.group>0) groupCount[pp.group]--;
            }
            end_outbound();
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Wypływamy */
        boat2_inrejs=1;
        logMsg("[BOAT2] Wypływam z %d pasażerami.\n", rejsCount);
        pthread_mutex_unlock(&mutex);

        sleep(T2);

        pthread_mutex_lock(&mutex);
        boat2_inrejs=0;
        logMsg("[BOAT2] Rejs koniec -> OUTBOUND.\n");
        start_outbound();
        logMsg("[BOAT2] pasażerowie wyszli.\n");
        end_outbound();
        if(!boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[BOAT2] przerwanie po wyład.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);
        usleep(50000);
    }

    logMsg("[BOAT2] koniec wątku.\n");
    return NULL;
}

/* main */
int main(int argc, char*argv[])
{
    setbuf(stdout,NULL);
    if(argc<2){
        fprintf(stderr,"Użycie: %s <timeout_s>\n",argv[0]);
        return 1;
    }
    int timeout_value= atoi(argv[1]);
    start_time=time(NULL);
    end_time= start_time+ timeout_value;

    sternikLog= fopen("sternik.log","w");
    if(!sternikLog){
        perror("[STERNIK] sternik.log");
    }

    /* sygnały */
    struct sigaction sa1, sa2;
    memset(&sa1,0,sizeof(sa1));
    sa1.sa_handler= sigusr1_handler;
    sigaction(SIGUSR1,&sa1,NULL);

    memset(&sa2,0,sizeof(sa2));
    sa2.sa_handler= sigusr2_handler;
    sigaction(SIGUSR2,&sa2,NULL);

    initQueue(&queueBoat1);
    initQueue(&queueBoat1_skip);
    initQueue(&queueBoat2);
    initQueue(&queueBoat2_skip);

    pomost_state= FREE;
    pomost_count=0;

    unlink("fifo_sternik_in");
    mkfifo("fifo_sternik_in",0666);
    int fd_in= open("fifo_sternik_in", O_RDONLY|O_NONBLOCK);
    if(fd_in<0){
        perror("[STERNIK] fifo_sternik_in");
        if(sternikLog) fclose(sternikLog);
        return 1;
    }

    pthread_t t1,t2;
    pthread_create(&t1,NULL, boat1_thread,NULL);
    pthread_create(&t2,NULL, boat2_thread,NULL);

    logMsg("[STERNIK] start (timeout=%d).\n",timeout_value);

    char buf[256];
    while(1){
        ssize_t n= read(fd_in, buf,sizeof(buf)-1);
        if(n>0){
            buf[n]='\0';
            if(!strncmp(buf,"QUEUE_SKIP",10)){
                /* QUEUE_SKIP pid boat */
                int pid=0, bno=0;
                sscanf(buf,"QUEUE_SKIP %d %d",&pid,&bno);
                pthread_mutex_lock(&mutex);
                /* skip domyślnie disc=50, group=0? 
                   Niestety nie mamy group parametru. 
                   Lepiej by było "QUEUE_SKIP <pid> <boat> <group>" 
                   ale na razie uprośćmy. 
                */
                PassengerItem pi={ pid, 50, 0};
                if(bno==1 && boat1_active){
                    if(!isFull(&queueBoat1_skip)){
                        enqueue(&queueBoat1_skip, pi);
                        logMsg("[STERNIK] skip pass %d -> queueBoat1_skip.\n",pid);
                    } else {
                        logMsg("[STERNIK] queueBoat1_skip full -> odrzucam %d\n",pid);
                    }
                }
                else if(bno==2 && boat2_active){
                    if(!isFull(&queueBoat2_skip)){
                        enqueue(&queueBoat2_skip, pi);
                        logMsg("[STERNIK] skip pass %d -> queueBoat2_skip.\n",pid);
                    } else {
                        logMsg("[STERNIK] queueBoat2_skip full -> odrzucam %d\n",pid);
                    }
                }
                else {
                    logMsg("[STERNIK] boat %d nieaktywna => odrzucono %d\n",bno,pid);
                }
                pthread_mutex_unlock(&mutex);

            } else if(!strncmp(buf,"QUEUE",5)){
                /* QUEUE pid boat disc */
                int pid=0,bno=0,disc=0;
                /* tu brakuje group parametru, w minimalnej wersji 
                   go nie mamy. Możemy dodać: "QUEUE <pid> <bno> <disc> <group>"
                   i w PassengerItem dodać group. 
                   => Zmieniamy protokół: "QUEUE %d %d %d %d"
                */
                int c= sscanf(buf,"QUEUE %d %d %d",&pid,&bno,&disc);
                if(c<3){
                    printf("[STERNIK] Błędne queue: %s\n",buf);
                    continue;
                }
                PassengerItem pi={pid, disc, 0};
                pthread_mutex_lock(&mutex);
                if(bno==1 && boat1_active){
                    if(!isFull(&queueBoat1)){
                        enqueue(&queueBoat1, pi);
                        logMsg("[STERNIK] pass %d->boat1 disc=%d\n",pid,disc);
                    } else {
                        logMsg("[STERNIK] queueBoat1 full => odrzucono %d\n",pid);
                    }
                }
                else if(bno==2 && boat2_active){
                    if(!isFull(&queueBoat2)){
                        enqueue(&queueBoat2, pi);
                        logMsg("[STERNIK] pass %d->boat2 disc=%d\n",pid,disc);
                    } else {
                        logMsg("[STERNIK] queueBoat2 full => odrzucono %d\n",pid);
                    }
                }
                else {
                    logMsg("[STERNIK] boat %d inactive => %d odrzucony\n",bno,pid);
                }
                pthread_mutex_unlock(&mutex);

            } else if(!strncmp(buf,"INFO",4)){
                pthread_mutex_lock(&mutex);
                const char *st= (pomost_state==FREE)?"FREE": 
                                (pomost_state==INBOUND)?"INBOUND":"OUTBOUND";
                logMsg("[INFO] boat1_act=%d rejs=%d, boat2_act=%d rejs=%d, q1=%d skip=%d, q2=%d skip=%d, p_count=%d, st=%s\n",
                       boat1_active, boat1_inrejs,
                       boat2_active, boat2_inrejs,
                       queueBoat1.count, queueBoat1_skip.count,
                       queueBoat2.count, queueBoat2_skip.count,
                       pomost_count, st);
                pthread_mutex_unlock(&mutex);
            } else if(!strncmp(buf,"QUIT",4)){
                logMsg("[STERNIK] QUIT => end.\n");
                break;
            }
        }
        pthread_mutex_lock(&mutex);
        if(!boat1_active && !boat2_active){
            pthread_mutex_unlock(&mutex);
            logMsg("[STERNIK] obie łodzie inactive -> end.\n");
            break;
        }
        pthread_mutex_unlock(&mutex);

        usleep(50000);
    }

    pthread_mutex_lock(&mutex);
    boat1_active=0;
    boat2_active=0;
    pthread_mutex_unlock(&mutex);

    pthread_join(t1,NULL);
    pthread_join(t2,NULL);

    close(fd_in);
    logMsg("[STERNIK] end.\n");
    if(sternikLog) fclose(sternikLog);
    return 0;
}