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

#define WIDTH 80
#define HEIGHT 80
#define FOV 100.0
#define PI 3.14159265359
#define FRAME_INTERVAL_NS (16666667L)  // 100ms = 10 FPS

typedef struct {
    double x, y, z;
} Vec3;

typedef struct {
    double x, y;
} Vec2;

typedef struct {
    Vec3 position;
    double width, height, depth;
    double rotation_y;
} Cuboid;

typedef struct {
    Vec3 position;
    double side;
} Square;

typedef struct {
    Vec3 position;
    double yaw;   // Rotation around Y axis
} Camera;

typedef struct {
    char pixels[HEIGHT][WIDTH];
    float zbuffer[HEIGHT][WIDTH];
} FrameBuffer;

FrameBuffer screen;

void drawLineZ(
    int x0, int y0, float z0,
    int x1, int y1, float z1,
    int width, int height,
    char screen[height][width],
    float zbuffer[height][width]
) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int steps = (dx > dy) ? dx : dy; // total steps for interpolation
    int step = 0;

    int x = x0;
    int y = y0;

    while (1) {
        // Interpolate Z
        float t = (steps == 0) ? 0.0f : (float)step / steps;
        float z = z0 + t * (z1 - z0);

        // Draw pixel if it's in bounds and closer than current zbuffer
        if (x >= 0 && x < width && y >= 0 && y < height) {
            if (z < zbuffer[y][x]) {
                screen[y][x] = '*';      // or whatever pixel value
                zbuffer[y][x] = z;       // update depth
            }
        }

        if (x == x1 && y == y1)
            break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }

        step++;
    }
}

void drawCuboid(const Cuboid cuboid) {
    // Generate corner vectors
    Vec3 corners[8];
    double hw = cuboid.width / 2.0; // half width
    double hh = cuboid.height / 2.0; // half height
    double hd = cuboid.depth / 2.0; // half depth
    corners[0] = (Vec3){-hw + cuboid.position.x, -hh + cuboid.position.y, -hd + cuboid.position.z};
    corners[1] = (Vec3){ hw + cuboid.position.x, -hh + cuboid.position.y, -hd + cuboid.position.z};
    corners[2] = (Vec3){ hw + cuboid.position.x,  hh + cuboid.position.y, -hd + cuboid.position.z};
    corners[3] = (Vec3){-hw + cuboid.position.x,  hh + cuboid.position.y, -hd + cuboid.position.z};
    corners[4] = (Vec3){-hw + cuboid.position.x, -hh + cuboid.position.y,  hd + cuboid.position.z};
    corners[5] = (Vec3){ hw + cuboid.position.x, -hh + cuboid.position.y,  hd + cuboid.position.z};
    corners[6] = (Vec3){ hw + cuboid.position.x,  hh + cuboid.position.y,  hd + cuboid.position.z};
    corners[7] = (Vec3){-hw + cuboid.position.x,  hh + cuboid.position.y,  hd + cuboid.position.z};
    
    // Projection parameters
    double fov_rad = FOV * PI / 180.0;
    double fov_scale = 1.0 / tan(fov_rad / 2.0);
    double aspect = (double)WIDTH / HEIGHT;
    
    // Draw edges
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for(int i = 0; i < 12; i++) {
        Vec3 c0 = corners[edges[i][0]];
        Vec3 c1 = corners[edges[i][1]];
        
        // Skip if behind camera
        if (c0.z <= 0.1 || c1.z <= 0.1) continue;
        
        // Proper perspective projection with FOV
        int x0 = (int)((c0.x / c0.z) * fov_scale * aspect * (WIDTH / 2) + WIDTH / 2);
        int y0 = (int)(-(c0.y / c0.z) * fov_scale * (HEIGHT / 2) + HEIGHT / 2);
        int x1 = (int)((c1.x / c1.z) * fov_scale * aspect * (WIDTH / 2) + WIDTH / 2);
        int y1 = (int)(-(c1.y / c1.z) * fov_scale * (HEIGHT / 2) + HEIGHT / 2);
        
        drawLineZ(
            x0, y0, (float)c0.z,
            x1, y1, (float)c1.z,
            WIDTH, HEIGHT,
            screen.pixels,
            (float (*)[WIDTH])screen.zbuffer
        );
    }
}

void clearScreen() {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            screen.pixels[y][x] = ' ';
            screen.zbuffer[y][x] = 1000.0f; // Initialize zbuffer to farthest depth
        }
    }
}

void render(){
    for(int y = 0; y < HEIGHT; y++) {
        for(int x = 0; x < WIDTH; x++) {
            putchar(screen.pixels[y][x]);
        }
        putchar('\n');
    }
}

int main() {
    printf("Hello\n");
    Cuboid cuboid = {
        .position = {0.0, 0.0, 0.0},
        .width = 2.0,
        .height = 2.0,
        .depth = 2.0,
        .rotation_y = 0.0
    };
    for (int i = 0; i < 1000; i++) {
        system("clear");
        clearScreen();
        cuboid.position.z = 0 + 0.005 * i;
        drawCuboid(cuboid);
        render();
        fflush(stdout);
        usleep(1000);
    }
    drawCuboid(cuboid);
    render();
    return 0;
}
