// ff_math.h — matemática pura para ESP e aimbot (sem deps do jogo)
#pragma once

#include "../../Shared/include/ff_types.h"
#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

// ─── World-to-screen ──────────────────────────────────────────────────────────
// Unity usa matriz coluna-major.
// ViewProjection = Projection * View (fornecida pelo Camera.projectionMatrix * worldToCameraMatrix)
// screen_w / screen_h: resolução atual da view.
// out_x / out_y: coordenadas em pixels (0,0 = canto superior esquerdo).
// Retorna false se o ponto estiver atrás da câmera.
static inline bool world_to_screen(const Matrix4x4 *vp, const Vec3 *world,
                                   float screen_w, float screen_h,
                                   float *out_x, float *out_y) {
    // Multiplica pelo View-Projection (coluna-major: acesso m[col][row])
    float x = vp->m[0][0]*world->x + vp->m[1][0]*world->y + vp->m[2][0]*world->z + vp->m[3][0];
    float y = vp->m[0][1]*world->x + vp->m[1][1]*world->y + vp->m[2][1]*world->z + vp->m[3][1];
    float w = vp->m[0][3]*world->x + vp->m[1][3]*world->y + vp->m[2][3]*world->z + vp->m[3][3];
    // z para clipping:
    float z = vp->m[0][2]*world->x + vp->m[1][2]*world->y + vp->m[2][2]*world->z + vp->m[3][2];
    (void)z;

    if (w <= 0.001f) return false; // ponto atrás da câmera

    float inv_w = 1.0f / w;
    float ndcx = x * inv_w;  // [-1, 1]
    float ndcy = y * inv_w;  // [-1, 1] Unity NDC Y positivo = cima

    *out_x = ( ndcx + 1.0f) * 0.5f * screen_w;
    *out_y = (-ndcy + 1.0f) * 0.5f * screen_h; // flip Y para coordenada de tela
    return true;
}

// ─── Distância 3D ─────────────────────────────────────────────────────────────
static inline float vec3_dist(Vec3 a, Vec3 b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

// ─── Aimbot: ângulos para alvo ────────────────────────────────────────────────
// Retorna yaw e pitch (graus) da câmera local para 'target'.
// camera_pos: posição da câmera no mundo.
// target_pos: posição do alvo (cabeça ou tórax).
static inline void calc_aim_angles(Vec3 camera_pos, Vec3 target_pos,
                                   float *out_yaw, float *out_pitch) {
    float dx = target_pos.x - camera_pos.x;
    float dy = target_pos.y - camera_pos.y;
    float dz = target_pos.z - camera_pos.z;
    float horiz = sqrtf(dx*dx + dz*dz);
    *out_yaw   =  atan2f(dx, dz) * (180.0f / (float)M_PI);
    *out_pitch = -atan2f(dy, horiz) * (180.0f / (float)M_PI); // negativo: Unity inverte pitch
}

// ─── Smooth step para aimbot ──────────────────────────────────────────────────
// Interpola 'current' em direção a 'target' por 'factor' (0..1 por frame).
static inline float smooth_angle(float current, float target, float factor) {
    float delta = target - current;
    // Normaliza delta para [-180, 180]
    while (delta >  180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return current + delta * factor;
}

// ─── FOV check: distância angular do centro da tela ──────────────────────────
// target_sx/sy: posição do alvo na tela
// cx/cy: centro da tela
// Retorna distância em pixels (usar < fov_pixels para qualificar).
static inline float screen_fov_dist(float target_sx, float target_sy,
                                    float cx, float cy) {
    float dx = target_sx - cx;
    float dy = target_sy - cy;
    return sqrtf(dx*dx + dy*dy);
}

// ─── Clamp / lerp utilitários ─────────────────────────────────────────────────
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}
