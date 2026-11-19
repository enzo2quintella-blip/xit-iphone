// ff_offsets.h — offsets de structs do Free Fire por versão de build
//
// COMO USAR:
//   1. Rode Tools/dump_offsets.py no device (precisa de acesso ao processo)
//   2. Ele gera Config/offsets_<buildUUID>.json
//   3. ff_offsets_load() carrega o JSON correto na inicialização
//
// COMO ENCONTRAR MANUALMENTE:
//   - GameAssembly.dylib: use Il2CppDumper com global-metadata.dat
//   - Strings de debug no binário (ex: "LocalPlayer", "PlayerInfo")
//   - Busca por RTTI: Il2CppClass* para "PlayerInfo", xref para vtable
//
// Cada campo OFFSET_UNKNOWN tem comentário com dica de como achar.
#pragma once

#include "ff_types.h"

// ─── Offsets dentro de GameAssembly.dylib (RVA da base do módulo) ─────────────

typedef struct FF_GameAssemblyOffsets {
    // ── Funções exportadas do IL2CPP runtime ──
    uintptr_t il2cpp_domain_get;        // export: "il2cpp_domain_get"
    uintptr_t il2cpp_domain_get_assemblies; // export: "il2cpp_domain_get_assemblies"
    uintptr_t il2cpp_class_from_name;   // export: "il2cpp_class_from_name"
    uintptr_t il2cpp_class_get_method_from_name; // export
    uintptr_t il2cpp_class_get_field_from_name;  // export
    uintptr_t il2cpp_field_get_offset;  // export
    uintptr_t il2cpp_field_static_get_value; // export
    uintptr_t il2cpp_field_static_set_value; // export
    uintptr_t il2cpp_object_new;        // export
    uintptr_t il2cpp_runtime_invoke;    // export

    // ── Classes do jogo (ponteiro para Il2CppClass*) ──
    // Para achar: Il2CppDumper → dump.cs → busca pela classe → pega o RVA
    uintptr_t klass_PlayerInfo;         // OFFSET_UNKNOWN — busca "PlayerInfo" no dump.cs
    uintptr_t klass_GameController;     // OFFSET_UNKNOWN — busca "GameController"
    uintptr_t klass_WeaponController;   // OFFSET_UNKNOWN — busca "WeaponController"
    uintptr_t klass_BulletControl;      // OFFSET_UNKNOWN — busca "BulletControl"
    uintptr_t klass_VehicleControl;     // OFFSET_UNKNOWN
    uintptr_t klass_AntiCheat;          // OFFSET_UNKNOWN — classe de verificação AC

    // ── Métodos estáticos úteis ──
    uintptr_t fn_GetLocalPlayer;        // OFFSET_UNKNOWN — método que retorna o jogador local
    uintptr_t fn_GetAllPlayers;         // OFFSET_UNKNOWN — retorna lista de todos jogadores
    uintptr_t fn_WorldToScreen;         // OFFSET_UNKNOWN — Camera.WorldToScreenPoint ou similar
    uintptr_t fn_GameUpdate;            // OFFSET_UNKNOWN — LateUpdate/Update do GameController

} FF_GameAssemblyOffsets;

// ─── Offsets dentro das structs do jogo (campo dentro do objeto IL2CPP) ───────

typedef struct FF_StructOffsets {
    // PlayerInfo struct
    uint32_t PlayerInfo_health;             // OFFSET_UNKNOWN — campo HP atual
    uint32_t PlayerInfo_health_max;         // OFFSET_UNKNOWN — campo HP máximo
    uint32_t PlayerInfo_armor;              // OFFSET_UNKNOWN — campo armadura
    uint32_t PlayerInfo_name;               // OFFSET_UNKNOWN — Il2CppString* nome
    uint32_t PlayerInfo_team_id;            // OFFSET_UNKNOWN — int32 ID do time
    uint32_t PlayerInfo_is_dead;            // OFFSET_UNKNOWN — bool morto
    uint32_t PlayerInfo_is_local;           // OFFSET_UNKNOWN — bool é o jogador local
    uint32_t PlayerInfo_transform;          // OFFSET_UNKNOWN — UnityEngine.Transform*
    uint32_t PlayerInfo_bone_root;          // OFFSET_UNKNOWN — Transform raiz do esqueleto
    uint32_t PlayerInfo_weapon_controller;  // OFFSET_UNKNOWN — WeaponController*

    // Transform (UnityEngine.Transform interno, não Mono public)
    uint32_t Transform_position;        // OFFSET_UNKNOWN — Vec3 posição world
    uint32_t Transform_rotation;        // OFFSET_UNKNOWN — Quaternion rotação

    // WeaponController
    uint32_t WeaponController_currentAmmo;    // OFFSET_UNKNOWN
    uint32_t WeaponController_maxAmmo;        // OFFSET_UNKNOWN
    uint32_t WeaponController_reserveAmmo;    // OFFSET_UNKNOWN — ammo em reserva
    uint32_t WeaponController_reloadTime;     // OFFSET_UNKNOWN — float tempo de reload
    uint32_t WeaponController_spreadFactor;   // OFFSET_UNKNOWN — float dispersão

    // CharacterController (movimento)
    uint32_t CharController_moveSpeed;        // OFFSET_UNKNOWN — velocidade de movimento

    // GameController (singleton)
    uint32_t GameController_instance;        // OFFSET_UNKNOWN — campo static Instance
    uint32_t GameController_allPlayers;      // OFFSET_UNKNOWN — List<PlayerInfo>
    uint32_t GameController_gamePhase;       // OFFSET_UNKNOWN — enum fase do jogo

    // Camera / ViewMatrix
    uint32_t Camera_projMatrix;         // OFFSET_UNKNOWN — Matrix4x4
    uint32_t Camera_viewMatrix;         // OFFSET_UNKNOWN — Matrix4x4

} FF_StructOffsets;

// ─── Container geral de offsets ──────────────────────────────────────────────

typedef struct FF_Offsets {
    char     build_uuid[64];   // UUID do build do GameAssembly.dylib que esses offsets cobrem
    uint32_t game_version[3];  // major.minor.patch

    FF_GameAssemblyOffsets ga;
    FF_StructOffsets       st;
} FF_Offsets;

// Carrega offsets do arquivo JSON correspondente ao build em execução.
// Retorna FF_OK ou FF_ERR_VERSION se não houver config para esse build.
ff_status_t ff_offsets_load(FF_Offsets *out);

// Retorna true se o campo ainda é OFFSET_UNKNOWN (não foi medido)
static inline bool offset_is_unknown(uintptr_t v) {
    return v == OFFSET_UNKNOWN;
}
