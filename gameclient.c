#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <netdb.h>
#include <errno.h>
#include "game.h"

// Try to include evdev support
#ifdef __linux__
#include <linux/input.h>
#endif

#define MAX_CMD_SIZE 8192
#define FRAME_INTERVAL_NS_CLIENT (16666667L)  // 60 FPS
#define INPUT_SERVER_PORT 53850

#define STUN_SERVER_ADDRESS "stun.l.google.com"
#define STUN_SERVER_PORT 19302

// Query STUN server to discover public IP:port (returns 1 on success, 0 on failure)
static int query_stun_server(const char *stun_host, int stun_port, int sockfd, char *public_ip, int ip_size, int *public_port) {
    // Build STUN Binding Request
    uint8_t request[20];
    uint16_t msg_type = htons(0x0001);
    uint16_t msg_len = htons(0);
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
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_V4MAPPED | AI_ALL;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", stun_port);
    
    if (getaddrinfo(stun_host, port_str, &hints, &res) != 0) {
        printf("STUN: Failed to resolve %s\n", stun_host);
        return 0;
    }
    
    // Send request to first available address
    int sent = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET6) {
            if (sendto(sockfd, request, sizeof(request), 0, rp->ai_addr, rp->ai_addrlen) >= 0) {
                printf("STUN: Request sent to %s\n", stun_host);
                sent = 1;
                break;
            } else {
                printf("STUN: Send failed: %s\n", strerror(errno));
            }
        }
    }
    
    freeaddrinfo(res);
    if (!sent) {
        printf("STUN: No IPv6 address available or send failed\n");
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
            printf("STUN: No response (timeout or error: %s)\n", strerror(errno));
        } else {
            printf("STUN: Response too short (%zd bytes)\n", n);
        }
        return 0;
    }
    
    printf("STUN: Received %zd byte response\n", n);
    
    // Parse STUN response
    uint32_t resp_cookie = ntohl(*(uint32_t *)(buffer + 4));
    if (resp_cookie != 0x2112A442) {
        printf("STUN: Invalid magic cookie (0x%08x)\n", resp_cookie);
        return 0;
    }
    
    uint16_t resp_len = ntohs(*(uint16_t *)(buffer + 2));
    printf("STUN: Parsing attributes (response length: %d, total: %d)...\n", resp_len, (int)n);
    int offset = 20;
    int attr_count = 0;
    while (offset + 4 <= n && offset < 20 + resp_len) {
        uint16_t attr_type = ntohs(*(uint16_t *)(buffer + offset));
        uint16_t attr_len = ntohs(*(uint16_t *)(buffer + offset + 2));
        attr_count++;
        printf("STUN: Attribute #%d: type=0x%04x, len=%d\n", attr_count, attr_type, attr_len);
        offset += 4;
        
        if (offset + attr_len > n) {
            printf("STUN: Attribute extends beyond response, stopping\n");
            break;
        }
        
        if ((attr_type == 0x0020 || attr_type == 0x0001) && attr_len >= 8) {
            uint8_t family = buffer[offset + 1];
            printf("STUN: Found %s attribute, family=0x%02x\n", 
                   attr_type == 0x0020 ? "XOR-MAPPED-ADDRESS" : "MAPPED-ADDRESS", family);
            if (family == 0x01) {
                uint16_t port_val = (buffer[offset + 2] << 8) | buffer[offset + 3];
                uint32_t addr = (buffer[offset + 4] << 24) | (buffer[offset + 5] << 16) |
                               (buffer[offset + 6] << 8) | buffer[offset + 7];
                
                printf("STUN: Raw port=0x%04x, addr=0x%08x\n", port_val, addr);
                
                if (attr_type == 0x0020) {
                    port_val ^= (0x2112A442 >> 16);
                    addr ^= 0x2112A442;
                    printf("STUN: After XOR: port=%d, addr=0x%08x\n", port_val, addr);
                }
                
                struct in_addr ina;
                ina.s_addr = htonl(addr);
                inet_ntop(AF_INET, &ina, public_ip, ip_size);
                *public_port = port_val;
                printf("STUN: Successfully discovered public address\n");
                return 1;
            } else if (family == 0x02 && attr_len >= 20) {  // IPv6
                uint16_t port_val = (buffer[offset + 2] << 8) | buffer[offset + 3];
                uint8_t addr6[16];
                memcpy(addr6, buffer + offset + 4, 16);
                
                if (attr_type == 0x0020) {  // XOR-MAPPED
                    port_val ^= (0x2112A442 >> 16);
                    // XOR with magic cookie + transaction ID
                    uint8_t xor_data[16];
                    memcpy(xor_data, buffer + 4, 4);  // magic cookie
                    memcpy(xor_data + 4, buffer + 8, 12);  // transaction ID
                    for (int i = 0; i < 16; i++) {
                        addr6[i] ^= xor_data[i];
                    }
                }
                
                struct in6_addr in6;
                memcpy(&in6, addr6, 16);
                inet_ntop(AF_INET6, &in6, public_ip, ip_size);
                *public_port = port_val;
                printf("STUN: Successfully discovered public IPv6 address\n");
                return 1;
            } else {
                printf("STUN: Unsupported address family 0x%02x\n", family);
            }
        }
        
        offset += attr_len;
        if (attr_len % 4) offset += 4 - (attr_len % 4);
    }
    
    printf("STUN: No mapped address in response\n");
    return 0;
}

