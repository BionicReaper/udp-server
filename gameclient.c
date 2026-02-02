#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdint.h>
#include "game.h"

#define MAX_CMD_SIZE 8192
#define FRAME_INTERVAL_NS_CLIENT (16666667L)  // 60 FPS

// Client state
static int sockfd;
static struct sockaddr_in6 server_addr;
static struct termios orig_termios;
static short my_player_id = -1;
static int connected = 0;
static int receiver_terminated = 0;
static int game_running = 0;

// Chunked onboarding reassembly (avoids relying on UDP/IP fragmentation)
static unsigned char onboarding_buf[sizeof(CmdOnboarding)];
static uint32_t onboarding_total = 0;
static uint16_t onboarding_chunk_size = 0;
static int onboarding_chunks_expected = 0;
static unsigned char onboarding_chunks_received[64];
static int onboarding_in_progress = 0;

// Input state tracking
static short key_w = 0, key_a = 0, key_s = 0, key_d = 0;
static short key_left = 0, key_right = 0;
static short prev_moving = 0;
static short prev_rotating = 0;
static double prev_forward = 0, prev_right = 0, prev_up = 0;
static short prev_rot_dir = 0;

// Player movement state (for local simulation)
typedef struct {
    double forward;
    double right;
    double up;
    short rotation_direction;
} LocalPlayerMovement;

LocalPlayerMovement localMovement[16];

// Mutexes for thread safety
static pthread_mutex_t game_mutex = PTHREAD_MUTEX_INITIALIZER;

static void handle_onboarding(const unsigned char *data, int length);

void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    write(STDOUT_FILENO, "\033[?25h", 6);  // Show cursor
}

void set_raw_mode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    raw.c_cc[VMIN] = 0;   // Non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Send a command to the server
void send_command(unsigned char cmd_code, void *payload, int payload_size) {
    unsigned char buffer[MAX_CMD_SIZE];
    buffer[0] = cmd_code;
    if (payload && payload_size > 0) {
        memcpy(buffer + 1, payload, payload_size);
    }
    sendto(sockfd, buffer, 1 + payload_size, 0,
           (struct sockaddr*)&server_addr, sizeof(server_addr));
}

static void onboarding_reset(void) {
    onboarding_total = 0;
    onboarding_chunk_size = 0;
    onboarding_chunks_expected = 0;
    memset(onboarding_chunks_received, 0, sizeof(onboarding_chunks_received));
    onboarding_in_progress = 0;
}

static void handle_onboarding_begin(const unsigned char *data, int length) {
    if (length < (int)sizeof(CmdOnboardingBegin)) return;
    const CmdOnboardingBegin *begin = (const CmdOnboardingBegin *)data;

    if (begin->total_size == 0 || begin->total_size > sizeof(CmdOnboarding)) {
        onboarding_reset();
        return;
    }
    if (begin->chunk_size == 0 || begin->chunk_size > 4096) {
        onboarding_reset();
        return;
    }

    onboarding_total = begin->total_size;
    onboarding_chunk_size = begin->chunk_size;
    onboarding_chunks_expected = (int)((onboarding_total + onboarding_chunk_size - 1) / onboarding_chunk_size);
    if (onboarding_chunks_expected <= 0 || onboarding_chunks_expected > (int)sizeof(onboarding_chunks_received)) {
        onboarding_reset();
        return;
    }

    memset(onboarding_buf, 0, sizeof(onboarding_buf));
    memset(onboarding_chunks_received, 0, sizeof(onboarding_chunks_received));
    onboarding_in_progress = 1;
}

static void try_finish_onboarding(void) {
    if (!onboarding_in_progress) return;
    if (onboarding_total == 0 || onboarding_chunks_expected <= 0) return;

    for (int i = 0; i < onboarding_chunks_expected; i++) {
        if (!onboarding_chunks_received[i]) return;
    }

    // We have a full CmdOnboarding payload; reuse existing handler.
    handle_onboarding(onboarding_buf, (int)onboarding_total);
    connected = 1;
    onboarding_in_progress = 0;
}

