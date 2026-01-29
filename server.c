#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>

#define INTERVAL_NS (16666667L)

typedef struct connection_info {
    struct sockaddr_in6 addr;
    socklen_t addr_len;
    short active;
} conn_info;

conn_info subscribers[512];

typedef struct command_queue {
    char commands[512][16];
    int head;
    int tail;
} cmd_q;

cmd_q qA;
cmd_q qB;

cmd_q * receiver_q;
cmd_q * consumer_q;

cmd_q qC;
cmd_q qD;

cmd_q * sender_q;
cmd_q * producer_q;

static pthread_mutex_t recv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cons_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t prod_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t recv_swap_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t send_swap_cond = PTHREAD_COND_INITIALIZER;
static int receiver_terminated = 0;
static int swapper_terminated = 0;
static int producer_terminated = 0;
static char terminate_buffer[9] = {0};

void swap_queues() {
    /* Swap receiver/consumer queues */
    pthread_mutex_lock(&recv_mutex);
    pthread_mutex_lock(&cons_mutex);

    cmd_q *temp = receiver_q;
    receiver_q = consumer_q;
    consumer_q = temp;

    pthread_cond_broadcast(&recv_swap_cond);

    pthread_mutex_unlock(&cons_mutex);
    pthread_mutex_unlock(&recv_mutex);

    /* Swap producer/sender queues */
    pthread_mutex_lock(&prod_mutex);
    pthread_mutex_lock(&send_mutex);

    cmd_q *temp2 = producer_q;
    producer_q = sender_q;
    sender_q = temp2;

    pthread_cond_broadcast(&send_swap_cond);

    pthread_mutex_unlock(&send_mutex);
    pthread_mutex_unlock(&prod_mutex);
}

void init_queue(cmd_q *q) {
    q->head = 0;
    q->tail = 0;
}

int enqueue(cmd_q *q, const char *cmd, pthread_mutex_t *mutex) {
    int rc = 0;
    pthread_mutex_lock(mutex);
    if (((q->tail + 1) & 511) == q->head) {
        rc = -1;
    } else {
        strncpy(q->commands[q->tail], cmd, 15);
        q->commands[q->tail][15] = '\0';
        q->tail = (q->tail + 1) & 511;
    }
    pthread_mutex_unlock(mutex);
    return rc;
}

/* dequeue_locked: assumes caller holds appropriate mutex */
static int dequeue_locked(cmd_q *q, char *cmd) {
    if (q->head == q->tail) {
        return -1;
    }
    strncpy(cmd, q->commands[q->head], 16);
    q->head = (q->head + 1) & 511;
    return 0;
}

void receiver() {
    int sockfd;
    struct sockaddr_in6 server_addr, client_addr;
    char buffer[16];
    socklen_t addr_len = sizeof(client_addr);

    // 1. create socket
    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    // 2. bind to port 53847 (IPv6)
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(53847);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(1);
    }

    printf("UDP server listening on port 53847...\n");
    fflush(stdout);

    // 3. receive characters
    while (1) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0,
                         (struct sockaddr*)&client_addr, &addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        if (strcmp(buffer, "LOGIN") == 0) {
            /* Register new subscriber if not already present */
            int found = 0;
            for (int i = 0; i < 512; i++) {
                if (subscribers[i].active &&
                    memcmp(&subscribers[i].addr, &client_addr, sizeof(client_addr)) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (int i = 0; i < 512; i++) {
                    if (!subscribers[i].active) {
                        memcpy(&subscribers[i].addr, &client_addr, sizeof(client_addr));
                        subscribers[i].addr_len = addr_len;
                        subscribers[i].active = 1;
                        printf("New subscriber added.\n");
                        fflush(stdout);
                        /* Send acknowledgment to client */
                        const char *ack = "OK";
                        sendto(sockfd, ack, strlen(ack), 0,
                               (struct sockaddr*)&client_addr, addr_len);
                        break;
                    }
                }
            } else {
                /* Already registered, still send acknowledgment */
                const char *ack = "OK";
                sendto(sockfd, ack, strlen(ack), 0,
                       (struct sockaddr*)&client_addr, addr_len);
            }
            continue;
        }
        buffer[n] = '\0';

        enqueue(receiver_q, buffer, &recv_mutex);

        /* Check for TERMINATE sequence in the buffer using sliding window */
        for (int i = 0; i < n; i++) {
            memmove(terminate_buffer, terminate_buffer + 1, 8);
            terminate_buffer[8] = buffer[i];
            if (memcmp(terminate_buffer, "TERMINATE", 9) == 0) {
                printf("\n\nTERMINATE sequence received; server exiting.\n");
                fflush(stdout);
                /* mark receiver terminated and wake other threads */
                pthread_mutex_lock(&recv_mutex);
                receiver_terminated = 1;
                pthread_mutex_unlock(&recv_mutex);
                close(sockfd);
                printf("Receiver terminating...\n");
                fflush(stdout);
                return;
            }
        }
    }

    close(sockfd);
    return;
}