// Input method enumeration
typedef enum {
    INPUT_METHOD_NONE,
    INPUT_METHOD_EVDEV,
    INPUT_METHOD_NETWORK,
    INPUT_METHOD_TERMINAL
} InputMethod;

static InputMethod current_input_method = INPUT_METHOD_NONE;
static int evdev_fd = -1;
static int input_server_fd = -1;
static pthread_t input_thread;

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
static short key_space = 0, key_space_prev = 0;
static short prev_moving = 0;
static short prev_rotating = 0;
static double prev_forward = 0, prev_right = 0, prev_up = 0;
static short prev_rot_dir = 0;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int input_thread_running = 0;

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

// Cleanup state tracking
static volatile sig_atomic_t cleanup_done = 0;
static volatile sig_atomic_t terminal_initialized = 0;

static void handle_onboarding(const unsigned char *data, int length);

// Forward declarations for cleanup
static void cleanup_input_system(void);

// Cleanup function registered with atexit - always runs on exit
static void cleanup_all(void) {
    if (cleanup_done) return;
    cleanup_done = 1;
    
    // Stop threads first
    input_thread_running = 0;
    receiver_terminated = 1;
    game_running = 0;
    
    // Close input system (this will signal threads to stop)
    if (current_input_method == INPUT_METHOD_EVDEV && evdev_fd >= 0) {
        close(evdev_fd);
        evdev_fd = -1;
    }
    if (current_input_method == INPUT_METHOD_NETWORK && input_server_fd >= 0) {
        close(input_server_fd);
        input_server_fd = -1;
    }
    
    // Close game socket
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    
    // Restore terminal (only if we modified it)
    if (terminal_initialized) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        write(STDOUT_FILENO, "\033[?25h", 6);  // Show cursor
        write(STDOUT_FILENO, "\n", 1);
    }
}

void restore_terminal() {
    if (terminal_initialized) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        write(STDOUT_FILENO, "\033[?25h", 6);  // Show cursor
    }
}

void set_raw_mode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    raw.c_cc[VMIN] = 0;   // Non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    terminal_initialized = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// ============================================================================
// EVDEV INPUT SUPPORT
// ============================================================================

#ifdef __linux__
// Map evdev keycodes to our internal key states
static void handle_evdev_event(int code, int value) {
    // value: 0 = release, 1 = press, 2 = repeat (we treat repeat as held)
    short state = (value != 0) ? 1 : 0;
    
    pthread_mutex_lock(&input_mutex);
    switch (code) {
        case KEY_W:
            key_w = state;
            break;
        case KEY_A:
            key_a = state;
            break;
        case KEY_S:
            key_s = state;
            break;
        case KEY_D:
            key_d = state;
            break;
        case KEY_LEFT:
            key_left = state;
            break;
        case KEY_RIGHT:
            key_right = state;
            break;
        case KEY_SPACE:
            key_space = state;
            break;
        case KEY_UP:
            key_w = state;
            break;
        case KEY_DOWN:
            key_s = state;
            break;
    }
    pthread_mutex_unlock(&input_mutex);
}

// Thread function for evdev input
static void *evdev_input_thread(void *arg) {
    (void)arg;
    struct input_event ev;
    
    while (input_thread_running && evdev_fd >= 0) {
        ssize_t n = read(evdev_fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                handle_evdev_event(ev.code, ev.value);
            }
        } else if (n < 0 && errno != EAGAIN) {
            break;
        }
        usleep(1000); // Small sleep to prevent busy loop
    }
    return NULL;
}

