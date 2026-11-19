/**
 * frida_transform_pos.js
 * Verifica o offset de Transform.position em runtime no Free Fire (iOS arm64).
 *
 * O que faz:
 *   1. Encontra UnityFramework via Process.getModuleByName.
 *   2. Resolve Player class via il2cpp_class_from_name("COW.GamePlay","Player").
 *   3. Lê todos os campos da classe Player e imprime offsets e tipos.
 *   4. Lê o campo que tem tipo Il2CppClass == UnityEngine.Transform (ou string "Transform").
 *   5. A partir de uma instância Player, lê ptr_transform = *(Player + off_transform).
 *   6. Imprime os 12 bytes a partir de off=0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70
 *      como float[3] para identificar qual offset contém um XYZ plausível de posição.
 *
 * Uso:
 *   frida -U -n garena.mobilelegends  -l frida_transform_pos.js --no-pause
 *   ou (se tiver o bundle id correto do Free Fire iOS):
 *   frida -U -n com.garena.game.freefire -l frida_transform_pos.js --no-pause
 *
 * Dependências: Frida >= 16, Free Fire rodando em background, iOS 17+.
 *
 * NOTA: Transform.position é um Vector3 (3x float = 12 bytes). O offset
 * "0x60" é default da comunidade para Unity 2021 arm64 mas NÃO foi medido
 * neste build. Este script mede.
 */

"use strict";

// ---------------------------------------------------------------------------
// Config — ajuste se o bundle mudar
// ---------------------------------------------------------------------------
const UF_MODULE   = "UnityFramework";   // módulo que exporta il2cpp_*
const NS_GAMEPLAY = "COW.GamePlay";
const CLASS_PLAYER = "Player";

