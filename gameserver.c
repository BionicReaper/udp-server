#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>
#include <netdb.h>
#include <errno.h>
#include "game.h"

#define INTERVAL_NS (16666667L)  // 60 FPS
#define MAX_CMD_SIZE 256
#define SHOOT_COOLDOWN 4.0  // 4 seconds between shots

#define STUN_SERVER_ADDRESS "stun.l.google.com"
#define STUN_SERVER_PORT 19302

// Keep onboarding chunks below typical MTU to avoid IPv6/UDP fragmentation issues.
// 1200 is conservative and generally safe across LAN/Wi-Fi.
#define ONBOARDING_CHUNK_SIZE 1200

// Query STUN server to discover public IP:port (returns 1 on success, 0 on failure)
int query_stun_server(const char *stun_host, int stun_port, int sockfd, char *public_ip, int ip_size, int *public_port) {
    // Build STUN Binding Request
    uint8_t request[20];
    uint16_t msg_type = htons(0x0001);       // Binding Request
    uint16_t msg_len = htons(0);             // No attributes
    uint32_t magic_cookie = htonl(0x2112A442);
    uint8_t transaction_id[12];
    for (int i = 0; i < 12; i++) transaction_id[i] = rand() & 0xFF;
    
    memcpy(request, &msg_type, 2);
    memcpy(request + 2, &msg_len, 2);
    memcpy(request + 4, &magic_cookie, 4);
    memcpy(request + 8, transaction_id, 12);
    
    // Resolve STUN server
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", stun_port);
    
    if (getaddrinfo(stun_host, port_str, &hints, &res) != 0) {
        printf("STUN: Failed to resolve STUN server %s\n", stun_host);
        return 0;
    }
    
    // Send request to first available address
    // Try IPv6 first, then IPv4 with automatic mapping via IPV6_V6ONLY=0
    int sent = 0;
    struct addrinfo *ipv6_addr = NULL;
    struct addrinfo *ipv4_addr = NULL;
    
    // Scan for IPv6 and IPv4 addresses
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET6 && !ipv6_addr) {
            ipv6_addr = rp;
            printf("STUN: Found IPv6 address for %s\n", stun_host);
        } else if (rp->ai_family == AF_INET && !ipv4_addr) {
            ipv4_addr = rp;
            char ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
            printf("STUN: Found IPv4 address %s for %s\n", ip_str, stun_host);
        }
    }
    
    // Try IPv6 first
    if (ipv6_addr) {
        printf("STUN: Trying IPv6 address...\n");
        if (sendto(sockfd, request, sizeof(request), 0, ipv6_addr->ai_addr, ipv6_addr->ai_addrlen) >= 0) {
            printf("STUN: Request sent via IPv6\n");
            sent = 1;
        } else {
            printf("STUN: Failed to send via IPv6: %s\n", strerror(errno));
        }
    }
    
    // If IPv6 failed or not available, try IPv4 with automatic mapping
    if (!sent && ipv4_addr) {
        printf("STUN: Trying IPv4-mapped IPv6 address...\n");
        struct sockaddr_in *sin4 = (struct sockaddr_in *)ipv4_addr->ai_addr;
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = sin4->sin_port;
        // Create IPv4-mapped IPv6 address: ::ffff:x.x.x.x
        sin6.sin6_addr.s6_addr[10] = 0xff;
        sin6.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&sin6.sin6_addr.s6_addr[12], &sin4->sin_addr, 4);
        if (sendto(sockfd, request, sizeof(request), 0, (struct sockaddr *)&sin6, sizeof(sin6)) >= 0) {
            printf("STUN: Request sent via IPv4-mapped IPv6\n");
            sent = 1;
        } else {
            printf("STUN: Failed to send via IPv4-mapped: %s\n", strerror(errno));
        }
    }
    
    freeaddrinfo(res);
    
    if (!sent) {
        printf("STUN: Failed to send request (no working address family)\n");
        return 0;
    }
    
    // Save original socket timeout
    struct timeval tv_orig;
    socklen_t tv_len = sizeof(tv_orig);
    getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_orig, &tv_len);
    
    // Set receive timeout for STUN query
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    uint8_t buffer[2048];
    struct sockaddr_in6 from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &fromlen);
    
    // Restore original socket timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_orig, sizeof(tv_orig));
    
    if (n < 20) {
        if (n < 0) {
            printf("STUN: No response received (timeout or error: %s)\n", strerror(errno));
        } else {
            printf("STUN: Response too short (%zd bytes, expected at least 20)\n", n);
        }
        return 0;
    }
    
    printf("STUN: Received %zd byte response\n", n);
    
    // Parse STUN response
    uint16_t resp_type = ntohs(*(uint16_t *)(buffer));
    uint16_t resp_len = ntohs(*(uint16_t *)(buffer + 2));
    uint32_t resp_cookie = ntohl(*(uint32_t *)(buffer + 4));
    
    if (resp_cookie != 0x2112A442) {
        printf("STUN: Invalid magic cookie in response (0x%08x, expected 0x2112A442)\n", resp_cookie);
        return 0;
    }
    
    // Parse attributes
    int offset = 20;
    while (offset + 4 <= n && offset < 20 + resp_len) {
        uint16_t attr_type = ntohs(*(uint16_t *)(buffer + offset));
        uint16_t attr_len = ntohs(*(uint16_t *)(buffer + offset + 2));
        offset += 4;
        
        if (offset + attr_len > n) break;
        
        // XOR-MAPPED-ADDRESS (0x0020) or MAPPED-ADDRESS (0x0001)
        if ((attr_type == 0x0020 || attr_type == 0x0001) && attr_len >= 8) {
            uint8_t family = buffer[offset + 1];
            if (family == 0x01) {  // IPv4
                uint16_t port = (buffer[offset + 2] << 8) | buffer[offset + 3];
                uint32_t addr = (buffer[offset + 4] << 24) | (buffer[offset + 5] << 16) |
                               (buffer[offset + 6] << 8) | buffer[offset + 7];
                
                if (attr_type == 0x0020) {  // XOR-MAPPED
                    port ^= (0x2112A442 >> 16);
                    addr ^= 0x2112A442;
                }
                
                struct in_addr ina;
                ina.s_addr = htonl(addr);
                inet_ntop(AF_INET, &ina, public_ip, ip_size);
                *public_port = port;
                printf("STUN: Successfully discovered public address\n");
                return 1;
            }
        }
        
        offset += attr_len;
        if (attr_len % 4) offset += 4 - (attr_len % 4);
    }
    
    printf("STUN: No mapped address found in response (parsed %d bytes of attributes)\n", offset - 20);
    return 0;
}

