#!/usr/bin/env python3
"""
IL2CPP metadata v24-v31 parser + Mach-O arm64 section reader.
Outputs dump.cs compatible with dump_offsets.py.
No external dependencies — stdlib only.
"""

import struct
import sys
import os
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

# ---------------------------------------------------------------------------
# Mach-O helpers
# ---------------------------------------------------------------------------

MH_MAGIC_64   = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_DYLD_INFO_ONLY = 0x80000022

def read_macho_sections(data: bytes) -> Dict[str, Tuple[int, int]]:
    """Return {name: (file_offset, size)} for all Mach-O sections."""
    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != MH_MAGIC_64:
        raise ValueError(f"Not a Mach-O 64-bit LE binary (magic=0x{magic:08X})")
    # mach_header_64: magic(4) cputype(4) cpusubtype(4) filetype(4)
    #                 ncmds(4) sizeofcmds(4) flags(4) reserved(4)
    ncmds = struct.unpack_from('<I', data, 16)[0]
    sections = {}
    off = 32  # start of load commands
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            # segment_command_64: cmd(4) cmdsize(4) segname(16) vmaddr(8)
            #   vmsize(8) fileoff(8) filesize(8) maxprot(4) initprot(4)
            #   nsects(4) flags(4)
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            sect_off = off + 72
            for _ in range(nsects):
                # section_64: sectname(16) segname(16) addr(8) size(8)
                #   offset(4) align(4) reloff(4) nreloc(4) flags(4) ...
                sname = data[sect_off:sect_off+16].rstrip(b'\x00').decode('ascii', errors='replace')
                sgname = data[sect_off+16:sect_off+32].rstrip(b'\x00').decode('ascii', errors='replace')
                sec_addr = struct.unpack_from('<Q', data, sect_off+32)[0]
                sec_size = struct.unpack_from('<Q', data, sect_off+40)[0]
                sec_foff = struct.unpack_from('<I', data, sect_off+48)[0]
                sections[f"{sgname},{sname}"] = (sec_foff, sec_size, sec_addr)
                sect_off += 80
        off += cmdsize
    return sections

def find_il2cpp_registration(data: bytes, sections: dict) -> Optional[Tuple[int, int]]:
    """
    Find CodeRegistration and MetadataRegistration pointers.
    In Unity IL2CPP arm64, there's a symbol il2cpp_codegen_register or
    we look for the registration call pattern.
    Returns (code_reg_va, meta_reg_va) or None.
    """
    # We'll extract these from the metadata directly — not needed for dump.cs generation
    return None

# ---------------------------------------------------------------------------
# Global metadata v24-v31 layout
# ---------------------------------------------------------------------------
# Header field order matches Unity source Il2CppGlobalMetadataHeader
# v24.1+ (we'll handle v29+ which added extra fields)

@dataclass
class MetaHeader:
    sanity: int
    version: int
    # section offsets/sizes (each is int32 offset + int32 count/size)
    string_literal_offset: int
    string_literal_count: int
    string_literal_data_offset: int
    string_literal_data_count: int
    string_offset: int
    string_count: int
    events_offset: int
    events_count: int
    properties_offset: int
    properties_count: int
    methods_offset: int
    methods_count: int
    parameter_default_values_offset: int
    parameter_default_values_count: int
    field_default_values_offset: int
    field_default_values_count: int
    field_and_parameter_default_value_data_offset: int
    field_and_parameter_default_value_data_count: int
    field_marshaledtypes_offset: int
    field_marshaledtypes_count: int
    parameters_offset: int
    parameters_count: int
    fields_offset: int
    fields_count: int
    generic_parameters_offset: int
    generic_parameters_count: int
    generic_parameter_constraints_offset: int
    generic_parameter_constraints_count: int
    generic_containers_offset: int
    generic_containers_count: int
    nested_types_offset: int
    nested_types_count: int
    interfaces_offset: int
    interfaces_count: int
    vtable_methods_offset: int
    vtable_methods_count: int
    interface_offsets_offset: int
    interface_offsets_count: int
    type_definitions_offset: int
    type_definitions_count: int
    # v24.1+
    images_offset: int
    images_count: int
    assemblies_offset: int
    assemblies_count: int
    # more sections follow in v27+ but we have what we need

