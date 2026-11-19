// ff_game.h — acesso às structs do Free Fire via offsets carregados
#pragma once

#include "../../Shared/include/ff_types.h"
#include "../../Shared/include/ff_offsets.h"
#include "ff_offsets_loader.h"
#include <stdbool.h>

// Instância global de offsets (carregada em ff_init)
extern FF_Offsets g_offsets;

// ─── Helpers de leitura de campo ─────────────────────────────────────────────
// Lê um campo de um objeto IL2CPP dado o endereço do objeto e o offset do campo.
// Retorna false se o ponteiro ou offset for inválido.

static inline bool ff_read_field_f32(uintptr_t obj, uint32_t field_off, float *out) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_read_mem(obj + field_off, out, sizeof(float));
}

static inline bool ff_read_field_i32(uintptr_t obj, uint32_t field_off, int32_t *out) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_read_mem(obj + field_off, out, sizeof(int32_t));
}

static inline bool ff_read_field_bool(uintptr_t obj, uint32_t field_off, bool *out) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    uint8_t v = 0;
    bool ok = ff_read_mem(obj + field_off, &v, 1);
    if (ok) *out = v != 0;
    return ok;
}

static inline bool ff_read_field_ptr(uintptr_t obj, uint32_t field_off, uintptr_t *out) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_read_mem(obj + field_off, out, sizeof(uintptr_t));
}

static inline bool ff_read_field_vec3(uintptr_t obj, uint32_t field_off, Vec3 *out) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_read_mem(obj + field_off, out, sizeof(Vec3));
}

static inline bool ff_write_field_f32(uintptr_t obj, uint32_t field_off, float val) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_write_mem(obj + field_off, &val, sizeof(float));
}

static inline bool ff_write_field_i32(uintptr_t obj, uint32_t field_off, int32_t val) {
    if (!ptr_valid(obj) || field_off == (uint32_t)OFFSET_UNKNOWN) return false;
    return ff_write_mem(obj + field_off, &val, sizeof(int32_t));
}

// ─── PlayerInfo wrapper ───────────────────────────────────────────────────────
typedef struct {
    uintptr_t ptr;       // endereço do objeto PlayerInfo no heap do Unity
    Vec3      position;  // posição world (atualizada em ff_player_refresh)
    float     health;
    float     health_max;
    float     armor;
    int32_t   team_id;
    bool      is_dead;
    bool      is_local;
    char      name[64];
    uintptr_t transform; // Transform* (para buscar bones)
} FF_Player;

// Atualiza os campos de um FF_Player a partir do objeto no heap.
// Retorna false se o objeto for inválido ou os offsets não estiverem carregados.
bool ff_player_refresh(FF_Player *p);

// Retorna o jogador local (proc_self do jogo, não kernel).
// Retorna NULL se não for possível localizar.
FF_Player *ff_local_player(void);

// Preenche `out` com até `max_count` jogadores no servidor.
// Retorna o número de jogadores válidos encontrados.
int ff_get_players(FF_Player *out, int max_count);

// ─── Il2CPP runtime (resolve via exports de GameAssembly) ────────────────────
typedef void* (*fn_il2cpp_class_from_name_t)(void *image, const char *ns, const char *name);
typedef void* (*fn_il2cpp_class_get_method_from_name_t)(void *klass, const char *name, int argc);
typedef void* (*fn_il2cpp_runtime_invoke_t)(void *method, void *obj, void **params, void **ex);

// Chama um método IL2CPP por classe+nome (resolve na primeira chamada, cacheado).
uintptr_t ff_il2cpp_invoke(const char *class_name, const char *method_name,
                            void *obj, void **args);
