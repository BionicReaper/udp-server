#include "game.h"

// Global variable definitions
Player players[16];
ProjectileQueue projectileQueue;
Camera playerCamera = {
    (Vec3) {0.0, 0.0, 0.0},
    0.0
};
FrameBuffer screen;
FrameBuffer antiAliased;
char frameString[7 + HEIGHT * WIDTH * (21) + HEIGHT + 1 + 4];
unsigned long frameStringSize;
short activeMSAA = 1;

void setActiveMSAA(short activate){
    activeMSAA = activate;
}

void initProjectileQueue(ProjectileQueue *queue) {
    queue->head = 0;
    queue->tail = 0;
}

void enqueueProjectile(ProjectileQueue *queue, Projectile proj) {
    queue->projectiles[queue->tail] = proj;
    queue->tail = (queue->tail + 1) % 64;
}

void dequeueProjectile(ProjectileQueue *queue) {
    if (queue->head != queue->tail) {
        queue->head = (queue->head + 1) % 64;
    }
}

void initPlayers(Player * players, short numPlayers){
    // Set hp to 0
    for(short i = 0; i < numPlayers; i++){
        players->hp = 0;
    }
}

// Rotate a Vec3 around Y-axis by angle (radians)
Vec3 rotateY(Vec3 v, double theta) {
    Vec3 result;
    result.x = v.z * sin(theta) + v.x * cos(theta);
    result.z = v.z * cos(theta) - v.x * sin(theta);
    result.y = v.y; // Y stays the same
    return result;
}

int projectileCuboidCollision(Projectile proj, Cuboid cuboid) {
    // Generate corner vectors
    Vec3 cuboidCorners[2];
    double hw = cuboid.width / 2.0; // half width
    double hh = cuboid.height / 2.0; // half height
    double hd = cuboid.depth / 2.0; // half depth
    cuboidCorners[0] = (Vec3){-hw, -hh , -hd };
    cuboidCorners[1] = (Vec3){ hw,  hh ,  hd };

    Vec3 projectileCorners[2];
    projectileCorners[0] = (Vec3){
        0.0,
        0.0,
        -proj.length / 2.0
    };
    projectileCorners[1] = (Vec3){
        0.0,
        0.0,
        proj.length / 2.0
    };

    for(int i = 0; i < 2; i++) {
        projectileCorners[i] = rotateY(projectileCorners[i], proj.rotation_y - cuboid.rotation_y);
        projectileCorners[i].x += proj.position.x - cuboid.position.x;
        projectileCorners[i].y += proj.position.y - cuboid.position.y;
        projectileCorners[i].z += proj.position.z - cuboid.position.z;
    }

    double x_t_max, x_t_min, y_t_max, y_t_min, z_t_max, z_t_min;

    if(projectileCorners[1].x - projectileCorners[0].x == 0.0) {
        // Parallel to X axis
        if (projectileCorners[0].x < -hw || projectileCorners[0].x > hw)
            return 0; // No collision
        x_t_min = 0.0;
        x_t_max = 1.0;
    } else {
        x_t_max = (hw - projectileCorners[0].x) / (projectileCorners[1].x - projectileCorners[0].x);
        x_t_min = (-hw - projectileCorners[0].x) / (projectileCorners[1].x - projectileCorners[0].x);
        
        // Ensure min < max
        if (x_t_min > x_t_max) { double temp = x_t_min; x_t_min = x_t_max; x_t_max = temp; }
    }

    if(projectileCorners[1].y - projectileCorners[0].y == 0.0) {
        // Parallel to Y axis
        if (projectileCorners[0].y < -hh || projectileCorners[0].y > hh)
            return 0; // No collision
        y_t_min = 0.0;
        y_t_max = 1.0;
    } else {
        y_t_max = (hh - projectileCorners[0].y) / (projectileCorners[1].y - projectileCorners[0].y);
        y_t_min = (-hh - projectileCorners[0].y) / (projectileCorners[1].y - projectileCorners[0].y);

        // Ensure min < max
        if (y_t_min > y_t_max) { double temp = y_t_min; y_t_min = y_t_max; y_t_max = temp; }
    }

    if(projectileCorners[1].z - projectileCorners[0].z == 0.0) {
        // Parallel to Z axis
        if (projectileCorners[0].z < -hd || projectileCorners[0].z > hd)
            return 0; // No collision
        z_t_min = 0.0;
        z_t_max = 1.0;
    } else {
        z_t_max = (hd - projectileCorners[0].z) / (projectileCorners[1].z - projectileCorners[0].z);
        z_t_min = (-hd - projectileCorners[0].z) / (projectileCorners[1].z - projectileCorners[0].z);
        
        // Ensure min < max
        if (z_t_min > z_t_max) { double temp = z_t_min; z_t_min = z_t_max; z_t_max = temp; }
    }
    
    // Check if all three intervals intersect
    double overall_t_min = fmax(fmax(x_t_min, y_t_min), z_t_min);
    double overall_t_max = fmin(fmin(x_t_max, y_t_max), z_t_max);
    
    return (overall_t_min <= overall_t_max) ? 1 : 0;
}

