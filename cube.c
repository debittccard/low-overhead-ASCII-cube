#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define W 32
#define H 16
#define PITCH (W + 1)
#define N (H * PITCH)
#define SIDE_LENGTH 3
#define CAM_DIST 11
#define ZOOM 30
#define ROTX 2
#define ROTY 1
#define FRAMETIME 16670

static char buf[N];

/* Fixed-point sine table for 0-90 degrees. */
static const int16_t sin_quad[65] = {
    0, 6, 12, 18, 25, 31, 37, 43, 49, 56, 62, 68, 74, 80, 86, 92,
    97, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167, 171, 176,
    180, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225, 228, 231, 233,
    236, 238, 240, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 254, 255, 255, 256
};

static int16_t isin(int16_t a) {
    a &= 255;
    if (a < 64) return sin_quad[a];
    if (a < 128) return sin_quad[128 - a];
    if (a < 192) return -sin_quad[a - 128];
    return -sin_quad[256 - a];
}

static int16_t icos(int16_t a) { return isin(a + 64); }

static const int8_t verts[8][3] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1,1},  {1,-1,1},  {1,1,1},  {-1,1,1}
};
static const int8_t faces[6][4] = {
    {0, 1, 2, 3}, {1, 5, 6, 2}, {5, 4, 7, 6},
    {4, 0, 3, 7}, {4, 5, 1, 0}, {3, 2, 6, 7}
};

static const char face_char[6] = {'@', '#', '*', '+', '=', '%'};

#define SWAP(a, b) { int32_t t = a; a = b; b = t; }

/* Fill a horizontal span with one character. */
static void draw_span(int y, int32_t xA, int32_t xB, char c) {
    if (xA > xB) SWAP(xA, xB);

    int x_start = xA >> 16;
    int x_end   = xB >> 16;
    if (x_start >= W || x_end < 0) return;

    if (x_start < 0) x_start = 0;
    if (x_end >= W)  x_end = W - 1;

    int offset = y * PITCH;
    for (int x = x_start; x <= x_end; x++) {
        buf[offset + x] = c;
    }
}

/* Rasterize a flat-shaded triangle. */
static void rasterize_tri(int x1, int y1,
                          int x2, int y2,
                          int x3, int y3,
                          char c) {
    if (y1 > y2) { SWAP(x1, x2); SWAP(y1, y2); }
    if (y1 > y3) { SWAP(x1, x3); SWAP(y1, y3); }
    if (y2 > y3) { SWAP(x2, x3); SWAP(y2, y3); }

    int total_height = y3 - y1;
    if (total_height == 0) return;

    int32_t dx13 = ((int32_t)(x3 - x1) << 16) / total_height;
    int32_t ax = x1 << 16;
    int32_t bx = ax;

    int top_height = y2 - y1;
    if (top_height > 0) {
        int32_t dx12 = ((int32_t)(x2 - x1) << 16) / top_height;
        for (int y = y1; y < y2; y++) {
            if (y >= 0 && y < H) draw_span(y, ax, bx, c);
            ax += dx13;
            bx += dx12;
        }
    }

    int bot_height = y3 - y2;
    if (bot_height > 0) {
        int32_t dx23 = ((int32_t)(x3 - x2) << 16) / bot_height;
        bx = x2 << 16;
        for (int y = y2; y <= y3; y++) {
            if (y >= 0 && y < H) draw_span(y, ax, bx, c);
            ax += dx13;
            bx += dx23;
        }
    }
}

static void clear_buffer() {
    for (int y = 0; y < H; y++) {
        memset(&buf[y * PITCH], ' ', W);
        buf[y * PITCH + W] = '\n';
    }
}

int main() {
    write(1, "\x1b[2J\x1b[?25l", 10);

    int16_t A = 0, B = 0;
    int16_t S = SIDE_LENGTH * 256;
    int16_t D = CAM_DIST * 256;
    int16_t K = ZOOM;
    int16_t px[8], py[8];

    while (1) {
        int16_t ca = icos(A), sa = isin(A);
        int16_t cb = icos(B), sb = isin(B);

        clear_buffer();

        /* Rotate, translate, and project each vertex. */
        for (int i = 0; i < 8; i++) {
            int32_t X = verts[i][0] * S, Y = verts[i][1] * S, Z = verts[i][2] * S;
            int16_t x1 = ((int32_t)X * cb - (int32_t)Z * sb) >> 8;
            int16_t z1 = ((int32_t)X * sb + (int32_t)Z * cb) >> 8;
            int16_t y1 = ((int32_t)Y * ca - (int32_t)z1 * sa) >> 8;
            int16_t z2 = (((int32_t)Y * sa + (int32_t)z1 * ca) >> 8) + D;

            if (z2 < 1) z2 = 1;

            px[i] = W / 2 + ((int32_t)x1 * K) / z2;
            py[i] = H / 2 + ((int32_t)y1 * K) / (z2 * 2);
        }

        /* Cull backfaces and draw the visible faces. */
        for (int f = 0; f < 6; f++) {
            int v0 = faces[f][0], v1 = faces[f][1], v2 = faces[f][2], v3 = faces[f][3];

            /* Use the projected winding to cull faces. */
            if ((px[v1]-px[v0]) * (py[v2]-py[v0]) -
                (py[v1]-py[v0]) * (px[v2]-px[v0]) < 0)
                continue;

            /* Split each quad into two triangles. */
            rasterize_tri(px[v0], py[v0],
                          px[v1], py[v1],
                          px[v2], py[v2], face_char[f]);

            rasterize_tri(px[v0], py[v0],
                          px[v2], py[v2],
                          px[v3], py[v3], face_char[f]);
        }

        write(1, "\x1b[H", 3);
        write(1, buf, N);

        A += ROTX; B += ROTY;
        usleep(FRAMETIME);
    }
    return 0;
}