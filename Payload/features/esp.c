// esp.c — ESP completo: box 2D, skeleton, health bar, nome, distância, snap lines
// Renderização via Metal/ImGui delegada para overlay.c; aqui calculamos geometria.
// O overlay consome ff_esp_frame_data após esp_render() processar os jogadores.

#include "esp.h"
#include "../include/ff_math.h"
#include "../include/ff_game.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

ESP_Config g_esp = {0};

// ─── Estrutura de dados de frame (consumida pelo overlay) ────────────────────
#define ESP_MAX_ENTITIES 64

typedef struct {
    // Caixa 2D
    float box_x, box_y, box_w, box_h;  // em pixels
    // Health bar: posição absoluta (esquerda da caixa)
    float hp_ratio;   // 0..1
    float armor_ratio;// 0..1  (Armor / 200.0)
    float ep_ratio;   // 0..1  (EP / 50.0 — Energy Points Free Fire = 0..50)
    // Label
    char  label[96];  // nome + distância
    // Snap line: origem é o centro-inferior da caixa, destino = centro da tela inferior
    bool  valid_snap;
    // Bones: até 10 segmentos de linha, cada um com 2 pontos de tela
    int   bone_count;
    struct { float x1,y1,x2,y2; } bones[16];
    // Status
    bool  is_dead;
    bool  is_local;
    bool  valid;
    // Cor (muda por time/distância)
    uint8_t r, g, b;
} ESP_Entity;

typedef struct {
    ESP_Entity entities[ESP_MAX_ENTITIES];
    int        count;
    float      screen_w, screen_h;
} ESP_FrameData;

// Buffer de frame duplo (escreve enquanto renderiza o anterior)
static ESP_FrameData s_frames[2] = {0};
static int           s_write_idx = 0; // índice de escrita
// Leitura: overlay usa (s_write_idx ^ 1)
// Sem lock aqui — single-threaded; em produção usar atomic swap ou spinlock leve.

ESP_FrameData *ff_esp_frame_data_read(void)  { return &s_frames[s_write_idx ^ 1]; }
// Declaração externa para o overlay
ESP_FrameData *ff_esp_frame_data_write(void) { return &s_frames[s_write_idx];     }
void           ff_esp_frame_swap(void)       { s_write_idx ^= 1; }

// ─── Índices de bone do esqueleto Free Fire (posições em Transform[]) ─────────
// Free Fire usa um Animator com um array de Transform* acessível via
// Animator.GetBoneTransform(HumanBodyBones enum).
// Sem offsets confirmados, geramos o esqueleto via worldToScreen de pontos
// estimados em relação à posição raiz do jogador. Marcado como ESTIMATE.
static const struct { float dx, dy, dz; const char *name; }
    kBoneOffsets[] = {
    // Offset relativo ao root (pés): valores em metros — ESTIMATE (ajustar com dump)
    { 0.0f,  1.75f, 0.0f, "cabeca"       }, // 0
    { 0.0f,  1.50f, 0.0f, "pescoco"      }, // 1
    { 0.0f,  1.35f, 0.0f, "torax"        }, // 2
    { 0.0f,  1.10f, 0.0f, "quadril"      }, // 3
    { 0.18f, 1.35f, 0.0f, "ombro_dir"    }, // 4
    { 0.18f, 1.00f, 0.0f, "cotovelo_dir" }, // 5
    { 0.18f, 0.65f, 0.0f, "mao_dir"      }, // 6
    {-0.18f, 1.35f, 0.0f, "ombro_esq"    }, // 7
    {-0.18f, 1.00f, 0.0f, "cotovelo_esq" }, // 8
    {-0.18f, 0.65f, 0.0f, "mao_esq"      }, // 9
    { 0.10f, 0.85f, 0.0f, "joelho_dir"   }, // 10
    { 0.10f, 0.40f, 0.0f, "pe_dir"       }, // 11
    {-0.10f, 0.85f, 0.0f, "joelho_esq"   }, // 12
    {-0.10f, 0.40f, 0.0f, "pe_esq"       }, // 13
};
#define NUM_BONES 14

// Conexões do esqueleto: pares de índices em kBoneOffsets
static const int kBoneLinks[][2] = {
    {0,1},{1,2},{2,3},          // coluna
    {2,4},{4,5},{5,6},          // braço dir
    {2,7},{7,8},{8,9},          // braço esq
    {3,10},{10,11},             // perna dir
    {3,12},{12,13},             // perna esq
};
#define NUM_BONE_LINKS 13

// ─── esp_init ─────────────────────────────────────────────────────────────────
void esp_init(void) {
    g_esp.enabled    = true;
    g_esp.boxes      = true;
    g_esp.bones      = true;
    g_esp.health_bar = true;
    g_esp.names      = true;
    g_esp.distance   = true;
    g_esp.snap_lines = false;
    g_esp.max_dist_m = 300.0f;
}