Color blend(Color dst, Color src, float a) {
    Color out;
    out.red   = dst.red   + (int)((src.red   - dst.red)   * a);
    out.green = dst.green + (int)((src.green - dst.green) * a);
    out.blue  = dst.blue  + (int)((src.blue  - dst.blue)  * a);
    return out;
}

void drawLineZ_Wu(
    Vec3 c0, Vec3 c1,
    int width, int height, Color lineColor,
    FrameBuffer *frame
) {
    // Adjust for camera
    c0.x -= playerCamera.position.x;
    c0.y -= playerCamera.position.y;
    c0.z -= playerCamera.position.z;

    c0 = rotateY(c0, -playerCamera.yaw);

    c1.x -= playerCamera.position.x;
    c1.y -= playerCamera.position.y;
    c1.z -= playerCamera.position.z;

    c1 = rotateY(c1, -playerCamera.yaw);

    // Skip if both points are behind camera
    if (c0.z <= 0 && c1.z <= 0) return;     
    
    double fov_rad = FOV * PI / 180.0;
    double fov_scale = 1.0 / tan(fov_rad / 2.0);
    double aspect = (double)WIDTH / HEIGHT;

    float scaledX0 = c0.x == 0.0? 0.0 : (c0.x / fabs(c0.z));
    float scaledY0 = c0.y == 0.0? 0.0 : (c0.y / fabs(c0.z));
    float scaledX1 = c1.x == 0.0? 0.0 : (c1.x / fabs(c1.z));
    float scaledY1 = c1.y == 0.0? 0.0 : (c1.y / fabs(c1.z));

    float px0 = (scaledX0) * fov_scale * (WIDTH / 2) + WIDTH / 2;
    float py0 = -(scaledY0) * fov_scale * aspect * (HEIGHT / 2) + HEIGHT / 2;
    float px1 = (scaledX1) * fov_scale  * (WIDTH / 2) + WIDTH / 2;
    float py1 = -(scaledY1) * fov_scale * aspect * (HEIGHT / 2) + HEIGHT / 2;

    float z0 = c0.z;
    float z1 = c1.z;

    char  (*screen)[WIDTH]  = frame->pixels;
    float (*zbuffer)[WIDTH] = frame->zbuffer;
    Color (*color)[WIDTH]   = frame->color;

    int steep = fabs(py1 - py0) > fabs(px1 - px0);
    if (steep) {
        float t;
        t = px0; px0 = py0; py0 = t;
        t = px1; px1 = py1; py1 = t;
    }
    if (px0 > px1) {
        float t;
        t = px0; px0 = px1; px1 = t;
        t = py0; py0 = py1; py1 = t;
        t = z0;  z0  = z1;  z1  = t;
    }

    float dx = px1 - px0;
    float dy = py1 - py0;
    float gradient = (dx == 0) ? 0 : dy / dx;

    // Handle infinity when casting to int
    int xStart, xEnd;
    if (isinf(px0) && px0 < 0) xStart = INT_MIN;
    else if (isinf(px0) && px0 > 0) xStart = INT_MAX;
    else xStart = (int)ceilf(px0);
    
    if (isinf(px1) && px1 < 0) xEnd = INT_MIN;
    else if (isinf(px1) && px1 > 0) xEnd = INT_MAX;
    else xEnd = (int)floorf(px1);

    if(xStart < 0) xStart = 0;
    if(xStart > (steep ? height : width)) xStart = (steep ? height : width);
    if(xEnd < 0) xEnd = 0;
    if(xEnd > (steep ? height : width)) xEnd = (steep ? height : width);
    for (int x = xStart; x <= xEnd; x++) {
        float t = (dx == 0) ? 0.0f : (x - px0) / dx;
        float z = z0 + t * (z1 - z0);
        if (z <= 0.0f) continue;

        float y = py0 + gradient * (x - px0);
        int yInt = (int)floorf(y);
        float frac = y - yInt;

        for (int k = 0; k < 2; k++) {
            int yy = yInt + k;
            float a = (k == 0) ? (1.0f - frac) : frac;

            int sx = steep ? yy : x;
            int sy = steep ? x  : yy;

            if (sx < 0 || sx >= width || sy < 0 || sy >= height)
                continue;

            if (z < zbuffer[sy][sx]) {
                color[sy][sx] = blend(color[sy][sx], lineColor, a);
                zbuffer[sy][sx] = z;
                screen[sy][sx] = ' ';
            }
        }
    }
}



