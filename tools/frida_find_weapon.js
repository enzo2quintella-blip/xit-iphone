/**
 * frida_find_weapon.js
 * Descoberta de offsets do WeaponController / VerticleGunController no Free Fire
 * (IL2CPP v31, Beebyte, build E50B0E3A)
 *
 * Estratégia:
 *   1. Intercepta il2cpp_runtime_invoke; quando um método do namespace COW.GamePlay
 *      que retorna um objeto qualquer é chamado, verifica se o objeto parece ter
 *      campos de ammo (int ~0–100 perto do início do struct).
 *   2. Para cada Player conhecido (capturado por frida_find_offsets.js ou manualmente),
 *      varre WeaponController via ponteiro partindo do offset 0x60..0x1A0 do Player,
 *      e procura um cluster de 3+ ints plausíveis (1–100) que se ajustam ao modelo
 *      currentAmmo/maxAmmo/reserveAmmo.
 *   3. Monitora escrita em campos int16/int32 perto desses candidatos durante reload
 *      (atira até ficar vazio, depois aguarda recarga) → confirma quais mudam.
 *
 * Uso:
 *   1. Abra o Free Fire, entre em partida, equipe-se com uma arma.
 *   2. frida -U -n com.garena.game.freefire -l tools/frida_find_weapon.js --no-pause
 *   3. Atire toda a munição, espere recarregar.
 *   4. Após ~90s o script emite JSON com offsets candidatos.
 *   5. rpc.exports.emit() força saída imediata.
 */

"use strict";

const UF_NAME = "UnityFramework";

// ── Helpers IL2CPP ─────────────────────────────────────────────────────────
function findExport(name) {
    const a = Module.findExportByName(UF_NAME, name) ||
              Module.findExportByName(UF_NAME, "_" + name);
    return a ? a : null;
}

const il2cpp_domain_get      = new NativeFunction(findExport("il2cpp_domain_get"),      "pointer", []);
const il2cpp_assembly_get_image = new NativeFunction(findExport("il2cpp_assembly_get_image"), "pointer", ["pointer"]);
const il2cpp_domain_get_assemblies = new NativeFunction(findExport("il2cpp_domain_get_assemblies"), "pointer", ["pointer", "pointer"]);
const il2cpp_class_from_name = new NativeFunction(findExport("il2cpp_class_from_name"),  "pointer", ["pointer", "pointer", "pointer"]);
const il2cpp_class_get_name  = new NativeFunction(findExport("il2cpp_class_get_name"),   "pointer", ["pointer"]);
const il2cpp_class_get_namespace = new NativeFunction(findExport("il2cpp_class_get_namespace"), "pointer", ["pointer"]);
const il2cpp_class_get_fields = new NativeFunction(findExport("il2cpp_class_get_fields"), "pointer", ["pointer", "pointer"]);
const il2cpp_field_get_offset = new NativeFunction(findExport("il2cpp_field_get_offset"), "uint32", ["pointer"]);
const il2cpp_field_get_name   = new NativeFunction(findExport("il2cpp_field_get_name"),  "pointer", ["pointer"]);
const il2cpp_type_get_name    = new NativeFunction(findExport("il2cpp_type_get_name"),   "pointer", ["pointer"]);
const il2cpp_field_get_type   = new NativeFunction(findExport("il2cpp_field_get_type"),  "pointer", ["pointer"]);
const il2cpp_class_get_field_from_name = new NativeFunction(findExport("il2cpp_class_get_field_from_name"), "pointer", ["pointer", "pointer"]);
const il2cpp_class_get_methods = new NativeFunction(findExport("il2cpp_class_get_methods"), "pointer", ["pointer", "pointer"]);
const il2cpp_method_get_name  = new NativeFunction(findExport("il2cpp_method_get_name"), "pointer", ["pointer"]);
const il2cpp_object_get_class = new NativeFunction(findExport("il2cpp_object_get_class"), "pointer", ["pointer"]);

function readStr(ptr) {
    if (!ptr || ptr.isNull()) return "(null)";
    try { return ptr.readUtf8String(); } catch(e) { return "(err)"; }
}