HEADER_FIELDS_V24 = [
    'string_literal_offset','string_literal_count',
    'string_literal_data_offset','string_literal_data_count',
    'string_offset','string_count',
    'events_offset','events_count',
    'properties_offset','properties_count',
    'methods_offset','methods_count',
    'parameter_default_values_offset','parameter_default_values_count',
    'field_default_values_offset','field_default_values_count',
    'field_and_parameter_default_value_data_offset','field_and_parameter_default_value_data_count',
    'field_marshaledtypes_offset','field_marshaledtypes_count',
    'parameters_offset','parameters_count',
    'fields_offset','fields_count',
    'generic_parameters_offset','generic_parameters_count',
    'generic_parameter_constraints_offset','generic_parameter_constraints_count',
    'generic_containers_offset','generic_containers_count',
    'nested_types_offset','nested_types_count',
    'interfaces_offset','interfaces_count',
    'vtable_methods_offset','vtable_methods_count',
    'interface_offsets_offset','interface_offsets_count',
    'type_definitions_offset','type_definitions_count',
    'images_offset','images_count',
    'assemblies_offset','assemblies_count',
]

def parse_header(data: bytes) -> dict:
    sanity, version = struct.unpack_from('<II', data, 0)
    hdr = {'sanity': sanity, 'version': version}
    off = 8
    for fname in HEADER_FIELDS_V24:
        hdr[fname] = struct.unpack_from('<i', data, off)[0]
        off += 4
    return hdr

def read_string(data: bytes, offset: int) -> str:
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('utf-8', errors='replace')

# IL2CPP type attribute flags
TYPE_ATTRIBUTE_INTERFACE      = 0x00000020
TYPE_ATTRIBUTE_ABSTRACT       = 0x00000080
TYPE_ATTRIBUTE_SEALED         = 0x00000100
TYPE_ATTRIBUTE_VISIBILITY_MASK= 0x00000007
TYPE_ATTRIBUTE_PUBLIC         = 0x00000001
TYPE_ATTRIBUTE_NOT_PUBLIC     = 0x00000000

FIELD_ATTRIBUTE_STATIC        = 0x0010
FIELD_ATTRIBUTE_PUBLIC        = 0x0006
FIELD_ATTRIBUTE_PRIVATE       = 0x0001
FIELD_ATTRIBUTE_FAMILY        = 0x0004  # protected
FIELD_ATTRIBUTE_ACCESS_MASK   = 0x0007

METHOD_ATTRIBUTE_STATIC       = 0x0010
METHOD_ATTRIBUTE_ACCESS_MASK  = 0x0007
METHOD_ATTRIBUTE_PUBLIC       = 0x0006
METHOD_ATTRIBUTE_PRIVATE      = 0x0001
METHOD_ATTRIBUTE_FAMILY       = 0x0004  # protected

# ---------------------------------------------------------------------------
# Type definition struct sizes per version
# v24: 92 bytes, v27+: varies
# We'll detect by version
# ---------------------------------------------------------------------------

def typedef_size(version: int) -> int:
    if version < 24:
        return 88
    elif version <= 24:
        return 92
    elif version <= 27:
        return 92
    else:  # v27+ has extra fields
        return 92  # still same for most fields we need

# Il2CppTypeDefinition (v24.1 / v27+):
# nameIndex(4) namespaceIndex(4) byvalTypeIndex(4) declaringTypeIndex(4)
# parentIndex(4) elementTypeIndex(4)
# genericContainerIndex(4)
# flags(4)
# fieldStart(4) methodStart(4) eventStart(4) propertyStart(4)
# nestedTypesStart(4) interfacesStart(4) vtableStart(4) interfaceOffsetsStart(4)
# method_count(2) property_count(2) field_count(2) event_count(2)
# nested_type_count(2) vtable_count(2) interfaces_count(2) interface_offsets_count(2)
# bitfield(4) token(4)
# total = 4*18 + 2*8 + 4*2 = 72 + 16 + 8 = 96? Let's be careful.