void drawLineZ(
    Vec3 c0, Vec3 c1,
    int width, int height, Color lineColor,
    FrameBuffer * frame
) {
    // Adjust for camera
    c0.x -= playerCamera.position.x;
    c0.y -= playerCamera.position.y;
    c0.z -= playerCamera.position.z;

    c0 = rotateY(c0, -playerCamera.yaw);

    c1.x -= playerCamera.position.x;
    c1.y -= playerCamera.position.y;
    c1.z -= playerCamera.position.z;

    c1 = rotateY(c1, -playerCamera.yaw);

    // Skip if both points are behind camera
    if (c0.z <= 0 && c1.z <= 0) return;

    // Projection parameters
    double fov_rad = FOV * PI / 180.0;
    double fov_scale = 1.0 / tan(fov_rad / 2.0);
    double aspect = (double)WIDTH / HEIGHT;

    // Proper perspective projection with FOV
    int x0 = (int)((c0.x / c0.z) * fov_scale * (WIDTH / 2) + WIDTH / 2);
    int y0 = (int)(-(c0.y / c0.z) * fov_scale * aspect * (HEIGHT / 2) + HEIGHT / 2);
    int x1 = (int)((c1.x / c1.z) * fov_scale * (WIDTH / 2) + WIDTH / 2);
    int y1 = (int)(-(c1.y / c1.z) * fov_scale * aspect * (HEIGHT / 2) + HEIGHT / 2);
    float z0 = c0.z;
    float z1 = c1.z;

    // Clamp to reasonable bounds to prevent infinite loops on extreme values
    if (x0 < -width) x0 = -width;
    if (x0 > width * 2) x0 = width * 2;
    if (y0 < -height) y0 = -height;
    if (y0 > height * 2) y0 = height * 2;
    if (x1 < -width) x1 = -width;
    if (x1 > width * 2) x1 = width * 2;
    if (y1 < -height) y1 = -height;
    if (y1 > height * 2) y1 = height * 2;

    char (*screen)[WIDTH] = frame->pixels;
    float (*zbuffer)[WIDTH] = frame->zbuffer;
    Color (*color)[WIDTH] = frame->color;

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

        // Skip if behind camera
        if (z <= 0.5) {
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
            continue;
        }

        // Draw pixel if it's in bounds and closer than current zbuffer
        if (x >= 0 && x < width && y >= 0 && y < height) {
            if (z < zbuffer[y][x] && z > 0) {
                screen[y][x] = ' ';      // or whatever pixel value
                zbuffer[y][x] = z;       // update depth
                color[y][x] = lineColor; // update color
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
    corners[0] = (Vec3){-hw, -hh , -hd };
    corners[1] = (Vec3){ hw, -hh , -hd };
    corners[2] = (Vec3){ hw,  hh , -hd };
    corners[3] = (Vec3){-hw,  hh , -hd };
    corners[4] = (Vec3){-hw, -hh ,  hd };
    corners[5] = (Vec3){ hw, -hh ,  hd };
    corners[6] = (Vec3){ hw,  hh ,  hd };
    corners[7] = (Vec3){-hw,  hh ,  hd };

    for(int i = 0; i < 8; i++) {
        corners[i] = rotateY(corners[i], cuboid.rotation_y);
    }
    for(int i = 0; i < 8; i++) {
        corners[i].x += cuboid.position.x;
        corners[i].y += cuboid.position.y;
        corners[i].z += cuboid.position.z;
    }
    
    // Draw edges
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    for(int i = 0; i < 12; i++) {
        Vec3 c0 = corners[edges[i][0]];
        Vec3 c1 = corners[edges[i][1]];
        if(activeMSAA)
            drawLineZ_Wu(
                c0, c1,
                WIDTH, HEIGHT, cuboid.color,
                &screen
            );
        else
            drawLineZ(
                c0, c1,
                WIDTH, HEIGHT, cuboid.color,
                &screen
            );
    }
}

void drawGun(const Gun gun) {
    if(activeMSAA)
        drawLineZ_Wu(
        (Vec3){
            gun.position.x,
            gun.position.y,
            gun.position.z
        },
        (Vec3){
            gun.position.x + sin(gun.rotation_y) * gun.length,
            gun.position.y,
            gun.position.z + cos(gun.rotation_y) * gun.length
        },
        WIDTH, HEIGHT, gun.color,
        &screen
    );
    else
        drawLineZ(
            (Vec3){
                gun.position.x,
                gun.position.y,
                gun.position.z
            },
            (Vec3){
                gun.position.x + sin(gun.rotation_y) * gun.length,
                gun.position.y,
                gun.position.z + cos(gun.rotation_y) * gun.length
            },
            WIDTH, HEIGHT, gun.color,
            &screen
        );
}

void drawPlayer(const Player player) {
    drawCuboid(player.cuboid);
    drawGun(player.gun);
    
    // Draw extra cuboids at top left and top right
    double offsetX = player.cuboid.width * 0.25;
    double offsetY = player.cuboid.height * 0.25;
    double smallSize = player.cuboid.width * 0.2;
    
    // Top left cuboid (in local space before rotation, use Z for left/right)
    Vec3 topLeftLocal = {-offsetX, offsetY, 0};
    Vec3 topLeftRotated = rotateY(topLeftLocal, player.cuboid.rotation_y);
    Cuboid topLeft = {
        .position = {
            player.cuboid.position.x + topLeftRotated.x,
            player.cuboid.position.y + topLeftRotated.y,
            player.cuboid.position.z + topLeftRotated.z
        },
        .width = smallSize,
        .height = smallSize,
        .depth = smallSize,
        .rotation_y = player.cuboid.rotation_y,
        .color = player.cuboid.color
    };
    
    // Top right cuboid (in local space before rotation, use Z for left/right)
    Vec3 topRightLocal = {offsetX, offsetY, 0};
    Vec3 topRightRotated = rotateY(topRightLocal, player.cuboid.rotation_y);
    Cuboid topRight = {
        .position = {
            player.cuboid.position.x + topRightRotated.x,
            player.cuboid.position.y + topRightRotated.y,
            player.cuboid.position.z + topRightRotated.z
        },
        .width = smallSize,
        .height = smallSize,
        .depth = smallSize,
        .rotation_y = player.cuboid.rotation_y,
        .color = player.cuboid.color
    };
    
    drawCuboid(topLeft);
    drawCuboid(topRight);
}

void drawAllPlayers() {
    for (short i = 0; i < 16; i++) {
        if (players[i].hp > 0) {
            drawPlayer(players[i]);
        }
    }
}

void movePlayer(short playerID, double forward, double right, double up, short globalCoordinates) {
    // Calculate forward and right vectors based on cuboid rotation (forward is z-axis)
    double yaw = globalCoordinates ? 0 : players[playerID].cuboid.rotation_y;
    Vec3 forwardVec = {
        .x = sin(yaw),
        .y = 0,
        .z = cos(yaw)
    };
    Vec3 rightVec = {
        .x = cos(yaw),
        .y = 0,
        .z = -sin(yaw)
    };

    // Update position
    players[playerID].cuboid.position.x += forward * forwardVec.x + right * rightVec.x;
    players[playerID].cuboid.position.y += up;
    players[playerID].cuboid.position.z += forward * forwardVec.z + right * rightVec.z;

    // Update gun position to match cuboid
    players[playerID].gun.position = players[playerID].cuboid.position;
    players[playerID].gun.position.y -= players[playerID].cuboid.height / 4.0; // Adjust gun height
}

void rotatePlayer(short playerID, double delta_yaw) {
    players[playerID].cuboid.rotation_y += delta_yaw;
    players[playerID].gun.rotation_y += delta_yaw;
}

void changePlayerColor(short playerID, Color newColor) {
    players[playerID].cuboid.color = newColor;
    players[playerID].gun.color = newColor;
}

void drawProjectiles(ProjectileQueue *queue) {
    int index = queue->head;
    while (index != queue->tail) {
        Projectile proj = queue->projectiles[index];
        Vec3 start = {0, 0, proj.length * -0.5};
        Vec3 end = {0, 0, proj.length * 0.5};
        start = rotateY(start, proj.rotation_y);
        end = rotateY(end, proj.rotation_y);
        start.x += proj.position.x;
        start.y += proj.position.y;
        start.z += proj.position.z;
        end.x += proj.position.x;
        end.y += proj.position.y;
        end.z += proj.position.z;
        if(!proj.collided){
            if(activeMSAA)
                drawLineZ_Wu(
                    start, end,
                    WIDTH, HEIGHT, proj.color,
                    &screen
                );
            else
                drawLineZ(
                    start, end,
                    WIDTH, HEIGHT, proj.color,
                    &screen
                );
        }
        index = (index + 1) % 64;
    }
}

// Print all projectiles stats for debugging
void printProjectiles(ProjectileQueue *queue) {
    int index = queue->head;
    printf("Projectiles in queue:\n");
    while (index != queue->tail) {
        Projectile proj = queue->projectiles[index];
        printf("Projectile at index %d: Position(%.2f, %.2f, %.2f), Distance left: %.2f, Collided: %d\n",
               index, proj.position.x, proj.position.y, proj.position.z, proj.distance_left, proj.collided);
        index = (index + 1) % 64;
    }
}

void shootProjectile(short playerID, ProjectileQueue *queue) {
    Projectile proj;
    proj.position = players[playerID].gun.position;
    proj.length = 3.0;
    proj.rotation_y = players[playerID].gun.rotation_y;
    proj.color = (Color){255, 255, 255};
    proj.distance_left = PROJECTILE_TRAVEL_DISTANCE;
    proj.speed = PROJECTILE_TRAVEL_SPEED;
    proj.ownerID = playerID;
    proj.collided = 0;

    enqueueProjectile(queue, proj);
}

void updateProjectiles(ProjectileQueue *queue, Player *players, short numPlayers, double deltaTime, short checkCollisions, CollisionCallback onCollision) {
    short index = queue->head;
    while (index != queue->tail) {
        Projectile *proj = &queue->projectiles[index];
        if(proj->collided == 1){
            if(index == queue->head){
                dequeueProjectile(queue);
            }
        } else {
            // Move projectile
            proj->position.x += sin(proj->rotation_y) * proj->speed * deltaTime;
            proj->position.z += cos(proj->rotation_y) * proj->speed * deltaTime;
            proj->distance_left -= proj->speed * deltaTime;

            // Check for collisions with players (only on server)
            if (checkCollisions) {
                for (short i = 0; i < numPlayers; i++) {
                    if (i != proj->ownerID && players[i].hp > 0) {
                        if (projectileCuboidCollision(*proj, players[i].cuboid)) {
                            // Collision detected
                            players[i].hp -= 1; // Decrease HP by 1
                            unsigned char newRed = (players[i].cuboid.color.red <= 204) ? (players[i].cuboid.color.red + 51) : 255;
                            unsigned char newGreen = (players[i].cuboid.color.green >= 51) ? (players[i].cuboid.color.green - 51) : 0;
                            changePlayerColor(i, (Color){newRed, newGreen, 0});
                            // Remove projectile
                            proj->collided = 1;
                            // Call collision callback if provided
                            if (onCollision) {
                                onCollision(index, i);
                            }
                            break;
                        }
                    }
                }
            }

            // Remove projectile if it has traveled its maximum distance
            if (proj->distance_left <= 0) {
                dequeueProjectile(queue);
            }
        }
        index = (index + 1) % 64;
    }
}

void clearScreen() {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            screen.pixels[y][x] = ' ';
            screen.zbuffer[y][x] = 1000.0f; // Initialize zbuffer to farthest depth
            screen.color[y][x] = (Color){0,0,0}; // Default color black
        }
    }
}

