#pragma once
#include <stdbool.h>

// Mantém currentAmmo = maxAmmo e reserveAmmo = 999 a cada tick.

void inf_ammo_set(bool enabled);
bool inf_ammo_enabled(void);
void inf_ammo_tick(void);