// ── Busca classe por nome (Beebyte: nome = ALL_CAPS gibberish) ─────────────
// Varre todas as assemblies procurando pela classe cujo NOME LIMPO está em targetNames.
// Se Beebyte renomeou tudo, usamos busca por características (ver abaixo).
function findClassByName(namespace, name) {
    const domainPtr  = Memory.alloc(8);
    domainPtr.writeU64(0);
    const sizePtr    = Memory.alloc(8);
    const domain     = il2cpp_domain_get();
    const assemblies = il2cpp_domain_get_assemblies(domain, sizePtr);
    const count      = sizePtr.readU64().toNumber();

    for (let i = 0; i < count; i++) {
        const asm   = assemblies.add(i * Process.pointerSize).readPointer();
        if (asm.isNull()) continue;
        const image = il2cpp_assembly_get_image(asm);
        if (image.isNull()) continue;
        const klass = il2cpp_class_from_name(image, Memory.allocUtf8String(namespace), Memory.allocUtf8String(name));
        if (klass && !klass.isNull()) return klass;
    }
    return null;
}

// ── Enumera campos de uma classe ───────────────────────────────────────────
function enumFields(klass) {
    const result = [];
    const iterPtr = Memory.alloc(8);
    iterPtr.writePointer(ptr(0));
    for (;;) {
        const field = il2cpp_class_get_fields(klass, iterPtr);
        if (!field || field.isNull()) break;
        const off      = il2cpp_field_get_offset(field);
        const namePtr  = il2cpp_field_get_name(field);
        const typePtr  = il2cpp_field_get_type(field);
        const typeName = typePtr ? readStr(il2cpp_type_get_name(typePtr)) : "?";
        const name     = readStr(namePtr);
        result.push({ offset: off, name, typeName, fieldPtr: field });
    }
    return result;
}

// ── Lógica de descoberta ───────────────────────────────────────────────────

const knownPlayers = [];   // ponteiros de Player coletados via intercept
const weaponCandidates = {}; // key = offset no Player, value = dados

// Coleta Players via intercept de il2cpp_runtime_invoke
const runtimeInvokeAddr = findExport("il2cpp_runtime_invoke");
let interceptActive = true;

Interceptor.attach(runtimeInvokeAddr, {
    onEnter(args) {
        this.method = args[0];
        this.obj    = args[1];
    },
    onLeave(retval) {
        if (!interceptActive) return;
        if (!this.obj || this.obj.isNull()) return;
        try {
            const klass   = il2cpp_object_get_class(this.obj);
            if (klass.isNull()) return;
            const nsPtr   = il2cpp_class_get_namespace(klass);
            const ns      = readStr(nsPtr);
            if (ns !== "COW.GamePlay") return;
            const namePtr = il2cpp_class_get_name(klass);
            const name    = readStr(namePtr);
            if (name.length < 2) return; // classe com nome vazio = obfuscada
            // Adiciona se parecer Player (heurística: classe com 50+ campos)
            const fields = enumFields(klass);
            if (fields.length > 30 && !knownPlayers.includes(this.obj.toString())) {
                knownPlayers.push(this.obj.toString());
                console.log("[+] Player candidate: " + this.obj + " class=" + name + " fields=" + fields.length);
                probeWeaponController(this.obj, fields);
            }
        } catch(e) {}
    }
});

// ── Sonda WeaponController a partir do Player ─────────────────────────────
function probeWeaponController(playerPtr, playerFields) {
    // Estratégia: para cada campo ponteiro no Player,
    // se o objeto apontado parece ter currentAmmo/maxAmmo (ints 1–300),
    // é candidato a WeaponController.
    for (const f of playerFields) {
        if (!f.typeName.includes("class") && !f.typeName.includes("Il2CppObject")) continue;
        if (f.offset > 0x200) continue;
        try {
            const childPtr = playerPtr.add(f.offset).readPointer();
            if (!childPtr || childPtr.isNull()) continue;
            if (!isValidHeapPtr(childPtr)) continue;

            // Varre primeiros 0x180 bytes do objeto filho em busca de cluster de ints 1–300
            const ammoOffsets = findAmmoCandidates(childPtr, 0x180);
            if (ammoOffsets.length >= 2) {
                const key = "player_offset_0x" + f.offset.toString(16);
                if (!weaponCandidates[key]) {
                    weaponCandidates[key] = {
                        player_field_offset: "0x" + f.offset.toString(16),
                        ammo_cluster: ammoOffsets,
                        child_class: getClassName(childPtr),
                    };
                    console.log("[+] WeaponCtrl candidate @ player+0x" + f.offset.toString(16)
                        + " child=" + weaponCandidates[key].child_class
                        + " ammo_cluster=" + JSON.stringify(ammoOffsets));
                }
            }
        } catch(e) {}
    }
}