void generateframeString() {
    int index = 0;
    frameString[index++] = '\033';
    frameString[index++] = '[';
    frameString[index++] = 'H';
    Color currentColor = {255, 255, 255};
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            //Apply color
            if(x == 0 && y == 0 ||
                screen.color[y][x].red != currentColor.red ||
                screen.color[y][x].green != currentColor.green ||
                screen.color[y][x].blue != currentColor.blue) {
                frameString[index++] = '\033';
                frameString[index++] = '[';
                frameString[index++] = '4';
                frameString[index++] = '8';
                frameString[index++] = ';';
                frameString[index++] = '2';
                frameString[index++] = ';';
                
                int r = screen.color[y][x].red, g = screen.color[y][x].green, b = screen.color[y][x].blue;
                
                // Red
                if (r >= 100) frameString[index++] = '0' + r / 100;
                if (r >= 10) frameString[index++] = '0' + (r / 10) % 10;
                frameString[index++] = '0' + r % 10;
                frameString[index++] = ';';
                
                // Green
                if (g >= 100) frameString[index++] = '0' + g / 100;
                if (g >= 10) frameString[index++] = '0' + (g / 10) % 10;
                frameString[index++] = '0' + g % 10;
                frameString[index++] = ';';
                
                // Blue
                if (b >= 100) frameString[index++] = '0' + b / 100;
                if (b >= 10) frameString[index++] = '0' + (b / 10) % 10;
                frameString[index++] = '0' + b % 10;
                
                frameString[index++] = 'm';
                currentColor = screen.color[y][x];
            }
            
            frameString[index++] = screen.pixels[y][x];
            frameString[index++] = ' ';
        }
        frameString[index++] = '\n';
    }
    // Reset color
    frameString[index++] = '\033';
    frameString[index++] = '[';
    frameString[index++] = '0';;
    frameString[index++] = 'm';
    // Null-terminate the string
    frameString[index] = '\0';
    frameStringSize = index;
}