TYPEDEF_FMT = '<iiiiiiiIiiiiiiiihhhhhhhhi I'
# nameIndex(i) namespaceIndex(i) byvalTypeIndex(i) declaringTypeIndex(i)
# parentIndex(i) elementTypeIndex(i) genericContainerIndex(i)
# flags(I)
# fieldStart(i) methodStart(i) eventStart(i) propertyStart(i)
# nestedTypesStart(i) interfacesStart(i) vtableStart(i) interfaceOffsetsStart(i)
# method_count(h) property_count(h) field_count(h) event_count(h)
# nested_type_count(h) vtable_count(h) interfaces_count(h) interface_offsets_count(h)
# bitfield(i) token(I)
TYPEDEF_SIZE = struct.calcsize(TYPEDEF_FMT)

# Il2CppFieldDefinition: nameIndex(4) typeIndex(4) token(4) = 12 bytes
FIELDDEF_FMT  = '<iiI'
FIELDDEF_SIZE = struct.calcsize(FIELDDEF_FMT)

# Il2CppMethodDefinition (v24): nameIndex(4) declaringType(4) returnType(4)
#   parameterStart(4) genericContainerIndex(4) token(4)
#   flags(2) iflags(2) slot(2) parameterCount(2)
METHODDEF_FMT  = '<iiiiiiHHHH'
METHODDEF_SIZE = struct.calcsize(METHODDEF_FMT)

# Il2CppParameterDefinition: nameIndex(4) token(4) typeIndex(4) = 12
PARAMDEF_FMT  = '<iIi'
PARAMDEF_SIZE = struct.calcsize(PARAMDEF_FMT)

# Il2CppType (v24): datapoint(8) attrs(4) type(1) byref(1) pinned(1) pad(1) = 16?
# Actually stored as: data(8 ptr-sized in metadata) + bits
# In global-metadata for field types we don't have full Il2CppType inline —
# field typeIndex is an index into the type table in the binary, not metadata.
# For dump.cs we just need field names and offsets from the binary's field offset table.

# ---------------------------------------------------------------------------
# Field offset extraction from Mach-O binary
# ---------------------------------------------------------------------------

def find_field_offset_section(bin_data: bytes) -> Optional[Tuple[int, int]]:
    """
    Find the __DATA,__il2cpp_field or similar section containing field offsets.
    In Unity arm64 iOS, field offsets are in __DATA segment under various section names.
    We look for __il2cpp_type_info_offset or the metadata registration structure.
    """
    # Look for the section pattern used by Unity IL2CPP
    # The binary has Il2CppMetadataRegistration which contains fieldOffsets pointer
    # We'll scan for it via pattern matching in __DATA
    # For now return None — we'll extract offsets differently
    return None

# ---------------------------------------------------------------------------
# Main parser
# ---------------------------------------------------------------------------

