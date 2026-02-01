#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>
#include "game.h"

#define INTERVAL_NS (16666667L)  // 60 FPS
#define MAX_CMD_SIZE 256
#define SHOOT_COOLDOWN 4.0  // 4 seconds between shots

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
static int server_sockfd;  // Global socket for sending

// Game timing
static double game_time = 0.0;

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
            // Already logged in, resend onboarding
            unsigned char response[1 + sizeof(CmdOnboarding)];
            response[0] = CMD_ONBOARDING;
            CmdOnboarding *onboard = (CmdOnboarding*)(response + 1);
            onboard->assigned_playerID = player_id;
            memcpy(onboard->players, players, sizeof(players));
            memcpy(&onboard->projectileQueue, &projectileQueue, sizeof(projectileQueue));
            sendto(server_sockfd, response, sizeof(response), 0,
                   (struct sockaddr*)client_addr, addr_len);
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
    
    // Send onboarding to new player
    unsigned char response[1 + sizeof(CmdOnboarding)];
    response[0] = CMD_ONBOARDING;
    CmdOnboarding *onboard = (CmdOnboarding*)(response + 1);
    onboard->assigned_playerID = player_id;
    memcpy(onboard->players, players, sizeof(players));
    memcpy(&onboard->projectileQueue, &projectileQueue, sizeof(projectileQueue));
    sendto(server_sockfd, response, sizeof(response), 0,
           (struct sockaddr*)client_addr, addr_len);
    
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

    // Bind to port 53847 (IPv6)
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(53847);

    if (bind(server_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_sockfd);
        exit(1);
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

void server_ctrlcHandler(int signum) {
    unsigned char terminate[1] = {CMD_TERMINATE};
    for (int i = 0; i < 512; i++) {
        if (subscribers[i].active) {
            sendto(server_sockfd, terminate, 1, 0,
                (struct sockaddr*)&subscribers[i].addr,
                subscribers[i].addr_len);
        }
    }
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

    pthread_t recv_thread, swap_thread, cons_thread, send_thread, ping_thread;

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

    pthread_join(recv_thread, NULL);
    pthread_join(swap_thread, NULL);
    pthread_join(cons_thread, NULL);
    pthread_join(send_thread, NULL);
    pthread_join(ping_thread, NULL);

    printf("\nServer terminated.\n");

    return 0;
}
