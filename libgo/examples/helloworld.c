#include <stdio.h>
#include "go.h"

void *print_message(void *arg) {
    char *message = (char *)arg;
    printf("%s\n", message);
    return NULL;
}

int main() {
    char *message1 = "Hello from thread 1";
    char *message2 = "Hello from thread 2";

    go(print_message, message1);
    go(print_message, message2);

    // Sleep to allow threads to finish execution
    sleep(1);

    return 0;
}