void applyAA(){
    const int coefficients[] = {1, 2, 1,
                              2, 6, 2,
                              1, 2, 1};
    for(int y = 0; y < HEIGHT; y++){
        for(int x = 0; x < WIDTH; x++){
            int r = 0, g = 0, b = 0;
            int count = 0;
            for(int dy = -1; dy <= 1; dy++){
                for(int dx = -1; dx <= 1; dx++){
                    int nx = x + dx;
                    int ny = y + dy;
                    if(nx >= 0 && nx < WIDTH && ny >= 0 && ny < HEIGHT){
                        int currentCoef = coefficients[(dy + 1) * 3 + (dx + 1)];
                        r += screen.color[ny][nx].red * currentCoef;
                        g += screen.color[ny][nx].green * currentCoef;
                        b += screen.color[ny][nx].blue * currentCoef;
                        count += currentCoef;
                    }
                }
            }
            antiAliased.color[y][x].red = r / count;
            antiAliased.color[y][x].green = g / count;
            antiAliased.color[y][x].blue = b / count;
            antiAliased.pixels[y][x] = screen.pixels[y][x];
        }
    }
}

void render(){
    //clear screen
    write(STDOUT_FILENO, frameString, frameStringSize);
}

void ctrlcHandler(int signum) {
    // Show cursor again and revert to normal colors
    write(STDOUT_FILENO, "\033[?25h\033[0m", 7);
    exit(0);
}

