// ff_types.h — tipos e structs base do projeto
// Offsets marcados OFFSET_UNKNOWN precisam ser medidos no device/IDA
#pragma once

#include <stdint.h>
#include <stdbool.h>

// ─── Build identity ───────────────────────────────────────────────────────────
#define PROJECT_NAME     "FFCheat"
#define PROJECT_VERSION  "1.0.0"

// Valor sentinela: offset ainda não medido
#define OFFSET_UNKNOWN   ((uintptr_t)0xDEADC0DEDEADC0DEull)

// ─── Tipos de ponteiro de jogo (IL2CPP) ──────────────────────────────────────
typedef struct Il2CppObject {
    void *klass;    // +0x00  ponteiro para Il2CppClass
    void *monitor;  // +0x08
} Il2CppObject;

typedef struct Il2CppString {
    Il2CppObject obj;
    int32_t      length;   // +0x10
    uint16_t     chars[1]; // +0x14  UTF-16
} Il2CppString;

typedef struct Il2CppArray {
    Il2CppObject obj;
    void        *bounds;   // +0x10
    uintptr_t    max_length; // +0x18
    // data começa em +0x20
} Il2CppArray;

// ─── Vetor 3D ─────────────────────────────────────────────────────────────────
typedef struct Vec3 {
    float x, y, z;
} Vec3;

typedef struct Vec2 {
    float x, y;
} Vec2;

typedef struct Quaternion {
    float x, y, z, w;
} Quaternion;

typedef struct Matrix4x4 {
    float m[4][4]; // coluna-maior (Unity padrão)
} Matrix4x4;

// ─── Resultado de operações ───────────────────────────────────────────────────
typedef enum {
    FF_OK                 = 0,
    FF_OK_PARTIAL         = 8,   // resolvido parcialmente (alguns offsets desconhecidos)
    FF_ERR_NOT_FOUND      = 1,
    FF_ERR_INVALID_OFFSET = 2,
    FF_ERR_MEM_READ       = 3,
    FF_ERR_MEM_WRITE      = 4,
    FF_ERR_PROC           = 5,
    FF_ERR_EXPLOIT        = 6,
    FF_ERR_VERSION        = 7,
} ff_status_t;

// ─── Bounds para validação de ponteiros ──────────────────────────────────────
// Preenchido em runtime por ff_init() após localizar o módulo
typedef struct ModuleRange {
    uintptr_t base;
    uintptr_t end;   // base + size
} ModuleRange;

static inline bool ptr_in_module(uintptr_t ptr, const ModuleRange *r) {
    return ptr >= r->base && ptr < r->end;
}

static inline bool ptr_valid(uintptr_t ptr) {
    // Rejeita null, não-alinhado a 4 bytes, e endereços de kernel (bit63 set)
    return ptr != 0
        && (ptr & 0x3) == 0
        && (ptr >> 63) == 0;
}