// ─── esp_render ───────────────────────────────────────────────────────────────
void esp_render(const Matrix4x4 *vp, Vec3 camera_pos,
                float screen_w, float screen_h) {
    if (!g_esp.enabled) return;

    FF_Player players[64];
    int count = ff_get_players(players, 64);
    FF_Player *local = ff_local_player();

    ESP_FrameData *frame = ff_esp_frame_data_write();
    frame->count    = 0;
    frame->screen_w = screen_w;
    frame->screen_h = screen_h;

    for (int i = 0; i < count && frame->count < ESP_MAX_ENTITIES; i++) {
        FF_Player *p = &players[i];
        if (p->is_local) continue; // não desenha o local
        if (p->is_dead) continue;  // não desenha mortos

        float dist = vec3_dist(camera_pos, p->position);
        if (dist > g_esp.max_dist_m) continue;

        // Projeta o ponto de base (pés) e topo (cabeça ≈ +1.75m)
        Vec3 head_pos = { p->position.x, p->position.y + 1.75f, p->position.z };
        float sx_foot, sy_foot, sx_head, sy_head;
        bool ok_foot = world_to_screen(vp, &p->position, screen_w, screen_h, &sx_foot, &sy_foot);
        bool ok_head = world_to_screen(vp, &head_pos,    screen_w, screen_h, &sx_head, &sy_head);
        if (!ok_foot || !ok_head) continue;
        if (sy_foot <= sy_head) continue; // atrás ou fora do frustum

        ESP_Entity *e = &frame->entities[frame->count];
        memset(e, 0, sizeof(*e));

        // Caixa 2D
        float box_h = sy_foot - sy_head;
        float box_w = box_h * 0.4f; // proporção aproximada
        e->box_x = sx_head - box_w * 0.5f;
        e->box_y = sy_head;
        e->box_w = box_w;
        e->box_h = box_h;

        // HP ratio
        e->hp_ratio    = (p->health_max > 0.0f)
                         ? clampf(p->health / p->health_max, 0.0f, 1.0f) : 0.0f;
        // Armor do Free Fire: Colete Nível 3 = 190 durabilidade máxima.
        // Usar 200.0f como divisor para normalizar corretamente.
        e->armor_ratio = (p->armor > 0.0f)
                         ? clampf(p->armor / 200.0f, 0.0f, 1.0f) : 0.0f;
        // EP (Energy Points): 0..50. Usado como escudo temporário antes do HP.
        e->ep_ratio    = clampf(p->ep / 50.0f, 0.0f, 1.0f);

        // Label
        if (g_esp.names && g_esp.distance)
            snprintf(e->label, sizeof(e->label), "%s [%.0fm]", p->name, dist);
        else if (g_esp.names)
            snprintf(e->label, sizeof(e->label), "%s", p->name);
        else if (g_esp.distance)
            snprintf(e->label, sizeof(e->label), "%.0fm", dist);
        else
            e->label[0] = '\0';

        // Snap line
        e->valid_snap = g_esp.snap_lines;

        // Bones (estimativa offset relativo à posição raiz)
        if (g_esp.bones) {
            // Projeta cada bone point
            float bone_sx[NUM_BONES], bone_sy[NUM_BONES];
            bool  bone_ok[NUM_BONES];
            for (int b = 0; b < NUM_BONES; b++) {
                Vec3 bpos = {
                    p->position.x + kBoneOffsets[b].dx,
                    p->position.y + kBoneOffsets[b].dy,
                    p->position.z + kBoneOffsets[b].dz
                };
                bone_ok[b] = world_to_screen(vp, &bpos, screen_w, screen_h,
                                             &bone_sx[b], &bone_sy[b]);
            }
            e->bone_count = 0;
            for (int l = 0; l < NUM_BONE_LINKS && e->bone_count < 16; l++) {
                int a = kBoneLinks[l][0], bi = kBoneLinks[l][1];
                if (!bone_ok[a] || !bone_ok[bi]) continue;
                e->bones[e->bone_count].x1 = bone_sx[a];
                e->bones[e->bone_count].y1 = bone_sy[a];
                e->bones[e->bone_count].x2 = bone_sx[bi];
                e->bones[e->bone_count].y2 = bone_sy[bi];
                e->bone_count++;
            }
        }

        // Cor por time (inimigo = vermelho, aliado = verde)
        int32_t local_team = local ? local->team_id : -1;
        if (local_team >= 0 && p->team_id == local_team) {
            e->r = 0x00; e->g = 0xFF; e->b = 0x00; // aliado → verde
        } else {
            e->r = 0xFF; e->g = 0x00; e->b = 0x00; // inimigo → vermelho
        }

        e->is_dead  = p->is_dead;
        e->is_local = false;
        e->valid    = true;
        frame->count++;
    }

    ff_esp_frame_swap();
}