static void handle_onboarding_chunk(const unsigned char *data, int length) {
    if (!onboarding_in_progress) return;
    if (length < (int)sizeof(CmdOnboardingChunkHeader)) return;

    const CmdOnboardingChunkHeader *hdr = (const CmdOnboardingChunkHeader *)data;
    const unsigned char *chunk_data = data + sizeof(CmdOnboardingChunkHeader);
    int chunk_len = length - (int)sizeof(CmdOnboardingChunkHeader);

    if ((int)hdr->data_len != chunk_len) return;
    if (hdr->offset >= onboarding_total) return;
    if (hdr->data_len == 0) return;
    if (hdr->offset + hdr->data_len > onboarding_total) return;

    memcpy(onboarding_buf + hdr->offset, chunk_data, hdr->data_len);

    int chunk_index = (int)(hdr->offset / onboarding_chunk_size);
    if (chunk_index >= 0 && chunk_index < (int)sizeof(onboarding_chunks_received)) {
        onboarding_chunks_received[chunk_index] = 1;
    }

    try_finish_onboarding();
}

// Handle CMD_ONBOARDING
static void handle_onboarding(const unsigned char *data, int length) {
    if (length < sizeof(CmdOnboarding)) return;
    
    CmdOnboarding *onboard = (CmdOnboarding*)data;
    
    pthread_mutex_lock(&game_mutex);
    my_player_id = onboard->assigned_playerID;
    memcpy(players, onboard->players, sizeof(players));
    memcpy(&projectileQueue, &onboard->projectileQueue, sizeof(projectileQueue));
    
    // Initialize local movement state
    for (int i = 0; i < 16; i++) {
        localMovement[i].forward = 0;
        localMovement[i].right = 0;
        localMovement[i].up = 0;
        localMovement[i].rotation_direction = 0;
    }
    
    game_running = 1;
    pthread_mutex_unlock(&game_mutex);
    
    printf("Logged in as player %d\n", my_player_id);
    fflush(stdout);
}

// Handle CMD_MOVE_EXECUTED
void handle_move_executed(const unsigned char *data, int length) {
    if (length < sizeof(CmdMoveExecuted)) return;
    
    CmdMoveExecuted *exec = (CmdMoveExecuted*)data;
    
    pthread_mutex_lock(&game_mutex);
    if (exec->playerID >= 0 && exec->playerID < 16) {
        // Teleport player to server position and rotation
        players[exec->playerID].cuboid.position = exec->position;
        players[exec->playerID].cuboid.rotation_y = exec->rotation_y;
        players[exec->playerID].gun.position = exec->position;
        players[exec->playerID].gun.position.y -= players[exec->playerID].cuboid.height / 4.0;
        players[exec->playerID].gun.rotation_y = exec->rotation_y;
        
        // Update local movement state
        localMovement[exec->playerID].forward = exec->forward;
        localMovement[exec->playerID].right = exec->right;
        localMovement[exec->playerID].up = exec->up;
        localMovement[exec->playerID].rotation_direction = exec->rotation_direction;
    }
    pthread_mutex_unlock(&game_mutex);
}

// Handle CMD_SHOOT_EXECUTED
void handle_shoot_executed(const unsigned char *data, int length) {
    if (length < sizeof(CmdShootExecuted)) return;
    
    CmdShootExecuted *exec = (CmdShootExecuted*)data;
    
    pthread_mutex_lock(&game_mutex);
    // Create projectile locally
    Projectile proj;
    proj.position = exec->gun_position;
    proj.length = 3.0;
    proj.rotation_y = exec->gun_rotation_y;
    proj.color = (Color){255, 255, 255};
    proj.distance_left = PROJECTILE_TRAVEL_DISTANCE;
    proj.speed = PROJECTILE_TRAVEL_SPEED;
    proj.ownerID = exec->playerID;
    proj.collided = 0;
    enqueueProjectile(&projectileQueue, proj);
    pthread_mutex_unlock(&game_mutex);
}

