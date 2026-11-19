#!/usr/bin/env python3
"""
patch_offsets.py
Auto-patcha o JSON de offsets com os resultados dos scripts Frida.

Uso:
    # Redirecionar saída do Frida para arquivo:
    frida -U -n com.garena.game.freefire -l tools/frida_find_offsets.js \
        --no-pause 2>&1 | tee /tmp/frida_out.txt

    # Depois de o script ter emitido o JSON:
    python3 tools/patch_offsets.py /tmp/frida_out.txt

    # Ou passar JSON diretamente:
    python3 tools/patch_offsets.py --json '{"fn_GetLocalPlayer":"0x1234",...}'

    # Merge de múltiplos arquivos de saída:
    python3 tools/patch_offsets.py /tmp/frida_out1.txt /tmp/frida_out2.txt

O script:
  1. Extrai blocos JSON da saída do Frida (entre linhas de '=====').
  2. Merge todos os blocos num dicionário plano (chave → valor).
  3. Carrega Config/offsets_E50B0E3A-D931-385B-9E4C-9FFDE6D1B923.json.
  4. Para cada chave no resultado Frida:
       - Se o valor no JSON é "0xDEADC0DEDEADC0DE" (placeholder), substitui.
       - Se já tem valor real, imprime aviso e NÃO sobrescreve a menos que --force.
  5. Salva o JSON atualizado (backup automático em .bak).
  6. Imprime relatório: campos preenchidos / ainda vazios.
"""

import sys
import re
import json
import shutil
import argparse
from pathlib import Path

DEAD = "0xDEADC0DEDEADC0DE"
JSON_PATH = Path(__file__).parent.parent / "Config" / "offsets_E50B0E3A-D931-385B-9E4C-9FFDE6D1B923.json"


def extract_json_blocks(text: str) -> list[dict]:
    """Extrai todos os objetos JSON de uma string (saída do Frida)."""
    blocks = []
    # Regex: acha '{' balanceado (simplificado — funciona para JSON plano)
    # Estratégia: captura tudo entre linhas de '===' que contêm JSON
    sections = re.split(r"={5,}", text)
    for section in sections:
        section = section.strip()
        if not section.startswith("{"):
            # Tenta encontrar JSON na seção
            m = re.search(r"\{[\s\S]+\}", section)
            if m:
                section = m.group(0)
            else:
                continue
        try:
            obj = json.loads(section)
            if isinstance(obj, dict):
                blocks.append(obj)
        except json.JSONDecodeError:
            # Tenta consertar JSON truncado
            # (Frida às vezes trunca quando o buffer enche)
            try:
                fixed = section.rstrip(",\n") + "\n}"
                obj = json.loads(fixed)
                if isinstance(obj, dict):
                    blocks.append(obj)
                    print(f"[warn] JSON truncado foi reparado (pode estar incompleto)")
            except Exception:
                print(f"[warn] Bloco JSON inválido ignorado: {section[:60]}...")
    return blocks


def flatten(obj: dict, prefix: str = "") -> dict:
    """Achata dict aninhado em dict plano para merge no JSON de offsets."""
    result = {}
    for k, v in obj.items():
        full_key = (prefix + "_" + k) if prefix else k
        if isinstance(v, dict):
            result.update(flatten(v, full_key))
        elif isinstance(v, (str, int)):
            # Normaliza inteiros para hex string
            if isinstance(v, int):
                v = hex(v)
            result[full_key] = v
    return result


def set_nested(obj: dict, key: str, value: str) -> bool:
    """
    Busca e atualiza uma chave no JSON aninhado (ga.*, st.*).
    Retorna True se encontrou e (potencialmente) atualizou.
    """
    # Busca direta na raiz
    if key in obj:
        return True, obj, key
    # Busca em sub-dicts (ga, st)
    for section_key, section_val in obj.items():
        if isinstance(section_val, dict) and key in section_val:
            return True, section_val, key
    return False, None, None


def find_key_location(offsets: dict, search_key: str):
    """
    Retorna (container_dict, key) onde a chave existe no JSON aninhado.
    Aceita tanto 'fn_GetLocalPlayer' quanto 'ga_fn_GetLocalPlayer'.
    """
    # Remove prefixo de seção se presente
    plain_key = search_key
    for prefix in ("ga_", "st_"):
        if search_key.startswith(prefix):
            plain_key = search_key[len(prefix):]
            break

    # Busca em raiz
    if plain_key in offsets:
        return offsets, plain_key

    # Busca em sub-dicts
    for section_val in offsets.values():
        if isinstance(section_val, dict) and plain_key in section_val:
            return section_val, plain_key

    # Tenta com a chave original (pode ter underscores próprios)
    if search_key in offsets:
        return offsets, search_key
    for section_val in offsets.values():
        if isinstance(section_val, dict) and search_key in section_val:
            return section_val, search_key

    return None, None


