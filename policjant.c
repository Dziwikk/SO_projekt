/* File: policjant.c
   Wywołanie: ./policjant <pid_sternika>
   Po paru sekundach wysyła SIGUSR1 i SIGUSR2 do sternika
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

int main(int argc, char *argv[])
{
    if (argc<2) {
        fprintf(stdout, "[POLICJANT] Użycie: %s <pid_sternika>\n", argv[0]);
        return 1;
    }
    pid_t pid_sternik = atoi(argv[1]);
    fprintf(stdout, "[POLICJANT] Cel: sternik PID=%d\n", pid_sternik);

    srand(time(NULL));

    sleep((rand()%5)+2);
    fprintf(stdout, "[POLICJANT] Wysyłam SIGUSR1 -> łódź1\n");
    kill(pid_sternik, SIGUSR1);

    sleep((rand()%5)+2);
    fprintf(stdout, "[POLICJANT] Wysyłam SIGUSR2 -> łódź2\n");
    kill(pid_sternik, SIGUSR2);

    fprintf(stdout, "[POLICJANT] Koniec.\n");
    return 0;
}