// Offsets de candidate Transform.position para testar
const PROBE_OFFSETS = [0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function readFloat(ptr) {
    try { return ptr.readFloat(); } catch(e) { return NaN; }
}

function readVec3(base) {
    return {
        x: readFloat(base.add(0)),
        y: readFloat(base.add(4)),
        z: readFloat(base.add(8)),
    };
}

function isPlausiblePos(v) {
    // Coordenadas Free Fire ficam tipicamente em [-5000, 5000].
    // NaN, Inf ou valores > 1e6 são lixo de memória.
    const ok = (n) => !isNaN(n) && isFinite(n) && Math.abs(n) < 1e5;
    return ok(v.x) && ok(v.y) && ok(v.z);
}

function isPlausibleWorld(v) {
    // Y costuma ser altitude (0–100 m), X/Z posição horizontal (0–10000).
    return isPlausiblePos(v) && v.y >= -50 && v.y < 500;
}

function readPtr(base, off) {
    try { return base.add(off).readPointer(); }
    catch(e) { return NULL; }
}

const NULL = ptr(0);

// ---------------------------------------------------------------------------
// Resolve il2cpp exports
// ---------------------------------------------------------------------------
function getExport(modBase, name) {
    // Tenta via Module.findExportByName primeiro (mais rápido)
    let addr = Module.findExportByName(UF_MODULE, name);
    if (addr && !addr.isNull()) return addr;
    // Tenta com underscore prefixado (Mach-O C exports)
    addr = Module.findExportByName(UF_MODULE, "_" + name);
    if (addr && !addr.isNull()) return addr;
    return null;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
setTimeout(function main() {
    console.log("[frida_transform_pos] Iniciando...");

    const mod = Process.findModuleByName(UF_MODULE);
    if (!mod) {
        console.error("[!] Módulo " + UF_MODULE + " não encontrado. Jogo carregado?");
        return;
    }
    console.log("[+] " + UF_MODULE + " base=" + mod.base + " size=" + mod.size.toString(16));

    // --- Resolve exports necessários ---
    const sym_domain_get      = getExport(mod.base, "il2cpp_domain_get");
    const sym_dom_assemblies  = getExport(mod.base, "il2cpp_domain_get_assemblies");
    const sym_asm_get_image   = getExport(mod.base, "il2cpp_assembly_get_image");
    const sym_class_from_name = getExport(mod.base, "il2cpp_class_from_name");
    const sym_class_get_fields= getExport(mod.base, "il2cpp_class_get_fields");
    const sym_field_get_name  = getExport(mod.base, "il2cpp_field_get_name");
    const sym_field_get_offset= getExport(mod.base, "il2cpp_field_get_offset");
    const sym_field_get_type  = getExport(mod.base, "il2cpp_field_get_type");
    const sym_type_get_type   = getExport(mod.base, "il2cpp_type_get_type");
    const sym_class_from_type = getExport(mod.base, "il2cpp_class_from_il2cpp_type");
    const sym_class_get_name  = getExport(mod.base, "il2cpp_class_get_name");
    const sym_class_instance_size = getExport(mod.base, "il2cpp_class_instance_size");

    const missing = [];
    if (!sym_domain_get)      missing.push("il2cpp_domain_get");
    if (!sym_dom_assemblies)  missing.push("il2cpp_domain_get_assemblies");
    if (!sym_asm_get_image)   missing.push("il2cpp_assembly_get_image");
    if (!sym_class_from_name) missing.push("il2cpp_class_from_name");
    if (!sym_class_get_fields)missing.push("il2cpp_class_get_fields");
    if (!sym_field_get_name)  missing.push("il2cpp_field_get_name");
    if (!sym_field_get_offset)missing.push("il2cpp_field_get_offset");
    if (!sym_field_get_type)  missing.push("il2cpp_field_get_type");
    if (!sym_type_get_type)   missing.push("il2cpp_type_get_type");
    if (!sym_class_get_name)  missing.push("il2cpp_class_get_name");

    if (missing.length > 0) {
        console.error("[!] Exports não encontrados: " + missing.join(", "));
        console.error("    Verifique se UnityFramework exporta com prefixo '_il2cpp_'");
        return;
    }

    // Wrap em NativeFunction
    const il2cpp_domain_get = new NativeFunction(sym_domain_get, "pointer", []);
    const il2cpp_domain_get_assemblies = new NativeFunction(sym_dom_assemblies, "pointer", ["pointer","pointer"]);
    const il2cpp_assembly_get_image = new NativeFunction(sym_asm_get_image, "pointer", ["pointer"]);
    const il2cpp_class_from_name = new NativeFunction(sym_class_from_name, "pointer", ["pointer","pointer","pointer"]);
    const il2cpp_class_get_fields = new NativeFunction(sym_class_get_fields, "pointer", ["pointer","pointer"]);
    const il2cpp_field_get_name = new NativeFunction(sym_field_get_name, "pointer", ["pointer"]);
    const il2cpp_field_get_offset = new NativeFunction(sym_field_get_offset, "int32", ["pointer"]);
    const il2cpp_field_get_type = new NativeFunction(sym_field_get_type, "pointer", ["pointer"]);
    const il2cpp_type_get_type = new NativeFunction(sym_type_get_type, "int", ["pointer"]);
    const il2cpp_class_get_name = new NativeFunction(sym_class_get_name, "pointer", ["pointer"]);

    // class_from_il2cpp_type e class_instance_size são opcionais
    const il2cpp_class_from_il2cpp_type = sym_class_from_type ?
        new NativeFunction(sym_class_from_type, "pointer", ["pointer"]) : null;
    const il2cpp_class_instance_size = sym_class_instance_size ?
        new NativeFunction(sym_class_instance_size, "uint32", ["pointer"]) : null;

    // --- Encontra domain e assemblies ---
    const domain = il2cpp_domain_get();
    if (domain.isNull()) {
        console.error("[!] il2cpp_domain_get() retornou NULL — runtime não inicializado ainda?");
        return;
    }

    const sizePtr = Memory.alloc(8);
    sizePtr.writeU64(0);
    const assemblies = il2cpp_domain_get_assemblies(domain, sizePtr);
    const count = sizePtr.readU64().toNumber();
    console.log("[+] Assemblies: " + count);

    // --- Encontra a imagem que contém COW.GamePlay.Player ---
    let mainImage = NULL;
    for (let i = 0; i < count; i++) {
        const asmPtr = assemblies.add(i * Process.pointerSize).readPointer();
        if (asmPtr.isNull()) continue;
        const img = il2cpp_assembly_get_image(asmPtr);
        if (img.isNull()) continue;
        const kl = il2cpp_class_from_name(img,
            Memory.allocUtf8String(NS_GAMEPLAY),
            Memory.allocUtf8String(CLASS_PLAYER));
        if (!kl.isNull()) {
            mainImage = img;
            console.log("[+] Assembly contendo " + NS_GAMEPLAY + "." + CLASS_PLAYER + ": assembly[" + i + "]");
            break;
        }
    }
    if (mainImage.isNull()) {
        console.error("[!] Não encontrei " + NS_GAMEPLAY + "." + CLASS_PLAYER + " — verifique namespace/nome");
        return;
    }

    // --- Resolve Player class ---
    const playerKlass = il2cpp_class_from_name(mainImage,
        Memory.allocUtf8String(NS_GAMEPLAY),
        Memory.allocUtf8String(CLASS_PLAYER));
    if (playerKlass.isNull()) {
        console.error("[!] il2cpp_class_from_name(" + NS_GAMEPLAY + ", " + CLASS_PLAYER + ") = NULL");
        return;
    }
    console.log("[+] Player klass = " + playerKlass);

    if (il2cpp_class_instance_size) {
        const instSize = il2cpp_class_instance_size(playerKlass);
        console.log("[+] Player instance size = 0x" + instSize.toString(16) + " (" + instSize + " bytes)");
    }

    // --- Enumera campos de Player: procura Transform ---
    // Il2CppTypeEnum: CLASS=0x12, OBJECT=0x1C, VALUETYPE=0x11
    console.log("\n[+] Campos de Player class:");
    console.log("    offset  | type_enum | field_class_name");
    console.log("    --------|-----------|----------------");

    let transformOffset = -1;
    const iterPtr = Memory.alloc(Process.pointerSize);
    iterPtr.writePointer(NULL);

    while (true) {
        const fi = il2cpp_class_get_fields(playerKlass, iterPtr);
        if (!fi || fi.isNull()) break;

        const namePtr = il2cpp_field_get_name(fi);
        const name = namePtr && !namePtr.isNull() ? namePtr.readUtf8String() : "???";
        const offset = il2cpp_field_get_offset(fi);
        const ftype = il2cpp_field_get_type(fi);
        const typeEnum = ftype && !ftype.isNull() ? il2cpp_type_get_type(ftype) : -1;

        // Tenta resolver nome da classe do campo
        let fieldClassName = "";
        if (il2cpp_class_from_il2cpp_type && ftype && !ftype.isNull()) {
            const fklass = il2cpp_class_from_il2cpp_type(ftype);
            if (fklass && !fklass.isNull()) {
                const cnPtr = il2cpp_class_get_name(fklass);
                fieldClassName = cnPtr && !cnPtr.isNull() ? cnPtr.readUtf8String() : "";
            }
        }

        // Offset < 16 = campo estático (endereço em tabela estática, não offset de instância)
        const isStatic = offset < 16 && offset >= 0;

        console.log("    0x" + offset.toString(16).padStart(4,"0") +
                    (isStatic ? "(s)" : "   ") +
                    " | 0x" + typeEnum.toString(16).padStart(2,"0") +
                    "      | " + name + (fieldClassName ? (" : " + fieldClassName) : ""));

        // Detecta campo Transform
        if (fieldClassName === "Transform" || fieldClassName.includes("Transform")) {
            if (!isStatic && offset >= 16) {
                transformOffset = offset;
                console.log("    *** offset Transform detectado: 0x" + offset.toString(16) + " ***");
            }
        }
    }

    if (transformOffset < 0) {
        console.log("\n[!] Transform não detectado por nome de tipo — usando offset default 0x60 para sondagem");
        // Ainda faremos a sondagem de floats abaixo em qualquer caso
    } else {
        console.log("\n[+] Transform encontrado no offset 0x" + transformOffset.toString(16));
    }

    // --- Encontra uma instância Player via GObjects / AvatarManager ---
    // Estratégia: hook em il2cpp_class_get_fields não dá instância, então hookamos
    // Update de MonoBehaviour. Alternativa mais simples: lemos o campo estático
    // AvatarManager.Instance -> _players array -> elemento[0].
    //
    // Implementação: hook em il2cpp_runtime_invoke para interceptar chamada que
    // retorne um objeto Player. Mais confiável: hook no inicio de um método
    // que recebe Player* como this pointer.
    //
    // Para simplicidade agora: intercepta il2cpp_runtime_invoke e quando o
    // resultado for um ponteiro cujo klass == playerKlass, captura e sonda.

    const sym_runtime_invoke = getExport(mod.base, "il2cpp_runtime_invoke");
    if (!sym_runtime_invoke) {
        console.log("\n[!] il2cpp_runtime_invoke não encontrado — não posso capturar instância Player automaticamente.");
        console.log("    Estratégia manual: pausar no debugger quando um Player estiver carregado");
        console.log("    e chamar este script com o endereço da instância via rpc.exports.");
        installManualRpc(playerKlass, transformOffset);
        return;
    }

    console.log("\n[+] Interceptando il2cpp_runtime_invoke para capturar instância Player...");
    let captured = false;

    Interceptor.attach(sym_runtime_invoke, {
        onLeave: function(retval) {
            if (captured) return;
            const obj = retval;
            if (!obj || obj.isNull()) return;
            try {
                // Il2CppObject: klass ptr está nos primeiros 8 bytes (arm64)
                const klass = obj.readPointer();
                if (!klass.equals(playerKlass)) return;
            } catch(e) { return; }

            captured = true;
            console.log("\n[!!!] Instância Player capturada: " + obj);
            probeTransformOffsets(obj, transformOffset);
        }
    });

    // RPC para sondagem manual via endereço
    installManualRpc(playerKlass, transformOffset);

    console.log("[+] Aguardando instância Player em il2cpp_runtime_invoke...");
    console.log("    (Entre em partida para triggers de Player aparecerem)");

}, 3000); // aguarda 3s para o runtime inicializar

// ---------------------------------------------------------------------------
// Sonda offsets em um objeto Player para encontrar position XYZ
// ---------------------------------------------------------------------------
function probeTransformOffsets(playerObj, knownTransformOffset) {
    console.log("\n--- Sondagem de offsets Transform.position ---");
    console.log("    playerObj = " + playerObj);

    // Passo 1: lê o ponteiro Transform no offset conhecido (ou 0x60 default)
    const off_transform = knownTransformOffset > 0 ? knownTransformOffset : 0x60;
    let transformPtr = NULL;
    try {
        transformPtr = playerObj.add(off_transform).readPointer();
    } catch(e) {}

    if (!transformPtr || transformPtr.isNull()) {
        console.log("[!] Player+0x" + off_transform.toString(16) + " = NULL — offset transform incorreto?");
        // Tenta offsets alternativos para o ponteiro Transform
        for (let off = 0x18; off <= 0x100; off += 8) {
            try {
                const p = playerObj.add(off).readPointer();
                if (!p.isNull() && p.and(ptr("0xFFFFF000")).compare(ptr("0x1000")) > 0) {
                    // Lê o que parece ser um objeto Il2CppObject (tem klass ptr)
                    const kptr = p.readPointer();
                    if (!kptr.isNull()) {
                        // Sonda floats dentro desse objeto
                        const v = readVec3(p.add(0x60));
                        if (isPlausibleWorld(v)) {
                            console.log("[+] Offset transform candidato: 0x" + off.toString(16) +
                                        " -> obj=" + p + " -> pos@+0x60: " +
                                        v.x.toFixed(2) + ", " + v.y.toFixed(2) + ", " + v.z.toFixed(2));
                        }
                    }
                }
            } catch(e) {}
        }
        return;
    }

    console.log("[+] transformPtr (Player+0x" + off_transform.toString(16) + ") = " + transformPtr);

    // Passo 2: sonda PROBE_OFFSETS dentro do objeto Transform
    console.log("\n    Sondando floats dentro do objeto Transform:");
    console.log("    offset  | x          | y          | z          | plausível?");
    console.log("    --------|------------|------------|------------|----------");

    const PROBE_OFFSETS = [0x10, 0x18, 0x20, 0x28, 0x38, 0x40, 0x48, 0x50,
                           0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90];
    for (const off of PROBE_OFFSETS) {
        try {
            const v = readVec3(transformPtr.add(off));
            const plaus = isPlausiblePos(v) ? (isPlausibleWorld(v) ? "WORLD ✓" : "plaus") : "";
            console.log("    0x" + off.toString(16).padStart(4,"0") +
                        "  | " + (isNaN(v.x) ? "NaN" : v.x.toFixed(3)).padEnd(10) +
                        " | " + (isNaN(v.y) ? "NaN" : v.y.toFixed(3)).padEnd(10) +
                        " | " + (isNaN(v.z) ? "NaN" : v.z.toFixed(3)).padEnd(10) +
                        " | " + plaus);
        } catch(e) {
            console.log("    0x" + off.toString(16).padStart(4,"0") + "  | READ_FAULT");
        }
    }

    // Passo 3: imprime resultado do campo transform.position @ 0x60 (default)
    try {
        const posDefault = readVec3(transformPtr.add(0x60));
        console.log("\n[RESULTADO] Transform.position @ transformPtr+0x60: " +
                    posDefault.x.toFixed(3) + ", " +
                    posDefault.y.toFixed(3) + ", " +
                    posDefault.z.toFixed(3));
        if (isPlausibleWorld(posDefault)) {
            console.log("            ^^^ PARECE VÁLIDO (offset default 0x60 confirmado)");
        } else {
            console.log("            ^^^ SUSPEITO — veja tabela acima para offset correto");
        }
    } catch(e) {}

    // Polling: lê a posição a cada 2s para confirmar que muda com movimento
    console.log("\n[+] Polling a cada 2s — mova o personagem para confirmar offset...");
    let pollCount = 0;
    const pollInterval = setInterval(function() {
        if (pollCount++ >= 10) {
            clearInterval(pollInterval);
            console.log("[+] Polling encerrado. Resultados acima.");
            return;
        }
        try {
            const v = readVec3(transformPtr.add(0x60));
            console.log("[poll " + pollCount + "] pos@+0x60: " +
                        v.x.toFixed(3) + " " + v.y.toFixed(3) + " " + v.z.toFixed(3));
        } catch(e) {}
    }, 2000);
}

// ---------------------------------------------------------------------------
// RPC manual: probe com endereço fornecido externamente
// rpc.exports.probe("0x1234567890") — chame do REPL Frida
// ---------------------------------------------------------------------------
function installManualRpc(playerKlass, knownTransformOffset) {
    rpc.exports = {
        probe: function(addrStr) {
            const obj = ptr(addrStr);
            console.log("\n[RPC] probe(" + obj + ")");
            probeTransformOffsets(obj, knownTransformOffset);
            return "done";
        },
        listFields: function() {
            // Re-imprime a lista de campos com offsets
            console.log("[RPC] listFields — ver output inicial acima");
            return "done";
        },
        // Lê position diretamente dado Player* e offset transform
        readPos: function(playerAddrStr, offTransform, offPosition) {
            const off_t = parseInt(offTransform, 16);
            const off_p = parseInt(offPosition, 16);
            try {
                const player = ptr(playerAddrStr);
                const transform = player.add(off_t).readPointer();
                const v = readVec3(transform.add(off_p));
                const result = { x: v.x, y: v.y, z: v.z };
                console.log("[RPC] readPos: " + JSON.stringify(result));
                return result;
            } catch(e) {
                return { error: e.message };
            }
        }
    };
    console.log("\n[+] RPC exports disponíveis:");
    console.log("    rpc.exports.probe('0xPLAYER_ADDR')     — sonda offsets em instância Player");
    console.log("    rpc.exports.readPos('0xPLAYER', '0x60', '0x60') — lê position dado offsets");
}