// Try to find and open a keyboard evdev device
static int try_evdev_init(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;
    
    struct dirent *entry;
    char path[512];
    int found = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        
        // Check if this device has keyboard capabilities
        unsigned long evbit[((EV_MAX + 7) / 8 + sizeof(unsigned long) - 1) / sizeof(unsigned long)] = {0};
        unsigned long keybit[((KEY_MAX + 7) / 8 + sizeof(unsigned long) - 1) / sizeof(unsigned long)] = {0};
        
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0 &&
            (evbit[0] & (1UL << EV_KEY)) &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
            
            // Check for W, A, S, D keys
            if ((keybit[KEY_W / (8 * sizeof(unsigned long))] & (1UL << (KEY_W % (8 * sizeof(unsigned long))))) &&
                (keybit[KEY_A / (8 * sizeof(unsigned long))] & (1UL << (KEY_A % (8 * sizeof(unsigned long))))) &&
                (keybit[KEY_S / (8 * sizeof(unsigned long))] & (1UL << (KEY_S % (8 * sizeof(unsigned long))))) &&
                (keybit[KEY_D / (8 * sizeof(unsigned long))] & (1UL << (KEY_D % (8 * sizeof(unsigned long)))))) {
                evdev_fd = fd;
                found = 1;
                break;
            }
        }
        close(fd);
    }
    closedir(dir);
    
    if (found) {
        input_thread_running = 1;
        if (pthread_create(&input_thread, NULL, evdev_input_thread, NULL) != 0) {
            close(evdev_fd);
            evdev_fd = -1;
            return 0;
        }
        return 1;
    }
    return 0;
}
#else
static int try_evdev_init(void) { return 0; }
#endif

// ============================================================================
// NETWORK INPUT SERVER SUPPORT (for WSL/Windows)
// ============================================================================

// Protocol: each message is 2 bytes: [keycode][state]
// keycode: 'W', 'A', 'S', 'D', 'L' (left), 'R' (right), ' ' (space)
// state: 0 = release, 1 = press

static void handle_network_input(unsigned char keycode, unsigned char state) {
    short key_state = (state != 0) ? 1 : 0;
    
    pthread_mutex_lock(&input_mutex);
    switch (keycode) {
        case 'W': case 'w':
            key_w = key_state;
            break;
        case 'A': case 'a':
            key_a = key_state;
            break;
        case 'S': case 's':
            key_s = key_state;
            break;
        case 'D': case 'd':
            key_d = key_state;
            break;
        case 'L': case 'l': // Left arrow
            key_left = key_state;
            break;
        case 'R': case 'r': // Right arrow
            key_right = key_state;
            break;
        case ' ': // Space
            key_space = key_state;
            break;
        case 'U': case 'u': // Up arrow (mapped to W)
            key_w = key_state;
            break;
        case 'N': case 'n': // Down arrow (mapped to S)
            key_s = key_state;
            break;
    }
    pthread_mutex_unlock(&input_mutex);
}

// Thread function for network input
static void *network_input_thread(void *arg) {
    (void)arg;
    unsigned char buffer[2];
    
    while (input_thread_running && input_server_fd >= 0) {
        ssize_t n = recv(input_server_fd, buffer, 2, 0);
        if (n == 2) {
            handle_network_input(buffer[0], buffer[1]);
        } else if (n <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break; // Connection closed or error
            }
        }
        usleep(1000); // Small sleep to prevent busy loop
    }
    return NULL;
}

// Try to connect to the input server
static int try_network_input_init(void) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(INPUT_SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    // Set a short timeout for connection attempt
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }
    
    // Set non-blocking after connection
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    input_server_fd = fd;
    input_thread_running = 1;
    
    if (pthread_create(&input_thread, NULL, network_input_thread, NULL) != 0) {
        close(input_server_fd);
        input_server_fd = -1;
        return 0;
    }
    
    return 1;
}

// ============================================================================
// INPUT SYSTEM INITIALIZATION
// ============================================================================

static void init_input_system(void) {
    // Try evdev first (native Linux with proper permissions)
#ifdef __linux__
    if (try_evdev_init()) {
        current_input_method = INPUT_METHOD_EVDEV;
        printf("Input: Using evdev (direct keyboard access)\n");
        return;
    }
#endif
    
    // Try network input server (for WSL/Windows)
    if (try_network_input_init()) {
        current_input_method = INPUT_METHOD_NETWORK;
        printf("Input: Using network input server (port %d)\n", INPUT_SERVER_PORT);
        return;
    }
    
    // Fall back to terminal input
    current_input_method = INPUT_METHOD_TERMINAL;
    printf("Input: Using terminal (may have repeat delay)\n");
}

