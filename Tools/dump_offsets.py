#!/usr/bin/env python3
"""
dump_offsets.py — gera offsets_<UUID>.json a partir de um dump do Il2CppDumper
Uso: python3 dump_offsets.py <dump.cs> <il2cpp.h> <metadata.json> <output_dir>

Dependências: nenhuma (stdlib apenas)
"""

import sys
import os
import re
import json
import uuid as uuidlib
from pathlib import Path

OFFSET_UNKNOWN = "0xDEADC0DEDEADC0DE"

# ─── Parsers de dump.cs ───────────────────────────────────────────────────────

def parse_field_offsets(dump_cs: str, class_name: str) -> dict:
    """Extrai offsets de campos de uma classe do dump.cs do Il2CppDumper."""
    result = {}
    # Encontra o bloco da classe
    # Padrão: // Namespace: ...\npublic class ClassName ... {\n... // 0xOFFSET\n}
    class_pattern = re.compile(
        r'(?:// Namespace:[^\n]*\n)?'
        r'(?:public |private |internal |protected |abstract |sealed )*'
        r'class\s+' + re.escape(class_name) + r'\b[^{]*\{(.*?)\n\}',
        re.DOTALL
    )
    m = class_pattern.search(dump_cs)
    if not m:
        return result

    block = m.group(1)

    # Formato padrão do Il2CppDumper: "public float health; // 0x18"
    # (offset no FIM da linha)
    field_pattern_end = re.compile(
        r'(?:public|private|protected|internal|static|readonly|\s)+'
        r'(?:static\s+)?(?:readonly\s+)?'
        r'[\w\[\]<>.,\s]+\s+(\w+)\s*;\s*//\s*(0x[0-9a-fA-F]+)'
    )
    for fm in field_pattern_end.finditer(block):
        field_name  = fm.group(1)
        offset_hex  = fm.group(2)
        result[field_name] = int(offset_hex, 16)

    # Fallback: formato antigo com offset ANTES: "// 0x18\npublic float health;"
    if not result:
        field_pattern_start = re.compile(
            r'//\s*(0x[0-9a-fA-F]+)\s*\n\s*(?:public|private|protected|internal|static)?\s*'
            r'(?:readonly\s+)?[\w\[\]<>,\s]+\s+(\w+)\s*;'
        )
        for fm in field_pattern_start.finditer(block):
            offset_hex = fm.group(1)
            field_name  = fm.group(2)
            result[field_name] = int(offset_hex, 16)

    return result

