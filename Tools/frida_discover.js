/**
 * frida_discover.js — Free Fire IL2CPP offset discovery
 * Build UUID: E50B0E3A-D931-385B-9E4C-9FFDE6D1B923
 * Unity 2021.x  |  IL2CPP v31  |  Beebyte obfuscation
 *
 * Usage:
 *   frida -U -f com.dena.a12026418 -l frida_discover.js --no-pause
 *   frida -U --attach-pid <PID>     -l frida_discover.js
 *
 * Output: one JSON line per DISC event.  Collect:
 *   frida ... 2>&1 | grep '^DISC' > offsets_dump.json
 *
 * What it does:
 *   1. Resolves all il2cpp_* exports by RVA from UnityFramework
 *   2. Enumerates ALL assemblies + classes → prints class list (CLASS lines)
 *   3. For classes matching known game patterns → dumps all field offsets (DISC lines)
 *   4. Hooks il2cpp_class_from_name to log every dynamic class lookup (LOOKUP lines)
 *   5. Hooks il2cpp_field_get_offset to log field offset resolutions (FIELD lines)
 *
 * RVAs below are from the export trie of UnityFramework (UUID above).
 * They are offsets from the module's load address — add Module.getBaseAddress("UnityFramework").
 */

"use strict";

// ─── RVAs from export trie (measured, not invented) ─────────────────────────
const RVA = {
    il2cpp_domain_get:                 0xa111b28,
    il2cpp_domain_get_assemblies:      0xa111b34,
    il2cpp_assembly_get_image:         0xa1115cc,
    il2cpp_class_from_name:            0xa111604,
    il2cpp_class_get_field_from_name:  0xa111624,
    il2cpp_class_get_fields:           0xa111610,
    il2cpp_field_get_offset:           0xa111d34,
    il2cpp_class_get_method_from_name: 0xa111630,
    il2cpp_runtime_invoke:             0xa112030,
    il2cpp_string_new:                 0xa11204c,
    il2cpp_object_get_class:           0xa111fd8,
    il2cpp_field_static_get_value:     0xa111d50,
    il2cpp_class_is_assignable_from:   0xa1115f0,
    il2cpp_resolve_icall:              0xa1115a0,
    // Extra introspection APIs
    il2cpp_class_get_name:             0xa111634,
    il2cpp_class_get_namespace:        0xa111638,
    il2cpp_field_get_name:             0xa111d14,  // il2cpp_field_get_name RVA
    il2cpp_image_get_class_count:      0xa111b40,
    il2cpp_image_get_class:            0xa111b44,
    il2cpp_domain_get_assemblies:      0xa111b34,
    il2cpp_assembly_get_image:         0xa1115cc,
};

// ─── Class name patterns to dump fields for ─────────────────────────────────
// Free Fire uses Beebyte: real class names are ALL_CAPS random strings.
// These are human-readable substrings from non-obfuscated classes OR
// exact Beebyte names discovered via previous Frida sessions or
// il2cpp_class_from_name calls captured at runtime.
//
// To find: set DUMP_ALL_CLASSES = true below, run, grep CLASS lines for
// anything with "player", "health", "weapon", "character", "attack" etc.
const CLASS_PATTERNS = [
    // Known non-obfuscated classes (from dump.cs)
    "PlayerInfo",
    "GameController",
    "WeaponController",
    "BaseCharacterController",
    "CharacterController",
    "PlayerAttributes",
    "AttackableEntity",
    "PlayerBehavior",
    "GameManager",
    "UIHudPlayerStats",
    // Pattern matches for Beebyte all-caps (add confirmed names here)
    // e.g. "KADFAHHHMCK", "LIECEIDFCGG"
];

// Set true to enumerate ALL classes to stderr (slow ~30s but finds Beebyte names)
const DUMP_ALL_CLASSES   = true;
// Set true to also dump every field of matched classes
const DUMP_FIELD_OFFSETS = true;

// ─── Helpers ─────────────────────────────────────────────────────────────────

const UF_BASE = Module.getBaseAddress("UnityFramework");
if (UF_BASE === null) {
    console.error("[!] UnityFramework not loaded — attach after game init");
}

function api(name) {
    if (!(name in RVA)) { console.error("[!] Unknown RVA for " + name); return null; }
    return new NativeFunction(UF_BASE.add(RVA[name]), "pointer", ["pointer", "pointer", "pointer", "pointer", "int"], {});
}

// Typed wrappers
const _domain_get = new NativeFunction(UF_BASE.add(RVA.il2cpp_domain_get), "pointer", []);
const _domain_get_assemblies = new NativeFunction(UF_BASE.add(RVA.il2cpp_domain_get_assemblies), "pointer", ["pointer", "pointer"]);
const _assembly_get_image = new NativeFunction(UF_BASE.add(RVA.il2cpp_assembly_get_image), "pointer", ["pointer"]);
const _class_from_name = new NativeFunction(UF_BASE.add(RVA.il2cpp_class_from_name), "pointer", ["pointer", "pointer", "pointer"]);
const _class_get_fields = new NativeFunction(UF_BASE.add(RVA.il2cpp_class_get_fields), "pointer", ["pointer", "pointer"]);
const _field_get_offset = new NativeFunction(UF_BASE.add(RVA.il2cpp_field_get_offset), "size_t", ["pointer"]);
const _class_get_name = new NativeFunction(UF_BASE.add(RVA.il2cpp_class_get_name), "pointer", ["pointer"]);
const _class_get_namespace = new NativeFunction(UF_BASE.add(RVA.il2cpp_class_get_namespace), "pointer", ["pointer"]);
const _field_get_name = new NativeFunction(UF_BASE.add(RVA.il2cpp_field_get_name), "pointer", ["pointer"]);
const _image_get_class_count = new NativeFunction(UF_BASE.add(RVA.il2cpp_image_get_class_count), "uint32", ["pointer"]);
const _image_get_class = new NativeFunction(UF_BASE.add(RVA.il2cpp_image_get_class), "pointer", ["pointer", "uint32"]);

