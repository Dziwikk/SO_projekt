/*******************************************************
 * File: policjant.c
 *
 * Uruchamia się z parametrem PID sternika,
 * wysyła kolejno sygnały:
 *   - SIGUSR1 (zakończenie łodzi1)
 *   - SIGUSR2 (zakończenie łodzi2)
 * po losowym (kilkusekundowym) czasie.
 ******************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    if (argc<2) {
        fprintf(stdout, "[POLICJANT] Użycie: %s <pid_sternika>\n", argv[0]);
        return 1;
    }
    pid_t pid_sternik = atoi(argv[1]);
    printf("[POLICJANT] Cel: sternik PID=%d\n", pid_sternik);

    srand(time(NULL));

    /* Po kilku sekundach -> SIGUSR1 (zabrania łodzi1 pływać dalej) */
    sleep((rand()%5)+2);
    printf("[POLICJANT] SIGUSR1 -> łódź1\n");
    kill(pid_sternik, SIGUSR1);

    /* Po kolejnych kilku sekundach -> SIGUSR2 (zabrania łodzi2 pływać dalej) */
    sleep((rand()%5)+2);
    printf("[POLICJANT] SIGUSR2 -> łódź2\n");
    kill(pid_sternik, SIGUSR2);

    printf("[POLICJANT] Koniec.\n");
    return 0;
}
