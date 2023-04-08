#include "types.h"
#include "user.h"

int main() {
    schedlog(5000); // 5000 is arbitrary, enough for running test program.
    
    int dummy = 0;
    if (priofork(0) == 0) {
        for (unsigned int i = 0; i < 4e8; i++) {
            dummy += i;
        }
        exit();
    } 

    wait();

    exit();
}
