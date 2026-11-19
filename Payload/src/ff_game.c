// ff_game.c — acesso às structs do Free Fire via IL2CPP
//
// ff_local_player() e ff_get_players() usam dois caminhos:
//   1. il2cpp_runtime_invoke(g_mi_local_player / g_mi_all_players) — primário
//      Funciona sempre que o resolver encontrou os MethodInfo*.
//   2. fn_GetLocalPlayer / fn_GetAllPlayers do JSON — fallback (RVA medido manualmente)
//      Usado quando runtime_invoke não está disponível ou o método não foi encontrado.
//
// Il2CppArray layout (arm64, Unity 2021 IL2CPP):
//   +0x00  klass*     (8 bytes)
//   +0x08  monitor*   (8 bytes)
//   +0x10  bounds*    (8 bytes)   — geralmente NULL para arrays unidimensionais
//   +0x18  max_length (uint64)
//   +0x20  elemento[0]            — primeiro elemento

#include "../include/ff_game.h"
#include "../include/ff_il2cpp_resolver.h"
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

FF_Offsets g_offsets = {0};

// ─── Cache do jogador local ───────────────────────────────────────────────────
static FF_Player s_local_player = {0};

// ─── Helpers de leitura de string IL2CPP ─────────────────────────────────────
// Il2CppString: klass(8) + monitor(8) + length(4) + chars (UTF-16 LE)
static bool read_il2cpp_string(uintptr_t str_ptr, char *out, size_t out_size) {
    if (!ptr_valid(str_ptr)) return false;
    int32_t len = 0;
    if (!ff_read_mem(str_ptr + 0x10, &len, 4)) return false;
    if (len <= 0 || len > 127) { out[0] = '\0'; return true; }
    uint16_t wchars[128] = {0};
    if (!ff_read_mem(str_ptr + 0x14, wchars, (size_t)(len * 2))) return false;
    int n = len < (int)(out_size - 1) ? len : (int)(out_size - 1);
    for (int i = 0; i < n; i++) out[i] = (char)(wchars[i] & 0x7F);
    out[n] = '\0';
    return true;
}

// ─── ff_player_refresh ───────────────────────────────────────────────────────
bool ff_player_refresh(FF_Player *p) {
    if (!p || !ptr_valid(p->ptr)) return false;
    FF_StructOffsets *st = &g_offsets.st;

    ff_read_field_f32(p->ptr, st->PlayerInfo_health,     &p->health);
    ff_read_field_f32(p->ptr, st->PlayerInfo_health_max, &p->health_max);
    ff_read_field_f32(p->ptr, st->PlayerInfo_armor,      &p->armor);
    ff_read_field_i32(p->ptr, st->PlayerInfo_team_id,    &p->team_id);
    ff_read_field_bool(p->ptr, st->PlayerInfo_is_dead,   &p->is_dead);

    // Plausibility: health fora do range razoável → objeto provavelmente inválido
    if (p->health_max > 0.0f &&
        (p->health < -1.0f || p->health > p->health_max * 2.0f)) {
        return false;
    }

    // Posição: lida via Transform → position
    uintptr_t transform = 0;
    if (ff_read_field_ptr(p->ptr, st->PlayerInfo_transform, &transform) &&
        ptr_valid(transform)) {
        p->transform = transform;
        ff_read_field_vec3(transform, st->Transform_position, &p->position);
    }

    // Coordenadas absurdas → objeto inválido (Free Fire map range ~-8000..8000)
    if (fabsf(p->position.x) > 100000.0f ||
        fabsf(p->position.y) > 100000.0f ||
        fabsf(p->position.z) > 100000.0f) {
        return false;
    }

    // Nome via Il2CppString*
    uintptr_t name_ptr = 0;
    if (ff_read_field_ptr(p->ptr, st->PlayerInfo_name, &name_ptr) &&
        ptr_valid(name_ptr)) {
        read_il2cpp_string(name_ptr, p->name, sizeof(p->name));
    } else {
        snprintf(p->name, sizeof(p->name), "Player");
    }

    return true;
}

// ─── Caminho invoke: obtém objeto-resultado de um método IL2CPP ──────────────
// Il2CppObject* ret → ptr do objeto interno está logo após o header (offset 0x10
// para tipos de referência; boxed value types têm o valor em 0x10).
// Para métodos que retornam um Il2CppObject*, o ponteiro bruto retornado já é
// o endereço do objeto heap (ou NULL).
static uintptr_t invoke_ret_ptr(void* mi, void* obj, void** args) {
    void* ret = ff_il2cpp_invoke_method(mi, obj, args);
    return (uintptr_t)ret; // NULL → 0, ponteiro heap → VA válida
}

// ─── ff_local_player ─────────────────────────────────────────────────────────
FF_Player *ff_local_player(void) {
    FF_GameAssemblyOffsets *ga = &g_offsets.ga;
    uintptr_t obj = 0;

    // ── Caminho 1: runtime_invoke via MethodInfo cacheado ─────────────────
    void* mi = ff_il2cpp_get_mi_local_player();
    if (mi) {
        // Tenta chamada estática (sem objeto receptor)
        obj = invoke_ret_ptr(mi, NULL, NULL);
        // Se falhou, tenta com AvatarManager singleton como receptor
        if (!obj || !ptr_valid(obj)) {
            void* avmgr = ff_il2cpp_get_avmgr_singleton();
            if (avmgr) obj = invoke_ret_ptr(mi, avmgr, NULL);
        }
    }

    // ── Caminho 2: RVA fixo do JSON ───────────────────────────────────────
    if ((!obj || !ptr_valid(obj)) && ga->fn_GetLocalPlayer != OFFSET_UNKNOWN) {
        uintptr_t fn_va = ff_rva_to_va(ga->fn_GetLocalPlayer);
        if (fn_va) {
            typedef uintptr_t (*get_local_t)(void);
            obj = ((get_local_t)fn_va)();
        }
    }

    if (!obj || !ptr_valid(obj)) return NULL;

    s_local_player.ptr      = obj;
    s_local_player.is_local = true;
    if (!ff_player_refresh(&s_local_player)) return NULL;
    return &s_local_player;
}

