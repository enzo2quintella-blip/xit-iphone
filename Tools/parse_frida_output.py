#!/usr/bin/env python3
"""
parse_frida_output.py — converte saída do frida_discover.js em offsets_<UUID>.json

Uso:
  frida -U -f com.dena.a12026418 -l frida_discover.js --no-pause 2>&1 | \
      grep -E '^(DISC|FIELD|LOOKUP|CLASS)' > frida_out.txt

  python3 parse_frida_output.py frida_out.txt \
      Config/offsets_E50B0E3AD931385B9E4C9FFDE6D1B923.json \
      Config/offsets_E50B0E3AD931385B9E4C9FFDE6D1B923.json

Preenche os campos DEADC0DE com offsets reais encontrados pelo Frida.
Mantém todos os campos existentes; sobrescreve apenas os que foram encontrados.
"""

import sys
import json
import re
from pathlib import Path

UNKNOWN = "0xDEADC0DEDEADC0DE"


def parse_frida_output(frida_txt: str) -> dict:
    """
    Lê linhas DISC/FIELD/LOOKUP e retorna:
      {
        "classes":  { "ClassName": { "fieldName": "0xHH", ... }, ... },
        "lookups":  { "ClassName": "0xKLASS_PTR" (at runtime — apenas referência) },
        "fields":   { "fieldName": "0xHH" }  (de qualquer classe)
      }
    """
    result = {"classes": {}, "lookups": {}, "fields": {}}

    for line in frida_txt.splitlines():
        line = line.strip()

        if line.startswith("DISC "):
            try:
                obj = json.loads(line[5:])
                cname = obj.get("class", "")
                for fd in obj.get("fields", []):
                    fname = fd["name"]
                    foff  = fd["offset"]
                    result["classes"].setdefault(cname, {})[fname] = foff
                    result["fields"][fname] = foff
            except Exception as e:
                print(f"  [!] DISC parse error: {e} — line: {line[:80]}", file=sys.stderr)

        elif line.startswith("FIELD "):
            try:
                obj = json.loads(line[6:])
                result["fields"][obj["field"]] = obj["offset"]
            except Exception:
                pass

        elif line.startswith("LOOKUP "):
            try:
                obj = json.loads(line[7:])
                result["lookups"][obj["name"]] = obj["klass"]
            except Exception:
                pass

    return result


