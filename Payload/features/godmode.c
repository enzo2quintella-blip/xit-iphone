// godmode.c — god mode via escrita direta no campo health do jogador local
// Estratégia: a cada tick, se health < health_max, restaura para health_max.
// Mais seguro que NOP no código (evita detectar patches via hash do texto):
// Escrevemos dados no heap, não na GameAssembly.__text.

#include "../include/ff_game.h"
#include <stdbool.h>

static bool s_godmode_enabled = false;

void godmode_set(bool enabled) {
    s_godmode_enabled = enabled;
}

bool godmode_enabled(void) { return s_godmode_enabled; }

void godmode_tick(void) {
    if (!s_godmode_enabled) return;
    FF_Player *local = ff_local_player();
    if (!local) return;
    FF_StructOffsets *st = &g_offsets.st;

    // Se morreu, não tenta restaurar (causaria desync)
    if (local->is_dead) return;

    // Restaura health para max
    if (local->health < local->health_max && local->health_max > 0.0f) {
        ff_write_field_f32(local->ptr, st->PlayerInfo_health, local->health_max);
    }

    // Restaura armor para 100 se tiver armor slot
    if (st->PlayerInfo_armor != (uint32_t)OFFSET_UNKNOWN) {
        float armor = 0.0f;
        ff_read_field_f32(local->ptr, st->PlayerInfo_armor, &armor);
        if (armor < 100.0f) {
            ff_write_field_f32(local->ptr, st->PlayerInfo_armor, 100.0f);
        }
    }
}
