#ifndef GAME_H
#define GAME_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <stdint.h>

// Constants
#define WIDTH 400
#define HEIGHT 220
#define FOV 100.0
#define PI 3.14159265359
#define FRAME_INTERVAL_NS (6060606L)  // 165 FPS

// Movement and rotation speeds (per second)
#define MOVE_SPEED 8.0  // 8 units a second
#define ROTATION_SPEED 2.09439510239 //2 pi / 3 

// Projectile config
#define PROJECTILE_TRAVEL_DISTANCE 100.0
#define PROJECTILE_TRAVEL_SPEED 12.0

// Type definitions
typedef struct {
    double x, y, z;
} Vec3;

typedef struct {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
} Color;

typedef struct {
    Vec3 position;
    double width, height, depth;
    double rotation_y;
    Color color;
} Cuboid; //59 Bytes

typedef struct {
    Vec3 position;
    double length;
    double rotation_y;
    Color color;
} Gun; //29 Bytes

typedef struct {
    Cuboid cuboid;
    Gun gun;
    short hp;
} Player; // 90 Bytes

typedef struct {
    Vec3 position;
    double side;
} Square;

typedef struct {
    Vec3 position;
    double length;
    double rotation_y;
    Color color;
    double distance_left;
    double speed;
    short ownerID;
    short collided;
} Projectile; // 72 Bytes

typedef struct {
    Projectile projectiles[64];
    short head;
    short tail;
} ProjectileQueue;

typedef struct {
    Vec3 position;
    double yaw;   // Rotation around Y axis
} Camera;

typedef struct {
    char pixels[HEIGHT][WIDTH];
    float zbuffer[HEIGHT][WIDTH];
    Color color[HEIGHT][WIDTH];
} FrameBuffer;

// Global variable declarations (extern)
extern Player players[16];
extern ProjectileQueue projectileQueue;
extern Camera playerCamera;
extern FrameBuffer screen;
extern FrameBuffer antiAliased;
extern char frameString[7 + HEIGHT * WIDTH * (21) + HEIGHT + 1 + 4];
extern unsigned long frameStringSize;
extern short activeMSAA;

// Function declarations
void setActiveMSAA(short activate);
void initProjectileQueue(ProjectileQueue *queue);
void enqueueProjectile(ProjectileQueue *queue, Projectile proj);
void dequeueProjectile(ProjectileQueue *queue);
void initPlayers(Player * players, short numPlayers);
Vec3 rotateY(Vec3 v, double theta);
int projectileCuboidCollision(Projectile proj, Cuboid cuboid);
Color blend(Color dst, Color src, float a);
void drawLineZ_Wu(Vec3 c0, Vec3 c1, int width, int height, Color lineColor, FrameBuffer *frame);
void drawLineZ(Vec3 c0, Vec3 c1, int width, int height, Color lineColor, FrameBuffer * frame);
void drawCuboid(const Cuboid cuboid);
void drawGun(const Gun gun);
void drawPlayer(const Player player);
void drawAllPlayers();
void movePlayer(short playerID, double forward, double right, double up, short globalCoordinates);
void rotatePlayer(short playerID, double delta_yaw);
void changePlayerColor(short playerID, Color newColor);
void drawProjectiles(ProjectileQueue *queue);
void printProjectiles(ProjectileQueue *queue);
void shootProjectile(short playerID, ProjectileQueue *queue);
// Collision callback: (projectile_index, hit_player_id)
typedef void (*CollisionCallback)(short, short);
void updateProjectiles(ProjectileQueue *queue, Player *players, short numPlayers, double deltaTime, short checkCollisions, CollisionCallback onCollision);
void clearScreen();
void generateframeString();
void applyAA();
void render();
void ctrlcHandler(int signum);
void moveCamera(Vec3 newPosition);
void setCameraRotation(double theta);

// ============== NETWORK PROTOCOL ==============

