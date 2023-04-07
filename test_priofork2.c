#include "types.h"
#include "user.h"
#include "param.h"

#define N 5

int main() {
    schedlog(1000);

    printf(1, "rsdl.h: levels=%d, starting_level=%d, proc_quantum=%d, level_quantum=%d\n",
        RSDL_LEVELS, RSDL_STARTING_LEVEL, RSDL_PROC_QUANTUM, RSDL_LEVEL_QUANTUM);

    int priolevels[N] = {0, 1, 0, 1, 3};

    for (int i = 0; i < N; i++) {
        if (priofork(priolevels[i]) == 0) {
            char *argv[] = {"ls", 0};
            exec("ls", argv);
        }
    }

    for (int i = 0; i < N; i++) {
        wait();
    }

    shutdown();
}