// Handle CMD_PROJECTILE_HIT
void handle_projectile_hit(const unsigned char *data, int length) {
    if (length < sizeof(CmdProjectileHit)) return;
    
    CmdProjectileHit *hit = (CmdProjectileHit*)data;
    
    pthread_mutex_lock(&game_mutex);
    if (hit->hit_playerID >= 0 && hit->hit_playerID < 16) {
        players[hit->hit_playerID].hp -= 1;
        unsigned char newRed = (players[hit->hit_playerID].cuboid.color.red <= 204) ? (players[hit->hit_playerID].cuboid.color.red + 51) : 255;
        unsigned char newGreen = (players[hit->hit_playerID].cuboid.color.green >= 51) ? (players[hit->hit_playerID].cuboid.color.green - 51) : 0;
        changePlayerColor(hit->hit_playerID, (Color){newRed, newGreen, 0});
    }
    // Mark projectile as collided if we have the index
    if (hit->projectile_index >= 0 && hit->projectile_index < 64) {
        projectileQueue.projectiles[hit->projectile_index].collided = 1;
    }
    pthread_mutex_unlock(&game_mutex);
}

// Handle CMD_NEW_PLAYER
void handle_new_player(const unsigned char *data, int length) {
    if (length < sizeof(CmdNewPlayer)) return;
    
    CmdNewPlayer *newPlayer = (CmdNewPlayer*)data;
    
    pthread_mutex_lock(&game_mutex);
    if (newPlayer->playerID >= 0 && newPlayer->playerID < 16) {
        players[newPlayer->playerID] = newPlayer->player;
        localMovement[newPlayer->playerID].forward = 0;
        localMovement[newPlayer->playerID].right = 0;
        localMovement[newPlayer->playerID].up = 0;
        localMovement[newPlayer->playerID].rotation_direction = 0;
    }
    pthread_mutex_unlock(&game_mutex);
}

// Handle CMD_PLAYER_KILLED
void handle_player_killed(const unsigned char *data, int length) {
    if (length < sizeof(CmdPlayerKilled)) return;
    
    CmdPlayerKilled *kill = (CmdPlayerKilled*)data;
    
    pthread_mutex_lock(&game_mutex);
    if (kill->playerID >= 0 && kill->playerID < 16) {
        players[kill->playerID].hp = 0;
    }
    pthread_mutex_unlock(&game_mutex);
}

// Handle CMD_LOGIN_DENIED
void handle_login_denied() {
    printf("Server is full. Cannot join.\n");
    fflush(stdout);
    restore_terminal();
    close(sockfd);
    exit(1);
}

void *receiver_thread(void *arg) {
    unsigned char buffer[MAX_CMD_SIZE];
    struct sockaddr_in6 from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (!receiver_terminated) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&from_addr, &from_len);
        if (n < 0) {
            continue;
        }
        
        if (n < 1) continue;
        
        unsigned char cmd_code = buffer[0];
        unsigned char *payload = buffer + 1;
        int payload_len = n - 1;

        switch (cmd_code) {
            case CMD_PING:
                // Respond with PONG
                send_command(CMD_PONG, NULL, 0);
                break;
                
            case CMD_TERMINATE:
                printf("\n\nServer sent TERMINATE. Exiting.\n");
                receiver_terminated = 1;
                restore_terminal();
                close(sockfd);
                exit(0);
                break;
                
            case CMD_ONBOARDING:
                handle_onboarding(payload, payload_len);
                break;

            case CMD_ONBOARDING_BEGIN:
                handle_onboarding_begin(payload, payload_len);
                break;

            case CMD_ONBOARDING_CHUNK:
                handle_onboarding_chunk(payload, payload_len);
                break;

            case CMD_ONBOARDING_END:
                // Optional marker; attempt completion in case final chunk arrived earlier.
                try_finish_onboarding();
                break;
                
            case CMD_MOVE_EXECUTED:
                handle_move_executed(payload, payload_len);
                break;
                
            case CMD_SHOOT_EXECUTED:
                handle_shoot_executed(payload, payload_len);
                break;
                
            case CMD_PROJECTILE_HIT:
                handle_projectile_hit(payload, payload_len);
                break;
                
            case CMD_NEW_PLAYER:
                handle_new_player(payload, payload_len);
                break;
                
            case CMD_PLAYER_KILLED:
                handle_player_killed(payload, payload_len);
                break;
                
            case CMD_LOGIN_DENIED:
                handle_login_denied();
                break;
                
            default:
                break;
        }
    }
    return NULL;
}

