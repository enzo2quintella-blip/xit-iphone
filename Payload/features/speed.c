// speed.c — speed hack via escrita no CharacterController.moveSpeed
// Estratégia: multiplica o campo de velocidade de movimento pelo fator configurado.
// Resetamos para o valor original quando desativado para não deixar estado sujo.

#include "../include/ff_game.h"
#include <stdbool.h>

static bool  s_speed_enabled  = false;
static float s_speed_factor   = 2.0f;   // 2x por padrão — UNCALIBRATED
static float s_original_speed = 0.0f;   // velocidade original salva
static bool  s_captured       = false;

void speed_set(bool enabled, float factor) {
    s_speed_enabled = enabled;
    s_speed_factor  = factor;
    if (!enabled) {
        // Restaura velocidade original
        FF_Player *local = ff_local_player();
        if (local && s_captured) {
            FF_StructOffsets *st = &g_offsets.st;
            if (st->CharController_moveSpeed != (uint32_t)OFFSET_UNKNOWN) {
                ff_write_field_f32(local->ptr, st->CharController_moveSpeed, s_original_speed);
            }
        }
        s_captured = false;
    }
}

bool speed_enabled(void) { return s_speed_enabled; }

void speed_tick(void) {
    if (!s_speed_enabled) return;
    FF_Player *local = ff_local_player();
    if (!local) return;
    FF_StructOffsets *st = &g_offsets.st;
    if (st->CharController_moveSpeed == (uint32_t)OFFSET_UNKNOWN) return;

    // Captura velocidade original uma vez
    if (!s_captured) {
        if (!ff_read_field_f32(local->ptr, st->CharController_moveSpeed, &s_original_speed))
            return;
        // Plausibility check: valores razoáveis para Free Fire (2–10 m/s base)
        if (s_original_speed < 0.5f || s_original_speed > 50.0f) return;
        s_captured = true;
    }

    ff_write_field_f32(local->ptr, st->CharController_moveSpeed,
                       s_original_speed * s_speed_factor);
}
