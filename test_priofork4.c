#include "types.h"
#include "user.h"
#include "param.h"

#define N 5

int main() {
    schedlog(10000);

    printf(1, "rsdl.h: levels=%d, starting_level=%d, proc_quantum=%d, level_quantum=%d\n",
        RSDL_LEVELS, RSDL_STARTING_LEVEL, RSDL_PROC_QUANTUM, RSDL_LEVEL_QUANTUM);

    unsigned int dummy1 = 0;
    if (priofork(5) == 0) {
        for (unsigned int i = 0; i < 4e7; i++) {
            dummy1 ++;
        }
        printf(1, "dummy1 final value %d\n", dummy1);
        sleep(0);   // sleep on channel 0
        exit();
    }

    unsigned int dummy2 = 0;
    if (priofork(2) == 0) {
        for (unsigned int i = 0; i < 4e8; i++) {
            dummy2 ++;
        }
        printf(1, "dummy2 final value %d\n", dummy2);
        exit();
    }

    unsigned int dummy3 = 0;
    if (priofork(0) == 0) {
        for (unsigned int i = 0; i < 4e7; i++) {
            if (i == 20) {
                if (fork() == 0) {
                    for (unsigned int j = 0; j < 4e8; j++) {
                        dummy3++;
                    }
                    exit();
                }
            }
            dummy3++;
        }
        printf(1, "dummy3 final value %d\n", dummy3);
        kill(7);
        exit();
    }

    wait();
    wait();
    wait();

    shutdown();
}