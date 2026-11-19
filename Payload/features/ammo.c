// ammo.c — infinite ammo via escrita no WeaponController.currentAmmo
// Estratégia: restaura munição atual para max a cada tick enquanto ativa.

#include "../include/ff_game.h"
#include <stdbool.h>

static bool s_inf_ammo_enabled = false;

void inf_ammo_set(bool enabled) { s_inf_ammo_enabled = enabled; }
bool inf_ammo_enabled(void)     { return s_inf_ammo_enabled; }

void inf_ammo_tick(void) {
    if (!s_inf_ammo_enabled) return;
    FF_Player *local = ff_local_player();
    if (!local) return;

    FF_StructOffsets *st = &g_offsets.st;
    if (st->PlayerInfo_weapon_controller == (uint32_t)OFFSET_UNKNOWN) return;

    uintptr_t weapon_ctrl = 0;
    if (!ff_read_field_ptr(local->ptr, st->PlayerInfo_weapon_controller, &weapon_ctrl)) return;
    if (!ptr_valid(weapon_ctrl)) return;

    // WeaponController.currentAmmo → restaura para max
    if (st->WeaponController_currentAmmo != (uint32_t)OFFSET_UNKNOWN &&
        st->WeaponController_maxAmmo     != (uint32_t)OFFSET_UNKNOWN) {
        int32_t max_ammo = 0;
        if (ff_read_field_i32(weapon_ctrl, st->WeaponController_maxAmmo, &max_ammo) &&
            max_ammo > 0) {
            ff_write_field_i32(weapon_ctrl, st->WeaponController_currentAmmo, max_ammo);
        }
    }

    // Munição em reserva (magazine reserve)
    if (st->WeaponController_reserveAmmo != (uint32_t)OFFSET_UNKNOWN) {
        ff_write_field_i32(weapon_ctrl, st->WeaponController_reserveAmmo, 999);
    }
}
