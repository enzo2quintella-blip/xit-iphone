/**
 * frida_find_speed.js
 * Encontra o offset de moveSpeed (CharController / PhysicalCCT) no Free Fire.
 *
 * Estratégia:
 *   1. Monitora floats entre 3.0 e 10.0 m/s que MUDAM durante o movimento
 *      do personagem (caminhar vs correr vs parar).
 *   2. Usa il2cpp_class_get_field_from_name nas classes candidatas para tentar
 *      achá-las pelo nome (funciona se Beebyte preservou o nome do campo).
 *   3. Se o campo não foi encontrado por nome, faz varredura de memória:
 *      a partir de um objeto PhysicalCCT/CharController capturado via
 *      il2cpp_runtime_invoke, monitoramos quais floats mudam ao mover.
 *
 * Uso:
 *   1. Entre em partida, carregue o script.
 *   2. Caminhe normalmente → o script detecta velocidade base.
 *   3. Use sprint → velocidade aumenta → ponto de mudança = offset.
 *   4. Fique parado → velocidade volta a 0 / base.
 *   5. Output JSON após 120s ou rpc.exports.emit().
 *
 * frida -U -n com.garena.game.freefire -l tools/frida_find_speed.js --no-pause
 */

"use strict";

const UF_NAME = "UnityFramework";

function findExport(name) {
    return Module.findExportByName(UF_NAME, name) ||
           Module.findExportByName(UF_NAME, "_" + name) || null;
}

const il2cpp_domain_get          = new NativeFunction(findExport("il2cpp_domain_get"),          "pointer", []);
const il2cpp_assembly_get_image  = new NativeFunction(findExport("il2cpp_assembly_get_image"),  "pointer", ["pointer"]);
const il2cpp_domain_get_assemblies = new NativeFunction(findExport("il2cpp_domain_get_assemblies"), "pointer", ["pointer", "pointer"]);
const il2cpp_class_from_name     = new NativeFunction(findExport("il2cpp_class_from_name"),     "pointer", ["pointer", "pointer", "pointer"]);
const il2cpp_class_get_name      = new NativeFunction(findExport("il2cpp_class_get_name"),      "pointer", ["pointer"]);
const il2cpp_class_get_namespace = new NativeFunction(findExport("il2cpp_class_get_namespace"), "pointer", ["pointer"]);
const il2cpp_class_get_fields    = new NativeFunction(findExport("il2cpp_class_get_fields"),    "pointer", ["pointer", "pointer"]);
const il2cpp_field_get_offset    = new NativeFunction(findExport("il2cpp_field_get_offset"),    "uint32",  ["pointer"]);
const il2cpp_field_get_name      = new NativeFunction(findExport("il2cpp_field_get_name"),      "pointer", ["pointer"]);
const il2cpp_field_get_type      = new NativeFunction(findExport("il2cpp_field_get_type"),      "pointer", ["pointer"]);
const il2cpp_type_get_name       = new NativeFunction(findExport("il2cpp_type_get_name"),       "pointer", ["pointer"]);
const il2cpp_object_get_class    = new NativeFunction(findExport("il2cpp_object_get_class"),    "pointer", ["pointer"]);
const il2cpp_class_get_field_from_name = new NativeFunction(findExport("il2cpp_class_get_field_from_name"), "pointer", ["pointer", "pointer"]);

function readStr(ptr) {
    if (!ptr || ptr.isNull()) return "(null)";
    try { return ptr.readUtf8String(); } catch(e) { return "(err)"; }
}

// ── Snap de floats candidatos ──────────────────────────────────────────────
// Guarda snapshots periódicos dos floats num objeto; detecta quais mudam
// em sincronia com o movimento do jogador.
const snapshots = {}; // key=addr, value=histórico de leituras

function snapObject(basePtr, rangeBytes) {
    const result = {};
    for (let off = 0x08; off < rangeBytes; off += 4) {
        try {
            const v = basePtr.add(off).readFloat();
            if (isFinite(v) && v > 0.5 && v < 50.0) {
                result["0x" + off.toString(16)] = v;
            }
        } catch(e) {}
    }
    return result;
}

// ── Captura de objetos CharController via il2cpp_runtime_invoke ─────────────
const charCtrlCandidates = [];
const knownClasses = ["PhysicalCCT", "CharController", "CC", "MovementController"];

