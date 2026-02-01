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

// Constants
#define WIDTH 400
#define HEIGHT 220
#define FOV 100.0
#define PI 3.14159265359
#define FRAME_INTERVAL_NS (6060606L)  // 165 FPS

// Movement and rotation speeds (per second)
#define MOVE_SPEED 1.0  // 1 unit a second
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
void movePlayer(short playerID, double forward, double right, double up, short globalCoordinates);
void rotatePlayer(short playerID, double delta_yaw);
void changePlayerColor(short playerID, Color newColor);
void drawProjectiles(ProjectileQueue *queue);
void printProjectiles(ProjectileQueue *queue);
void shootProjectile(short playerID, ProjectileQueue *queue);
void updateProjectiles(ProjectileQueue *queue, Player *players, short numPlayers, double deltaTime);
void clearScreen();
void generateframeString();
void applyAA();
void render();
void ctrlcHandler(int signum);
void moveCamera(Vec3 newPosition);
void setCameraRotation(double theta);

#endif // GAME_H
