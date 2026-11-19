// hooks.h — infraestrutura de hooks inline (trampoline) para arm64
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ─── Trampoline hook arm64 ────────────────────────────────────────────────────
// Instala um hook numa função arm64 escrevendo:
//   LDR X16, [PC+8]   ; carrega endereço absoluto do hook
//   BR X16
//   .quad <hook_va>
// (12 bytes, sem depender de range de BL/B)
// Salva os bytes originais + stub para chamar a função original.

typedef struct {
    uintptr_t  target_va;      // VA da função hookada
    uintptr_t  hook_fn;        // VA da nossa função
    uint8_t    orig_bytes[16]; // bytes originais salvos (suficientes para o stub)
    uintptr_t  trampoline_va;  // VA do trampoline (original + B.back) — alocado em RWX
    bool       installed;
} HookEntry;

#define MAX_HOOKS 32
extern HookEntry g_hooks[MAX_HOOKS];
extern int       g_hook_count;

// Instala hook em target_va. hook_fn será chamado em lugar de target.
// orig_out: recebe VA do trampoline para chamar a função original.
// Retorna false em erro (aloca memória RWX via mmap).
bool hook_install(uintptr_t target_va, uintptr_t hook_fn, uintptr_t *orig_out);

// Remove todos os hooks instalados (restaura bytes originais).
void hooks_cleanup(void);

// ─── VMT hook ────────────────────────────────────────────────────────────────
// Troca um ponteiro em uma vtable (endereço em memória mapeada como RW).
// vtable_ptr: ponteiro para o array de ponteiros de função (a vtable em si).
// index: índice do método a substituir.
// new_fn: novo ponteiro de função.
// orig_out: recebe o ponteiro original.
bool vmt_hook(void **vtable_ptr, int index, void *new_fn, void **orig_out);
void vmt_unhook(void **vtable_ptr, int index, void *orig_fn);