typedef struct connection_info {
    struct sockaddr_in6 addr;
    socklen_t addr_len;
    short active;
    short pinged;
} conn_info;

conn_info subscribers[512];

// Player-subscriber mapping for O(1) lookup
PlayerConnection playerConnections[16];

typedef struct pong_response {
    struct sockaddr_in6 addr;
    socklen_t addr_len;
} pong_resp;

typedef struct pinger_queue {
    pong_resp responses[512];
    int head;
    int tail;
} pinger_q;

pinger_q ping_queue;

// Command queue now stores binary data with sender info
typedef struct {
    unsigned char data[MAX_CMD_SIZE];
    int length;
    struct sockaddr_in6 sender_addr;
    socklen_t sender_addr_len;
} cmd_entry;

typedef struct command_queue {
    cmd_entry commands[512];
    int head;
    int tail;
} cmd_q;

cmd_q qA;
cmd_q qB;

cmd_q * receiver_q;
cmd_q * consumer_q;

// Output queue for sending responses
typedef struct {
    unsigned char data[MAX_CMD_SIZE];
    int length;
    short target_subscriber;  // -1 for broadcast, -2 for broadcast except one, >= 0 for specific
    short exclude_subscriber; // Used when target_subscriber == -2
} out_entry;

typedef struct output_queue {
    out_entry entries[512];
    int head;
    int tail;
} out_q;

out_q qC;
out_q qD;

out_q * sender_q;
out_q * producer_q;

static pthread_mutex_t recv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cons_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t prod_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t recv_swap_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t send_swap_cond = PTHREAD_COND_INITIALIZER;
static int receiver_terminated = 0;
static int swapper_terminated = 0;
static int producer_terminated = 0;
static pthread_t pinger_thread_id;
static pthread_t stdin_thread_id;
static int server_sockfd;  // Global socket for sending

// Game timing
static double game_time = 0.0;

