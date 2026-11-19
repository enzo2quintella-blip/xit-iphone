// ff_module.c — localização e acesso ao GameAssembly.dylib no processo
#include "../include/ff_offsets_loader.h"
#include "../../Shared/include/ff_types.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <mach-o/loader.h>
#  include <mach/mach.h>
#else
// Stubs Linux para syntax-check
#  define MH_MAGIC_64  0xfeedfacf
#  define LC_SEGMENT_64 0x19
struct load_command       { uint32_t cmd; uint32_t cmdsize; };
struct segment_command_64 { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t fileoff; uint64_t filesize; int maxprot; int initprot; uint32_t nsects; uint32_t flags; };
struct mach_header_64     { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; uint32_t reserved; };
struct mach_header        { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; };
static inline uint32_t _dyld_image_count(void) { return 0; }
static inline const char* _dyld_get_image_name(uint32_t i) { (void)i; return NULL; }
static inline uintptr_t _dyld_get_image_vmaddr_slide(uint32_t i) { (void)i; return 0; }
static inline const struct mach_header* _dyld_get_image_header(uint32_t i) { (void)i; return NULL; }
typedef int kern_return_t;
#  define KERN_SUCCESS 0
typedef unsigned int mach_port_t;
typedef mach_port_t task_t;
typedef uintptr_t mach_vm_address_t;
typedef uintptr_t mach_vm_size_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef int vm_prot_t;
#  define VM_PROT_READ 1
#  define VM_PROT_WRITE 2
#  define VM_PROT_EXECUTE 4
static inline task_t mach_task_self_(void) { return 0; }
#  define mach_task_self() mach_task_self_()
static inline kern_return_t vm_protect(task_t t, vm_address_t a, vm_size_t s, bool sc, vm_prot_t p) { (void)t;(void)a;(void)s;(void)sc;(void)p; return 0; }
#endif

// Cache da base e tamanho do módulo (preenchido na primeira chamada)
static uintptr_t s_ga_base = 0;
static size_t    s_ga_size = 0;
static bool      s_ga_found = false;

static void find_gameassembly(void) {
    if (s_ga_found) return;

    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        // Free Fire iOS: IL2CPP está dentro do UnityFramework, não GameAssembly.dylib
        if (!name) continue;
        if (!strstr(name, "UnityFramework") && !strstr(name, "GameAssembly")) continue;

        const struct mach_header *mh = _dyld_get_image_header(i);
        if (!mh) continue;

        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        s_ga_base = (uintptr_t)mh;

        // Calcular tamanho: soma dos segmentos __TEXT + __DATA
        bool is64 = (mh->magic == 0xFEEDFACF);
        size_t hdr_size = is64 ? 32 : 28;
        const uint8_t *lc = (const uint8_t *)mh + hdr_size;
        size_t total_size = 0;

        for (uint32_t j = 0; j < mh->ncmds; j++) {
            uint32_t cmd     = *(uint32_t *)lc;
            uint32_t cmdsize = *(uint32_t *)(lc + 4);

            if (cmd == 0x19) { // LC_SEGMENT_64
                uint64_t vmsize = *(uint64_t *)(lc + 40);
                uint64_t vmaddr = *(uint64_t *)(lc + 24);
                uint64_t end    = vmaddr + vmsize + (uint64_t)slide;
                if (end > (uint64_t)s_ga_base + total_size)
                    total_size = (size_t)(end - s_ga_base);
            } else if (cmd == 0x1) { // LC_SEGMENT (32-bit, caso raro)
                uint32_t vmsize = *(uint32_t *)(lc + 20);
                total_size += vmsize;
            }
            lc += cmdsize;
        }

        s_ga_size  = total_size ? total_size : 64 * 1024 * 1024; // fallback 64MB
        s_ga_found = true;
        return;
    }
}

uintptr_t ff_gameassembly_base(void) {
    find_gameassembly();
    return s_ga_base;
}

size_t ff_gameassembly_size(void) {
    find_gameassembly();
    return s_ga_size;
}

// ─── Leitura/escrita de memória interna ──────────────────────────────────────
// Como somos uma dylib injetada no processo do Free Fire,
// a memória do jogo é diretamente acessível via ponteiro.
// Não precisamos de vm_read_overwrite — apenas castamos e lemos.
// A validação de range previne leituras de lixo.

bool ff_read_mem(uintptr_t va, void *out, size_t size) {
    if (!va || !out || !size) return false;
    if (!s_ga_found) find_gameassembly();

    // Validação básica: endereço dentro da faixa esperada do processo
    // (não limitamos ao módulo porque o jogo aloca objetos no heap)
    if (!ptr_valid(va)) return false;

    // Leitura direta — estamos no mesmo espaço de endereço
    // Em caso de acesso inválido, o processo vai crashar; em produção
    // seria necessário um signal handler, mas aqui mantemos simples.
    memcpy(out, (const void *)va, size);
    return true;
}

bool ff_write_mem(uintptr_t va, const void *data, size_t size) {
    if (!va || !data || !size) return false;
    if (!ptr_valid(va)) return false;

    // Para páginas de código (patch de IL), precisamos mudar proteção
    // vm_protect antes de escrever
    kern_return_t kr = vm_protect(mach_task_self(),
                                  (vm_address_t)(va & ~0xFFF),
                                  (vm_size_t)((size + 0xFFF) & ~0xFFF),
                                  false,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        // Falhou em mudar proteção — tenta mesmo assim (heap já é rw)
    }

    memcpy((void *)va, data, size);
    return true;
}