// ─── ff_get_players ───────────────────────────────────────────────────────────
int ff_get_players(FF_Player *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    FF_GameAssemblyOffsets *ga = &g_offsets.ga;
    uintptr_t arr = 0;

    // ── Caminho 1: runtime_invoke via MethodInfo cacheado ─────────────────
    void* mi = ff_il2cpp_get_mi_all_players();
    if (mi) {
        arr = invoke_ret_ptr(mi, NULL, NULL);
        if (!arr || !ptr_valid(arr)) {
            void* avmgr = ff_il2cpp_get_avmgr_singleton();
            if (avmgr) arr = invoke_ret_ptr(mi, avmgr, NULL);
        }
    }

    // ── Caminho 2: RVA fixo do JSON ───────────────────────────────────────
    if ((!arr || !ptr_valid(arr)) && ga->fn_GetAllPlayers != OFFSET_UNKNOWN) {
        uintptr_t fn_va = ff_rva_to_va(ga->fn_GetAllPlayers);
        if (fn_va) {
            typedef uintptr_t (*get_all_t)(void);
            arr = ((get_all_t)fn_va)();
        }
    }

    if (!arr || !ptr_valid(arr)) return 0;

    // ── Decodifica Il2CppArray ─────────────────────────────────────────────
    // Il2CppArray layout: klass(8)+monitor(8)+bounds(8)+max_length(8)+elem[0]
    // = 0x20 offset para o primeiro ponteiro de elemento
    uint64_t count64 = 0;
    if (!ff_read_mem(arr + 0x18, &count64, 8)) return 0;
    uint32_t count = (count64 < 128) ? (uint32_t)count64 : 128;
    if (count == 0) return 0;

    int found = 0;
    uintptr_t elem_base = arr + 0x20;
    for (uint32_t i = 0; i < count && found < max_count; i++) {
        uintptr_t player_ptr = 0;
        if (!ff_read_mem(elem_base + i * sizeof(uintptr_t),
                         &player_ptr, sizeof(uintptr_t))) continue;
        if (!ptr_valid(player_ptr)) continue;
        out[found].ptr      = player_ptr;
        out[found].is_local = false;
        if (ff_player_refresh(&out[found])) found++;
    }
    return found;
}

// ─── ff_il2cpp_invoke ─────────────────────────────────────────────────────────
// Versão simplificada que usa o resolver cacheado; prefira ff_il2cpp_invoke_method.
uintptr_t ff_il2cpp_invoke(const char *class_name, const char *method_name,
                            void *obj, void **args) {
    // Resolve image → class → method via IL2CPP runtime exports.
    static void *s_uf_handle = NULL;
    if (!s_uf_handle) {
        s_uf_handle = dlopen("@rpath/UnityFramework.framework/UnityFramework",
                             RTLD_NOLOAD | RTLD_LAZY);
        if (!s_uf_handle)
            s_uf_handle = dlopen("UnityFramework", RTLD_NOLOAD | RTLD_LAZY);
        if (!s_uf_handle) return 0;
    }

    typedef void* (*domain_get_t)(void);
    typedef void** (*domain_get_assemblies_t)(void *domain, size_t *size);
    typedef void* (*assembly_get_image_t)(void *assembly);
    typedef void* (*class_from_name_t)(void *image, const char *ns, const char *name);
    typedef void* (*class_get_method_t)(void *klass, const char *name, int argc);
    typedef void* (*runtime_invoke_t)(void *method, void *obj, void **params, void **ex);

    domain_get_t            domain_get    = dlsym(s_uf_handle, "il2cpp_domain_get");
    domain_get_assemblies_t get_assems    = dlsym(s_uf_handle, "il2cpp_domain_get_assemblies");
    assembly_get_image_t    get_image     = dlsym(s_uf_handle, "il2cpp_assembly_get_image");
    class_from_name_t       cfn           = dlsym(s_uf_handle, "il2cpp_class_from_name");
    class_get_method_t      get_method    = dlsym(s_uf_handle, "il2cpp_class_get_method_from_name");
    runtime_invoke_t        invoke        = dlsym(s_uf_handle, "il2cpp_runtime_invoke");

    if (!domain_get || !get_assems || !get_image || !cfn || !get_method || !invoke)
        return 0;

    void *domain = domain_get();
    if (!domain) return 0;

    size_t assem_count = 0;
    void **assemblies = get_assems(domain, &assem_count);
    if (!assemblies) return 0;

    void *target_method = NULL;
    for (size_t i = 0; i < assem_count && !target_method; i++) {
        void *image = get_image(assemblies[i]);
        if (!image) continue;
        void *klass = cfn(image, "COW.GamePlay", class_name);
        if (!klass) klass = cfn(image, "COW", class_name);
        if (!klass) klass = cfn(image, "", class_name);
        if (!klass) continue;
        target_method = get_method(klass, method_name, -1);
    }
    if (!target_method) return 0;

    void *exception = NULL;
    void *result = invoke(target_method, obj, args, &exception);
    if (exception) return 0;
    return (uintptr_t)result;
}
