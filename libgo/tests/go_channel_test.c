#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "go.h"  // Include your GoChannel implementation header

void *send_to_channel(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    
    for (int i = 0; i < 5; i++) {
        printf("Sending value %d to the channel...\n", i);
        go_channel_send(chan, (void *)(long)i);  // Send integer values as void pointers
        sleep(1);  // Simulate some work
    }

    printf("All values sent! Closing channel...\n");
    go_channel_close(chan);  // Close the channel to signal the end
    return NULL;
}

void *receive_from_channel(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    void *value;

    while (go_channel_receive(chan, &value) == 0) {  // Receive from the channel
        printf("Received value: %ld from the channel\n", (long)value);
    }

    printf("Channel closed, no more values to receive.\n");
    return NULL;
}

int main() {
    printf("Creating a channel...\n");

    // Create a GoChannel with capacity for 3 messages
    GoChannel *chan = go_channel_create(3);

    // Create threads for sending and receiving values
    pthread_t sender_thread, receiver_thread;
    pthread_create(&sender_thread, NULL, send_to_channel, (void *)chan);
    pthread_create(&receiver_thread, NULL, receive_from_channel, (void *)chan);

    // Wait for both threads to finish
    pthread_join(sender_thread, NULL);
    pthread_join(receiver_thread, NULL);

    // Free the channel
    go_channel_free(chan);

    printf("Test complete!\n");
    return 0;
}
