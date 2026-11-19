// aimbot.h — aimbot com FOV limit, smooth, hitbox selecionável
#pragma once

#include "../include/ff_game.h"

typedef enum {
    AIM_HITBOX_HEAD   = 0,
    AIM_HITBOX_NECK   = 1,
    AIM_HITBOX_CHEST  = 2,
} AimHitbox;

typedef struct {
    bool      enabled;
    float     fov_deg;       // ângulo de FOV para qualificar alvo (ex: 10.0°)
    float     smooth;        // 0..1; 1=instantâneo, 0.05=muito suave
    bool      aim_key_held;  // atualizado pelo hook de input (L2 / botão de fogo)
    AimHitbox hitbox;
    bool      visible_check; // se verdadeiro, só mira em alvos com LOS (futuro: raycast)
} Aimbot_Config;

extern Aimbot_Config g_aimbot;

void aimbot_init(void);

// Calcula e retorna o delta de ângulo (dyaw, dpitch em graus) a aplicar.
// camera_yaw/pitch: ângulos atuais da câmera em graus.
// camera_pos: posição da câmera no mundo.
// screen_w/h: resolução para cálculo de FOV em pixels.
// vp: ViewProjection para conversão world→screen.
// Retorna false se não houver alvo qualificado.
bool aimbot_tick(float camera_yaw, float camera_pitch, Vec3 camera_pos,
                 const Matrix4x4 *vp, float screen_w, float screen_h,
                 float *out_dyaw, float *out_dpitch);