void moveCamera(Vec3 newPosition){
    playerCamera.position = newPosition;
}

void setCameraRotation(double theta){
    playerCamera.yaw = theta;
}

// int main() {
//     // char stdin_buffer[7 + HEIGHT * WIDTH * (21) + HEIGHT + 1 + 4];
//     // setvbuf(stdout, stdin_buffer, _IOFBF, 7 + HEIGHT * WIDTH * (21) + HEIGHT + 1 + 4);

//     // Disable buffer
//     setvbuf(stdout, NULL, _IONBF, 0);
    
//     // Hide cursor
//     write(STDOUT_FILENO, "\033[?25l", 6);
//     signal(SIGINT, ctrlcHandler);

//     Cuboid cuboid = {
//         .position = {0.0, 0.0, 0.0},
//         .width = 2.0,
//         .height = 2.0,
//         .depth = 2.0,
//         .rotation_y = 0.0,
//         .color = {0, 255, 0}
//     };

//     Gun gun = {
//         .position = {0.0, cuboid.position.y - cuboid.height / 4.0, 0.0},
//         .length = 4.0,
//         .rotation_y = 0.0,
//         .color = {255, 0, 0}
//     };

//     initProjectileQueue(&projectileQueue);
//     initPlayers(&players[0], 16);

//     Player * player = &players[0];

