// ff_offsets.c — carrega offsets do JSON correto para o build em execução
#include "../include/ff_offsets_loader.h"
#include "../../Shared/include/ff_offsets.h"
#include "../../Shared/include/ff_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <mach-o/loader.h>
#else
// Stubs Linux para syntax-check
#  include <stdint.h>
#  include <stdbool.h>
#  define MH_MAGIC_64  0xfeedfacf
#  define LC_SEGMENT_64 0x19
#  define LC_UUID 0x1b
struct load_command   { uint32_t cmd; uint32_t cmdsize; };
struct segment_command_64 { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t fileoff; uint64_t filesize; int maxprot; int initprot; uint32_t nsects; uint32_t flags; };
struct uuid_command   { uint32_t cmd; uint32_t cmdsize; uint8_t uuid[16]; };
struct mach_header_64 { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; uint32_t reserved; };
struct mach_header    { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; };
static inline uint32_t _dyld_image_count(void) { return 0; }
static inline const char* _dyld_get_image_name(uint32_t i) { (void)i; return NULL; }
static inline const struct mach_header* _dyld_get_image_header(uint32_t i) { (void)i; return NULL; }
// dlfcn — Dl_info não está em <dlfcn.h> sem _GNU_SOURCE no Linux com -std=c11
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
static inline int dladdr(const void* a, Dl_info* i) { (void)a;(void)i; return 0; }
#  ifndef RTLD_NOLOAD
#    define RTLD_NOLOAD 0x10
#  endif
#  ifndef RTLD_LAZY
#    define RTLD_LAZY   0x01
#  endif
#endif

// ─── Localiza o UUID do GameAssembly.dylib em execução ───────────────────────
// O UUID fica no LC_UUID load command do Mach-O.
static bool get_gameassembly_uuid(char *out, size_t out_len) {
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name) continue;
        // Free Fire iOS: IL2CPP está dentro do UnityFramework, não GameAssembly.dylib
        if (!strstr(name, "UnityFramework") && !strstr(name, "GameAssembly")) continue;

        const struct mach_header *mh = _dyld_get_image_header(i);
        if (!mh) continue;

        // Iterar load commands
        const uint8_t *lc_ptr = (const uint8_t *)mh
            + (mh->magic == 0xFEEDFACF ? 32 : 28); // 64 vs 32 bit header

        for (uint32_t j = 0; j < mh->ncmds; j++) {
            const struct load_command *lc = (const struct load_command *)lc_ptr;
            if (lc->cmd == LC_UUID) {
                // struct uuid_command { uint32_t cmd; uint32_t cmdsize; uint8_t uuid[16]; }
                const uint8_t *uuid = lc_ptr + 8;
                snprintf(out, out_len,
                    "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                    uuid[0],  uuid[1],  uuid[2],  uuid[3],
                    uuid[4],  uuid[5],
                    uuid[6],  uuid[7],
                    uuid[8],  uuid[9],
                    uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
                return true;
            }
            lc_ptr += lc->cmdsize;
        }
    }
    return false;
}

// ─── Parser JSON mínimo (sem dependência externa) ─────────────────────────────
// Extrai o valor de uma chave string do JSON plano. Suporta strings e hex numbers.
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++; // skip opening "
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool json_get_uint64(const char *json, const char *key, uint64_t *out) {
    char val[64];
    if (!json_get_string(json, key, val, sizeof(val))) return false;
    if (strcmp(val, "EXPORT") == 0) { *out = 0; return true; } // exports resolvidos dinamicamente
    if (strcmp(val, "0xDEADC0DEDEADC0DE") == 0 ||
        strcmp(val, "0xDEADC0DE") == 0) {
        *out = OFFSET_UNKNOWN; return true;
    }
    char *end;
    *out = strtoull(val, &end, 16);
    return end != val;
}

// ─── Resolve exports do il2cpp dinamicamente ─────────────────────────────────
static void resolve_il2cpp_exports(FF_GameAssemblyOffsets *ga, uintptr_t base) {
    // Todos os il2cpp_* são exports reais no UnityFramework — dlsym resolve direto
    // Free Fire iOS: IL2CPP está dentro do UnityFramework.framework
    void *handle = dlopen("@rpath/UnityFramework.framework/UnityFramework",
                           RTLD_NOLOAD | RTLD_NOW);
    if (!handle) handle = dlopen("UnityFramework", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) return;

#define RESOLVE(field, sym) \
    do { \
        void *addr = dlsym(handle, sym); \
        if (addr) ga->field = (uintptr_t)addr - base; \
    } while(0)

    RESOLVE(il2cpp_domain_get,                "il2cpp_domain_get");
    RESOLVE(il2cpp_domain_get_assemblies,     "il2cpp_domain_get_assemblies");
    RESOLVE(il2cpp_class_from_name,           "il2cpp_class_from_name");
    RESOLVE(il2cpp_class_get_method_from_name,"il2cpp_class_get_method_from_name");
    RESOLVE(il2cpp_class_get_field_from_name, "il2cpp_class_get_field_from_name");
    RESOLVE(il2cpp_field_get_offset,          "il2cpp_field_get_offset");
    RESOLVE(il2cpp_field_static_get_value,    "il2cpp_field_static_get_value");
    RESOLVE(il2cpp_field_static_set_value,    "il2cpp_field_static_set_value");
    RESOLVE(il2cpp_object_new,                "il2cpp_object_new");
    RESOLVE(il2cpp_runtime_invoke,            "il2cpp_runtime_invoke");
#undef RESOLVE

    dlclose(handle);
}