// Calculate direction vector from WASD keys
void get_movement_direction(double *forward, double *right, double *up) {
    *forward = 0;
    *right = 0;
    *up = 0;
    
    if (key_w) *forward += 1.0;
    if (key_s) *forward -= 1.0;
    if (key_d) *right += 1.0;
    if (key_a) *right -= 1.0;
    
    // Normalize if moving diagonally
    if (*forward != 0 && *right != 0) {
        double len = sqrt((*forward) * (*forward) + (*right) * (*right));
        *forward /= len;
        *right /= len;
    }
}

// Get rotation direction from arrow keys
short get_rotation_direction() {
    if (key_right && !key_left) return 1;  // Right
    if (key_left && !key_right) return 2;  // Left
    return 0;  // Stop
}

// Process keyboard input
void process_input() {
    char c;
    char escape_seq[3] = {0};
    
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 27) {  // Escape sequence (arrow keys)
            if (read(STDIN_FILENO, &escape_seq[0], 1) == 1 && escape_seq[0] == '[') {
                if (read(STDIN_FILENO, &escape_seq[1], 1) == 1) {
                    switch (escape_seq[1]) {
                        case 'A': // Up arrow
                            key_w = 1;
                            break;
                        case 'B': // Down arrow
                            key_s = 1;
                            break;
                        case 'C': // Right arrow
                            key_right = 1;
                            break;
                        case 'D': // Left arrow
                            key_left = 1;
                            break;
                    }
                }
            }
        } else {
            switch (c) {
                case 'w': case 'W':
                    key_w = 1;
                    break;
                case 'a': case 'A':
                    key_a = 1;
                    break;
                case 's': case 'S':
                    key_s = 1;
                    break;
                case 'd': case 'D':
                    key_d = 1;
                    break;
                case ' ':  // Spacebar - shoot
                    send_command(CMD_SHOOT, NULL, 0);
                    break;
                case 3:  // Ctrl+C
                    receiver_terminated = 1;
                    restore_terminal();
                    close(sockfd);
                    printf("\nExiting...\n");
                    exit(0);
                    break;
            }
        }
    }
}

// Check if a key is still being pressed (simplified - we reset after each frame)
void reset_key_states() {
    key_w = 0;
    key_a = 0;
    key_s = 0;
    key_d = 0;
    key_left = 0;
    key_right = 0;
}

