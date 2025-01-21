/*******************************************************
 * File: policjant.c
 *
 * Uruchamia się z parametrem PID sternika,
 * wysyła kolejno sygnały:
 *   - SIGUSR1 (zakończenie łodzi1)
 *   - SIGUSR2 (zakończenie łodzi2)
 *******************************************************/

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

    // Wysyłamy od razu SIGUSR1
    printf("[POLICJANT] => SIGUSR1 (zakończ łódź1)\n");
    kill(pid_sternik, SIGUSR1);

    // Wysyłamy od razu SIGUSR2
    printf("[POLICJANT] => SIGUSR2 (zakończ łódź2)\n");
    kill(pid_sternik, SIGUSR2);

    printf("[POLICJANT] Koniec.\n");
    return 0;
}