// ─── Implementação pública ────────────────────────────────────────────────────
ff_status_t ff_offsets_load(FF_Offsets *out) {
    if (!out) return FF_ERR_INVALID_OFFSET;
    memset(out, 0, sizeof(*out));

    // 1. Descobrir UUID do GameAssembly em execução
    char uuid[64] = {0};
    if (!get_gameassembly_uuid(uuid, sizeof(uuid))) {
        // GameAssembly não encontrado no processo — não estamos dentro do FF
        return FF_ERR_NOT_FOUND;
    }
    strncpy(out->build_uuid, uuid, sizeof(out->build_uuid) - 1);

    // 2. Localizar arquivo de config correspondente
    // O app bundle fica em <bundle>/Config/offsets_<UUID>.json
    // Fallback: /var/mobile/Documents/FFCheat/offsets_<UUID>.json (atualizável sem rebuild)
    char config_path[512];

    // Tentar Documents primeiro (permite atualizar offsets sem rebuild)
    snprintf(config_path, sizeof(config_path),
        "/var/mobile/Documents/FFCheat/offsets_%s.json", uuid);

    FILE *f = fopen(config_path, "r");
    if (!f) {
        // Fallback: bundle do app
        // Pega o path do bundle dinamicamente
        Dl_info di;
        if (dladdr((void*)ff_offsets_load, &di) && di.dli_fname) {
            char bundle[256];
            strncpy(bundle, di.dli_fname, sizeof(bundle) - 1);
            char *last_slash = strrchr(bundle, '/');
            if (last_slash) {
                // Vai dois níveis acima (dylib está em Frameworks/, bundle está acima)
                *last_slash = '\0';
                last_slash = strrchr(bundle, '/');
                if (last_slash) *last_slash = '\0';
            }
            snprintf(config_path, sizeof(config_path),
                "%s/Config/offsets_%s.json", bundle, uuid);
            f = fopen(config_path, "r");
        }
    }

    if (!f) {
        // Sem config para esse build
        // Preenche os exports il2cpp que não precisam de config
        uintptr_t base = ff_gameassembly_base();
        if (base) resolve_il2cpp_exports(&out->ga, base);
        return FF_ERR_VERSION;
    }

    // 3. Ler o JSON inteiro
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = (char *)malloc(sz + 1);
    if (!json) { fclose(f); return FF_ERR_MEM_READ; }
    fread(json, 1, sz, f);
    json[sz] = '\0';
    fclose(f);

    // 4. Parsear campos
    uint64_t v;
#define LOAD_GA(field) \
    if (json_get_uint64(json, #field, &v)) out->ga.field = (uintptr_t)v
#define LOAD_ST(field) \
    if (json_get_uint64(json, #field, &v)) out->st.field = (uint32_t)v

    LOAD_GA(klass_PlayerInfo);
    LOAD_GA(klass_GameController);
    LOAD_GA(klass_WeaponController);
    LOAD_GA(klass_BulletControl);
    LOAD_GA(klass_VehicleControl);
    LOAD_GA(klass_AntiCheat);
    LOAD_GA(fn_GetLocalPlayer);
    LOAD_GA(fn_GetAllPlayers);
    LOAD_GA(fn_WorldToScreen);
    LOAD_GA(fn_GameUpdate);

    LOAD_ST(PlayerInfo_health);
    LOAD_ST(PlayerInfo_health_max);
    LOAD_ST(PlayerInfo_ep);
    LOAD_ST(PlayerInfo_armor);
    LOAD_ST(PlayerInfo_name);
    LOAD_ST(PlayerInfo_team_id);
    LOAD_ST(PlayerInfo_is_dead);
    LOAD_ST(PlayerInfo_is_local);
    LOAD_ST(PlayerInfo_transform);
    LOAD_ST(PlayerInfo_bone_root);
    LOAD_ST(PlayerInfo_weapon_controller);
    LOAD_ST(Transform_position);
    LOAD_ST(Transform_rotation);
    LOAD_ST(WeaponController_currentAmmo);
    LOAD_ST(WeaponController_maxAmmo);
    LOAD_ST(WeaponController_reserveAmmo);
    LOAD_ST(WeaponController_reloadTime);
    LOAD_ST(WeaponController_spreadFactor);
    LOAD_ST(CharController_moveSpeed);
    LOAD_ST(GameController_instance);
    LOAD_ST(GameController_allPlayers);
    LOAD_ST(GameController_gamePhase);
    LOAD_ST(Camera_projMatrix);
    LOAD_ST(Camera_viewMatrix);

#undef LOAD_GA
#undef LOAD_ST

    free(json);

    // 5. Resolver exports il2cpp dinamicamente (sobrescreve zeros do JSON)
    uintptr_t base = ff_gameassembly_base();
    if (base) resolve_il2cpp_exports(&out->ga, base);

    return FF_OK;
}