int main(int argc, char *argv[]) {
    char server_ip[256];
    int use_msaa = 1;  // Default to MSAA on
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--noaa") == 0) {
            use_msaa = 0;
        }
    }
    
    // Set MSAA mode
    setActiveMSAA(use_msaa);
    
    printf("Enter server IPv6 address (e.g., ::1 or fe80::1): ");
    if (fgets(server_ip, sizeof(server_ip), stdin) == NULL) {
        fprintf(stderr, "Failed to read server address\n");
        exit(1);
    }
    server_ip[strcspn(server_ip, "\n")] = '\0';

    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_port = htons(53847);
    if (inet_pton(AF_INET6, server_ip, &server_addr.sin6_addr) <= 0) {
        fprintf(stderr, "Invalid IPv6 address: %s\n", server_ip);
        close(sockfd);
        exit(1);
    }

    // Set receive timeout for login phase
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Connecting to server...\n");
    fflush(stdout);

    onboarding_reset();

    // Try to login
    for (int attempt = 0; attempt < 12 && !connected; attempt++) {
        printf("\rAttempt %02d: Sending LOGIN...", attempt + 1);
        fflush(stdout);
        
        send_command(CMD_LOGIN, NULL, 0);
        
        unsigned char buffer[MAX_CMD_SIZE];
        struct sockaddr_in6 from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&from_addr, &from_len);
        if (n > 0) {
            unsigned char code = buffer[0];
            if (code == CMD_ONBOARDING) {
                handle_onboarding(buffer + 1, n - 1);
                connected = 1;
                printf(" Connected!\n");
            } else if (code == CMD_ONBOARDING_BEGIN) {
                handle_onboarding_begin(buffer + 1, n - 1);
            } else if (code == CMD_ONBOARDING_CHUNK) {
                handle_onboarding_chunk(buffer + 1, n - 1);
                if (connected) {
                    printf(" Connected!\n");
                }
            } else if (code == CMD_ONBOARDING_END) {
                try_finish_onboarding();
                if (connected) {
                    printf(" Connected!\n");
                }
            } else if (code == CMD_LOGIN_DENIED) {
                handle_login_denied();
            }
        }
    }

    if (!connected) {
        printf(" Failed to connect after 12 attempts.\n");
        close(sockfd);
        return 1;
    }

    // Clear receive timeout for normal operation
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Set non-blocking for game loop
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    // Start receiver thread
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create failed");
        close(sockfd);
        return 1;
    }

    // Set terminal to raw mode (no echo, no line buffering)
    set_raw_mode();
    
    // Hide cursor
    write(STDOUT_FILENO, "\033[?25l", 6);
    
    // Clear screen
    write(STDOUT_FILENO, "\033[2J\033[H", 7);

    // Main game loop
    struct timespec next_frame, current, prev_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);
    prev_frame = next_frame;

    while (!receiver_terminated && game_running) {
        // Calculate next frame time
        next_frame.tv_nsec += FRAME_INTERVAL_NS_CLIENT;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }

        // Wait until next frame
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);

        // Calculate delta time
        clock_gettime(CLOCK_MONOTONIC, &current);
        double delta_time = (current.tv_sec - prev_frame.tv_sec) + 
                           (current.tv_nsec - prev_frame.tv_nsec) / 1000000000.0;
        prev_frame = current;

        // Process input
        process_input();

        // Calculate current movement state
        double forward, right, up;
        get_movement_direction(&forward, &right, &up);
        short rot_dir = get_rotation_direction();

        // Check if movement state changed
        short moving = (forward != 0 || right != 0 || up != 0);
        short rotating = (rot_dir != 0);

        if (forward != prev_forward || right != prev_right || up != prev_up || 
            rot_dir != prev_rot_dir || 
            (prev_moving && !moving) || (prev_rotating && !rotating)) {
            // Send move command
            CmdMoveRotate cmd;
            cmd.forward = forward;
            cmd.right = right;
            cmd.up = up;
            cmd.rotation_direction = rot_dir;
            send_command(CMD_MOVE_ROTATE, &cmd, sizeof(cmd));
            
            prev_forward = forward;
            prev_right = right;
            prev_up = up;
            prev_rot_dir = rot_dir;
        }

        prev_moving = moving;
        prev_rotating = rotating;

        // Reset key states for next frame
        reset_key_states();

        // Update game state locally
        pthread_mutex_lock(&game_mutex);
        
        // Move all players based on their movement state
        for (short i = 0; i < 16; i++) {
            if (players[i].hp > 0) {
                double fwd = localMovement[i].forward;
                double rgt = localMovement[i].right;
                double up_mov = localMovement[i].up;
                short rot = localMovement[i].rotation_direction;
                
                if (fwd != 0 || rgt != 0 || up_mov != 0) {
                    movePlayer(i, fwd * MOVE_SPEED * delta_time, 
                              rgt * MOVE_SPEED * delta_time, 
                              up_mov * MOVE_SPEED * delta_time, 0);
                }
                if (rot == 1) {
                    rotatePlayer(i, ROTATION_SPEED * delta_time);
                } else if (rot == 2) {
                    rotatePlayer(i, -ROTATION_SPEED * delta_time);
                }
            }
        }

        // Update projectiles (no collision check on client, no callback)
        updateProjectiles(&projectileQueue, players, 16, delta_time, 0, NULL);

        // Update camera to follow our player
        if (my_player_id >= 0 && my_player_id < 16) {
            Vec3 cam_offset = {0, 2.0, -8.0};  // Behind and above player
            cam_offset = rotateY(cam_offset, players[my_player_id].cuboid.rotation_y);
            Vec3 cam_pos = {
                players[my_player_id].cuboid.position.x + cam_offset.x,
                players[my_player_id].cuboid.position.y + cam_offset.y,
                players[my_player_id].cuboid.position.z + cam_offset.z
            };
            moveCamera(cam_pos);
            setCameraRotation(players[my_player_id].cuboid.rotation_y);
        }

        // Render
        clearScreen();
        drawProjectiles(&projectileQueue);
        drawAllPlayers();
        generateframeString();
        render();
        
        pthread_mutex_unlock(&game_mutex);
    }

    // Cleanup
    restore_terminal();
    close(sockfd);
    pthread_join(recv_thread, NULL);
    
    printf("\nClient terminated.\n");
    return 0;
}