def apply_to_config(config: dict, frida: dict) -> tuple:
    """
    Preenche campos DEADC0DE no config com valores do Frida.
    Retorna (config_atualizado, lista_de_mudancas).
    """
    changes = []
    classes = frida["classes"]
    fields  = frida["fields"]

    def find_field(candidates: list) -> str | None:
        for name in candidates:
            if name in fields:
                return fields[name]
        return None

    st = config.get("st", {})
    ga = config.get("ga", {})

    # ── st.* ──────────────────────────────────────────────────────────────────
    FIELD_MAP = {
        "PlayerInfo_health":           ["health", "HP", "hp", "currentHp", "curHp", "mHealth", "mHp", "m_health"],
        "PlayerInfo_health_max":       ["healthMax", "maxHp", "maxHealth", "MaxHP", "mHealthMax", "m_maxHealth"],
        "PlayerInfo_armor":            ["armor", "Armor", "shield", "Shield", "armorValue", "mArmor"],
        "PlayerInfo_team_id":          ["teamId", "TeamID", "team", "Team", "mTeamId", "teamID", "groupId"],
        "PlayerInfo_is_dead":          ["isDead", "IsDead", "dead", "Dead", "bDead", "mIsDead"],
        "PlayerInfo_transform":        ["transform", "Transform", "rootTransform", "mTransform"],
        "PlayerInfo_name":             ["playerName", "PlayerName", "nickName", "NickName", "name", "mName"],
        "PlayerInfo_weapon_controller":["weaponController", "WeaponController", "mWeaponController"],
        "WeaponController_currentAmmo":["currentAmmo", "CurrentAmmo", "ammo", "curAmmo", "bulletCount"],
        "WeaponController_maxAmmo":    ["maxAmmo", "MaxAmmo", "clipSize", "maxBullet", "magazineSize"],
        "WeaponController_reserveAmmo":["reserveAmmo", "ReserveAmmo", "totalAmmo", "backpackAmmo"],
        "CharController_moveSpeed":    ["moveSpeed", "MoveSpeed", "walkSpeed", "speed", "Speed", "runSpeed"],
        "Camera_viewMatrix":           ["viewMatrix", "ViewMatrix", "_viewMatrix", "m_viewMatrix"],
        "Camera_projMatrix":           ["projMatrix", "ProjMatrix", "projectionMatrix"],
        "Camera_fov":                  ["fieldOfView", "fov", "FOV", "mFov", "m_fov"],
    }

    for key, candidates in FIELD_MAP.items():
        if st.get(key) == UNKNOWN:
            found = find_field(candidates)
            if found:
                st[key] = found
                changes.append(f"st.{key} = {found}")

    # ── ga.klass_* — from LOOKUP lines ────────────────────────────────────────
    # Se o Frida capturou um il2cpp_class_from_name("Assembly-CSharp","","BEEBYTE_NAME"),
    # o klass ptr (endereço runtime) fica em lookups[BEEBYTE_NAME].
    # Não é um RVA, é um ponteiro de runtime — anotamos como comentário.
    for key in ["klass_PlayerInfo", "klass_GameController", "klass_WeaponController"]:
        human_name = key.replace("klass_", "")
        if ga.get(key) == UNKNOWN:
            # Procura pelo nome exato ou variantes
            for lname, lptr in frida["lookups"].items():
                if human_name.lower() in lname.lower():
                    # Runtime pointer — não é um RVA fixo, mas serve para validação
                    ga[key] = f"RUNTIME_PTR:{lptr}"
                    changes.append(f"ga.{key} = RUNTIME_PTR:{lptr}")
                    break

    config["st"] = st
    config["ga"] = ga
    return config, changes


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    frida_path  = Path(sys.argv[1])
    config_in   = Path(sys.argv[2])
    config_out  = Path(sys.argv[3])

    if not frida_path.exists():
        print(f"Erro: {frida_path} não encontrado", file=sys.stderr)
        sys.exit(1)

    if not config_in.exists():
        print(f"Erro: {config_in} não encontrado", file=sys.stderr)
        sys.exit(1)

    frida_txt = frida_path.read_text(encoding='utf-8', errors='replace')
    config    = json.loads(config_in.read_text())

    frida = parse_frida_output(frida_txt)
    print(f"Frida output: {len(frida['classes'])} classes, {len(frida['fields'])} fields, {len(frida['lookups'])} lookups")

    # Print all discovered classes to help identify Beebyte names
    if frida["classes"]:
        print("\nClasses com offsets de campos:")
        for cname, fields in sorted(frida["classes"].items()):
            print(f"  {cname}: {len(fields)} campos")

    config_updated, changes = apply_to_config(config, frida)

    if changes:
        print(f"\nAtualizações aplicadas ({len(changes)}):")
        for c in changes:
            print(f"  + {c}")
    else:
        print("\nNenhum campo novo encontrado — verifique os nomes de classe em CLASS lines")
        print("Dica: grep 'CLASS' frida_out.txt | python3 -c \"import sys,json; [print(json.loads(l.split(' ',1)[1])['name']) for l in sys.stdin]\" | sort | grep -i 'player\\|health\\|weapon'")

    config_out.parent.mkdir(parents=True, exist_ok=True)
    config_out.write_text(json.dumps(config_updated, indent=2))
    print(f"\nSalvo em: {config_out}")

    # Resumo de campos ainda desconhecidos
    remaining = json.dumps(config_updated).count(UNKNOWN)
    resolved  = json.dumps(config_updated).count('"0x') - remaining
    print(f"Resolvidos: {resolved}  |  Ainda UNKNOWN: {remaining}")
    if remaining > 0:
        print("\nCampos ainda UNKNOWN:")
        for section in ["ga", "st"]:
            for k, v in config_updated.get(section, {}).items():
                if v == UNKNOWN:
                    print(f"  {section}.{k}")


if __name__ == "__main__":
    main()
