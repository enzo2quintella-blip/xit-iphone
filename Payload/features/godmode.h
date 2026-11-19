#pragma once
#include <stdbool.h>

// Mantém HP = HP_max e armadura = 100 a cada tick.
// Deve ser chamado de dentro do hook de LateUpdate (hooked_game_update).

void godmode_set(bool enabled);
bool godmode_enabled(void);
void godmode_tick(void);