static void send_onboarding_chunked(const struct sockaddr_in6 *client_addr, socklen_t addr_len, short player_id) {
    CmdOnboarding onboard;
    onboard.assigned_playerID = player_id;
    memcpy(onboard.players, players, sizeof(players));
    memcpy(&onboard.projectileQueue, &projectileQueue, sizeof(projectileQueue));

    const uint8_t *bytes = (const uint8_t *)&onboard;
    const uint32_t total = (uint32_t)sizeof(onboard);

    unsigned char begin_msg[1 + sizeof(CmdOnboardingBegin)];
    begin_msg[0] = CMD_ONBOARDING_BEGIN;
    CmdOnboardingBegin *begin = (CmdOnboardingBegin *)(begin_msg + 1);
    begin->assigned_playerID = player_id;
    begin->total_size = total;
    begin->chunk_size = ONBOARDING_CHUNK_SIZE;
    sendto(server_sockfd, begin_msg, sizeof(begin_msg), 0,
        (struct sockaddr *)client_addr, addr_len);

    uint32_t offset = 0;
    while (offset < total) {
     uint16_t len = (uint16_t)((total - offset > ONBOARDING_CHUNK_SIZE) ? ONBOARDING_CHUNK_SIZE : (total - offset));
     unsigned char chunk_msg[1 + sizeof(CmdOnboardingChunkHeader) + ONBOARDING_CHUNK_SIZE];
     chunk_msg[0] = CMD_ONBOARDING_CHUNK;
     CmdOnboardingChunkHeader *hdr = (CmdOnboardingChunkHeader *)(chunk_msg + 1);
     hdr->offset = offset;
     hdr->data_len = len;
     memcpy(chunk_msg + 1 + sizeof(CmdOnboardingChunkHeader), bytes + offset, len);
     sendto(server_sockfd, chunk_msg, 1 + sizeof(CmdOnboardingChunkHeader) + len, 0,
         (struct sockaddr *)client_addr, addr_len);
     offset += len;
    }

    // Optional end marker (not strictly required, client can complete on total bytes)
    unsigned char end_msg[1];
    end_msg[0] = CMD_ONBOARDING_END;
    sendto(server_sockfd, end_msg, sizeof(end_msg), 0,
        (struct sockaddr *)client_addr, addr_len);
}

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

    out_q *temp2 = producer_q;
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

void init_out_queue(out_q *q) {
    q->head = 0;
    q->tail = 0;
}

void init_pinger_queue(pinger_q *q) {
    q->head = 0;
    q->tail = 0;
}

int enqueue_pong(pinger_q *q, const struct sockaddr_in6 *addr, socklen_t addr_len) {
    int rc = 0;
    pthread_mutex_lock(&ping_mutex);
    if (((q->tail + 1) & 511) == q->head) {
        rc = -1;
    } else {
        memcpy(&q->responses[q->tail].addr, addr, sizeof(struct sockaddr_in6));
        q->responses[q->tail].addr_len = addr_len;
        q->tail = (q->tail + 1) & 511;
    }
    pthread_mutex_unlock(&ping_mutex);
    return rc;
}

int dequeue_pong(pinger_q *q, pong_resp * resp) {
    int rc = 0;
    pthread_mutex_lock(&ping_mutex);
    if (q->head == q->tail) {
        rc = -1;
    } else {
        memcpy(&resp->addr, &q->responses[q->head].addr, sizeof(struct sockaddr_in6));
        resp->addr_len = q->responses[q->head].addr_len;
        q->head = (q->head + 1) & 511;
    }
    pthread_mutex_unlock(&ping_mutex);
    return rc;
}

int enqueue_cmd(cmd_q *q, const unsigned char *data, int length, 
                const struct sockaddr_in6 *sender_addr, socklen_t sender_addr_len,
                pthread_mutex_t *mutex) {
    int rc = 0;
    pthread_mutex_lock(mutex);
    if (((q->tail + 1) & 511) == q->head) {
        rc = -1;
    } else {
        memcpy(q->commands[q->tail].data, data, length);
        q->commands[q->tail].length = length;
        memcpy(&q->commands[q->tail].sender_addr, sender_addr, sizeof(struct sockaddr_in6));
        q->commands[q->tail].sender_addr_len = sender_addr_len;
        q->tail = (q->tail + 1) & 511;
    }
    pthread_mutex_unlock(mutex);
    return rc;
}

int enqueue_out(out_q *q, const unsigned char *data, int length, 
                short target_subscriber, short exclude_subscriber,
                pthread_mutex_t *mutex) {
    int rc = 0;
    pthread_mutex_lock(mutex);
    if (((q->tail + 1) & 511) == q->head) {
        rc = -1;
    } else {
        memcpy(q->entries[q->tail].data, data, length);
        q->entries[q->tail].length = length;
        q->entries[q->tail].target_subscriber = target_subscriber;
        q->entries[q->tail].exclude_subscriber = exclude_subscriber;
        q->tail = (q->tail + 1) & 511;
    }
    pthread_mutex_unlock(mutex);
    return rc;
}

/* dequeue_locked: assumes caller holds appropriate mutex */
static int dequeue_cmd_locked(cmd_q *q, cmd_entry *entry) {
    if (q->head == q->tail) {
        return -1;
    }
    memcpy(entry, &q->commands[q->head], sizeof(cmd_entry));
    q->head = (q->head + 1) & 511;
    return 0;
}

