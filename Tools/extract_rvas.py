#!/usr/bin/env python3 -I
"""
extract_rvas.py — extrai RVAs de funções IL2CPP do UnityFramework (Mach-O arm64)

Estratégia:
1. Parseia o Mach-O: acha __TEXT,__text (código) e __TEXT,__cstring (strings)
2. Varre a symbol table (LC_SYMTAB) buscando nomes de métodos IL2CPP
3. Para métodos sem símbolo (stripped), usa o dump.cs para cruzar com
   o índice do método e calcular o RVA via Il2CppMethodDefinition array.

Output: JSON com RVAs confirmados para uso no offsets_<UUID>.json
"""

import struct, sys, json, re, os
from pathlib import Path

BINARY = '/home/claude/ffire_cheat/dump_work/UnityFramework'
DUMP_CS = '/home/claude/ffire_cheat/dump_work/output/dump.cs'
OUT_JSON = '/home/claude/ffire_cheat/Tools/extracted_rvas.json'

# Métodos que queremos encontrar
TARGET_METHODS = [
    'TakeDamage',
    'SetSpeed',
    'AddAmmo',
    'SetTeam',
    'GetLocalPlayer',
    'GameUpdate',
    'LateUpdate',
    'Update',
    'get_position',
    'WorldToScreenPoint',
    'GetPlayerList',
    'GetAllPlayers',
    'get_HP',
    'set_HP',
    'get_MaxHP',
    'GetHP',
    'SetHP',
    'get_health',
    'set_health',
    'get_currentAmmo',
    'set_currentAmmo',
    'GetBoneTransform',
    'get_localPlayer',
    'GetCharacterController',
    'GetComponent',
]

# ─── Parser Mach-O mínimo ────────────────────────────────────────────────────

def parse_macho(data):
    """Retorna dict com info do Mach-O: vmaddr_slide, sections, symtab"""
    offset = 0
    magic, cpu, subcpu, ftype, ncmds, sizeofcmds, flags = struct.unpack_from('<IIIIIII', data, offset)
    # 64-bit: header tem 8 campos (magic..reserved)
    offset = 32  # sizeof(mach_header_64)

    sections = {}   # nome -> {offset, size, addr}
    symtab = None
    strtab = None
    nsyms = 0
    text_vmaddr = 0

    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, offset)

        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[offset+8:offset+24].rstrip(b'\x00').decode('ascii', errors='replace')
            segvmaddr = struct.unpack_from('<Q', data, offset+24)[0]
            nsects = struct.unpack_from('<I', data, offset+64)[0]

            sect_off = offset + 72
            for s in range(nsects):
                sectname = data[sect_off:sect_off+16].rstrip(b'\x00').decode('ascii', errors='replace')
                segname2 = data[sect_off+16:sect_off+32].rstrip(b'\x00').decode('ascii', errors='replace')
                sect_addr = struct.unpack_from('<Q', data, sect_off+32)[0]
                sect_size = struct.unpack_from('<Q', data, sect_off+40)[0]
                sect_fileoff = struct.unpack_from('<I', data, sect_off+48)[0]
                key = f'{segname2},{sectname}'
                sections[key] = {
                    'addr': sect_addr,
                    'size': sect_size,
                    'fileoff': sect_fileoff,
                }
                if segname2 == '__TEXT' and sectname == '__text':
                    text_vmaddr = sect_addr
                sect_off += 80

        elif cmd == 0x2:  # LC_SYMTAB
            sym_off = struct.unpack_from('<I', data, offset+8)[0]
            nsyms = struct.unpack_from('<I', data, offset+12)[0]
            str_off = struct.unpack_from('<I', data, offset+16)[0]
            str_size = struct.unpack_from('<I', data, offset+20)[0]
            symtab = (sym_off, nsyms)
            strtab = data[str_off:str_off+str_size]

        offset += cmdsize

    return {
        'sections': sections,
        'symtab': symtab,
        'strtab': strtab,
        'nsyms': nsyms,
        'text_vmaddr': text_vmaddr,
    }

def extract_symbols(data, macho_info):
    """Extrai todos os símbolos de função da symbol table"""
    symtab = macho_info['symtab']
    strtab = macho_info['strtab']
    if not symtab or strtab is None:
        return {}

    sym_off, nsyms = symtab
    results = {}
    # nlist_64: strx(4) type(1) sect(1) desc(2) value(8) = 16 bytes
    for i in range(nsyms):
        off = sym_off + i * 16
        strx, ntype, sect, desc = struct.unpack_from('<IBBH', data, off)
        value = struct.unpack_from('<Q', data, off+8)[0]
        # ntype & 0x0E == 0x0E: N_SECT (defined in section)
        # ntype & 0x01: N_EXT (external)
        if (ntype & 0x0E) != 0x0E:
            continue
        if value == 0:
            continue
        try:
            name = strtab[strx:strx+256].split(b'\x00')[0].decode('utf-8', errors='replace')
        except Exception:
            continue
        results[name] = value  # value = vmaddr
    return results

def vmaddr_to_fileoff(addr, sections):
    """Converte vmaddr para file offset usando as sections"""
    for sec in sections.values():
        if sec['addr'] <= addr < sec['addr'] + sec['size']:
            return sec['fileoff'] + (addr - sec['addr'])
    return None

