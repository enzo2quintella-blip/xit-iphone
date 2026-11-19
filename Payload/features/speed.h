#pragma once
#include <stdbool.h>

// Speed hack: multiplica CharController.moveSpeed por um fator.
// factor 1.0 = velocidade original. factor 2.0 = dobro. Max recomendado: 3.0.

void speed_set(bool enabled, float factor);
bool speed_enabled(void);
void speed_tick(void);
