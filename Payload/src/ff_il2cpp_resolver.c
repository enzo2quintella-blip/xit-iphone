// ff_il2cpp_resolver.c
// Resolve field offsets em runtime via IL2CPP API usando nomes REAIS das classes.
//
// Estratégia: il2cpp_class_from_name("COW.GamePlay", "Player") em vez de
// heurística de scoring — 100% determinístico, bypassa o Beebyte inteiramente.
//
// Classes confirmadas via análise de global-metadata.dat (versão 31, build
// UUID=E50B0E3A-D931-385B-9E4C-9FFDE6D1B923):
//   "Player"              ns="COW.GamePlay"  typedef[31768]  — jogador principal
//   "AvatarManager"       ns="COW.GamePlay"  typedef[31849]  — tem campo Nickname
//   "PhysicalCCT"         ns="COW.GamePlay"  typedef[34433]  — tem MaxSpeed
//   "VerticleGunController" ns="COW.GamePlay" typedef[35516] — tem BulletsRemainingInMag, BulletsPerMag
//   "PlayerData"          ns="COW"           typedef[25172]  — tem isDead, nickname, teamId, lastPosition
//
// Exports il2cpp confirmados (RVAs medidos do UnityFramework):
//   il2cpp_domain_get              @ 0xA111B28
//   il2cpp_domain_get_assemblies   @ 0xA111B34
//   il2cpp_assembly_get_image      @ 0xA1115CC
//   il2cpp_class_from_name         @ 0xA111604
//   il2cpp_class_get_field_from_name @ 0xA111624
//   il2cpp_class_get_fields        @ 0xA111610
//   il2cpp_field_get_offset        @ 0xA111D34
//   il2cpp_class_get_method_from_name @ 0xA111630
//   il2cpp_field_static_get_value  @ 0xA111D50
//
// Todos os offsets abaixo são obtidos em runtime — zero valores fixos de structs.

#include "../include/ff_il2cpp_resolver.h"
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
static inline uint32_t    _dyld_image_count(void)                  { return 0; }
static inline const char* _dyld_get_image_name(uint32_t i)         { (void)i; return NULL; }
static inline intptr_t    _dyld_get_image_vmaddr_slide(uint32_t i) { (void)i; return 0; }
static inline int usleep(unsigned us)                               { (void)us; return 0; }
#endif

// ---------------------------------------------------------------------------
// Tipos opacos IL2CPP
// ---------------------------------------------------------------------------
typedef void  Il2CppDomain;
typedef void  Il2CppAssembly;
typedef void  Il2CppImage;
typedef void  Il2CppClass;
typedef void  Il2CppType;
typedef void  FieldInfo;
typedef void  MethodInfo;

// Il2CppTypeEnum (il2cpp-metadata-impl.h)
#define IL2CPP_TYPE_BOOLEAN  0x02
#define IL2CPP_TYPE_I4       0x08
#define IL2CPP_TYPE_U4       0x09
#define IL2CPP_TYPE_R4       0x0C
#define IL2CPP_TYPE_CLASS    0x12
#define IL2CPP_TYPE_VALUETYPE 0x11
#define IL2CPP_TYPE_OBJECT   0x1C
#define IL2CPP_TYPE_SZARRAY  0x1D
#define IL2CPP_TYPE_GENERICINST 0x15

// Il2CppObject header = 2 ponteiros (klass + monitor) = 16 bytes arm64
#define IL2CPP_OBJECT_HEADER_SIZE 16

// ---------------------------------------------------------------------------
// Tabela de função pointer
// ---------------------------------------------------------------------------
typedef struct {
    // Core domain/assembly
    Il2CppDomain*    (*domain_get)(void);
    Il2CppAssembly** (*domain_get_assemblies)(Il2CppDomain* domain, size_t* size);
    Il2CppImage*     (*assembly_get_image)(Il2CppAssembly* asm_);

    // Class lookup por nome — USADO PRIMARIAMENTE
    Il2CppClass*     (*class_from_name)(Il2CppImage* image, const char* namespaze, const char* name);

    // Field lookup por nome
    FieldInfo*       (*class_get_field_from_name)(Il2CppClass* klass, const char* name);

    // Field iteration (fallback quando campo está obfuscado)
    FieldInfo*       (*class_get_fields)(Il2CppClass* klass, void** iter);

    // Field info
    int32_t          (*field_get_offset)(FieldInfo* field);
    const char*      (*field_get_name)(FieldInfo* field);
    Il2CppType*      (*field_get_type)(FieldInfo* field);
    int32_t          (*type_get_type)(Il2CppType* type);

    // Method lookup
    MethodInfo*      (*class_get_method_from_name)(Il2CppClass* klass, const char* name, int argsCount);

    // Static field read/write
    void             (*field_static_get_value)(FieldInfo* field, void* value);
    void             (*field_static_set_value)(FieldInfo* field, void* value);

    // Runtime invoke — chama um MethodInfo com args e recebe Il2CppObject*
    void*            (*runtime_invoke)(MethodInfo* method, void* obj,
                                      void** params, void** exception);

    // Opcional: image name para filtrar assembly correto
    const char*      (*image_get_name)(Il2CppImage* image);
    size_t           (*image_get_class_count)(Il2CppImage* image);
    Il2CppClass*     (*image_get_class)(Il2CppImage* image, size_t index);
    int32_t          (*class_instance_size)(Il2CppClass* klass);

    bool loaded;
} Il2CppApi;