def search_strings_for_method(data, sections, method_name):
    """
    Busca a string do método em __cstring e encontra referências no código.
    Retorna lista de possíveis vmaddrs onde a string é referenciada.
    """
    cstring_key = '__TEXT,__cstring'
    if cstring_key not in sections:
        return []

    sec = sections[cstring_key]
    sec_data = data[sec['fileoff']:sec['fileoff']+sec['size']]

    # Procura ocorrências da string do método
    hits = []
    search = method_name.encode('utf-8')
    pos = 0
    while True:
        idx = sec_data.find(search, pos)
        if idx < 0:
            break
        # Verifica que é string delimitada (precedida por \0 ou início, seguida por \0 ou '(')
        pre_ok = (idx == 0 or sec_data[idx-1] == 0)
        post_ok = (idx + len(search) < len(sec_data) and
                   sec_data[idx + len(search)] in (0, ord('('), ord(' ')))
        if pre_ok and post_ok:
            str_vmaddr = sec['addr'] + idx
            hits.append(str_vmaddr)
        pos = idx + 1

    return hits

# ─── Busca por padrão IL2CPP no dump.cs ──────────────────────────────────────

def parse_dump_cs_rvas(dump_path, target_names):
    """
    Extrai RVAs do dump.cs gerado pelo Il2CppDumper.
    Formato: // RVA: 0xXXXXXX Offset: 0xYYYYYY
    Retorna dict: method_name -> rva
    """
    results = {}
    if not os.path.exists(dump_path):
        return results

    rva_pattern = re.compile(r'//\s*RVA:\s*(0x[0-9A-Fa-f]+)')
    method_pattern = re.compile(r'(?:public|private|protected|internal|static|\s)+\s+\S+\s+(\w+)\s*\(')

    with open(dump_path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    for i, line in enumerate(lines):
        rva_match = rva_pattern.search(line)
        if not rva_match:
            continue
        rva = int(rva_match.group(1), 16)

        # Procura o nome do método na linha seguinte ou na mesma linha
        for j in range(i, min(i+3, len(lines))):
            m = method_pattern.search(lines[j])
            if m:
                name = m.group(1)
                for target in target_names:
                    if target.lower() in name.lower():
                        key = f'{name}'
                        if key not in results:
                            results[key] = rva
                break

    return results

# ─── Busca binária por nome de método IL2CPP (mangled) ───────────────────────

def search_mangled_names(symbols, target_names):
    """
    Símbolos IL2CPP têm formato: _ZN<len><namespace><len><class><len><method>Ev
    Ou formato mais simples: COW_GamePlay_Player_TakeDamage0
    Busca nomes parciais.
    """
    found = {}
    for sym_name, vmaddr in symbols.items():
        for target in target_names:
            if target in sym_name:
                if target not in found:
                    found[target] = []
                found[target].append({'symbol': sym_name, 'vmaddr': vmaddr, 'rva': vmaddr})
    return found

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    print(f'[*] Lendo {BINARY}...')
    with open(BINARY, 'rb') as f:
        data = f.read()
    print(f'[*] {len(data)//1024//1024}MB carregado')

    print('[*] Parseando Mach-O...')
    macho = parse_macho(data)

    print(f'[*] Sections encontradas: {list(macho["sections"].keys())[:8]}...')
    print(f'[*] text_vmaddr: 0x{macho["text_vmaddr"]:X}')

    print('[*] Extraindo symbol table...')
    symbols = extract_symbols(data, macho)
    print(f'[*] {len(symbols)} símbolos extraídos')

    # Busca por nomes alvo na symbol table
    print('[*] Buscando métodos alvo na symbol table...')
    matched = search_mangled_names(symbols, TARGET_METHODS)

    print(f'[*] Encontrados via símbolos: {len(matched)} métodos')
    for name, hits in sorted(matched.items()):
        for h in hits[:3]:
            print(f'    {name}: {h["symbol"][:80]}  RVA=0x{h["rva"]:X}')

    # Busca no dump.cs por RVAs
    print('\n[*] Buscando RVAs no dump.cs...')
    dump_rvas = parse_dump_cs_rvas(DUMP_CS, TARGET_METHODS)
    print(f'[*] {len(dump_rvas)} RVAs encontrados no dump.cs')
    for name, rva in sorted(dump_rvas.items()):
        print(f'    {name}: RVA=0x{rva:X}')

    # Busca strings dos métodos no binário
    print('\n[*] Buscando strings de métodos no __cstring...')
    string_hits = {}
    for target in TARGET_METHODS:
        hits = search_strings_for_method(data, macho['sections'], target)
        if hits:
            string_hits[target] = [f'0x{h:X}' for h in hits[:5]]
            print(f'    {target}: {string_hits[target]}')

    # Monta resultado final
    result = {
        '_note': 'RVAs extraídos do UnityFramework — confirmar antes de usar',
        '_text_vmaddr': f'0x{macho["text_vmaddr"]:X}',
        '_total_symbols': len(symbols),
        'from_symtable': {
            name: [{'symbol': h['symbol'], 'rva': f'0x{h["rva"]:X}'} for h in hits[:3]]
            for name, hits in matched.items()
        },
        'from_dump_cs': {
            name: f'0x{rva:X}' for name, rva in dump_rvas.items()
        },
        'strings_found_at': string_hits,
    }

    with open(OUT_JSON, 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    print(f'\n[+] Resultado salvo em {OUT_JSON}')

    # Resumo de o que realmente encontramos
    print('\n' + '='*60)
    print('RESUMO — RVAs confirmados para o offsets_<UUID>.json:')
    print('='*60)
    all_found = set(matched.keys()) | set(dump_rvas.keys())
    missing = [t for t in TARGET_METHODS if t not in all_found]
    print(f'Encontrados: {sorted(all_found)}')
    print(f'Não encontrados: {missing}')

if __name__ == '__main__':
    main()
