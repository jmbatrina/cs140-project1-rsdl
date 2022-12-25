#include "types.h"
#include "user.h"

int main() {
    schedlog(10000);

    // TODO: investigate why pid of first loop proc becomes very high (592)
    // AFTER running usertests.c. This does not happen when compared to Lab 5 baseline
    for (int i = 0; i < 3; i++) {
        if (fork() == 0) {
            char *argv[] = {"test_loop", 0};
            exec("test_loop", argv);
        }
    }

    for (int i = 0; i < 3; i++) {
        wait();
    }

    shutdown();
}
