#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>

typedef struct {
    int sockfd;
    struct termios *orig_termios;
} receiver_args;

int connected = 0;
int receiver_terminated = 0;

void *receiver_thread(void *arg) {
    receiver_args *args = (receiver_args *)arg;
    char buffer[16];
    struct sockaddr_in6 from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (1) {
        int n = recvfrom(args->sockfd, buffer, sizeof(buffer)-1, 0,
                         (struct sockaddr*)&from_addr, &from_len);
        if (n < 0) {
            continue;
        }
        buffer[n] = '\0';

        /* Send PONG response to PING */
        if (strcmp(buffer, "PING") == 0) {
            const char *pong = "PONG";
            struct sockaddr_in6 server_addr;
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin6_family = AF_INET6;
            server_addr.sin6_port = htons(53847);
            memcpy(&server_addr.sin6_addr, &from_addr.sin6_addr, sizeof(struct in6_addr));
            sendto(args->sockfd, pong, strlen(pong), 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
            continue;
        }

        /* Check if it's a TERMINATE command from server */
        if (strcmp(buffer, "TERMINATE") == 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, args->orig_termios);
            printf("\n\nServer sent TERMINATE. Client exiting.\n");
            close(args->sockfd);
            receiver_terminated = 1;
            exit(0);
        }

        /* Print received characters */
        for (int i = 0; buffer[i] != '\0'; i++) {
            if (buffer[i] == '\b') {
                printf("\b \b");
            } else {
                putchar(buffer[i]);
            }
        }
        fflush(stdout);
    }
    return NULL;
}

void set_raw_mode(struct termios *orig) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, orig);
    raw = *orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
    int sockfd;
    struct sockaddr_in6 server_addr;
    struct termios orig_termios;
    char c;
    char terminate_buffer[9] = {0};
    char server_ip[256];

    printf("Enter server IPv6 address (e.g., ::1 or fe80::1): ");
    if (fgets(server_ip, sizeof(server_ip), stdin) == NULL) {
        fprintf(stderr, "Failed to read server address\n");
        exit(1);
    }
    // Remove newline
    server_ip[strcspn(server_ip, "\n")] = '\0';

    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_port = htons(53847);
    if (inet_pton(AF_INET6, server_ip, &server_addr.sin6_addr) <= 0) {
        fprintf(stderr, "Invalid IPv6 address: %s\n", server_ip);
        close(sockfd);
        exit(1);
    }

    printf("Connected to server. Type characters (Ctrl+C to exit):\n");

    /* Send LOGIN and wait for response (retry for up to 60 seconds) */
    const char *login_msg = "LOGIN";
    char ack_buffer[64];
    struct sockaddr_in6 from_addr;
    socklen_t from_len = sizeof(from_addr);

    /* Set receive timeout for LOGIN phase */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Connecting to server...\n");
    fflush(stdout);

    for (int attempt = 0; attempt < 12 && !connected; attempt++) {
        printf("\rAttempt %02d: Sending LOGIN...", attempt + 1);
        fflush(stdout);
        sendto(sockfd, login_msg, strlen(login_msg), 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        int n = recvfrom(sockfd, ack_buffer, sizeof(ack_buffer)-1, 0,
                         (struct sockaddr*)&from_addr, &from_len);
        if (n >= 0) {
            connected = 1;
            printf(" Connected!\n");
        } else {
            printf(".");
            fflush(stdout);
        }
    }

    if (!connected) {
        printf(" Failed to connect after 12 attempts (60 seconds).\n");
        close(sockfd);
        return 1;
    }

    /* Clear receive timeout for normal operation */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Start receiver thread */
    pthread_t recv_thread;
    receiver_args args = {sockfd, &orig_termios};
    if (pthread_create(&recv_thread, NULL, receiver_thread, &args) != 0) {
        perror("pthread_create failed");
        close(sockfd);
        return 1;
    }

    // Set terminal to raw mode for per-keystroke input
    set_raw_mode(&orig_termios);

    while (1) {
        if (read(STDIN_FILENO, &c, 1) == 1) {
            // Map backspace key (DEL/127) to '\b'
            if (c == 127) {
                c = '\b';
            }
            // Map carriage return to newline
            if (c == '\r') {
                c = '\n';
            }

            // Send character to server
            sendto(sockfd, &c, 1, 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));

            // Track for TERMINATE sequence using sliding window
            memmove(terminate_buffer, terminate_buffer + 1, 8);
            terminate_buffer[8] = c;
            if (memcmp(terminate_buffer, "TERMINATE", 9) == 0) {
                // Restore terminal mode
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                close(sockfd);
                printf("\n\nClient exiting after TERMINATE.\n");
                return 0;
            }

            // Exit on Ctrl+C (ETX)
            if (c == 3 || receiver_terminated) {
                break;
            }
        }
    }

    // Restore terminal mode
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    close(sockfd);
    printf("\nClient exiting.\n");
    // Join receiver thread
    pthread_join(recv_thread, NULL);
    
    return 0;
}
