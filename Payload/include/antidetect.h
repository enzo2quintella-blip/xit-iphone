// antidetect.h — camuflagem anti-detecção para iOS dylib injetada
// Alvo: Free Fire iOS, anti-cheat Garena (cliente + servidor)
//
// Estratégias implementadas:
//   1. Remoção do dylib do array dyld_all_image_infos (oculta de enumerações)
//   2. Zeragem do Mach-O header em memória (dificulta dump de imagem)
//   3. XOR de strings sensíveis em tempo de compilação — STRX macro
//   4. Delay de ativação randômico (evita assinatura temporal de injeção)
//   5. Nomes genéricos de threads (oculta nossa thread de ferramentas de profiling)
//   6. Guard de janela de jogo: só ativa se a partida já começou
//
// NÃO coberto aqui (server-side, fora do escopo desta dylib):
//   - Ofuscação de pacotes de rede
//   - Anti-screenshot do overlay
//   - Spoof de hash do binário do jogo

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Inicialização ────────────────────────────────────────────────────────────
// Chame uma vez, antes de qualquer outra feature, dentro da worker thread.
// Retorna false se detectar ambiente hostil (debugger, jailbreak scanner ativo).
bool antidetect_init(void);

// ─── Ocultar nossa dylib da lista dyld ───────────────────────────────────────
// Remove a entrada correspondente ao nosso dylib de dyld_all_image_infos.
// Deve ser chamado logo após a dylib ser carregada (dentro do constructor
// ou no início da worker thread) — antes que qualquer scanner itere a lista.
void antidetect_hide_image(void);

// ─── Zeragem de header Mach-O em memória ─────────────────────────────────────
// Sobrescreve os primeiros 4096 bytes do nosso próprio Mach-O em memória com
// zeros, eliminando magic, load commands e seções de debug.
// AVISO: após chamar isso, dladdr() e backtrace_symbols() falham para nosso código.
// Chame somente depois que todos os símbolos internos já foram resolvidos.
void antidetect_zero_header(void);

// ─── Delay randômico de ativação ─────────────────────────────────────────────
// Dorme entre min_ms e max_ms milissegundos.
// Use antes de instalar hooks para desincronizar da assinatura temporal de injeção.
void antidetect_random_delay(uint32_t min_ms, uint32_t max_ms);

// ─── Nomear thread de forma discreta ─────────────────────────────────────────
// Define o nome da thread atual como algo inocente.
void antidetect_name_thread(const char *name);

// ─── Verificação de debugger/ambiente ────────────────────────────────────────
// Retorna true se detectar ptrace/sysctl P_TRACED ou exception ports suspeitos.
// Use para decidir se aborta a ativação.
bool antidetect_debugger_present(void);

// ─── Macro XOR para strings em tempo de compilação ───────────────────────────
// Uso: char buf[16]; STRX_DECODE(buf, STRX("GameAssembly"));
// STRX_DECODE preenche 'buf' com a string decodificada em tempo de execução.
//
// Implementação: cada byte da string literal é XOR'd com uma chave derivada
// do índice, montada character-by-character no stack — nunca aparece completa
// no segmento __TEXT,__cstring do binário.
//
// LIMITE: strings até 63 chars + nulo.
#define STRX_KEY 0x37u

// XOR de um char pelo índice (evita padrão uniforme)
#define _SX(c, i) ((char)((uint8_t)(c) ^ (STRX_KEY ^ (uint8_t)(i))))

// Declara array de chars com cada elemento XOR'd — expande até 16 chars.
// Para strings maiores, adicione mais linhas _SX seguindo o padrão.
#define STRX16(s) {                                 \
    _SX((s)[0],  0), _SX((s)[1],  1), _SX((s)[2],  2), _SX((s)[3],  3), \
    _SX((s)[4],  4), _SX((s)[5],  5), _SX((s)[6],  6), _SX((s)[7],  7), \
    _SX((s)[8],  8), _SX((s)[9],  9), _SX((s)[10],10), _SX((s)[11],11), \
    _SX((s)[12],12), _SX((s)[13],13), _SX((s)[14],14), _SX((s)[15],15), \
    '\0' }

// Decodifica para buffer em tempo de execução.
// 'enc' deve ser um char[] de tamanho fixo inicializado com STRX16().
#define STRX_DECODE(dst, enc) do {                              \
    const char _e[] = enc;                                      \
    int _n = (int)(sizeof(_e) - 1);                             \
    for (int _i = 0; _i < _n; _i++)                            \
        (dst)[_i] = (char)((uint8_t)_e[_i] ^ (STRX_KEY ^ (uint8_t)_i)); \
    (dst)[_n] = '\0';                                           \
} while(0)

#ifdef __cplusplus
}
#endif
