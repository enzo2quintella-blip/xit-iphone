// ff_offsets_loader.h — interface do loader de offsets + acesso ao módulo do jogo
#pragma once

#include "../../Shared/include/ff_offsets.h"
#include <stddef.h>   // size_t
#include <stdbool.h>

// Retorna o endereço base do GameAssembly.dylib no processo atual.
// Retorna 0 se o módulo não estiver carregado.
uintptr_t ff_gameassembly_base(void);

// Retorna o tamanho em bytes do GameAssembly.dylib.
size_t ff_gameassembly_size(void);

// Converte RVA (relativo à base do módulo) para VA (endereço virtual absoluto).
// Retorna 0 se a base não estiver disponível ou o RVA for OFFSET_UNKNOWN.
static inline uintptr_t ff_rva_to_va(uintptr_t rva) {
    if (rva == OFFSET_UNKNOWN || rva == 0) return 0;
    uintptr_t base = ff_gameassembly_base();
    return base ? base + rva : 0;
}

// Lê N bytes do endereço VA com verificação de range.
// Retorna false se o endereço estiver fora do módulo ou for inválido.
bool ff_read_mem(uintptr_t va, void *out, size_t size);

// Escreve N bytes no endereço VA.
// Requer que o endereço esteja dentro do range do jogo.
bool ff_write_mem(uintptr_t va, const void *data, size_t size);
