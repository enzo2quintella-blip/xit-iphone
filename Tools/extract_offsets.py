#!/usr/bin/env python3 -I
"""
extract_offsets.py — Injects real IL2CPP field offsets from UnityFramework binary
into IL2CppMetadata, then regenerates dump.cs with real // 0xHH comments.

Measured facts (from prior analysis of THIS build):
  - fieldOffsets array VA/fileoff = 0xB846008
  - 43420 entries × 8 bytes each (pointer array, arm64 so 8-byte ptrs)
  - Each pointer targets __TEXT,__const (0xA48F280 – 0xAD3CDF1)
  - vmaddr == fileoff throughout (slide = 0, confirmed from Mach-O load commands)
  - Each pointer lands on a 16-byte-aligned block of 4 × int32 field offsets
  - -1 (0xFFFFFFFF) = static/thread-static (no instance offset)
  - 0 = absent / no field at that slot
  - Per Unity 2021 IL2CPP v31 iOS: each type gets exactly the fields
    DEFINED in that type (not inherited), mapped 1:1 with metadata
    fieldStart..fieldStart+fieldCount by INSTANCE field order
    (static fields are present in metadata but their slot = -1)
"""

import sys
import os
import struct

# ── paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

DUMP_WORK   = os.path.join(SCRIPT_DIR, '..', 'dump_work')
META_PATH   = os.path.join(DUMP_WORK, 'global-metadata.dat')
BIN_PATH    = os.path.join(DUMP_WORK, 'UnityFramework')
OUT_DIR     = os.path.join(DUMP_WORK, 'output')

# ── measured constants ────────────────────────────────────────────────────────
FIELD_OFFSETS_FILEOFF = 0xB846008   # fieldOffsets ptr-array in binary
NUM_TYPES             = 43420        # validated against metadata type count
TEXT_CONST_START      = 0xA48F280   # __TEXT,__const section start (fileoff/vmaddr)
TEXT_CONST_END        = 0xAD3CDF1   # __TEXT,__const section end

def load_binary(path: str) -> bytes:
    with open(path, 'rb') as f:
        return f.read()

def read_ptr(data: bytes, off: int) -> int:
    """Read a little-endian 64-bit pointer."""
    return struct.unpack_from('<Q', data, off)[0]

def read_i32(data: bytes, off: int) -> int:
    return struct.unpack_from('<i', data, off)[0]

def build_field_offset_map(binary: bytes) -> dict:
    """
    Returns {type_idx: [i32_slot0, i32_slot1, i32_slot2, i32_slot3]}
    for all 43420 types.  Slots with value -1 are static; 0 means absent.
    """
    print(f"[*] Reading fieldOffsets array at 0x{FIELD_OFFSETS_FILEOFF:X}", file=sys.stderr)
    result = {}

    # Validate binary length
    needed = FIELD_OFFSETS_FILEOFF + NUM_TYPES * 8
    if len(binary) < needed:
        raise RuntimeError(f"Binary too short: {len(binary):#x} < {needed:#x}")

    for ti in range(NUM_TYPES):
        ptr_off = FIELD_OFFSETS_FILEOFF + ti * 8
        ptr     = read_ptr(binary, ptr_off)

        # Dereference: ptr is both VA and fileoff (slide=0)
        # Validate it's inside __TEXT,__const
        if ptr < TEXT_CONST_START or ptr >= TEXT_CONST_END:
            # Type has no instance fields defined — all static or empty
            result[ti] = []
            continue

        # Read 4 × int32 slots from __TEXT,__const
        slots = []
        for s in range(4):
            slots.append(read_i32(binary, ptr + s * 4))

        result[ti] = slots

    # Quick sanity
    nonempty = sum(1 for v in result.values() if v)
    print(f"[*] fieldOffsets map built: {NUM_TYPES} types, {nonempty} with data", file=sys.stderr)
    return result

def inject_and_dump(field_offset_map: dict):
    """
    Import IL2CppMetadata, patch its field_offsets dict with real data,
    then call generate_dump_cs.

    The parser's field_offsets dict maps:
        type_idx -> [offset_for_field0, offset_for_field1, ...]
    but only for INSTANCE fields in order (skipping statics).

    The binary slots contain ALL fields (instance + static), where
    static = -1.  We must filter to match the metadata field order.
    """
    from il2cpp_parser import IL2CppMetadata

    print("[*] Instantiating IL2CppMetadata (parses typedefs + fields)...", file=sys.stderr)
    parser = IL2CppMetadata(META_PATH, BIN_PATH)

    print("[*] Injecting real field offsets...", file=sys.stderr)

    injected_types  = 0
    injected_fields = 0
    skipped_oob     = 0

    for td_idx, td in enumerate(parser.typedefs):
        # Typedef tuple layout (see il2cpp_parser.py TYPEDEF_FMT):
        # [0]  nameIndex
        # [1]  namespaceIndex
        # ...
        # [8]  fieldStart
        # ...
        # [18] field_count  (int16)
        field_start = td[8]
        field_count = td[18]

        if field_count <= 0:
            continue

        slots = field_offset_map.get(td_idx, [])
        if not slots:
            continue

        # Walk metadata fields for this type, assigning binary slots IN ORDER.
        # Slots that are -1 correspond to static fields; we still consume a slot
        # but don't set an instance offset.
        # The mapping: slot index → field definition index (absolute in self.fields)
        # is 1:1 by field order.
        offsets_for_type = []
        for slot_i in range(min(field_count, len(slots))):
            abs_f = field_start + slot_i
            if abs_f < 0 or abs_f >= len(parser.fields):
                skipped_oob += 1
                offsets_for_type.append(-1)
                continue
            slot_val = slots[slot_i]
            offsets_for_type.append(slot_val)
            if slot_val != -1 and slot_val != 0:
                injected_fields += 1

        if any(v > 0 for v in offsets_for_type):
            parser.field_offsets[td_idx] = offsets_for_type
            injected_types += 1

    print(f"[*] Injected: {injected_types} types, {injected_fields} real instance offsets", file=sys.stderr)
    if skipped_oob:
        print(f"[!] Skipped {skipped_oob} OOB field references", file=sys.stderr)

    os.makedirs(OUT_DIR, exist_ok=True)
    out_cs = os.path.join(OUT_DIR, 'dump.cs')
    n = parser.generate_dump_cs(out_cs)
    print(f"[+] dump.cs: {n} types → {out_cs}")
    return out_cs

def main():
    print("[*] Loading UnityFramework binary...", file=sys.stderr)
    binary = load_binary(BIN_PATH)
    print(f"[*] Binary size: {len(binary)//1024//1024} MB", file=sys.stderr)

    offset_map = build_field_offset_map(binary)

    # Sample check — type 8749 (PlayerInfo from prior analysis) → expect [0x48, -1, 0, 0]
    sample = offset_map.get(8749, [])
    print(f"[*] Sanity check type[8749]: {[hex(x) if x >= 0 else x for x in sample]}", file=sys.stderr)

    out_cs = inject_and_dump(offset_map)

    # Print first 30 lines of dump.cs with real offsets
    print("\n── dump.cs preview (first 30 lines with real offsets) ──")
    with open(out_cs, 'r') as f:
        for i, line in enumerate(f):
            if i >= 30:
                break
            print(line, end='')

if __name__ == '__main__':
    main()