Interceptor.attach(findExport("il2cpp_runtime_invoke"), {
    onEnter(args) { this.obj = args[1]; },
    onLeave(retval) {
        if (!this.obj || this.obj.isNull()) return;
        try {
            const klass = il2cpp_object_get_class(this.obj);
            if (klass.isNull()) return;
            const name = readStr(il2cpp_class_get_name(klass));
            const ns   = readStr(il2cpp_class_get_namespace(klass));

            // Aceita qualquer classe do namespace COW.GamePlay que tenha floats plausíveis
            if (!ns.startsWith("COW") && ns !== "UnityEngine") return;

            if (!charCtrlCandidates.find(c => c.addr === this.obj.toString())) {
                charCtrlCandidates.push({ addr: this.obj.toString(), klass, name, ns });
                console.log("[+] Objeto capturado: " + this.obj + " class=" + ns + "." + name);
            }
        } catch(e) {}
    }
});

// ── Poll periódico: compara snapshots ─────────────────────────────────────
let pollCount = 0;
const prevSnaps = {}; // addr -> snap anterior
const changingFields = {}; // addr -> Set de offsets que mudaram

const poller = setInterval(function() {
    pollCount++;
    for (const c of charCtrlCandidates) {
        try {
            const basePtr = ptr(c.addr);
            const snap = snapObject(basePtr, 0x200);
            const prev = prevSnaps[c.addr] || {};

            for (const [off, val] of Object.entries(snap)) {
                const pv = prev[off];
                if (pv !== undefined && Math.abs(val - pv) > 0.01) {
                    if (!changingFields[c.addr]) changingFields[c.addr] = {};
                    if (!changingFields[c.addr][off]) {
                        changingFields[c.addr][off] = { changes: 0, values: [] };
                    }
                    changingFields[c.addr][off].changes++;
                    changingFields[c.addr][off].values.push(parseFloat(val.toFixed(4)));
                    if (changingFields[c.addr][off].changes === 1) {
                        console.log("[~] MUDANÇA DETECTADA " + c.name + " @ " + c.addr
                            + " off=" + off + " " + pv.toFixed(3) + " → " + val.toFixed(3));
                    }
                }
            }
            prevSnaps[c.addr] = snap;
        } catch(e) {}
    }
}, 500);

// ── Busca estática por nome de campo (caso Beebyte não tenha renomeado) ────
function tryFindByFieldName(klassPtrOrNull) {
    const speedNames = ["moveSpeed", "m_MoveSpeed", "_moveSpeed", "speed",
                        "walkSpeed", "runSpeed", "FIELD_14"];
    const klass = klassPtrOrNull;
    if (!klass) return null;
    for (const n of speedNames) {
        const fld = il2cpp_class_get_field_from_name(klass, Memory.allocUtf8String(n));
        if (fld && !fld.isNull()) {
            const off = il2cpp_field_get_offset(fld);
            console.log("[+] moveSpeed encontrado por nome '" + n + "' @ offset 0x" + off.toString(16));
            return { name: n, offset: "0x" + off.toString(16) };
        }
    }
    return null;
}

// ── Resultado ──────────────────────────────────────────────────────────────
function emitResult() {
    clearInterval(poller);

    const speedResults = {};
    for (const [addr, fields] of Object.entries(changingFields)) {
        const c = charCtrlCandidates.find(x => x.addr === addr);
        // Filtra campos que mudaram ≥3 vezes e cujos valores ficaram entre 0.5–20
        const speedFields = Object.entries(fields)
            .filter(([off, data]) => {
                if (data.changes < 3) return false;
                const vals = data.values;
                return vals.some(v => v > 0.5 && v < 20.0);
            })
            .map(([off, data]) => ({
                offset: off,
                change_count: data.changes,
                sample_values: data.values.slice(-5)
            }));

        if (speedFields.length > 0) {
            speedResults[addr] = {
                class: c ? c.ns + "." + c.name : "?",
                candidates: speedFields
            };
        }
    }

    console.log("\n========== RESULTADO frida_find_speed ==========");
    console.log(JSON.stringify({
        speed_candidates: speedResults,
        total_objects_observed: charCtrlCandidates.length,
        note: "O offset com change_count mais alto e valores 1.0–8.0 é moveSpeed. Preencha CharController_moveSpeed no JSON."
    }, null, 2));
    console.log("================================================");
}

setTimeout(emitResult, 120000);

rpc.exports = {
    emit:  () => emitResult(),
    list:  () => charCtrlCandidates.map(c => ({ addr: c.addr, class: c.ns + "." + c.name })),
    probe: (addr) => snapObject(ptr(addr), 0x200),
};

console.log("[*] frida_find_speed carregado.");
console.log("[*] WALK devagar / corra / pare para gerar variação de velocidade.");
console.log("[*] Auto-emit em 120s ou rpc.exports.emit().");
