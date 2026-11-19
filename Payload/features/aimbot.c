// aimbot.c — aimbot com FOV, smooth, hitbox selecionável
#include "aimbot.h"
#include "../include/ff_math.h"
#include <math.h>
#include <float.h>

Aimbot_Config g_aimbot = {0};

void aimbot_init(void) {
    g_aimbot.enabled      = false; // começa desligado
    g_aimbot.fov_deg      = 10.0f;
    g_aimbot.smooth       = 0.15f; // UNCALIBRATED: ajustar para o input do jogo
    g_aimbot.aim_key_held = false;
    g_aimbot.hitbox       = AIM_HITBOX_CHEST;
    g_aimbot.visible_check = false;
}

// Offset do hitbox relativo à posição raiz (pés) — ESTIMATE (ver esp.c kBoneOffsets)
static Vec3 hitbox_offset(AimHitbox h) {
    switch (h) {
        case AIM_HITBOX_HEAD:  return (Vec3){0.0f, 1.70f, 0.0f};
        case AIM_HITBOX_NECK:  return (Vec3){0.0f, 1.55f, 0.0f};
        default: /* CHEST */   return (Vec3){0.0f, 1.35f, 0.0f};
    }
}

bool aimbot_tick(float camera_yaw, float camera_pitch, Vec3 camera_pos,
                 const Matrix4x4 *vp, float screen_w, float screen_h,
                 float *out_dyaw, float *out_dpitch) {
    *out_dyaw = 0.0f; *out_dpitch = 0.0f;

    if (!g_aimbot.enabled || !g_aimbot.aim_key_held) return false;

    FF_Player players[64];
    int count = ff_get_players(players, 64);
    FF_Player *local = ff_local_player();

    float cx = screen_w * 0.5f, cy = screen_h * 0.5f;
    // FOV em pixels: converte graus → pixels usando metade da largura da tela
    // fov_px = tan(fov/2) * (screen_w / 2) / tan(vert_fov/2)
    // Aproximação: fov_deg é uma janela circular no espaço de tela
    float fov_px = screen_w * tanf(g_aimbot.fov_deg * 0.5f * (float)(M_PI/180.0));

    float best_dist_px = FLT_MAX;
    Vec3  best_aim_pos = {0};
    bool  found = false;
    int32_t local_team = local ? local->team_id : -1;

    for (int i = 0; i < count; i++) {
        FF_Player *p = &players[i];
        if (p->is_local || p->is_dead) continue;
        // Não miramos em aliados
        if (local_team >= 0 && p->team_id == local_team) continue;

        Vec3 off  = hitbox_offset(g_aimbot.hitbox);
        Vec3 aim_pos = {
            p->position.x + off.x,
            p->position.y + off.y,
            p->position.z + off.z
        };

        float sx, sy;
        if (!world_to_screen(vp, &aim_pos, screen_w, screen_h, &sx, &sy)) continue;

        float dist_px = screen_fov_dist(sx, sy, cx, cy);
        if (dist_px > fov_px) continue; // fora do FOV do aimbot

        if (dist_px < best_dist_px) {
            best_dist_px = dist_px;
            best_aim_pos = aim_pos;
            found = true;
        }
    }

    if (!found) return false;

    // Calcula ângulos-alvo
    float target_yaw, target_pitch;
    calc_aim_angles(camera_pos, best_aim_pos, &target_yaw, &target_pitch);

    // Smooth: interpola em direção ao alvo
    float smooth_yaw   = smooth_angle(camera_yaw,   target_yaw,   g_aimbot.smooth);
    float smooth_pitch = smooth_angle(camera_pitch,  target_pitch, g_aimbot.smooth);

    *out_dyaw   = smooth_yaw   - camera_yaw;
    *out_dpitch = smooth_pitch - camera_pitch;

    // Normaliza deltas
    while (*out_dyaw   >  180.0f) *out_dyaw   -= 360.0f;
    while (*out_dyaw   < -180.0f) *out_dyaw   += 360.0f;
    while (*out_dpitch >  180.0f) *out_dpitch -= 360.0f;
    while (*out_dpitch < -180.0f) *out_dpitch += 360.0f;

    return true;
}