function readCStr(ptr) {
    if (ptr === null || ptr.isNull()) return "";
    try { return ptr.readUtf8String(); } catch (_) { return ""; }
}

function allocStr(s) {
    const buf = Memory.allocUtf8String(s);
    return buf;
}

function dumpClassFields(klass, className) {
    const iterPtr = Memory.alloc(Process.pointerSize);
    iterPtr.writePointer(ptr(0));
    const fields = [];
    for (;;) {
        const field = _class_get_fields(klass, iterPtr);
        if (field === null || field.isNull()) break;
        try {
            const fname  = readCStr(_field_get_name(field));
            const foffset = _field_get_offset(field).toNumber();
            fields.push({ name: fname, offset: "0x" + foffset.toString(16) });
        } catch (e) { break; }
    }
    if (fields.length > 0) {
        console.log("DISC " + JSON.stringify({ class: className, fields: fields }));
    }
}

// ─── Main discovery — runs after a short delay to let IL2CPP init ─────────────

function runDiscovery() {
    console.log("[*] Starting IL2CPP discovery...");
    console.log("[*] UnityFramework base: " + UF_BASE);

    const domain = _domain_get();
    if (domain === null || domain.isNull()) {
        console.error("[!] il2cpp_domain_get returned null — retrying in 2s");
        setTimeout(runDiscovery, 2000);
        return;
    }
    console.log("[*] IL2CPP domain: " + domain);

    // Get assembly count
    const countPtr = Memory.alloc(4);
    countPtr.writeU32(0);
    const assemblies = _domain_get_assemblies(domain, countPtr);
    const count = countPtr.readU32();
    console.log("[*] Assemblies: " + count);

    for (let a = 0; a < count; a++) {
        const asm_ptr = assemblies.add(a * Process.pointerSize).readPointer();
        if (asm_ptr.isNull()) continue;

        const image = _assembly_get_image(asm_ptr);
        if (image === null || image.isNull()) continue;

        const classCount = _image_get_class_count(image);

        for (let c = 0; c < classCount; c++) {
            const klass = _image_get_class(image, c);
            if (klass === null || klass.isNull()) continue;

            const cname = readCStr(_class_get_name(klass));
            const cns   = readCStr(_class_get_namespace(klass));
            const full  = cns ? (cns + "." + cname) : cname;

            if (DUMP_ALL_CLASSES) {
                // Print every class — user greps for interesting ones
                console.log("CLASS " + JSON.stringify({ ns: cns, name: cname }));
            }

            if (DUMP_FIELD_OFFSETS) {
                const match = CLASS_PATTERNS.some(p => cname === p || cname.includes(p) || full.includes(p));
                if (match) {
                    console.log("[+] Dumping fields for: " + full);
                    dumpClassFields(klass, full);
                }
            }
        }
    }

    console.log("[*] Discovery complete.");
}

// ─── Hook il2cpp_class_from_name to log dynamic lookups ─────────────────────

Interceptor.attach(UF_BASE.add(RVA.il2cpp_class_from_name), {
    onEnter: function(args) {
        // args: (image*, namespace*, name*)
        const ns   = readCStr(args[1]);
        const name = readCStr(args[2]);
        if (name && name.length > 2) {
            this.lookupKey = (ns ? ns + "." : "") + name;
        }
    },
    onLeave: function(retval) {
        if (this.lookupKey && retval && !retval.isNull()) {
            console.log("LOOKUP " + JSON.stringify({ name: this.lookupKey, klass: retval.toString() }));
        }
    }
});

// ─── Hook il2cpp_field_get_offset to log all field offset resolutions ────────

Interceptor.attach(UF_BASE.add(RVA.il2cpp_field_get_offset), {
    onEnter: function(args) {
        this.field = args[0];
    },
    onLeave: function(retval) {
        if (this.field && !this.field.isNull()) {
            try {
                const fname = readCStr(_field_get_name(this.field));
                const off   = retval.toInt32();
                if (off > 0 && off < 0x2000) {
                    console.log("FIELD " + JSON.stringify({ field: fname, offset: "0x" + off.toString(16) }));
                }
            } catch (_) {}
        }
    }
});

// ─── Kick off discovery after 3s (let Unity + IL2CPP fully init) ─────────────

setTimeout(runDiscovery, 3000);

console.log("[*] frida_discover.js loaded — discovery starts in 3s");
console.log("[*] Hooks active: il2cpp_class_from_name, il2cpp_field_get_offset");