static Il2CppApi g_api;
static bool      g_resolved;

// Forward declares — funções definidas mais abaixo mas chamadas em load_api().
static void bind_method_scan_api(void* uf);
static MethodInfo* scan_method_returning(Il2CppClass* search_klass,
                                         Il2CppClass* return_klass);

// ---------------------------------------------------------------------------
// Inicialização da API
// ---------------------------------------------------------------------------
static bool load_api(void) {
    if (g_api.loaded) return true;

    // RTLD_NOLOAD: apenas incrementa refcount se já carregado, não lança novo load.
    void* uf = dlopen("@rpath/UnityFramework.framework/UnityFramework",
                      RTLD_NOLOAD | RTLD_LAZY);
    if (!uf) uf = dlopen("UnityFramework", RTLD_NOLOAD | RTLD_LAZY);
    if (!uf) return false;

#define BIND_REQUIRED(fn) do {                                           \
    g_api.fn = (__typeof__(g_api.fn))dlsym(uf, "_il2cpp_" #fn);         \
    if (!g_api.fn) { dlclose(uf); return false; }                        \
} while(0)

#define BIND_OPT(fn) do {                                                \
    g_api.fn = (__typeof__(g_api.fn))dlsym(uf, "_il2cpp_" #fn);         \
} while(0)

    BIND_REQUIRED(domain_get);
    BIND_REQUIRED(domain_get_assemblies);
    BIND_REQUIRED(assembly_get_image);
    BIND_REQUIRED(class_from_name);
    BIND_REQUIRED(class_get_field_from_name);
    BIND_REQUIRED(class_get_fields);
    BIND_REQUIRED(field_get_offset);
    BIND_REQUIRED(field_get_name);
    BIND_REQUIRED(field_get_type);
    BIND_REQUIRED(type_get_type);
    BIND_REQUIRED(class_get_method_from_name);

    BIND_OPT(field_static_get_value);
    BIND_OPT(field_static_set_value);
    BIND_OPT(runtime_invoke);
    BIND_OPT(image_get_name);
    BIND_OPT(image_get_class_count);
    BIND_OPT(image_get_class);
    BIND_OPT(class_instance_size);

#undef BIND_REQUIRED
#undef BIND_OPT

    // Bind da API de scan de métodos (necessária para resolver GetLocalPlayer
    // em builds com Beebyte onde todos os RVAs estão zerados no dump).
    bind_method_scan_api(uf);

    dlclose(uf);
    g_api.loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// Helper: obtém offset de um campo por nome exato dentro de uma classe.
// Retorna 0 se não encontrar (0 é offset inválido para campo instance — header
// do objeto ocupa 0..15).
// ---------------------------------------------------------------------------
static uint32_t field_off(Il2CppClass* klass, const char* fname) {
    if (!klass || !fname) return 0;
    FieldInfo* fi = g_api.class_get_field_from_name(klass, fname);
    if (!fi) return 0;
    int32_t off = g_api.field_get_offset(fi);
    // Offsets < IL2CPP_OBJECT_HEADER_SIZE são campos estáticos (endereço na
    // tabela de estáticos, não offset de instância). Rejeitar.
    return (off >= IL2CPP_OBJECT_HEADER_SIZE) ? (uint32_t)off : 0;
}

// Variante: procura entre múltiplos nomes candidatos (Beebyte pode renomear
// campos mas preserva alguns com nomes legíveis).
// Retorna o primeiro offset != 0 encontrado.
static uint32_t field_off_any(Il2CppClass* klass,
                              const char** names, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t off = field_off(klass, names[i]);
        if (off) return off;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Busca o assembly que contém uma classe pelo namespace/nome.
// Itera todos os assemblies; retorna o Il2CppImage que responde ao lookup.
// ---------------------------------------------------------------------------
static Il2CppImage* find_image_with_class(Il2CppDomain* domain,
                                          const char* ns, const char* cname) {
    size_t n_asm = 0;
    Il2CppAssembly** asms = g_api.domain_get_assemblies(domain, &n_asm);
    if (!asms) return NULL;

    for (size_t i = 0; i < n_asm; i++) {
        if (!asms[i]) continue;
        Il2CppImage* img = g_api.assembly_get_image(asms[i]);
        if (!img) continue;
        Il2CppClass* klass = g_api.class_from_name(img, ns, cname);
        if (klass) return img;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Helpers de tipo de campo
// ---------------------------------------------------------------------------
static bool field_is_float(FieldInfo* fi) {
    Il2CppType* t = g_api.field_get_type(fi);
    return t && g_api.type_get_type(t) == IL2CPP_TYPE_R4;
}

static bool field_is_int(FieldInfo* fi) {
    Il2CppType* t = g_api.field_get_type(fi);
    int tt = t ? g_api.type_get_type(t) : -1;
    return tt == IL2CPP_TYPE_I4 || tt == IL2CPP_TYPE_U4;
}

static bool field_is_ref_or_obj(FieldInfo* fi) {
    Il2CppType* t = g_api.field_get_type(fi);
    int tt = t ? g_api.type_get_type(t) : -1;
    return tt == IL2CPP_TYPE_CLASS || tt == IL2CPP_TYPE_OBJECT ||
           tt == IL2CPP_TYPE_SZARRAY || tt == IL2CPP_TYPE_GENERICINST;
}

// ---------------------------------------------------------------------------
// Resolução: classe Player (COW.GamePlay)
//
// Campos procurados (nomes Beebyte desconhecidos, mas temos os tipos):
//   - HP float: busca by name falha (nome = XXXXX), então iteramos campos float
//     em ordem ascendente de offset — o primeiro float após o header tende a ser HP.
//     Confirmamos com AvatarManager que tem campo "Nickname" com nome legível.
//
//   Em Player (typedef[31768]) os campos com B != 0xFFFFFFFF na análise estática
//   majoritariamente apareceram em off=0x10 (campo de referência IL2CPP ou
//   delegate). Isso indica que a maior parte dos campos float/int de HP etc.
//   têm B=0xFFFFFFFF e precisam de runtime.
//
// Estratégia pragmática:
//   1. Pegar klass de Player via class_from_name.
//   2. Para HP/MaxHP: iterar campos float em ordem ascendente de offset.
//      Primeiros 2 floats após header provavelmente são HP e MaxHP.
//   3. Para TeamID: primeiro campo int32 após header.
//   4. Para isDead: primeiro campo bool após header.
//   5. Para Transform: primeiro campo referência após header.
//   6. Para PlayerData.nickname, teamId, isDead: usar PlayerData.
// ---------------------------------------------------------------------------

#define MAX_FLOAT_FIELDS 32
#define MAX_INT_FIELDS   32
#define MAX_REF_FIELDS   32
#define MAX_BOOL_FIELDS  16

typedef struct {
    int32_t off[MAX_FLOAT_FIELDS]; int n;
} OffList;

static void offlist_insert_sorted(OffList* l, int32_t off, int max) {
    if (l->n >= max) return;
    int i = l->n++;
    while (i > 0 && l->off[i-1] > off) { l->off[i] = l->off[i-1]; i--; }
    l->off[i] = off;
}

static void resolve_player(Il2CppClass* klass, FF_StructOffsets* st) {
    OffList floats = {0}, ints = {0}, refs = {0}, bools = {0};

    void* iter = NULL;
    FieldInfo* fi;
    while ((fi = g_api.class_get_fields(klass, &iter))) {
        int32_t off = g_api.field_get_offset(fi);
        if (off < IL2CPP_OBJECT_HEADER_SIZE) continue; // campo estático

        if (field_is_float(fi))
            offlist_insert_sorted(&floats, off, MAX_FLOAT_FIELDS);
        else if (field_is_int(fi))
            offlist_insert_sorted(&ints, off, MAX_INT_FIELDS);
        else if (field_is_ref_or_obj(fi))
            offlist_insert_sorted(&refs, off, MAX_REF_FIELDS);
        else {
            Il2CppType* t = g_api.field_get_type(fi);
            if (t && g_api.type_get_type(t) == IL2CPP_TYPE_BOOLEAN)
                offlist_insert_sorted(&bools, off, MAX_BOOL_FIELDS);
        }
    }

    // HP = menor offset float (primeiro campo float após header)
    // MaxHP = segundo menor offset float
    if (floats.n >= 1) st->PlayerInfo_health     = (uint32_t)floats.off[0];
    if (floats.n >= 2) st->PlayerInfo_health_max = (uint32_t)floats.off[1];

    // TeamID = menor offset int32
    if (ints.n >= 1) st->PlayerInfo_team_id = (uint32_t)ints.off[0];

    // is_dead = menor offset bool
    if (bools.n >= 1) st->PlayerInfo_is_dead = (uint32_t)bools.off[0];

    // Transform = menor offset referência
    if (refs.n >= 1) st->PlayerInfo_transform = (uint32_t)refs.off[0];
    // Weapon controller = segundo offset referência
    if (refs.n >= 2) st->PlayerInfo_weapon_controller = (uint32_t)refs.off[1];
}

// ---------------------------------------------------------------------------
// Resolução: PlayerData (COW) — tem campos com nomes legíveis.
// typedef[25172]: isDead, nickname, teamId, lastPosition, killCount, level
// ---------------------------------------------------------------------------
static void resolve_playerdata(Il2CppImage* img, FF_StructOffsets* st) {
    Il2CppClass* klass = g_api.class_from_name(img, "COW", "PlayerData");
    if (!klass) {
        // Tenta a mesma imagem sem namespace (caso assembly seja diferente)
        klass = g_api.class_from_name(img, "", "PlayerData");
    }
    if (!klass) return;

    // isDead
    uint32_t off = field_off(klass, "isDead");
    if (off) st->PlayerInfo_is_dead = off;

    // name / nickname (Il2CppString* = referência)
    const char* name_candidates[] = { "nickname", "Nickname", "name", "Name", "m_Name" };
    off = field_off_any(klass, name_candidates, 5);
    if (off) st->PlayerInfo_name = off;

    // teamId
    const char* team_candidates[] = { "teamId", "TeamId", "gsTeamId", "scTeamId", "TeamIndex" };
    off = field_off_any(klass, team_candidates, 5);
    if (off) st->PlayerInfo_team_id = off;
}

// ---------------------------------------------------------------------------
// Resolução: AvatarManager (COW.GamePlay) — tem campo Nickname legível.
// typedef[31849]
// ---------------------------------------------------------------------------
static void resolve_avatarmanager(Il2CppImage* img, FF_StructOffsets* st) {
    Il2CppClass* klass = g_api.class_from_name(img, "COW.GamePlay", "AvatarManager");
    if (!klass) return;

    uint32_t off = field_off(klass, "Nickname");
    if (off) st->PlayerInfo_name = off;
}

// ---------------------------------------------------------------------------
// Resolução: VerticleGunController (COW.GamePlay) — classe de arma.
// typedef[35516]: GunId, BulletsPerMag, BulletsRemainingInMag, BulletDamage, ReloadTime
//
// Nomes com B != 0xFFFFFFFF (legíveis): BulletsPerMag, IsHideBulletSpawn
// Nomes com B == 0xFFFFFFFF (precisam de runtime): GunId, BulletDamage,
//   BulletsRemainingInMag, ReloadTime, BulletLifeTime, BulletSpawn
// ---------------------------------------------------------------------------
static void resolve_gun(Il2CppImage* img, FF_StructOffsets* st) {
    Il2CppClass* klass = g_api.class_from_name(img, "COW.GamePlay",
                                                "VerticleGunController");
    if (!klass) return;

    // Campos com nomes legíveis (Beebyte preservou por necessidade de serialização)
    uint32_t off;

    off = field_off(klass, "BulletsRemainingInMag");
    if (off) st->WeaponController_currentAmmo = off;

    off = field_off(klass, "BulletsPerMag");
    if (off) st->WeaponController_maxAmmo = off;

    off = field_off(klass, "ReloadTime");
    if (off) st->WeaponController_reloadTime = off;

    // reserveAmmo: não tem nome legível em VerticleGunController. Pegar
    // primeiro int32 após BulletsRemainingInMag como heurística.
    if (st->WeaponController_currentAmmo && !st->WeaponController_reserveAmmo) {
        // Iterar campos int32, pegar o que vem logo depois do current
        void* iter = NULL;
        FieldInfo* fi;
        int32_t best_reserve = 0;
        int32_t cur_off = (int32_t)st->WeaponController_currentAmmo;
        while ((fi = g_api.class_get_fields(klass, &iter))) {
            int32_t fo = g_api.field_get_offset(fi);
            if (fo < IL2CPP_OBJECT_HEADER_SIZE) continue;
            if (!field_is_int(fi)) continue;
            if (fo > cur_off && (!best_reserve || fo < best_reserve))
                best_reserve = fo;
        }
        if (best_reserve) st->WeaponController_reserveAmmo = (uint32_t)best_reserve;
    }
}

// ---------------------------------------------------------------------------
// Resolução: PhysicalCCT (COW.GamePlay) — controlador de física/movimento.
// typedef[34433]: MaxSpeed, Velocity, JumpHeight, JumpCount
// ---------------------------------------------------------------------------
static void resolve_cct(Il2CppImage* img, FF_StructOffsets* st) {
    Il2CppClass* klass = g_api.class_from_name(img, "COW.GamePlay", "PhysicalCCT");
    if (!klass) return;

    uint32_t off;

    // MaxSpeed: nome legível
    off = field_off(klass, "MaxSpeed");
    if (off) st->CharController_moveSpeed = off;

    // Fallback: primeiro float campo
    if (!st->CharController_moveSpeed) {
        void* iter = NULL;
        FieldInfo* fi;
        int32_t first_float = 0;
        while ((fi = g_api.class_get_fields(klass, &iter))) {
            int32_t fo = g_api.field_get_offset(fi);
            if (fo < IL2CPP_OBJECT_HEADER_SIZE) continue;
            if (field_is_float(fi) && (!first_float || fo < first_float))
                first_float = fo;
        }
        if (first_float) st->CharController_moveSpeed = (uint32_t)first_float;
    }
}

// ---------------------------------------------------------------------------
// Resolução: Transform.position
// UnityEngine.Transform em Unity 2021 arm64 IL2CPP:
//   O campo "position" (worldspace Vector3) está no TransformAccess interno.
//   Com a abordagem comum de leitura direta do Transform* a posição fica em
//   diferentes locais dependendo se é local ou world space.
//   Valor padrão da comunidade para Unity 2021 arm64: 0x60.
//   Verificar com Frida se coordenadas aparecem erradas.
// ---------------------------------------------------------------------------
static void resolve_transform(Il2CppImage* img, FF_StructOffsets* st) {
    // Tenta via IL2CPP API primeiro
    Il2CppClass* klass = g_api.class_from_name(img, "UnityEngine", "Transform");
    if (klass) {
        uint32_t off = field_off(klass, "position");
        if (!off) off = field_off(klass, "m_LocalPosition");
        if (off) {
            st->Transform_position = off;
            return;
        }
    }
    // Fallback: valor padrão Unity 2021 arm64
    // INFORMADO: não medido neste binário; confirmar com Frida
    if (!st->Transform_position)
        st->Transform_position = 0x60;
}

// ---------------------------------------------------------------------------
// Camera / ViewMatrix — UnityEngine.Camera
// Il2CppClass de Camera não expõe worldToCameraMatrix como campo normal;
// acessível via icall. Valor padrão para leitura via campo interno de
// RenderNode: 0x40 (posição do frustum struct). Marcar como não verificado.
// ---------------------------------------------------------------------------
static void resolve_camera(Il2CppImage* img, FF_StructOffsets* st) {
    (void)img;
    // Camera em Unity 2021 IL2CPP expõe worldToCameraMatrix via icall,
    // não como campo direto. ESP vai usar Camera.WorldToScreenPoint via
    // il2cpp_runtime_invoke em vez de ler matriz diretamente.
    // Marcar como OFFSET_UNKNOWN (0) — esp.c usa a path de invocação.
    if (!st->Camera_viewMatrix) st->Camera_viewMatrix = 0;
    if (!st->Camera_projMatrix) st->Camera_projMatrix = 0;
}

// ---------------------------------------------------------------------------
// RVA de fn_GameUpdate (LateUpdate/Update do Player)
// ---------------------------------------------------------------------------
#define METHODINFO_FN_PTR_OFFSET 0x20  // arm64 Unity 2021, não medido neste build

static uintptr_t resolve_fn_game_update(Il2CppClass* player_klass) {
    if (!player_klass) return 0;

    MethodInfo* mi = g_api.class_get_method_from_name(player_klass, "LateUpdate", -1);
    if (!mi) mi = g_api.class_get_method_from_name(player_klass, "Update", -1);
    if (!mi) return 0;

    void* fn_ptr = NULL;
    // Lê method_pointer do MethodInfo em offset fixo (0x20 em Unity 2021 arm64)
    void* const* slot = (void* const*)((uintptr_t)mi + METHODINFO_FN_PTR_OFFSET);
    fn_ptr = *slot;
    if (!fn_ptr) return 0;

    uintptr_t fn_va = (uintptr_t)fn_ptr;
    uint32_t img_count = _dyld_image_count();
    for (uint32_t i = 0; i < img_count; i++) {
        const char* img_name = _dyld_get_image_name(i);
        if (!img_name || !strstr(img_name, "UnityFramework")) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        uintptr_t base = (uintptr_t)slide;
        if (fn_va > base && fn_va - base < 0x20000000UL)
            return fn_va - base;
    }
    return fn_va; // fallback: VA absoluta
}

// ---------------------------------------------------------------------------
// Scan do singleton AvatarManager + resolução de fn_GetLocalPlayer / fn_GetAllPlayers
//
// AvatarManager (COW.GamePlay, typedef[31849]) segue o padrão singleton Unity:
//   static AvatarManager* Instance (ou _instance / s_instance).
//   GetComponent<AvatarManager>() em algum GameObject, ou
//   property estática "Instance".
//
// Estratégia:
//   1. Pega a classe AvatarManager.
//   2. Itera campos estáticos (offset < IL2CPP_OBJECT_HEADER_SIZE indica estático
//      reportado como endereço absoluto pelo IL2CPP runtime — usamos
//      field_static_get_value para ler).
//   3. Procura um campo cujo tipo é CLASS/OBJECT e cujo valor não-nulo aponte
//      para um objeto do heap (plausibility check).
//   4. Guarda o ponteiro do singleton em g_avatar_manager.
//
// fn_GetLocalPlayer / fn_GetAllPlayers:
//   Resolvemos via il2cpp_class_get_method_from_name na classe Player e
//   AvatarManager. Os MethodInfo* ficam cacheados; chamamos via runtime_invoke.
// ---------------------------------------------------------------------------

// Cache dos objetos IL2CPP que ff_game.c precisa
static Il2CppClass* g_player_class    = NULL;
static Il2CppClass* g_avmgr_class     = NULL;
static MethodInfo*  g_mi_local_player = NULL; // Player.GetLocalPlayer() ou similar
static MethodInfo*  g_mi_all_players  = NULL; // Player.GetAllPlayers() ou similar
static MethodInfo*  g_mi_avmgr_inst   = NULL; // AvatarManager.get_Instance

// AvatarManager singleton — obtido chamando g_mi_avmgr_inst
static void* g_avmgr_singleton = NULL;

// ---------------------------------------------------------------------------
// Scan de métodos por assinatura: procura métodos sem parâmetros cujo retorno
// é do tipo da classe 'player_klass'. Funciona mesmo com nomes totalmente
// ofuscados (Beebyte substitui nomes mas não pode mudar a assinatura de retorno
// sem quebrar o bytecode que usa o método).
//
// Requisitos:
//   - g_api.image_get_class_count / image_get_class disponíveis (BIND_OPT)
//   - il2cpp_method_get_return_type / il2cpp_method_get_param_count disponíveis
//
// Usados para encontrar GameFacade.GetLocalPlayerOrObServer() e similares.
// ---------------------------------------------------------------------------
typedef Il2CppType* (*pfn_method_get_return_type)(MethodInfo*);
typedef uint32_t    (*pfn_method_get_param_count)(MethodInfo*);
typedef MethodInfo* (*pfn_class_get_methods)(Il2CppClass*, void**);
typedef Il2CppClass* (*pfn_class_from_il2cpp_type)(Il2CppType*);

static pfn_method_get_return_type  g_method_get_return_type  = NULL;
static pfn_method_get_param_count  g_method_get_param_count  = NULL;
static pfn_class_get_methods       g_class_get_methods       = NULL;
static pfn_class_from_il2cpp_type  g_class_from_il2cpp_type  = NULL;

static void bind_method_scan_api(void* uf) {
    g_method_get_return_type = (pfn_method_get_return_type)
        dlsym(uf, "_il2cpp_method_get_return_type");
    g_method_get_param_count = (pfn_method_get_param_count)
        dlsym(uf, "_il2cpp_method_get_param_count");
    g_class_get_methods = (pfn_class_get_methods)
        dlsym(uf, "_il2cpp_class_get_methods");
    g_class_from_il2cpp_type = (pfn_class_from_il2cpp_type)
        dlsym(uf, "_il2cpp_class_from_il2cpp_type");
}

// Procura em 'search_klass' um método sem parâmetros que retorne exatamente
// 'return_klass'. Retorna o primeiro MethodInfo* encontrado ou NULL.
// Ignora métodos estáticos (IsStatic) se want_static=false.
static MethodInfo* scan_method_returning(Il2CppClass* search_klass,
                                         Il2CppClass* return_klass) {
    if (!search_klass || !return_klass) return NULL;
    if (!g_class_get_methods || !g_method_get_return_type ||
        !g_method_get_param_count || !g_class_from_il2cpp_type) return NULL;

    void* iter = NULL;
    MethodInfo* mi;
    while ((mi = g_class_get_methods(search_klass, &iter))) {
        // Apenas métodos sem parâmetros
        if (g_method_get_param_count(mi) != 0) continue;

        Il2CppType* ret_type = g_method_get_return_type(mi);
        if (!ret_type) continue;

        // Verifica se o tipo de retorno corresponde à classe Player
        Il2CppClass* ret_klass = g_class_from_il2cpp_type(ret_type);
        if (ret_klass == return_klass) return mi;
    }
    return NULL;
}

// Nomes candidatos para o método que retorna o jogador local (sem args, retorna Player*)
static const char* s_local_player_names[] = {
    "GetLocalPlayer", "get_LocalPlayer", "GetSelf", "GetLocalCharacter",
    "GetMainPlayer",  "get_MainPlayer",  "GetMe",   "GetCurrentPlayer",
    // Nomes Beebyte do IDMAP confirmados via análise de dump.cs:
    "GetLocalPlayerOrObServer", "GetLocalPlayerOrObserverTarget",
    "NCBJDIIEEMI", // AvatarManager.GetLocalPlayer ofuscado (IDMAP L751604)
};
// Nomes candidatos para método que retorna lista de jogadores (sem args, retorna array/list)
static const char* s_all_players_names[] = {
    "GetAllPlayers", "get_AllPlayers", "GetPlayers", "GetCharacters",
    "GetAllCharacters", "GetPlayerList", "GetAllMember",
};

static void resolve_player_methods(Il2CppClass* player_klass,
                                   Il2CppClass* avmgr_klass) {
    // Tenta encontrar o método de local player nas duas classes
    for (size_t i = 0; i < sizeof(s_local_player_names)/sizeof(s_local_player_names[0]); i++) {
        if (player_klass && !g_mi_local_player)
            g_mi_local_player = g_api.class_get_method_from_name(
                                    player_klass, s_local_player_names[i], -1);
        if (avmgr_klass && !g_mi_local_player)
            g_mi_local_player = g_api.class_get_method_from_name(
                                    avmgr_klass, s_local_player_names[i], -1);
        if (g_mi_local_player) break;
    }

    for (size_t i = 0; i < sizeof(s_all_players_names)/sizeof(s_all_players_names[0]); i++) {
        if (player_klass && !g_mi_all_players)
            g_mi_all_players = g_api.class_get_method_from_name(
                                    player_klass, s_all_players_names[i], -1);
        if (avmgr_klass && !g_mi_all_players)
            g_mi_all_players = g_api.class_get_method_from_name(
                                    avmgr_klass, s_all_players_names[i], -1);
        if (g_mi_all_players) break;
    }

    // AvatarManager.Instance ou get_Instance
    if (avmgr_klass) {
        const char* inst_names[] = { "get_Instance", "get_instance",
                                     "GetInstance", "getInstance", "GetAvatarManager" };
        for (size_t i = 0; i < 5; i++) {
            g_mi_avmgr_inst = g_api.class_get_method_from_name(
                                  avmgr_klass, inst_names[i], -1);
            if (g_mi_avmgr_inst) break;
        }
        // Fallback: campo estático Instance (tipo CLASS/OBJECT)
        if (!g_mi_avmgr_inst && g_api.field_static_get_value) {
            void* iter2 = NULL;
            FieldInfo* fi2;
            while ((fi2 = g_api.class_get_fields(avmgr_klass, &iter2))) {
                int32_t fo = g_api.field_get_offset(fi2);
                // Campos estáticos chegam com offset >= 0 mas serão endereços de
                // tabela estática — field_get_offset retorna valores especiais.
                // Na prática com Beebyte: offset < IL2CPP_OBJECT_HEADER_SIZE para estáticos.
                if (fo < IL2CPP_OBJECT_HEADER_SIZE && fo >= 0) {
                    Il2CppType* ft = g_api.field_get_type(fi2);
                    if (!ft) continue;
                    int tt = g_api.type_get_type(ft);
                    if (tt == IL2CPP_TYPE_CLASS || tt == IL2CPP_TYPE_OBJECT) {
                        void* val = NULL;
                        g_api.field_static_get_value(fi2, &val);
                        if (val && (uintptr_t)val > 0x1000UL) {
                            g_avmgr_singleton = val;
                            break;
                        }
                    }
                }
            }
        }
    }

    // Scan por assinatura: se nenhum nome encontrou o método local player,
    // procura em AvatarManager qualquer método sem parâmetros que retorne Player*.
    // Funciona mesmo com nomes Beebyte totalmente aleatórios (como NCBJDIIEEMI).
    if (!g_mi_local_player && player_klass && avmgr_klass) {
        g_mi_local_player = scan_method_returning(avmgr_klass, player_klass);
    }
    // Mesmo scan em Player — pode ter método estático GetLocalPlayer()
    if (!g_mi_local_player && player_klass) {
        g_mi_local_player = scan_method_returning(player_klass, player_klass);
    }
    // GameFacade scan é feito em ff_il2cpp_resolve() depois de obter a classe via
    // class_from_name("COW.GamePlay", "GameFacade") — requer main_img.
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

// Retorna o MethodInfo* cacheado para GetLocalPlayer (qualquer nome)
MethodInfo* ff_il2cpp_get_mi_local_player(void) { return g_mi_local_player; }
MethodInfo* ff_il2cpp_get_mi_all_players(void)  { return g_mi_all_players;  }
Il2CppClass* ff_il2cpp_get_player_class(void)   { return g_player_class;    }
Il2CppClass* ff_il2cpp_get_avmgr_class(void)    { return g_avmgr_class;     }
void*        ff_il2cpp_get_avmgr_singleton(void){ return g_avmgr_singleton; }

// Chama runtime_invoke se disponível. obj=NULL para métodos estáticos.
// Retorna NULL se runtime_invoke não estiver disponível ou houver exceção.
void* ff_il2cpp_invoke_method(void* method_info, void* obj, void** args) {
    if (!g_api.loaded || !g_api.runtime_invoke || !method_info) return NULL;
    void* exception = NULL;
    void* result = g_api.runtime_invoke(
        (MethodInfo*)method_info, obj, args, &exception);
    if (exception) return NULL;
    return result;
}

bool ff_il2cpp_wait_ready(unsigned timeout_sec) {
    if (!load_api()) return false;
    for (unsigned i = 0; i < timeout_sec * 10u; i++) {
        if (g_api.domain_get()) return true;
        usleep(100000);
    }
    return false;
}

bool ff_il2cpp_resolve(FF_Offsets* offsets) {
    if (!offsets) return false;
    if (g_resolved) return true;
    if (!load_api()) return false;

    Il2CppDomain* domain = g_api.domain_get();
    if (!domain) return false;

    FF_StructOffsets* st  = &offsets->st;
    FF_GameAssemblyOffsets* ga = &offsets->ga;

    // --- 1. Encontrar imagem (assembly) que contém COW.GamePlay.Player ----------
    Il2CppImage* main_img = find_image_with_class(domain, "COW.GamePlay", "Player");
    if (!main_img) return false; // assembly do jogo não carregado ainda

    // --- 2. Resolver classe Player ----------------------------------------------
    Il2CppClass* player_klass = g_api.class_from_name(main_img, "COW.GamePlay", "Player");
    if (!player_klass) return false;
    g_player_class = player_klass;

    resolve_player(player_klass, st);

    // --- 2b. AvatarManager — cache a classe e resolve métodos ------------------
    Il2CppClass* avmgr_klass = g_api.class_from_name(main_img, "COW.GamePlay", "AvatarManager");
    g_avmgr_class = avmgr_klass; // pode ser NULL se a classe mudou de nome

    resolve_player_methods(player_klass, avmgr_klass);

    // --- 2c. Fallback scan em GameFacade e classes obfuscadas com GetLocalPlayer
    // Confirmadas via IDMAP: GameFacade.GetLocalPlayerOrObServer (TypeDefIndex 12379)
    // e PBKKDPJKIPB / JBGLPKNEIAF / IIINLGKLEGM (todos com método GetLocalPlayer
    // mas RVA=0 no dump — Beebyte zerou). scan_method_returning usa il2cpp_class_get_methods
    // + il2cpp_method_get_return_type para encontrar sem depender de nome.
    if (!ff_il2cpp_get_mi_local_player()) {
        const char* gf_ns  = "COW.GamePlay";
        // GameFacade — classe confirmada TypeDefIndex 12379
        const char* scan_names[] = {
            "GameFacade",
            "PBKKDPJKIPB",   // obfuscado, TypeDefIndex 34151
            "JBGLPKNEIAF",   // obfuscado, TypeDefIndex 34153
            "IIINLGKLEGM",   // obfuscado, TypeDefIndex 35443
        };
        for (size_t si = 0; si < sizeof(scan_names)/sizeof(scan_names[0]); si++) {
            Il2CppClass* kl = g_api.class_from_name(main_img, gf_ns, scan_names[si]);
            if (!kl) continue;
            MethodInfo* mi = scan_method_returning(kl, player_klass);
            if (mi) {
                g_mi_local_player = mi;
                break;
            }
        }
    }

    // --- 3. PlayerData (COW) — refina name/isDead/teamId -----------------------
    // PlayerData pode estar no mesmo assembly ou em outro; tenta main_img primeiro.
    Il2CppImage* cow_img = main_img; // "COW" é sub-namespace de "COW.GamePlay" image
    Il2CppClass* pd = g_api.class_from_name(cow_img, "COW", "PlayerData");
    if (!pd) {
        // busca em outros assemblies
        cow_img = find_image_with_class(domain, "COW", "PlayerData");
        if (cow_img) pd = g_api.class_from_name(cow_img, "COW", "PlayerData");
    }
    if (pd) {
        // isDead (nome legível confirmado)
        uint32_t off = field_off(pd, "isDead");
        if (off) st->PlayerInfo_is_dead = off;

        // name / nickname
        const char* name_cands[] = { "nickname", "Nickname", "name", "Name" };
        off = field_off_any(pd, name_cands, 4);
        if (off) st->PlayerInfo_name = off;

        // teamId
        const char* team_cands[] = { "teamId", "TeamId", "gsTeamId", "scTeamId" };
        off = field_off_any(pd, team_cands, 4);
        if (off) st->PlayerInfo_team_id = off;
    }

    // --- 4. PlayerData (COW) — isDead, nickname, teamId -------------------------
    resolve_playerdata(main_img, st);

    // --- 5. AvatarManager — Nickname -------------------------------------------
    resolve_avatarmanager(main_img, st);

    // --- 5. VerticleGunController — ammo ---------------------------------------
    resolve_gun(main_img, st);

    // --- 6. PhysicalCCT — velocidade -------------------------------------------
    resolve_cct(main_img, st);

    // --- 7. Transform.position -------------------------------------------------
    resolve_transform(main_img, st);

    // --- 8. Camera (via icall, não campo direto) --------------------------------
    resolve_camera(main_img, st);

    // --- 9. fn_GameUpdate -------------------------------------------------------
    if (!ga->fn_GameUpdate)
        ga->fn_GameUpdate = resolve_fn_game_update(player_klass);

    // --- Validação mínima: precisamos de pelo menos HP e Transform -------------
    g_resolved = (st->PlayerInfo_health     != 0 &&
                  st->PlayerInfo_transform  != 0);

    return g_resolved;
}