static int dequeue_out_locked(out_q *q, out_entry *entry) {
    if (q->head == q->tail) {
        return -1;
    }
    memcpy(entry, &q->entries[q->head], sizeof(out_entry));
    q->head = (q->head + 1) & 511;
    return 0;
}

// Find subscriber index by address
short find_subscriber(const struct sockaddr_in6 *addr) {
    for (short i = 0; i < 512; i++) {
        if (subscribers[i].active &&
            memcmp(&subscribers[i].addr, addr, sizeof(struct sockaddr_in6)) == 0) {
            return i;
        }
    }
    return -1;
}

// Find player ID by subscriber index
short find_player_by_subscriber(short subscriber_index) {
    for (short i = 0; i < 16; i++) {
        if (playerConnections[i].active && 
            playerConnections[i].subscriber_index == subscriber_index) {
            return i;
        }
    }
    return -1;
}

// Broadcast to all or specific subscribers
void broadcast_message(const unsigned char *data, int length, short target, short exclude) {
    if (target >= 0) {
        // Send to specific subscriber
        if (subscribers[target].active) {
            sendto(server_sockfd, data, length, 0,
                   (struct sockaddr*)&subscribers[target].addr,
                   subscribers[target].addr_len);
        }
    } else if (target == -1) {
        // Broadcast to all
        for (int i = 0; i < 512; i++) {
            if (subscribers[i].active) {
                sendto(server_sockfd, data, length, 0,
                       (struct sockaddr*)&subscribers[i].addr,
                       subscribers[i].addr_len);
            }
        }
    } else if (target == -2) {
        // Broadcast to all except one
        for (int i = 0; i < 512; i++) {
            if (subscribers[i].active && i != exclude) {
                sendto(server_sockfd, data, length, 0,
                       (struct sockaddr*)&subscribers[i].addr,
                       subscribers[i].addr_len);
            }
        }
    }
}

// Handle CMD_LOGIN
void handle_login(const struct sockaddr_in6 *client_addr, socklen_t addr_len) {
    short subscriber_idx = find_subscriber(client_addr);
    
    // Check if already connected
    if (subscriber_idx >= 0) {
        short player_id = find_player_by_subscriber(subscriber_idx);
        if (player_id >= 0) {
            // Already logged in, resend onboarding (chunked)
            send_onboarding_chunked(client_addr, addr_len, player_id);
            return;
        }
    }
    
    // Find available player slot
    short player_id = -1;
    for (short i = 0; i < 16; i++) {
        if (!playerConnections[i].active) {
            player_id = i;
            break;
        }
    }
    
    if (player_id < 0) {
        // Server full
        unsigned char response[1];
        response[0] = CMD_LOGIN_DENIED;
        sendto(server_sockfd, response, 1, 0,
               (struct sockaddr*)client_addr, addr_len);
        return;
    }
    
    // Register subscriber if new
    if (subscriber_idx < 0) {
        for (short i = 0; i < 512; i++) {
            if (!subscribers[i].active) {
                memcpy(&subscribers[i].addr, client_addr, sizeof(struct sockaddr_in6));
                subscribers[i].addr_len = addr_len;
                subscribers[i].active = 1;
                subscribers[i].pinged = 0;
                subscriber_idx = i;
                break;
            }
        }
    }
    
    if (subscriber_idx < 0) {
        // No subscriber slots
        unsigned char response[1];
        response[0] = CMD_LOGIN_DENIED;
        sendto(server_sockfd, response, 1, 0,
               (struct sockaddr*)client_addr, addr_len);
        return;
    }
    
    // Initialize player
    playerConnections[player_id].subscriber_index = subscriber_idx;
    playerConnections[player_id].active = 1;
    playerConnections[player_id].last_shoot_time = -SHOOT_COOLDOWN;  // Allow immediate first shot
    playerConnections[player_id].forward = 0;
    playerConnections[player_id].right = 0;
    playerConnections[player_id].up = 0;
    playerConnections[player_id].rotation_direction = 0;
    
    // Setup player game state
    players[player_id].cuboid = (Cuboid){
        .position = {player_id * 5.0, 0.0, 0.0},  // Spread players apart
        .width = 2.0,
        .height = 2.0,
        .depth = 2.0,
        .rotation_y = 0.0,
        .color = {0, 255, 0}
    };
    players[player_id].gun = (Gun){
        .position = players[player_id].cuboid.position,
        .length = 4.0,
        .rotation_y = 0.0,
        .color = {255, 0, 0}
    };
    players[player_id].gun.position.y -= players[player_id].cuboid.height / 4.0;
    players[player_id].hp = 5;
    
    printf("Player %d logged in (subscriber %d)\n", player_id, subscriber_idx);
    fflush(stdout);
    
    // Send onboarding to new player (chunked)
    send_onboarding_chunked(client_addr, addr_len, player_id);
    
    // Broadcast new player to others
    unsigned char broadcast[1 + sizeof(CmdNewPlayer)];
    broadcast[0] = CMD_NEW_PLAYER;
    CmdNewPlayer *newPlayer = (CmdNewPlayer*)(broadcast + 1);
    newPlayer->playerID = player_id;
    newPlayer->player = players[player_id];
    enqueue_out(producer_q, broadcast, sizeof(broadcast), -2, subscriber_idx, &prod_mutex);
}