//     Player * cameraPlayer = &players[1];

//     player->cuboid = cuboid;
//     player->gun = gun;
//     player->hp = 5;

//     cameraPlayer->cuboid = cuboid;
//     cameraPlayer->gun = gun;
//     cameraPlayer->hp = 5;

//     // Get current time
//     struct timespec next_frame, current, prev_frame;
//     clock_gettime(CLOCK_MONOTONIC, &next_frame);
//     prev_frame = next_frame;

//     // First frame - clear screen once at start
//     write(STDOUT_FILENO, "\033[2J\033[H", 7);
//     clearScreen();
//     drawPlayer(*player);
//     drawPlayer(*cameraPlayer);
//     generateframeString();
//     render();

//     short reden = 0;
//     short greenen = 1;
//     short blueen = 1;
    
//     for (int i = 0; i < 10000; i++) {

//         if(i == 1500){
//             setActiveMSAA(1);
//         }
//         // Calculate next frame time
//         next_frame.tv_nsec += FRAME_INTERVAL_NS;
//         while (next_frame.tv_nsec >= 1000000000L) {
//             next_frame.tv_sec++;
//             next_frame.tv_nsec -= 1000000000L;
//         }

//         // Wait until the next frame time
//         clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
        
//         // Get current time and calculate delta time
//         clock_gettime(CLOCK_MONOTONIC, &current);
//         double delta_time = (current.tv_sec - prev_frame.tv_sec) + 
//                            (current.tv_nsec - prev_frame.tv_nsec) / 1000000000.0;
//         prev_frame = current;
//         clearScreen();
        
//         // Apply FPS-invariant movement and rotation
//         movePlayer(0, MOVE_SPEED * delta_time, 0.0, 0.0, 1);
//         //rotatePlayer(player, ROTATION_SPEED * delta_time);

//         moveCamera(
//             (Vec3){
//                 player->cuboid.position.x + 15.0,
//                 player->cuboid.position.y,
//                 player->cuboid.position.z
//             }
//         );
//         setCameraRotation(-PI/2);

//         if(i % 1000 == 0 && i > 0){
//             shootProjectile(1, &projectileQueue);
//             //shootProjectile(0, &projectileQueue);
//         }
//         updateProjectiles(&projectileQueue, &players[0], 16, delta_time);
//         drawProjectiles(&projectileQueue);
//         drawPlayer(*player);
//         drawPlayer(*cameraPlayer);
//         generateframeString();
//         render();
//         // printf("Camera Player HP: %d\n", cameraPlayer->hp);
//         // printf("Enemy Player HP: %d Position:(%.2f, %.2f, %.2f) Rotation: %.2f\n", player->hp, player->cuboid.position.x, player->cuboid.position.y, player->cuboid.position.z, player->cuboid.rotation_y);
//         // printf("i: %d\n", i);
//         // printf("Projectile at index 0: Position(%.2f, %.2f, %.2f) Collided: %d\n",
//         //        projectileQueue.projectiles[0].position.x,
//         //        projectileQueue.projectiles[0].position.y,
//         //        projectileQueue.projectiles[0].position.z,
//         //        projectileQueue.projectiles[0].collided);

//         // if(hasCollided){
//         //     printf("Collided Cuboid info: %.2f, %.2f, %.2f, %.2f\n Collided Projectile info: %.2f, %.2f, %.2f, %.2f\n", collidedCuboid.position.x, collidedCuboid.position.y, collidedCuboid.position.z, collidedCuboid.rotation_y, collidedProjectile.position.x, collidedProjectile.position.y, collidedProjectile.position.z, collidedProjectile.rotation_y);
//         // } else {
//         //     printf("No collision detected this frame.\n\n");
//         // }
//     }
//     write(STDOUT_FILENO, "\033[?25h", 6); // Show cursor
//     return 0;
// }