static void cleanup_input_system(void) {
    input_thread_running = 0;
    
    if (current_input_method == INPUT_METHOD_EVDEV && evdev_fd >= 0) {
        close(evdev_fd);
        evdev_fd = -1;
        pthread_join(input_thread, NULL);
    }
    
    if (current_input_method == INPUT_METHOD_NETWORK && input_server_fd >= 0) {
        close(input_server_fd);
        input_server_fd = -1;
        pthread_join(input_thread, NULL);
    }
}

// Signal handler for clean exit on Ctrl+C
static void sigint_handler(int sig) {
    (void)sig;
    // Set flags to stop all loops
    receiver_terminated = 1;
    game_running = 0;
    input_thread_running = 0;
    
    // Close file descriptors to unblock any threads waiting on I/O
    if (evdev_fd >= 0) {
        close(evdev_fd);
        evdev_fd = -1;
    }
    if (input_server_fd >= 0) {
        close(input_server_fd);
        input_server_fd = -1;
    }
    
    // Note: cleanup_all() will be called by atexit() when we exit
    // Using _exit() would skip atexit handlers, so we just return
    // and let the main loop detect the flags and exit normally.
    // If the main loop is stuck, a second Ctrl+C will terminate forcefully.
}

// ============================================================================
// END INPUT SYSTEM
// ============================================================================

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

static int get_local_port(void) {
    struct sockaddr_in6 local_addr;
    socklen_t len = sizeof(local_addr);
    if (getsockname(sockfd, (struct sockaddr*)&local_addr, &len) == 0) {
        return ntohs(local_addr.sin6_port);
    }
    return -1;
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
    exit(1);  // atexit(cleanup_all) handles cleanup
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
                exit(0);  // atexit(cleanup_all) handles cleanup
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
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_lock(&input_mutex);
    }
    
    if (key_w) *forward += 1.0;
    if (key_s) *forward -= 1.0;
    if (key_d) *right += 1.0;
    if (key_a) *right -= 1.0;
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_unlock(&input_mutex);
    }
    
    // Normalize if moving diagonally
    if (*forward != 0 && *right != 0) {
        double len = sqrt((*forward) * (*forward) + (*right) * (*right));
        *forward /= len;
        *right /= len;
    }
}

// Get rotation direction from arrow keys
short get_rotation_direction() {
    short result = 0;
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_lock(&input_mutex);
    }
    
    if (key_right && !key_left) result = 1;  // Right
    else if (key_left && !key_right) result = 2;  // Left
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_unlock(&input_mutex);
    }
    
    return result;
}

// Check if space key was just pressed (for shooting)
short check_shoot() {
    short should_shoot = 0;
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_lock(&input_mutex);
    }
    
    // Shoot on key press (transition from not pressed to pressed)
    if (key_space && !key_space_prev) {
        should_shoot = 1;
    }
    key_space_prev = key_space;
    
    if (current_input_method != INPUT_METHOD_TERMINAL) {
        pthread_mutex_unlock(&input_mutex);
    }
    
    return should_shoot;
}

// Process keyboard input (terminal fallback mode only)
void process_input_terminal() {
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
                    key_space = 1;
                    break;
                case 3:  // Ctrl+C
                    printf("\nExiting...\n");
                    exit(0);  // atexit(cleanup_all) handles cleanup
                    break;
            }
        }
    }
}

// Process input based on current input method
void process_input() {
    if (current_input_method == INPUT_METHOD_TERMINAL) {
        process_input_terminal();
    }
    // For EVDEV and NETWORK methods, input is handled by their respective threads
    // We just need to lock when reading the key states (done in get_movement_direction)
}

// Reset key states (only for terminal fallback mode)
void reset_key_states() {
    if (current_input_method == INPUT_METHOD_TERMINAL) {
        key_w = 0;
        key_a = 0;
        key_s = 0;
        key_d = 0;
        key_left = 0;
        key_right = 0;
        key_space = 0;
    }
}