void swapper() {
    struct timespec next_frame, current;
    
    // Get current time
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (1) {
        swap_queues();

        /* After performing a swap, check whether receiver has terminated
         * and both queues are empty. If so, signal consumer and exit.
         */
        pthread_mutex_lock(&recv_mutex);
        int recv_empty = (receiver_q->head == receiver_q->tail);
        if (receiver_terminated && recv_empty) {
            /* No more data will be produced by receiver for its current queue */
            pthread_mutex_lock(&cons_mutex);
            swapper_terminated = 1;
            pthread_cond_broadcast(&recv_swap_cond);
            pthread_mutex_unlock(&cons_mutex);
            pthread_mutex_lock(&send_mutex);
            pthread_cond_broadcast(&send_swap_cond);
            pthread_mutex_unlock(&send_mutex);
            pthread_mutex_unlock(&recv_mutex);
            printf("Swapper terminating...\n");
            fflush(stdout);
            return;
        }
        pthread_mutex_unlock(&recv_mutex);

        // Calculate next frame time
        next_frame.tv_nsec += INTERVAL_NS;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }

        // Wait until the next frame time
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }
    return;
}

void sender() {
    int sockfd;
    char cmd[16];

    /* Create socket for sending */
    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("sender socket failed");
        exit(1);
    }

    while (1) {
        /* Wait until sender_q has data */
        pthread_mutex_lock(&send_mutex);
        while (sender_q->head == sender_q->tail) {
            /* Check if producer terminated and sender queue is empty */
            pthread_mutex_lock(&prod_mutex);
            int prod_term = producer_terminated;
            int prod_empty = (producer_q->head == producer_q->tail);
            pthread_mutex_unlock(&prod_mutex);
            
            if (prod_term) {
                if(prod_empty) {
                    swap_queues();
                }
                else {
                    printf("Sender detected producer termination and empty queue.\n");
                    pthread_mutex_unlock(&send_mutex);
                    /* Broadcast TERMINATE command to all subscribers */
                    const char *terminate = "TERMINATE";
                    for (int i = 0; i < 512; i++) {
                        if (subscribers[i].active) {
                            sendto(sockfd, terminate, strlen(terminate), 0,
                                (struct sockaddr*)&subscribers[i].addr,
                                subscribers[i].addr_len);
                        }
                    }
                    printf("All TERMINATE commands sent to subscribers.\n");
                    close(sockfd);
                    
                    printf("Sender terminating...\n");
                    fflush(stdout);
                    return;
                }
            }
            pthread_cond_wait(&send_swap_cond, &send_mutex);
        }
        /* Dequeue all available items while holding send_mutex. */
        while (dequeue_locked(sender_q, cmd) == 0) {
            pthread_mutex_unlock(&send_mutex);
            /* Broadcast to all active subscribers */
            for (int i = 0; i < 512; i++) {
                if (subscribers[i].active) {
                    sendto(sockfd, cmd, strlen(cmd), 0,
                           (struct sockaddr*)&subscribers[i].addr,
                           subscribers[i].addr_len);
                }
            }
            pthread_mutex_lock(&send_mutex);
        }
        pthread_mutex_unlock(&send_mutex);
    }
    return;
}

void consumer() {
    char cmd[16];
    while (1) {
        /* Wait until consumer_q has data */
        pthread_mutex_lock(&cons_mutex);
        while (consumer_q->head == consumer_q->tail) {
            /* If swapper has terminated and there is nothing to do, exit */
            if (swapper_terminated) {
                pthread_mutex_unlock(&cons_mutex);
                /* Signal producer terminated and wake sender */
                pthread_mutex_lock(&send_mutex);
                pthread_mutex_lock(&prod_mutex);
                producer_terminated = 1;
                pthread_mutex_unlock(&prod_mutex);
                pthread_cond_broadcast(&send_swap_cond);
                pthread_mutex_unlock(&send_mutex);
                printf("Consumer terminating...\n");
                fflush(stdout);
                return;
            }
            pthread_cond_wait(&recv_swap_cond, &cons_mutex);
        }
        /* Dequeue all available items while holding cons_mutex. */
        while (dequeue_locked(consumer_q, cmd) == 0) {
            pthread_mutex_unlock(&cons_mutex);
            /* Print each character to stdout */
            /* Handle backspace: move back, print space, move back again */
            for (int i = 0; cmd[i] != '\0'; i++) {
                if (cmd[i] == '\b') {
                    printf("\b \b");
                } else {
                    putchar(cmd[i]);
                }
            }
            fflush(stdout);
            /* Enqueue to producer_q for broadcasting */
            enqueue(producer_q, cmd, &prod_mutex);
            pthread_mutex_lock(&cons_mutex);
        }
        pthread_mutex_unlock(&cons_mutex);
    }
    return;
}

int main() {
    for(int i = 0; i < 512; i++) {
        subscribers[i].active = 0;
    }
    init_queue(&qA);
    init_queue(&qB);
    init_queue(&qC);
    init_queue(&qD);
    receiver_q = &qA;
    consumer_q = &qB;
    producer_q = &qC;
    sender_q = &qD;

    pthread_t recv_thread, swap_thread, cons_thread, send_thread;

    if (pthread_create(&recv_thread, NULL, (void *(*)(void *))receiver, NULL) != 0) {
        perror("pthread_create receiver");
        return 1;
    }
    if (pthread_create(&swap_thread, NULL, (void *(*)(void *))swapper, NULL) != 0) {
        perror("pthread_create swapper");
        return 1;
    }
    if (pthread_create(&cons_thread, NULL, (void *(*)(void *))consumer, NULL) != 0) {
        perror("pthread_create consumer");
        return 1;
    }
    if (pthread_create(&send_thread, NULL, (void *(*)(void *))sender, NULL) != 0) {
        perror("pthread_create sender");
        return 1;
    }

    /* Join threads */
    pthread_join(recv_thread, NULL);
    pthread_join(swap_thread, NULL);
    pthread_join(cons_thread, NULL);
    pthread_join(send_thread, NULL);

    printf("\nServer terminated.\n");

    return 0;
}
