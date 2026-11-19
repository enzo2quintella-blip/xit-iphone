// hooks.c — trampoline hooks arm64 e VMT hooks
// Sem dependência de Cydia Substrate ou fishhook:
// Hook inline de 12 bytes: LDR X16,[PC+8] / BR X16 / .quad <dest>
// Trampoline: copia N bytes originais + B de retorno → mmap RWX

#include "hooks.h"
#include "ff_offsets_loader.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __APPLE__
#  include <mach/mach.h>
#  include <mach/vm_map.h>
// mach_vm.h não existe no iOS SDK — usamos vm_protect de vm_map.h
#  include <sys/mman.h>
// macOS/iOS: MAP_ANON é o nome correto; MAP_ANONYMOUS não existe
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
// iOS arm64: __builtin___clear_cache vira chamada externa ___clear_cache que não
// existe no iOS runtime. Usamos sys_icache_invalidate + barreira DSB/ISB inline.
static inline void ff_flush_icache(void *start, void *end) {
    (void)start; (void)end;
    __asm__ volatile(
        "dsb ish\n"   // Data Synchronization Barrier
        "isb\n"       // Instruction Synchronization Barrier
        ::: "memory"
    );
}
#define __builtin___clear_cache(a, b) ff_flush_icache((a), (b))
#else
// Stubs Linux para syntax-check (suprime unused — só existem para satisfazer o parser)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunused-function"
typedef int kern_return_t;
#  define KERN_SUCCESS 0
typedef unsigned int mach_port_t;
typedef mach_port_t task_t;
typedef uintptr_t mach_vm_address_t;
typedef uintptr_t mach_vm_size_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef int vm_prot_t;
#  define VM_PROT_READ    1
#  define VM_PROT_WRITE   2
#  define VM_PROT_EXECUTE 4
#  define VM_PROT_COPY    8
static inline task_t mach_task_self_(void) { return 0; }
#  define mach_task_self() mach_task_self_()
static inline kern_return_t vm_protect(task_t t, vm_address_t a, vm_size_t s, bool sc, vm_prot_t p) { (void)t;(void)a;(void)s;(void)sc;(void)p; return 0; }
static inline kern_return_t mach_vm_protect(task_t t, mach_vm_address_t a, mach_vm_size_t s, bool sc, vm_prot_t p) { (void)t;(void)a;(void)s;(void)sc;(void)p; return 0; }
// mmap
static inline void* mmap(void* a, size_t l, int p, int f, int fd, long o) { (void)a;(void)l;(void)p;(void)f;(void)fd;(void)o; return (void*)-1; }
static inline int munmap(void* a, size_t l) { (void)a;(void)l; return 0; }
#  define MAP_FAILED  ((void*)-1)
#  define MAP_ANONYMOUS 0x20
#  define MAP_PRIVATE   0x02
#  define PROT_READ   1
#  define PROT_WRITE  2
#  define PROT_EXEC   4
#  pragma clang diagnostic pop
#endif

HookEntry g_hooks[MAX_HOOKS];
int       g_hook_count = 0;

// ─── Helpers de memória arm64 ─────────────────────────────────────────────────

// Escreve o stub "LDR X16,[PC+8] / BR X16 / .quad <dest>" em 'dst' (12 bytes).
static void write_abs_jump(uint8_t *dst, uintptr_t dest) {
    // LDR X16, [PC, #8]: 0x58000050
    uint32_t ldr = 0x58000050u;
    // BR X16: 0xD61F0200
    uint32_t br  = 0xD61F0200u;
    memcpy(dst + 0, &ldr,  4);
    memcpy(dst + 4, &br,   4);
    memcpy(dst + 8, &dest, 8);
}

#define HOOK_STUB_SIZE 12

// Aloca página RWX para trampolines.
static uint8_t *alloc_rwx_page(void) {
    void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return (uint8_t *)p;
}

static uint8_t *s_trampoline_pool       = NULL;
static size_t   s_trampoline_pool_used  = 0;

static uintptr_t trampoline_alloc(size_t size) {
    if (!s_trampoline_pool) {
        s_trampoline_pool = alloc_rwx_page();
        if (!s_trampoline_pool) return 0;
    }
    if (s_trampoline_pool_used + size > 4096) return 0; // pool cheio
    uintptr_t va = (uintptr_t)(s_trampoline_pool + s_trampoline_pool_used);
    s_trampoline_pool_used += (size + 3) & ~3u; // alinha a 4 bytes
    return va;
}

