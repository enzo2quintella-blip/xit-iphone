// antidetect.c — camuflagem anti-detecção para iOS dylib injetada
// Veja antidetect.h para descrição de cada função.

#include "../include/antidetect.h"

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <mach-o/loader.h>
#  include <mach/mach.h>
#  include <mach/task_info.h>
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <dlfcn.h>
#  include <pthread.h>
#else
// ─── Stubs Linux para syntax-check (nunca compilado para iOS) ────────────────
// Suprime -Wunused-function nos stubs: eles só existem para que o parser resolva
// os tipos; no iOS o bloco #ifdef __APPLE__ acima substitui tudo isso.
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunused-function"
#  include <stdint.h>
#  include <stdbool.h>
#  include <string.h>
#  include <stdio.h>
#  include <stdlib.h>
// mach_header structs
#  define MH_MAGIC_64  0xfeedfacf
#  define LC_SEGMENT_64 0x19
#  define LC_UUID 0x1b
struct load_command   { uint32_t cmd; uint32_t cmdsize; };
struct segment_command_64 { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t fileoff; uint64_t filesize; int maxprot; int initprot; uint32_t nsects; uint32_t flags; };
struct uuid_command   { uint32_t cmd; uint32_t cmdsize; uint8_t uuid[16]; };
struct mach_header_64 { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; uint32_t reserved; };
struct mach_header    { uint32_t magic; int cputype; int cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; };
// dyld
static inline uint32_t _dyld_image_count(void) { return 0; }
static inline const char* _dyld_get_image_name(uint32_t i) { (void)i; return NULL; }
static inline const struct mach_header* _dyld_get_image_header(uint32_t i) { (void)i; return NULL; }
// mach
typedef int kern_return_t;
#  define KERN_SUCCESS 0
typedef unsigned int mach_port_t;
typedef mach_port_t task_t;
typedef uintptr_t mach_vm_address_t;
typedef uintptr_t mach_vm_size_t;
typedef uint32_t mach_msg_type_number_t;
typedef int vm_prot_t;
#  define VM_PROT_READ 1
#  define VM_PROT_WRITE 2
#  define VM_PROT_EXECUTE 4
static inline task_t mach_task_self_(void) { return 0; }
#  define mach_task_self() mach_task_self_()
#  define TASK_DYLD_INFO 17
#  define TASK_DYLD_INFO_COUNT ((mach_msg_type_number_t)(sizeof(struct task_dyld_info) / sizeof(uint32_t)))
typedef int task_flavor_t;
typedef int* task_info_t;
struct task_dyld_info { mach_vm_address_t all_image_info_addr; mach_vm_size_t all_image_info_size; int all_image_info_format; };
typedef struct task_dyld_info task_dyld_info_data_t;
#  define TASK_DYLD_INFO_COUNT ((mach_msg_type_number_t)(sizeof(struct task_dyld_info) / sizeof(uint32_t)))
static inline kern_return_t task_info(task_t t, task_flavor_t f, task_info_t i, mach_msg_type_number_t* c) { (void)t;(void)f;(void)i;(void)c; return 0; }
struct dyld_image_info { const struct mach_header* imageLoadAddress; const char* imageFilePath; uintptr_t imageFileModDate; };
struct dyld_all_image_infos { uint32_t version; uint32_t infoArrayCount; const struct dyld_image_info* infoArray; };
static inline kern_return_t vm_protect(task_t t, mach_vm_address_t a, mach_vm_size_t s, bool sc, vm_prot_t p) { (void)t;(void)a;(void)s;(void)sc;(void)p; return 0; }
// sysctl
static inline int sysctl(int* n, unsigned int nl, void* o, size_t* os, void* ni, size_t nl2) { (void)n;(void)nl;(void)o;(void)os;(void)ni;(void)nl2; return 0; }
#  define CTL_KERN 1
#  define KERN_PROC 14
#  define KERN_PROC_PID 1
#  define P_TRACED 0x800
struct kinfo_proc { struct { int p_flag; int kp_flags; } kp_proc; };
// dlfcn
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
static inline void* dlopen(const char* p, int f) { (void)p;(void)f; return NULL; }
static inline void* dlsym(void* h, const char* s) { (void)h;(void)s; return NULL; }
static inline int dlclose(void* h) { (void)h; return 0; }
static inline int dladdr(const void* a, Dl_info* i) { (void)a;(void)i; return 0; }
#  ifndef RTLD_NOLOAD
#    define RTLD_NOLOAD 0x10
#  endif
#  ifndef RTLD_LAZY
#    define RTLD_LAZY   0x01
#  endif
// pthread
typedef void* pthread_t;
typedef void* pthread_attr_t;
static inline int pthread_create(pthread_t* t, const pthread_attr_t* a, void*(*f)(void*), void* arg) { (void)t;(void)a;(void)f;(void)arg; return 0; }
static inline int pthread_attr_init(pthread_attr_t* a) { (void)a; return 0; }
static inline int pthread_setname_np(const char* n) { (void)n; return 0; }
// unistd
static inline unsigned int arc4random_uniform(unsigned int u) { (void)u; return 0; }
static inline int usleep(unsigned us) { (void)us; return 0; }
static inline int getpid(void) { return 1; }
// ptrace
static inline int ptrace(int r, int p, void* a, int d) { (void)r;(void)p;(void)a;(void)d; return 0; }
#  define PT_DENY_ATTACH 31
#  pragma clang diagnostic pop
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// ─── Referência à nossa própria imagem ───────────────────────────────────────
// __self_ptr_anchor é uma variável no nosso dylib; usamos dladdr() para achar
// de qual imagem ela vem — esse é o nosso Mach-O header.
static int s_anchor_symbol = 0xBEEF;