// Handle CMD_MOVE_ROTATE
void handle_move_rotate(short player_id, const CmdMoveRotate *cmd) {
    if (player_id < 0 || player_id >= 16 || !playerConnections[player_id].active) return;
    
    playerConnections[player_id].forward = cmd->forward;
    playerConnections[player_id].right = cmd->right;
    playerConnections[player_id].up = cmd->up;
    playerConnections[player_id].rotation_direction = cmd->rotation_direction;
    
    // Broadcast move executed to all players
    unsigned char response[1 + sizeof(CmdMoveExecuted)];
    response[0] = CMD_MOVE_EXECUTED;
    CmdMoveExecuted *exec = (CmdMoveExecuted*)(response + 1);
    exec->playerID = player_id;
    exec->position = players[player_id].cuboid.position;
    exec->rotation_y = players[player_id].cuboid.rotation_y;
    exec->forward = cmd->forward;
    exec->right = cmd->right;
    exec->up = cmd->up;
    exec->rotation_direction = cmd->rotation_direction;
    enqueue_out(producer_q, response, sizeof(response), -1, -1, &prod_mutex);
}

// Handle CMD_SHOOT
void handle_shoot(short player_id) {
    if (player_id < 0 || player_id >= 16 || !playerConnections[player_id].active) return;
    if (players[player_id].hp <= 0) return;
    
    // Check cooldown
    if (game_time - playerConnections[player_id].last_shoot_time < SHOOT_COOLDOWN) {
        return;  // Still on cooldown
    }
    
    playerConnections[player_id].last_shoot_time = game_time;
    shootProjectile(player_id, &projectileQueue);
    
    // Broadcast shoot executed to all players
    unsigned char response[1 + sizeof(CmdShootExecuted)];
    response[0] = CMD_SHOOT_EXECUTED;
    CmdShootExecuted *exec = (CmdShootExecuted*)(response + 1);
    exec->playerID = player_id;
    exec->gun_position = players[player_id].gun.position;
    exec->gun_rotation_y = players[player_id].gun.rotation_y;
    enqueue_out(producer_q, response, sizeof(response), -1, -1, &prod_mutex);
}

// Kill a player (disconnect or 0 hp)
void kill_player(short player_id) {
    if (player_id < 0 || player_id >= 16) return;
    
    players[player_id].hp = 0;
    
    // Broadcast kill to all players
    unsigned char response[1 + sizeof(CmdPlayerKilled)];
    response[0] = CMD_PLAYER_KILLED;
    CmdPlayerKilled *kill = (CmdPlayerKilled*)(response + 1);
    kill->playerID = player_id;
    enqueue_out(producer_q, response, sizeof(response), -1, -1, &prod_mutex);
}