// Command codes
#define CMD_MOVE_ROTATE     0   // Client -> Server: Move/rotate request
#define CMD_SHOOT           1   // Client -> Server: Shoot request
#define CMD_LOGIN           2   // Client -> Server: Login request
#define CMD_MOVE_EXECUTED   3   // Server -> Client: Move command executed
#define CMD_SHOOT_EXECUTED  4   // Server -> Client: Shoot command executed
#define CMD_PROJECTILE_HIT  5   // Server -> Client: Projectile collision
#define CMD_NEW_PLAYER      6   // Server -> Client: New player joined
#define CMD_ONBOARDING      7   // Server -> Client: Full game state for new player
#define CMD_LOGIN_DENIED    8   // Server -> Client: Server full
#define CMD_PING            9   // Ping
#define CMD_PONG            10  // Pong
#define CMD_TERMINATE       11  // Terminate
#define CMD_PLAYER_KILLED   12  // Server -> Client: Player disconnected/killed
// Chunked onboarding (avoids large single UDP datagrams / fragmentation issues)
#define CMD_ONBOARDING_BEGIN 13 // Server -> Client: Start chunked onboarding
#define CMD_ONBOARDING_CHUNK 14 // Server -> Client: Chunk of onboarding payload
#define CMD_ONBOARDING_END   15 // Server -> Client: End chunked onboarding (optional)

// Command payload structures (packed to ensure consistent sizes across platforms)
#pragma pack(push, 1)

// CMD_MOVE_ROTATE payload (Client -> Server)
typedef struct {
    double forward;             // Forward movement component
    double right;               // Right movement component
    double up;                  // Up movement component
    short rotation_direction;   // 0=stop, 1=right, 2=left
} CmdMoveRotate;

// CMD_SHOOT payload - no additional data needed, just the command byte

// CMD_LOGIN payload - no additional data needed, just the command byte

// CMD_MOVE_EXECUTED payload (Server -> Client)
typedef struct {
    short playerID;
    Vec3 position;
    double rotation_y;
    double forward;
    double right;
    double up;
    short rotation_direction;
} CmdMoveExecuted;

// CMD_SHOOT_EXECUTED payload (Server -> Client)
typedef struct {
    short playerID;
    Vec3 gun_position;
    double gun_rotation_y;
} CmdShootExecuted;

// CMD_PROJECTILE_HIT payload (Server -> Client)
typedef struct {
    short projectile_index;
    short hit_playerID;
} CmdProjectileHit;

// CMD_NEW_PLAYER payload (Server -> Client)
typedef struct {
    short playerID;
    Player player;
} CmdNewPlayer;

// CMD_ONBOARDING payload (Server -> Client)
typedef struct {
    short assigned_playerID;
    Player players[16];
    ProjectileQueue projectileQueue;
} CmdOnboarding;

// CMD_LOGIN_DENIED - no additional data

// CMD_PLAYER_KILLED payload (Server -> Client)
typedef struct {
    short playerID;
} CmdPlayerKilled;

// CMD_ONBOARDING_BEGIN payload (Server -> Client)
// total_size is the number of bytes of the CmdOnboarding payload (no command byte).
// chunk_size is the max chunk payload size used by the server.
typedef struct {
    short assigned_playerID;
    uint32_t total_size;
    uint16_t chunk_size;
} CmdOnboardingBegin;

// CMD_ONBOARDING_CHUNK payload header (Server -> Client)
// Followed by data_len bytes.
typedef struct {
    uint32_t offset;
    uint16_t data_len;
} CmdOnboardingChunkHeader;

#pragma pack(pop)

// Player-subscriber mapping for O(1) lookup
typedef struct {
    short subscriber_index;     // Index in subscribers array (-1 if not connected)
    short active;               // Whether this player slot is active
    double last_shoot_time;     // Timestamp of last successful shoot (for 4s cooldown)
    double forward;             // Current forward movement
    double right;               // Current right movement  
    double up;                  // Current up movement
    short rotation_direction;   // Current rotation direction
} PlayerConnection;

#endif // GAME_H