static const struct mach_header_64 *get_own_header(void) {
    Dl_info info;
    if (dladdr(&s_anchor_symbol, &info) == 0) return NULL;
    return (const struct mach_header_64 *)info.dli_fbase;
}

// ─── antidetect_hide_image ────────────────────────────────────────────────────
// dyld expõe dyld_all_image_infos via task_info(TASK_DYLD_INFO).
// A estrutura contém um array de dyld_image_info[].
// Removemos nossa entrada copiando o último elemento por cima dela e
// decrementando infoArrayCount.
//
// AVISO: essa estrutura é privada e muda entre versões do dyld.
// Testado em iOS 15–17.  Em builds futuros pode falhar silenciosamente.

void antidetect_hide_image(void) {
    const struct mach_header_64 *our_hdr = get_own_header();
    if (!our_hdr) return;

    // Obter ponteiro para dyld_all_image_infos via task_info
    struct task_dyld_info dyld_info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_DYLD_INFO,
                  (task_info_t)&dyld_info, &count) != KERN_SUCCESS) return;

    // dyld_all_image_infos* (struct all_image_infos em dyld_images.h)
    // Layout compatível com iOS 15–17:
    //   uint32_t version
    //   uint32_t infoArrayCount
    //   const struct dyld_image_info *infoArray
    struct {
        uint32_t  version;
        uint32_t  infoArrayCount;
        uintptr_t infoArray;        // const struct dyld_image_info *
    } *all_images = (void *)(uintptr_t)dyld_info.all_image_info_addr;

    if (!all_images) return;

    uint32_t cnt = all_images->infoArrayCount;
    // dyld_image_info: { header*, filePath*, mtime }  — cada entrada 24 bytes em arm64
    struct {
        const void *imageLoadAddress;
        const char *imageFilePath;
        uintptr_t   imageFileModDate;
    } *arr = (void *)all_images->infoArray;

    if (!arr) return;

    // Desprotege a página do array para escrita
    vm_address_t page = (vm_address_t)arr & ~(uintptr_t)0xFFF;
    kern_return_t kr = vm_protect(mach_task_self(), page, 4096,
                                  false, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) return;

    // Encontra e remove nossa entrada
    for (uint32_t i = 0; i < cnt; i++) {
        if (arr[i].imageLoadAddress == (const void *)our_hdr) {
            // Move o último elemento para cá e reduz o count
            arr[i] = arr[cnt - 1];
            all_images->infoArrayCount = cnt - 1;
            break;
        }
    }

    // Restaura proteção
    vm_protect(mach_task_self(), page, 4096, false, VM_PROT_READ);
}

// ─── antidetect_zero_header ───────────────────────────────────────────────────
void antidetect_zero_header(void) {
    const struct mach_header_64 *hdr = get_own_header();
    if (!hdr) return;

    vm_address_t page = (vm_address_t)hdr & ~(uintptr_t)0xFFF;
    // vm_protect falha sem jailbreak (sandbox bloqueia escrita em __TEXT).
    // Guard obrigatório: só zeramos se conseguimos tornar a página writable.
    // Sem isso: memset em read-only → SIGBUS → crash imediato.
    kern_return_t kr = vm_protect(mach_task_self(), page, 4096,
                                  false, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) return;  // sem privilégio — pula silenciosamente

    memset((void *)hdr, 0, 4096);
    vm_protect(mach_task_self(), page, 4096, false, VM_PROT_READ);
}

// ─── antidetect_random_delay ──────────────────────────────────────────────────
void antidetect_random_delay(uint32_t min_ms, uint32_t max_ms) {
    if (max_ms <= min_ms) {
        usleep(min_ms * 1000u);
        return;
    }
    // arc4random_uniform evita modulo bias
    uint32_t range = max_ms - min_ms;
    uint32_t delay = min_ms + (arc4random_uniform(range));
    usleep(delay * 1000u);
}

// ─── antidetect_name_thread ───────────────────────────────────────────────────
void antidetect_name_thread(const char *name) {
    pthread_setname_np(name);
}

// ─── antidetect_debugger_present ─────────────────────────────────────────────
// Verifica P_TRACED via sysctl kinfo_proc.
bool antidetect_debugger_present(void) {
    struct kinfo_proc info;
    size_t info_size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &info, &info_size, NULL, 0) != 0) return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

// ─── antidetect_init ──────────────────────────────────────────────────────────
bool antidetect_init(void) {
    // 1. Delay randômico: 2–8 segundos após injeção
    //    Isso desincroniza nossa ativação do padrão temporal típico de cheats
    //    (ativam imediatamente no dlopen).
    antidetect_random_delay(2000, 8000);

    // 2. Nomear nossa thread de forma inocente
    antidetect_name_thread("com.apple.metal.capture");

    // 3. Verificar debugger — se presente, não ativar features
    //    (evita análise em ambiente de análise dinâmica)
    if (antidetect_debugger_present()) {
        // Não ativa, mas não falha visivelmente — apenas retorna false
        return false;
    }

    // 4. Ocultar nossa imagem da lista dyld
    //    Feito aqui (não no constructor) para garantir que dladdr() ainda funcione
    antidetect_hide_image();

    // 5. Zeragem do header Mach-O
    //    Feito por último — após isso dladdr() falha para nosso código
    antidetect_zero_header();

    return true;
}