def normalize_hex(val: str) -> str:
    """Normaliza string hex: '0x1234' → '0x1234', '4660' → '0x1234'."""
    val = str(val).strip()
    if val.startswith("0x") or val.startswith("0X"):
        try:
            return "0x" + format(int(val, 16), "x")
        except ValueError:
            return val
    try:
        return "0x" + format(int(val), "x")
    except ValueError:
        return val


def main():
    parser = argparse.ArgumentParser(description="Patcha JSON de offsets com saída do Frida")
    parser.add_argument("files", nargs="*", help="Arquivos de saída do Frida (.txt)")
    parser.add_argument("--json", "-j", help="JSON inline de offsets")
    parser.add_argument("--force", "-f", action="store_true",
                        help="Sobrescreve campos já preenchidos (não-DEAD)")
    parser.add_argument("--dry-run", "-n", action="store_true",
                        help="Mostra o que seria alterado sem salvar")
    parser.add_argument("--config", "-c", type=Path, default=JSON_PATH,
                        help=f"Caminho do JSON de offsets (padrão: {JSON_PATH})")
    args = parser.parse_args()

    if not args.files and not args.json:
        parser.print_help()
        sys.exit(1)

    # ── Carrega JSON de offsets ────────────────────────────────────────────
    if not args.config.exists():
        print(f"[erro] Não encontrei: {args.config}")
        sys.exit(1)

    with open(args.config, "r") as f:
        offsets = json.load(f)

    print(f"[*] Config carregada: {args.config} ({len(offsets)} campos)")

    # ── Coleta resultados Frida ────────────────────────────────────────────
    merged = {}

    for path_str in args.files:
        path = Path(path_str)
        if not path.exists():
            print(f"[warn] Arquivo não existe: {path}")
            continue
        text = path.read_text(errors="replace")
        blocks = extract_json_blocks(text)
        print(f"[*] {path.name}: {len(blocks)} bloco(s) JSON extraído(s)")
        for block in blocks:
            merged.update(flatten(block))

    if args.json:
        try:
            obj = json.loads(args.json)
            merged.update(flatten(obj))
        except json.JSONDecodeError as e:
            print(f"[erro] --json inválido: {e}")
            sys.exit(1)

    if not merged:
        print("[erro] Nenhum dado Frida encontrado. Verifique os arquivos.")
        sys.exit(1)

    print(f"[*] Dados Frida: {len(merged)} chave(s) encontrada(s)")

    # ── Aplica merge ──────────────────────────────────────────────────────
    filled    = []
    skipped   = []
    not_found = []

    for frida_key, frida_val in merged.items():
        # Ignora campos de metadados do script
        if frida_key in ("note", "build_uuid", "player_count",
                         "total_objects_observed", "speed_candidates",
                         "weapon_candidates"):
            continue

        container, real_key = find_key_location(offsets, frida_key)
        if container is None:
            not_found.append(frida_key)
            continue

        current = container[real_key]
        if current == DEAD:
            new_val = normalize_hex(frida_val)
            if not args.dry_run:
                container[real_key] = new_val
            filled.append(f"  {real_key}: {DEAD} → {new_val}")
        else:
            if args.force:
                new_val = normalize_hex(frida_val)
                if not args.dry_run:
                    container[real_key] = new_val
                filled.append(f"  {real_key}: {current} → {new_val} (force)")
            else:
                skipped.append(f"  {real_key}: {current} (já preenchido, use --force)")

    # ── Salva ─────────────────────────────────────────────────────────────
    if filled and not args.dry_run:
        backup = args.config.with_suffix(".json.bak")
        shutil.copy2(args.config, backup)
        print(f"[*] Backup: {backup}")
        with open(args.config, "w") as f:
            json.dump(offsets, f, indent=4)
        print(f"[+] Config salva: {args.config}")

    # ── Relatório ─────────────────────────────────────────────────────────
    print(f"\n{'[DRY RUN] ' if args.dry_run else ''}Relatório:")
    print(f"  Preenchidos:    {len(filled)}")
    print(f"  Pulados:        {len(skipped)}")
    print(f"  Chave inesperada (não está no JSON): {len(not_found)}")

    if filled:
        print("\n=== Alterações ===")
        print("\n".join(filled))

    if skipped:
        print("\n=== Pulados (já tinham valor) ===")
        print("\n".join(skipped))

    if not_found:
        print("\n=== Chaves do Frida não encontradas no JSON ===")
        for k in not_found:
            print(f"  {k}")

    # Conta ainda-DEAD (incluindo sub-dicts)
    def collect_dead(obj):
        result = []
        for k, v in obj.items():
            if isinstance(v, dict):
                result.extend(collect_dead(v))
            elif v == DEAD:
                result.append(k)
        return result

    still_dead = collect_dead(offsets)
    if still_dead:
        print(f"\n=== Ainda DEAD ({len(still_dead)}) ===")
        for k in still_dead:
            print(f"  {k}")
    else:
        print("\n[+] Todos os offsets preenchidos! Pronto para build.")


if __name__ == "__main__":
    main()
