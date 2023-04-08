#include "types.h"
#include "user.h"
#include "param.h"

int main() {
    schedlog(5000); // 5000 is arbitrary, enough for running test program.

    printf(1, "rsdl.h: levels=%d, starting_level=%d, proc_quantum=%d, level_quantum=%d\n",
        RSDL_LEVELS, RSDL_STARTING_LEVEL, RSDL_PROC_QUANTUM, RSDL_LEVEL_QUANTUM);

    for (int i = 0; i < 10; i++) {
        if (priofork(i) == 0) {
            char *argv[] = {"test_loop", 0};
            exec("test_loop", argv);
        }
    }

    for (int i = 0; i < 3; i++) {
        wait();
    }

    shutdown();
}