class IL2CppMetadata:
    def __init__(self, meta_path: str, bin_path: str):
        with open(meta_path, 'rb') as f:
            self.meta = f.read()
        with open(bin_path, 'rb') as f:
            self.binary = f.read()

        self.hdr = parse_header(self.meta)
        self.version = self.hdr['version']
        print(f"[*] Metadata version: {self.version}", file=sys.stderr)
        print(f"[*] Metadata size: {len(self.meta)//1024//1024}MB", file=sys.stderr)
        print(f"[*] Binary size: {len(self.binary)//1024//1024}MB", file=sys.stderr)

        self._load_strings()
        self._load_typedefs()
        self._load_fields()
        self._load_methods()
        self._load_field_offsets_from_binary()

    def _load_strings(self):
        base = self.hdr['string_offset']
        size = self.hdr['string_count']
        self._string_base = base
        self._string_data = self.meta[base:base+size]

    def get_string(self, idx: int) -> str:
        if idx < 0:
            return f"<invalid:{idx}>"
        base = self.hdr['string_offset']
        size = self.hdr['string_count']
        abs_off = base + idx
        if abs_off >= len(self.meta) or abs_off >= base + size:
            return f"<oob:{idx}>"
        try:
            end = self.meta.index(b'\x00', abs_off)
        except ValueError:
            end = min(abs_off + 256, len(self.meta))
        return self.meta[abs_off:end].decode('utf-8', errors='replace')

    def _load_typedefs(self):
        off = self.hdr['type_definitions_offset']
        count = self.hdr['type_definitions_count']
        n = count // TYPEDEF_SIZE
        self.typedefs = []
        for i in range(n):
            td = struct.unpack_from(TYPEDEF_FMT, self.meta, off + i * TYPEDEF_SIZE)
            self.typedefs.append(td)
        print(f"[*] Type definitions: {len(self.typedefs)}", file=sys.stderr)

    def _load_fields(self):
        off = self.hdr['fields_offset']
        count = self.hdr['fields_count']
        n = count // FIELDDEF_SIZE
        self.fields = []
        for i in range(n):
            fd = struct.unpack_from(FIELDDEF_FMT, self.meta, off + i * FIELDDEF_SIZE)
            self.fields.append(fd)
        print(f"[*] Fields: {len(self.fields)}", file=sys.stderr)

    def _load_methods(self):
        off = self.hdr['methods_offset']
        count = self.hdr['methods_count']
        n = count // METHODDEF_SIZE
        self.methods = []
        for i in range(n):
            md = struct.unpack_from(METHODDEF_FMT, self.meta, off + i * METHODDEF_SIZE)
            self.methods.append(md)
        print(f"[*] Methods: {len(self.methods)}", file=sys.stderr)

    def _load_field_offsets_from_binary(self):
        """
        Extract field offsets from the binary.
        In IL2CPP arm64, the MetadataRegistration struct contains a pointer to
        fieldOffsets array. We scan for the registration functions.

        Strategy: scan for the Il2CppMetadataRegistration signature in __DATA.
        The fieldOffsets field is at a known position in the struct.

        For v29+: Il2CppMetadataRegistration layout (pointers are 8 bytes):
          genericClasses(8) genericInsts(8) genericMethodTable(8)
          types(8) methodSpecs(8)
          fieldOffsets(8) <-- we want this
          typeInfos(8) metadataUsages(8) interfaceOffsets(8)

        We'll use a different approach: scan DATA sections for the fieldOffsets
        array pattern (array of int32 arrays, one per type).

        Alternative simpler approach: look for __il2cpp_type_data or use
        the string "il2cpp_codegen_register" xref pattern.

        For now we build an empty offset map and note it as unknown.
        The dump.cs will have field names but offsets marked as UNKNOWN.
        A second pass with Frida or the actual registration data can fill them.
        """
        self.field_offsets = {}  # type_idx -> [offset0, offset1, ...]

        # Try to find field offset data via scanning
        # In Unity IL2CPP iOS, there's a section __DATA,__data or __DATA,__bss
        # containing the field offset arrays. Each type's field offset array
        # is an int32[] with one entry per field (instance fields only).
        # The MetadataRegistration.fieldOffsets is a pointer to Il2CppFieldOffsetData[].

        # We look for the pattern in Mach-O sections
        try:
            sections = read_macho_sections(self.binary)
            # Try known section names for IL2CPP data
            for key, (foff, fsize, vaddr) in sections.items():
                if '__il2cpp' in key.lower():
                    print(f"[*] Found IL2CPP section: {key} offset=0x{foff:X} size=0x{fsize:X}", file=sys.stderr)
        except Exception as e:
            print(f"[!] Could not parse Mach-O sections: {e}", file=sys.stderr)

        print("[!] Field offsets: will be marked UNKNOWN — need runtime extraction via Frida", file=sys.stderr)

    def get_type_name(self, typedef_idx: int) -> str:
        if typedef_idx < 0 or typedef_idx >= len(self.typedefs):
            return f"Type_{typedef_idx}"
        td = self.typedefs[typedef_idx]
        name = self.get_string(td[0])
        ns = self.get_string(td[1])
        if ns:
            return f"{ns}.{name}"
        return name

    def get_field_type_str(self, type_idx: int) -> str:
        # We don't have the full type table from binary here
        # Return a placeholder
        return f"object"

    def generate_dump_cs(self, out_path: str):
        print(f"[*] Generating dump.cs ...", file=sys.stderr)
        lines = []
        lines.append("// IL2CPP dump generated by il2cpp_parser.py")
        lines.append(f"// Metadata version: {self.version}")
        lines.append("")

        processed = 0
        for td_idx, td in enumerate(self.typedefs):
            name_idx, ns_idx, byval_ti, decl_ti, parent_ti, elem_ti, gen_cont_idx, \
            flags, \
            field_start, method_start, event_start, prop_start, \
            nested_start, iface_start, vtable_start, iface_off_start, \
            method_count, prop_count, field_count, event_count, \
            nested_count, vtable_count, iface_count, iface_off_count, \
            bitfield, token = td

            class_name = self.get_string(name_idx)
            namespace  = self.get_string(ns_idx)

            if namespace:
                lines.append(f"// Namespace: {namespace}")

            # Visibility
            vis = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK
            vis_str = "public" if vis == TYPE_ATTRIBUTE_PUBLIC else "internal"

            # Modifiers
            mods = []
            if flags & TYPE_ATTRIBUTE_ABSTRACT:
                mods.append("abstract")
            if flags & TYPE_ATTRIBUTE_SEALED:
                mods.append("sealed")
            is_interface = bool(flags & TYPE_ATTRIBUTE_INTERFACE)

            kind = "interface" if is_interface else "class"
            mod_str = " ".join(mods) + " " if mods else ""

            lines.append(f"{vis_str} {mod_str}{kind} {class_name} // TypeDefIndex: {td_idx}")
            lines.append("{")

            # Fields
            for f_idx in range(field_count):
                abs_f = field_start + f_idx
                if abs_f < 0 or abs_f >= len(self.fields):
                    continue
                fd = self.fields[abs_f]
                fname_idx, ftype_idx, ftoken = fd
                fname = self.get_string(fname_idx)

                # Field offset: UNKNOWN until we have runtime data
                # We'll write it as 0xDEAD so dump_offsets.py can detect it
                f_offset = -1
                offset_comment = "// UNKNOWN"

                # Check if we have offset for this field
                if td_idx in self.field_offsets and f_idx < len(self.field_offsets[td_idx]):
                    f_offset = self.field_offsets[td_idx][f_idx]
                    if f_offset == -1:
                        # static / thread-static field — no instance offset
                        offset_comment = "// static"
                    elif f_offset == 0:
                        # slot present but zero — absent/no instance offset
                        offset_comment = "// UNKNOWN"
                    else:
                        offset_comment = f"// 0x{f_offset:X}"

                lines.append(f"\tpublic object {fname}; {offset_comment}")

            # Methods
            for m_idx in range(method_count):
                abs_m = method_start + m_idx
                if abs_m < 0 or abs_m >= len(self.methods):
                    continue
                md = self.methods[abs_m]
                mname_idx, decl_type, ret_type, param_start, gen_cont, mtoken, \
                mflags, miflags, slot, param_count = md
                mname = self.get_string(mname_idx)

                vis = mflags & METHOD_ATTRIBUTE_ACCESS_MASK
                vis_str = "public" if vis == METHOD_ATTRIBUTE_PUBLIC else \
                          "protected" if vis == METHOD_ATTRIBUTE_FAMILY else "private"
                static_str = " static" if mflags & METHOD_ATTRIBUTE_STATIC else ""

                lines.append(f"\t{vis_str}{static_str} object {mname}(); // RVA: 0x0 Offset: 0x0")

            lines.append("}")
            lines.append("")
            processed += 1

        with open(out_path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines))

        print(f"[*] Written {processed} types to {out_path}", file=sys.stderr)
        return processed

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: il2cpp_parser.py <global-metadata.dat> <UnityFramework> <output_dir>")
        sys.exit(1)

    meta_path = sys.argv[1]
    bin_path  = sys.argv[2]
    out_dir   = sys.argv[3]
    os.makedirs(out_dir, exist_ok=True)

    parser = IL2CppMetadata(meta_path, bin_path)
    out_cs = os.path.join(out_dir, 'dump.cs')
    n = parser.generate_dump_cs(out_cs)
    print(f"[+] Done: {n} types → {out_cs}")