def parse_method_rva(dump_cs: str, class_name: str, method_name: str):
    """Extrai o RVA de um método a partir do dump.cs."""
    # Padrão: // RVA: 0x<RVA> Offset: 0x<offset>\n... RetType MethodName(...)
    class_block_pat = re.compile(
        r'class\s+' + re.escape(class_name) + r'\b[^{]*\{(.*?)\n\}',
        re.DOTALL
    )
    cm = class_block_pat.search(dump_cs)
    if not cm:
        return None

    block = cm.group(1)
    # RVA annotation
    rva_pat = re.compile(
        r'//\s*RVA:\s*(0x[0-9a-fA-F]+)\s+Offset:\s*0x[0-9a-fA-F]+\s*\n'
        r'[^\n]*\b' + re.escape(method_name) + r'\s*\('
    )
    m = rva_pat.search(block)
    if m:
        return int(m.group(1), 16)
    return None

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 4:
        print("Uso: dump_offsets.py <dump.cs> <metadata.json> <output_dir>")
        print()
        print("  dump.cs:       arquivo gerado pelo Il2CppDumper (ou Cpp2IL)")
        print("  metadata.json: arquivo gerado pelo Il2CppDumper (opcional, para UUID)")
        print("  output_dir:    diretório de saída para offsets_<UUID>.json")
        sys.exit(1)

    dump_path   = Path(sys.argv[1])
    meta_path   = Path(sys.argv[2])
    out_dir     = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    if not dump_path.exists():
        print(f"Erro: {dump_path} não encontrado")
        sys.exit(1)

    dump_cs = dump_path.read_text(encoding='utf-8', errors='replace')

    # Tenta ler UUID do metadata.json (campo "ImageBase" ou "GameAssembly")
    build_uuid = "00000000-0000-0000-0000-000000000000"
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text())
            # Il2CppDumper coloca "AssemblyName" e outros campos
            # O UUID real vem do LC_UUID do binário — extrai com:
            # python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read();
            #   i=d.find(b'\\x1b\\x00\\x00\\x00'); print(d[i+8:i+24].hex())"
            if "UUID" in meta:
                build_uuid = meta["UUID"]
        except Exception as e:
            print(f"Aviso: não foi possível ler metadata.json: {e}")

    print(f"Build UUID: {build_uuid}")

    # ── Extrai offsets de PlayerInfo ──────────────────────────────────────────
    player_fields = parse_field_offsets(dump_cs, "PlayerInfo")
    print(f"PlayerInfo campos encontrados: {list(player_fields.keys())}")

    # ── Extrai offsets de WeaponController ───────────────────────────────────
    weapon_fields = parse_field_offsets(dump_cs, "WeaponController")
    print(f"WeaponController campos encontrados: {list(weapon_fields.keys())}")

    # ── Extrai offsets de CharacterController ────────────────────────────────
    char_fields = parse_field_offsets(dump_cs, "BaseCharacterController")
    char_fields.update(parse_field_offsets(dump_cs, "CharacterController"))
    print(f"CharacterController campos encontrados: {list(char_fields.keys())}")

    # ── Extrai RVAs de métodos ────────────────────────────────────────────────
    fn_GetLocalPlayer = parse_method_rva(dump_cs, "GameController",   "GetLocalPlayer")
    fn_GetAllPlayers  = parse_method_rva(dump_cs, "GameController",   "GetAllPlayers")
    fn_GameUpdate     = parse_method_rva(dump_cs, "GameController",   "LateUpdate")
    fn_WorldToScreen  = parse_method_rva(dump_cs, "CameraController", "WorldToScreen")

    def hex_or_unknown(v):
        return hex(v) if v is not None else OFFSET_UNKNOWN

    def field_or_unknown(fields: dict, *candidates) -> str:
        for name in candidates:
            if name in fields:
                return hex(fields[name])
        return OFFSET_UNKNOWN

    # ── Monta JSON ────────────────────────────────────────────────────────────
    offsets = {
        "build_uuid":   build_uuid,
        "game_version": "UNKNOWN",

        "ga": {
            "il2cpp_domain_get":               "EXPORT",
            "il2cpp_domain_get_assemblies":    "EXPORT",
            "il2cpp_assembly_get_image":       "EXPORT",
            "il2cpp_class_from_name":          "EXPORT",
            "il2cpp_class_get_field_from_name":"EXPORT",
            "il2cpp_field_get_offset":         "EXPORT",
            "il2cpp_class_get_method_from_name":"EXPORT",
            "il2cpp_runtime_invoke":           "EXPORT",
            "il2cpp_string_new":               "EXPORT",
            "klass_PlayerInfo":        OFFSET_UNKNOWN,
            "klass_GameController":    OFFSET_UNKNOWN,
            "klass_WeaponController":  OFFSET_UNKNOWN,
            "fn_GetLocalPlayer":  hex_or_unknown(fn_GetLocalPlayer),
            "fn_GetAllPlayers":   hex_or_unknown(fn_GetAllPlayers),
            "fn_GameUpdate":      hex_or_unknown(fn_GameUpdate),
            "fn_WorldToScreen":   hex_or_unknown(fn_WorldToScreen),
        },

        "st": {
            "PlayerInfo_health":      field_or_unknown(player_fields,
                                        "health", "HP", "hp", "currentHp", "curHp",
                                        "mHealth", "mHp", "m_health"),
            "PlayerInfo_health_max":  field_or_unknown(player_fields,
                                        "healthMax", "maxHp", "maxHealth", "MaxHP",
                                        "mHealthMax", "m_maxHealth", "maxHp"),
            "PlayerInfo_armor":       field_or_unknown(player_fields,
                                        "armor", "Armor", "shield", "Shield",
                                        "armorValue", "mArmor"),
            "PlayerInfo_team_id":     field_or_unknown(player_fields,
                                        "teamId", "TeamID", "team", "Team",
                                        "mTeamId", "teamID", "groupId"),
            "PlayerInfo_is_dead":     field_or_unknown(player_fields,
                                        "isDead", "IsDead", "dead", "Dead",
                                        "bDead", "mIsDead", "isAlive"),
            "PlayerInfo_transform":   field_or_unknown(player_fields,
                                        "transform", "Transform", "rootTransform",
                                        "mTransform", "bodyTransform"),
            "PlayerInfo_name":        field_or_unknown(player_fields,
                                        "playerName", "PlayerName", "nickName",
                                        "NickName", "name", "mName", "userName"),
            "PlayerInfo_weapon_controller": field_or_unknown(player_fields,
                                        "weaponController", "WeaponController",
                                        "mWeaponController", "weapon", "Weapon"),
            "Transform_position":     "0x90",  # Unity padrão (campo position em Transform)

            "WeaponController_currentAmmo": field_or_unknown(weapon_fields,
                                        "currentAmmo", "CurrentAmmo", "ammo",
                                        "curAmmo", "mCurrentAmmo", "bulletCount"),
            "WeaponController_maxAmmo":     field_or_unknown(weapon_fields,
                                        "maxAmmo", "MaxAmmo", "clipSize",
                                        "mMaxAmmo", "maxBullet", "magazineSize"),
            "WeaponController_reserveAmmo": field_or_unknown(weapon_fields,
                                        "reserveAmmo", "ReserveAmmo", "totalAmmo",
                                        "mReserveAmmo", "backpackAmmo", "ammoInBag"),

            "CharController_moveSpeed": field_or_unknown(char_fields,
                                        "moveSpeed", "MoveSpeed", "walkSpeed",
                                        "mMoveSpeed", "speed", "Speed", "runSpeed"),

            "Camera_viewMatrix":      OFFSET_UNKNOWN,
            "Camera_projMatrix":      OFFSET_UNKNOWN,
            "Camera_fov":             OFFSET_UNKNOWN,
        }
    }

    # ── Salva ─────────────────────────────────────────────────────────────────
    uuid_clean = build_uuid.replace("-", "").upper()
    out_file = out_dir / f"offsets_{uuid_clean}.json"
    out_file.write_text(json.dumps(offsets, indent=2))
    print(f"\nSalvo em: {out_file}")

    # Resumo de campos ainda desconhecidos
    unknown_count = json.dumps(offsets).count(OFFSET_UNKNOWN)
    print(f"Campos ainda desconhecidos: {unknown_count}")
    print("→ Preencha manualmente os campos com DEADC0DE usando Cheat Engine / ReClass / x64dbg")

if __name__ == "__main__":
    main()
