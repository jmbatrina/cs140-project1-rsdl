#include "types.h"
#include "user.h"

int main() {
    schedlog(10000);

    for (int i = 0; i < 1; i++) {
        if (fork() == 0) {
            char *argv[] = {"usertests", 0};
            exec("usertests", argv);
        }
    }

    for (int i = 0; i < 1; i++) {
        wait();
    }

    shutdown();
}