void receiver() {
    struct sockaddr_in6 server_addr, client_addr;
    unsigned char buffer[MAX_CMD_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    // Create socket
    server_sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (server_sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    // Allow IPv4-mapped IPv6 addresses (accept both IPv4 and IPv6 connections)
    int no = 0;
    if (setsockopt(server_sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no)) < 0) {
        perror("setsockopt IPV6_V6ONLY failed");
        close(server_sockfd);
        exit(1);
    }

    // Bind to port 53847 (IPv6)
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(53847);

    if (bind(server_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_sockfd);
        exit(1);
    }

    // Query STUN server to get public IP:port
    char public_ip[INET_ADDRSTRLEN];
    int public_port = 0;
    struct sockaddr_in6 local_addr;
    socklen_t local_len = sizeof(local_addr);
    getsockname(server_sockfd, (struct sockaddr *)&local_addr, &local_len);
    int local_port = ntohs(local_addr.sin6_port);
    
    printf("Local port: %d\n", local_port);
    
    if (query_stun_server(STUN_SERVER_ADDRESS, STUN_SERVER_PORT, server_sockfd, public_ip, sizeof(public_ip), &public_port)) {
        printf("Public endpoint: %s:%d\n", public_ip, public_port);
    } else {
        printf("Failed to query STUN server\n");
    }
    
    printf("Game server listening on port 53847...\n");
    fflush(stdout);

    while (1) {
        int n = recvfrom(server_sockfd, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&client_addr, &addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        if (n < 1) continue;  // Need at least command byte

        unsigned char cmd_code = buffer[0];

        // Log received command with IP (skip PONG and MOVE_ROTATE for less noise)
        char ip_str[INET6_ADDRSTRLEN];
        if (client_addr.sin6_family == AF_INET6) {
            // Check if it's an IPv4-mapped IPv6 address
            if (IN6_IS_ADDR_V4MAPPED(&client_addr.sin6_addr)) {
                // Extract IPv4 address from IPv4-mapped IPv6
                struct in_addr ipv4_addr;
                memcpy(&ipv4_addr, &client_addr.sin6_addr.s6_addr[12], 4);
                inet_ntop(AF_INET, &ipv4_addr, ip_str, sizeof(ip_str));
            } else {
                inet_ntop(AF_INET6, &client_addr.sin6_addr, ip_str, sizeof(ip_str));
            }
        } else {
            snprintf(ip_str, sizeof(ip_str), "unknown");
        }
        printf("Received CMD %d from %s:%d (%d bytes)\n", 
                cmd_code, ip_str, ntohs(client_addr.sin6_port), n);
        fflush(stdout);
        
        // Handle PING/PONG/TERMINATE immediately in receiver
        if (cmd_code == CMD_PONG) {
            enqueue_pong(&ping_queue, &client_addr, addr_len);
            continue;
        }
        
        if (cmd_code == CMD_TERMINATE) {
            printf("\n\nTERMINATE received; server exiting.\n");
            fflush(stdout);
            pthread_mutex_lock(&recv_mutex);
            receiver_terminated = 1;
            pthread_mutex_unlock(&recv_mutex);
            pthread_cancel(pinger_thread_id);
            close(server_sockfd);
            printf("Receiver terminating...\n");
            fflush(stdout);
            return;
        }

        // Handle LOGIN immediately (needs socket access)
        if (cmd_code == CMD_LOGIN) {
            handle_login(&client_addr, addr_len);
            continue;
        }

        // Queue other commands for consumer
        enqueue_cmd(receiver_q, buffer, n, &client_addr, addr_len, &recv_mutex);
    }

    close(server_sockfd);
}

// Collision callback - broadcasts CMD_PROJECTILE_HIT to all subscribers
void on_projectile_collision(short proj_index, short hit_player) {
    unsigned char response[1 + sizeof(CmdProjectileHit)];
    response[0] = CMD_PROJECTILE_HIT;
    CmdProjectileHit *hit = (CmdProjectileHit*)(response + 1);
    hit->projectile_index = proj_index;
    hit->hit_playerID = hit_player;
    enqueue_out(producer_q, response, sizeof(response), -1, -1, &prod_mutex);
}

void swapper() {
    struct timespec next_frame, current, prev_frame;
    
    clock_gettime(CLOCK_MONOTONIC, &next_frame);
    prev_frame = next_frame;

    while (1) {
        swap_queues();

        pthread_mutex_lock(&recv_mutex);
        int recv_empty = (receiver_q->head == receiver_q->tail);
        if (receiver_terminated && recv_empty) {
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

        // Calculate delta time
        clock_gettime(CLOCK_MONOTONIC, &current);
        double delta_time = (current.tv_sec - prev_frame.tv_sec) + 
                           (current.tv_nsec - prev_frame.tv_nsec) / 1000000000.0;
        prev_frame = current;
        game_time += delta_time;

        // Update game state - move players based on their current movement state
        for (short i = 0; i < 16; i++) {
            if (playerConnections[i].active && players[i].hp > 0) {
                double fwd = playerConnections[i].forward;
                double rgt = playerConnections[i].right;
                double up = playerConnections[i].up;
                short rot = playerConnections[i].rotation_direction;
                
                if (fwd != 0 || rgt != 0 || up != 0) {
                    movePlayer(i, fwd * MOVE_SPEED * delta_time, 
                              rgt * MOVE_SPEED * delta_time, 
                              up * MOVE_SPEED * delta_time, 0);
                }
                if (rot == 1) {
                    rotatePlayer(i, ROTATION_SPEED * delta_time);
                } else if (rot == 2) {
                    rotatePlayer(i, -ROTATION_SPEED * delta_time);
                }
            }
        }

        // Update projectiles and check for collisions (callback broadcasts collision events)
        updateProjectiles(&projectileQueue, players, 16, delta_time, 1, on_projectile_collision);

        // Calculate next frame time
        next_frame.tv_nsec += INTERVAL_NS;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }
}

void sender() {
    out_entry entry;

    while (1) {
        pthread_mutex_lock(&send_mutex);
        while (sender_q->head == sender_q->tail) {
            pthread_mutex_lock(&prod_mutex);
            int prod_term = producer_terminated;
            int prod_empty = (producer_q->head == producer_q->tail);
            pthread_mutex_unlock(&prod_mutex);
            
            if (prod_term) {
                if (prod_empty) {
                    swap_queues();
                } else {
                    printf("Sender detected producer termination.\n");
                    pthread_mutex_unlock(&send_mutex);
                    
                    // Broadcast TERMINATE to all
                    unsigned char terminate[1] = {CMD_TERMINATE};
                    for (int i = 0; i < 512; i++) {
                        if (subscribers[i].active) {
                            sendto(server_sockfd, terminate, 1, 0,
                                (struct sockaddr*)&subscribers[i].addr,
                                subscribers[i].addr_len);
                        }
                    }
                    printf("Sender terminating...\n");
                    fflush(stdout);
                    return;
                }
            }
            pthread_cond_wait(&send_swap_cond, &send_mutex);
        }
        
        while (dequeue_out_locked(sender_q, &entry) == 0) {
            pthread_mutex_unlock(&send_mutex);
            broadcast_message(entry.data, entry.length, 
                            entry.target_subscriber, entry.exclude_subscriber);
            pthread_mutex_lock(&send_mutex);
        }
        pthread_mutex_unlock(&send_mutex);
    }
}

void pinger() {
    unsigned char ping_msg[1] = {CMD_PING};

    while (1) {
        sleep(15);

        int response_count = 0;
        pong_resp responses[512];
        while (dequeue_pong(&ping_queue, &responses[response_count]) == 0) {
            response_count++;
            if (response_count >= 512) break;
        }

        for (int i = 0; i < 512; i++) {
            if (subscribers[i].active && subscribers[i].pinged) {
                int ponged = 0;
                for (int j = 0; j < response_count; j++) {
                    if (memcmp(&subscribers[i].addr, &responses[j].addr, 
                              sizeof(struct sockaddr_in6)) == 0) {
                        ponged = 1;
                        break;
                    }
                }
                if (!ponged) {
                    // Find and kill the player
                    short player_id = find_player_by_subscriber(i);
                    if (player_id >= 0) {
                        printf("Player %d timed out\n", player_id);
                        kill_player(player_id);
                        playerConnections[player_id].active = 0;
                    }
                    subscribers[i].active = 0;
                    subscribers[i].pinged = 0;
                    printf("Subscriber %d timed out and removed.\n", i);
                    fflush(stdout);
                }
            }
        }

        if (receiver_terminated) {
            printf("Pinger terminating...\n");
            fflush(stdout);
            return;
        }

        // Send PING to all active subscribers
        for (int i = 0; i < 512; i++) {
            if (subscribers[i].active) {
                subscribers[i].pinged = 1;
                sendto(server_sockfd, ping_msg, 1, 0,
                       (struct sockaddr*)&subscribers[i].addr,
                       subscribers[i].addr_len);
            }
        }
    }
}

void consumer() {
    cmd_entry entry;
    
    while (1) {
        pthread_mutex_lock(&cons_mutex);
        while (consumer_q->head == consumer_q->tail) {
            if (swapper_terminated) {
                pthread_mutex_unlock(&cons_mutex);
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
        
        while (dequeue_cmd_locked(consumer_q, &entry) == 0) {
            pthread_mutex_unlock(&cons_mutex);
            
            if (entry.length < 1) {
                pthread_mutex_lock(&cons_mutex);
                continue;
            }
            
            unsigned char cmd_code = entry.data[0];
            short subscriber_idx = find_subscriber(&entry.sender_addr);
            short player_id = (subscriber_idx >= 0) ? 
                             find_player_by_subscriber(subscriber_idx) : -1;
            
            switch (cmd_code) {
                case CMD_MOVE_ROTATE:
                    if (entry.length >= 1 + sizeof(CmdMoveRotate)) {
                        handle_move_rotate(player_id, (CmdMoveRotate*)(entry.data + 1));
                    }
                    break;
                    
                case CMD_SHOOT:
                    handle_shoot(player_id);
                    break;
                    
                default:
                    // Unknown command, ignore
                    break;
            }
            
            pthread_mutex_lock(&cons_mutex);
        }
        pthread_mutex_unlock(&cons_mutex);
    }
}

void stdin_command_reader() {
    char input[512];
    printf("\nType IP:port to ping (e.g., 192.168.1.100:12345 or [::1]:8080)\n");
    fflush(stdout);
    
    while (1) {
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = '\0';
        
        if (strlen(input) == 0) continue;
        
        // Parse IP:port
        char addr_str[256];
        int port = 0;
        
        // Check for bracketed IPv6 format [::1]:port
        if (input[0] == '[') {
            char *bracket_end = strchr(input, ']');
            if (bracket_end && bracket_end[1] == ':') {
                size_t addr_len = bracket_end - input - 1;
                strncpy(addr_str, input + 1, addr_len);
                addr_str[addr_len] = '\0';
                port = atoi(bracket_end + 2);
            } else {
                printf("Invalid format. Use [IPv6]:port\n");
                continue;
            }
        } else {
            // IPv4 format or plain IPv6
            char *colon = strrchr(input, ':');
            if (colon) {
                size_t addr_len = colon - input;
                strncpy(addr_str, input, addr_len);
                addr_str[addr_len] = '\0';
                port = atoi(colon + 1);
            } else {
                printf("Invalid format. Use IP:port\n");
                continue;
            }
        }
        
        if (port <= 0 || port > 65535) {
            printf("Invalid port: %d\n", port);
            continue;
        }
        
        // Try to parse as IPv6 first, then IPv4-mapped
        struct sockaddr_in6 target;
        memset(&target, 0, sizeof(target));
        target.sin6_family = AF_INET6;
        target.sin6_port = htons(port);
        
        if (inet_pton(AF_INET6, addr_str, &target.sin6_addr) == 1) {
            // Valid IPv6
            unsigned char ping[1] = {CMD_PING};
            sendto(server_sockfd, ping, 1, 0, (struct sockaddr*)&target, sizeof(target));
            printf("Sent ping to [%s]:%d\n", addr_str, port);
        } else {
            // Try IPv4
            struct in_addr ipv4;
            if (inet_pton(AF_INET, addr_str, &ipv4) == 1) {
                // Map to IPv6
                target.sin6_addr.s6_addr[10] = 0xff;
                target.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&target.sin6_addr.s6_addr[12], &ipv4, 4);
                unsigned char ping[1] = {CMD_PING};
                sendto(server_sockfd, ping, 1, 0, (struct sockaddr*)&target, sizeof(target));
                printf("Sent ping to %s:%d\n", addr_str, port);
            } else {
                printf("Invalid IP address: %s\n", addr_str);
            }
        }
        fflush(stdout);
    }
}

void server_ctrlcHandler(int signum) {
    unsigned char terminate[1] = {CMD_TERMINATE};
    for (int i = 0; i < 512; i++) {
        if (subscribers[i].active) {
            sendto(server_sockfd, terminate, 1, 0,
                (struct sockaddr*)&subscribers[i].addr,
                subscribers[i].addr_len);
        }
    }
    pthread_cancel(stdin_thread_id);
    printf("\nCTRL-C detected. TERMINATE sent to subscribers. Exiting.\n");
    fflush(stdout);
    exit(0);
}

int main() {
    // Initialize subscribers
    for (int i = 0; i < 512; i++) {
        subscribers[i].active = 0;
        subscribers[i].pinged = 0;
    }
    
    // Initialize player connections
    for (int i = 0; i < 16; i++) {
        playerConnections[i].active = 0;
        playerConnections[i].subscriber_index = -1;
    }
    
    // Initialize game state
    initProjectileQueue(&projectileQueue);
    initPlayers(players, 16);
    
    // Initialize queues
    init_queue(&qA);
    init_queue(&qB);
    init_out_queue(&qC);
    init_out_queue(&qD);
    init_pinger_queue(&ping_queue);
    receiver_q = &qA;
    consumer_q = &qB;
    producer_q = &qC;
    sender_q = &qD;

    signal(SIGINT, server_ctrlcHandler);

    pthread_t recv_thread, swap_thread, cons_thread, send_thread, ping_thread, stdin_thread;

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
    if (pthread_create(&ping_thread, NULL, (void *(*)(void *))pinger, NULL) != 0) {
        perror("pthread_create pinger");
        return 1;
    }
    pinger_thread_id = ping_thread;
    
    if (pthread_create(&stdin_thread, NULL, (void *(*)(void *))stdin_command_reader, NULL) != 0) {
        perror("pthread_create stdin_reader");
        return 1;
    }
    stdin_thread_id = stdin_thread;
    pthread_detach(stdin_thread);  // Detach since we won't join it

    pthread_join(recv_thread, NULL);
    pthread_join(swap_thread, NULL);
    pthread_join(cons_thread, NULL);
    pthread_join(send_thread, NULL);
    pthread_join(ping_thread, NULL);

    printf("\nServer terminated.\n");

    return 0;
}
