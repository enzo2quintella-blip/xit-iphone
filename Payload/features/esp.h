// esp.h — ESP: box, bones, health bar, nome, distância
#pragma once

#include "../include/ff_game.h"
#include "../../Shared/include/ff_types.h"

typedef struct {
    bool enabled;
    bool boxes;       // caixa 2D ao redor do jogador
    bool bones;       // esqueleto 3D projetado
    bool health_bar;  // barra de HP lateral
    bool names;       // nome acima da caixa
    bool distance;    // distância em metros
    bool snap_lines;  // linha do centro inferior até o alvo
    float max_dist_m; // distância máxima para renderizar (metros)
} ESP_Config;

extern ESP_Config g_esp;

// Inicializa com defaults seguros.
void esp_init(void);

// Renderiza ESP de todos os jogadores no frame atual.
// vp: ViewProjection matrix (câmera)
// camera_pos: posição da câmera no mundo
// screen_w, screen_h: resolução atual
// Chame de dentro do hook de Present/render.
void esp_render(const Matrix4x4 *vp, Vec3 camera_pos,
                float screen_w, float screen_h);