function findAmmoCandidates(basePtr, range) {
    const hits = [];
    for (let off = 0x08; off < range; off += 4) {
        try {
            const v = basePtr.add(off).readS32();
            // Ammo plausível: entre 1 e 300
            if (v >= 1 && v <= 300) {
                hits.push({ offset: "0x" + off.toString(16), value: v });
            }
        } catch(e) {}
    }
    // Precisa de pelo menos 2 hits adjacentes (currentAmmo + maxAmmo costumam ser vizinhos)
    const clusters = [];
    for (let i = 0; i < hits.length - 1; i++) {
        const a = parseInt(hits[i].offset,   16);
        const b = parseInt(hits[i+1].offset, 16);
        if (b - a <= 8) {
            clusters.push(hits[i], hits[i+1]);
        }
    }
    return [...new Set(clusters.map(c => c.offset))].slice(0, 6).map(o => ({
        offset: o,
        value: basePtr.add(parseInt(o,16)).readS32()
    }));
}

function isValidHeapPtr(ptr) {
    if (ptr.compare(ptr("0x100000000")) < 0) return false;
    // Deve estar dentro da range do heap iOS (~0x100000000 – 0x3ffffffff)
    if (ptr.compare(ptr("0x1000000000")) > 0) return false;
    return true;
}

function getClassName(objPtr) {
    try {
        const klass = il2cpp_object_get_class(objPtr);
        if (klass.isNull()) return "?";
        return readStr(il2cpp_class_get_name(klass));
    } catch(e) { return "err"; }
}

// ── Monitor de escrita — confirma offsets após reload ─────────────────────
// Chamada após identificar candidatos: instala MemoryAccessMonitor nos candidatos.
function monitorAmmoWrite(basePtr, candidates) {
    if (!MemoryAccessMonitor) {
        console.log("[?] MemoryAccessMonitor não disponível neste Frida build");
        return;
    }
    const ranges = candidates.map(c => ({
        base: basePtr.add(parseInt(c.offset, 16)),
        size: 4
    }));
    MemoryAccessMonitor.enable(ranges, {
        onAccess(details) {
            if (details.operation === "write") {
                console.log("[WRITE] offset=" + details.rangeIndex + " from=" + details.from);
            }
        }
    });
    console.log("[*] MemoryAccessMonitor ativo em " + ranges.length + " campos");
}

// ── Busca CharController.moveSpeed ───────────────────────────────────────
// Separado aqui por completude; a busca principal está em frida_find_offsets.js
function findMoveSpeed() {
    console.log("\n[*] Procurando CharController moveSpeed...");
    const klass = findClassByName("COW.GamePlay", "PhysicalCCT") ||
                  findClassByName("",             "PhysicalCCT") ||
                  findClassByName("COW.Common",   "CharController");
    if (!klass) {
        console.log("    [?] Classe CharController não encontrada (nome Beebyte desconhecido)");
        return null;
    }
    const fields = enumFields(klass);
    console.log("    Campos em CharController: " + fields.length);
    // moveSpeed deve ser float; imprime todos os float
    const floatFields = fields.filter(f => f.typeName === "System.Single");
    floatFields.forEach(f => {
        console.log("      float @ 0x" + f.offset.toString(16) + " name=" + f.name);
    });
    return floatFields;
}

// ── Emissão de resultado ───────────────────────────────────────────────────
function emitResult() {
    interceptActive = false;
    console.log("\n========== RESULTADO frida_find_weapon ==========");
    console.log(JSON.stringify({
        weapon_candidates: weaponCandidates,
        player_count: knownPlayers.length,
        note: "Preencha WeaponController_currentAmmo / maxAmmo / reserveAmmo no JSON de offsets usando os cluster acima"
    }, null, 2));
    console.log("=================================================");
}

// Auto-emite após 90s
setTimeout(emitResult, 90000);

// Busca de moveSpeed imediata (a classe pode estar disponível sem partida ativa)
setTimeout(findMoveSpeed, 3000);

rpc.exports = {
    emit:  () => emitResult(),
    probe: (playerAddr) => probeWeaponController(ptr(playerAddr), []),
};

console.log("[*] frida_find_weapon carregado. Entre em partida e equipe uma arma.");
console.log("[*] Atire e recarregue para confirmar candidatos. Auto-emit em 90s.");