// ─── hook_install ─────────────────────────────────────────────────────────────
bool hook_install(uintptr_t target_va, uintptr_t hook_fn, uintptr_t *orig_out) {
    if (g_hook_count >= MAX_HOOKS) return false;
    if (!target_va || !hook_fn) return false;

    // Desprotege a página de código
    vm_address_t page = target_va & ~(uintptr_t)0xFFF;
    if (vm_protect(mach_task_self(), page, 4096,
                   false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY)
                   != KERN_SUCCESS) {
        fprintf(stderr, "[hook] vm_protect falhou em 0x%llx\n", (unsigned long long)target_va);
        return false;
    }

    HookEntry *h = &g_hooks[g_hook_count];
    h->target_va = target_va;
    h->hook_fn   = hook_fn;

    // Salva bytes originais (16 bytes = margem para instrução no limite de página)
    memcpy(h->orig_bytes, (void *)target_va, HOOK_STUB_SIZE);

    // Aloca trampoline: orig_bytes + abs_jump de volta para target+HOOK_STUB_SIZE
    size_t tramp_size = HOOK_STUB_SIZE + HOOK_STUB_SIZE; // 24 bytes
    uintptr_t tramp = trampoline_alloc(tramp_size);
    if (!tramp) {
        fprintf(stderr, "[hook] pool de trampolines cheio\n");
        return false;
    }

    uint8_t *tp = (uint8_t *)tramp;
    // 1ª parte: bytes originais que foram sobrescritos
    memcpy(tp, h->orig_bytes, HOOK_STUB_SIZE);
    // 2ª parte: salto de retorno para target_va + HOOK_STUB_SIZE
    write_abs_jump(tp + HOOK_STUB_SIZE, target_va + HOOK_STUB_SIZE);
    h->trampoline_va = tramp;

    // Escreve o hook no target
    write_abs_jump((uint8_t *)target_va, hook_fn);

    // Flush do cache de instruções arm64
    __builtin___clear_cache((char *)target_va, (char *)(target_va + HOOK_STUB_SIZE));

    // Restaura proteção da página
    vm_protect(mach_task_self(), page, 4096, false,
               VM_PROT_READ | VM_PROT_EXECUTE);

    h->installed = true;
    if (orig_out) *orig_out = tramp;
    g_hook_count++;
    return true;
}

// ─── hooks_cleanup ────────────────────────────────────────────────────────────
void hooks_cleanup(void) {
    for (int i = 0; i < g_hook_count; i++) {
        HookEntry *h = &g_hooks[i];
        if (!h->installed) continue;

        vm_address_t page = h->target_va & ~(uintptr_t)0xFFF;
        if (vm_protect(mach_task_self(), page, 4096,
                       false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY)
                       == KERN_SUCCESS) {
            memcpy((void *)h->target_va, h->orig_bytes, HOOK_STUB_SIZE);
            __builtin___clear_cache((char *)h->target_va,
                                    (char *)(h->target_va + HOOK_STUB_SIZE));
            vm_protect(mach_task_self(), page, 4096, false,
                       VM_PROT_READ | VM_PROT_EXECUTE);
        }
        h->installed = false;
    }
    g_hook_count = 0;
    // Libera pool de trampolines
    if (s_trampoline_pool) {
        munmap(s_trampoline_pool, 4096);
        s_trampoline_pool      = NULL;
        s_trampoline_pool_used = 0;
    }
}

// ─── VMT hook ─────────────────────────────────────────────────────────────────
bool vmt_hook(void **vtable_ptr, int index, void *new_fn, void **orig_out) {
    if (!vtable_ptr || !new_fn) return false;
    void *orig = vtable_ptr[index];

    vm_address_t page = (vm_address_t)&vtable_ptr[index] & ~(uintptr_t)0xFFF;
    if (vm_protect(mach_task_self(), page, 4096,
                   false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY)
                   != KERN_SUCCESS) return false;

    vtable_ptr[index] = new_fn;

    vm_protect(mach_task_self(), page, 4096, false,
               VM_PROT_READ | VM_PROT_EXECUTE);

    if (orig_out) *orig_out = orig;
    return true;
}

void vmt_unhook(void **vtable_ptr, int index, void *orig_fn) {
    if (!vtable_ptr || !orig_fn) return;
    vm_address_t page = (vm_address_t)&vtable_ptr[index] & ~(uintptr_t)0xFFF;
    if (vm_protect(mach_task_self(), page, 4096,
                   false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY)
                   == KERN_SUCCESS) {
        vtable_ptr[index] = orig_fn;
        vm_protect(mach_task_self(), page, 4096, false,
                   VM_PROT_READ | VM_PROT_EXECUTE);
    }
}