int main(int argc, char *argv[]) {
    char server_ip[256];
    int use_msaa = 1;  // Default to MSAA on
    int client_port = 0;  // 0 means OS assigns random port
    
    // Register cleanup handler - runs on any exit
    atexit(cleanup_all);
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--noaa") == 0) {
            use_msaa = 0;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            client_port = atoi(argv[i + 1]);
            if (client_port <= 0 || client_port > 65535) {
                fprintf(stderr, "Invalid client port number: %s\n", argv[i + 1]);
                exit(1);
            }
            i++;  // Skip next argument
        }
    }
    
    // Set MSAA mode
    setActiveMSAA(use_msaa);
    
    printf("Enter server IPv6 address (e.g., ::1 or [::1]:8080): ");
    if (fgets(server_ip, sizeof(server_ip), stdin) == NULL) {
        fprintf(stderr, "Failed to read server address\n");
        exit(1);
    }
    server_ip[strcspn(server_ip, "\n")] = '\0';

    // Parse address and optional port
    char addr_only[256];
    int port = 53847;  // Default port
    
    // Check for bracketed IPv6 with port: [::1]:8080
    if (server_ip[0] == '[') {
        char *bracket_end = strchr(server_ip, ']');
        if (bracket_end != NULL) {
            // Extract address between brackets
            size_t addr_len = bracket_end - server_ip - 1;
            strncpy(addr_only, server_ip + 1, addr_len);
            addr_only[addr_len] = '\0';
            
            // Check for port after bracket
            if (bracket_end[1] == ':') {
                port = atoi(bracket_end + 2);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port number\n");
                    exit(1);
                }
            }
        } else {
            strcpy(addr_only, server_ip);
        }
    } else {
        // Plain address without brackets
        strcpy(addr_only, server_ip);
    }

    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket failed");
        exit(1);
    }

    // Bind to specific port if requested
    if (client_port > 0) {
        struct sockaddr_in6 bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_addr = in6addr_any;
        bind_addr.sin6_port = htons(client_port);
        
        if (bind(sockfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            perror("bind failed");
            close(sockfd);
            exit(1);
        }
        printf("Bound to local port %d\n", client_port);
    }

    // Query STUN to get public endpoint before connecting
    char public_ip[INET_ADDRSTRLEN];
    int public_port = 0;
    struct sockaddr_in6 local_addr;
    socklen_t local_len = sizeof(local_addr);
    
    // Need to send something first to establish a local binding
    // We'll use a dummy STUN request for this
    if (query_stun_server(STUN_SERVER_ADDRESS, STUN_SERVER_PORT, sockfd, public_ip, sizeof(public_ip), &public_port)) {
        // Get local port after STUN query
        if (getsockname(sockfd, (struct sockaddr *)&local_addr, &local_len) == 0) {
            int local_port = ntohs(local_addr.sin6_port);
            printf("Local port: %d\n", local_port);
        }
        printf("Public endpoint: %s:%d\n", public_ip, public_port);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_port = htons(port);
    if (inet_pton(AF_INET6, addr_only, &server_addr.sin6_addr) <= 0) {
        fprintf(stderr, "Invalid IPv6 address: %s\n", addr_only);
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
        send_command(CMD_LOGIN, NULL, 0);
        int local_port = get_local_port();
        if (local_port > 0) {
            printf("\rAttempt %02d: Sending LOGIN... (local port %d)", attempt + 1, local_port);
        } else {
            printf("\rAttempt %02d: Sending LOGIN...", attempt + 1);
        }
        fflush(stdout);
        
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
    
    // Install signal handler for clean exit
    signal(SIGINT, sigint_handler);
    
    // Initialize input system (tries evdev, then network, then terminal fallback)
    init_input_system();
    
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

        // Check for shooting
        if (check_shoot()) {
            send_command(CMD_SHOOT, NULL, 0);
        }

        // Calculate current movement state
        double forward, right, up;
        get_movement_direction(&forward, &right, &up);
        short rot_dir = get_rotation_direction();

        // Check if movement state changed (use epsilon for float comparison to avoid precision issues)
        const double EPSILON = 0.0001;
        short moving = (forward != 0 || right != 0 || up != 0);
        short rotating = (rot_dir != 0);
        
        int forward_changed = (fabs(forward - prev_forward) > EPSILON);
        if(forward_changed)
        printf("Forward: %.f (prev: %.f)\n", forward, prev_forward);
        int right_changed = (fabs(right - prev_right) > EPSILON);
        int up_changed = (fabs(up - prev_up) > EPSILON);
        int rot_changed = (rot_dir != prev_rot_dir);
        int stop_moving = (prev_moving && !moving);
        int stop_rotating = (prev_rotating && !rotating);

        if (forward_changed || right_changed || up_changed || rot_changed || stop_moving || stop_rotating) {
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

    // Note: atexit(cleanup_all) handles all cleanup automatically
    printf("\nClient terminated.\n");
    return 0;
}
