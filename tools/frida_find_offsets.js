/**
 * frida_find_offsets.js
 * Descobre os offsets que estão como DEADC0DE no JSON de config.
 *
 * O que resolve:
 *   - fn_GetLocalPlayer   (RVA no UnityFramework)
 *   - fn_GetAllPlayers    (RVA no UnityFramework)
 *   - fn_GameUpdate       (RVA no UnityFramework)
 *   - PlayerInfo_health, _health_max, _armor, _team_id, _is_dead,
 *     _transform, _name, _weapon_controller (offsets de campo no heap)
 *   - WeaponController_currentAmmo, _maxAmmo, _reserveAmmo
 *   - CharController_moveSpeed
 *   - klass_PlayerInfo, klass_GameController, klass_WeaponController (RVAs)
 *
 * Saída: JSON pronto para colar em offsets_E50B0E3A-....json
 *
 * Uso:
 *   frida -U -n com.garena.game.freefire -l frida_find_offsets.js --no-pause
 *   Entre em uma partida, aguarde ~10s, leia o output.
 *
 * Estratégia geral:
 *   1. Resolve il2cpp_* exports do UnityFramework.
 *   2. Enumera campos das classes Player, AvatarManager, PhysicalCCT,
 *      VerticleGunController via il2cpp_class_get_fields → imprime offsets.
 *   3. Intercepta il2cpp_runtime_invoke: captura a primeira instância cujo
 *      klass == Player e sonda os campos por valor plausível.
 *   4. Intercepta a função de update do MonoBehaviour do Player para achar
 *      fn_GameUpdate: a função de Update que tem um Player* como primeiro arg.
 *   5. Calcula RVAs = VA - module_base e emite JSON.
 *
 * Dependência: Frida >= 16, Free Fire rodando, iOS 17+.
 */

"use strict";

// ─── Config ──────────────────────────────────────────────────────────────────
const UF_MODULE     = "UnityFramework";
const NS_GAMEPLAY   = "COW.GamePlay";
const NS_COW        = "COW";
// Nomes de classe reais (medidos com il2cpp_parser no metadata v31)
const CLS_PLAYER    = "Player";
const CLS_AVATAR_MG = "AvatarManager";
const CLS_PHYSCT    = "PhysicalCCT";
const CLS_GUN_CTRL  = "VerticleGunController";
const CLS_PLAYERDATA= "PlayerData";

// ─── Resultado que será emitido no final ─────────────────────────────────────
const result = {
    build_uuid: "E50B0E3A-D931-385B-9E4C-9FFDE6D1B923",
    ga: {
        // exports il2cpp_* já conhecidos (mantidos do JSON base)
        il2cpp_domain_get:                "0xa111b28",
        il2cpp_domain_get_assemblies:     "0xa111b34",
        il2cpp_assembly_get_image:        "0xa1115cc",
        il2cpp_class_from_name:           "0xa111604",
        il2cpp_class_get_field_from_name: "0xa111624",
        il2cpp_class_get_fields:          "0xa111610",
        il2cpp_field_get_offset:          "0xa111d34",
        il2cpp_class_get_method_from_name:"0xa111630",
        il2cpp_runtime_invoke:            "0xa112030",
        il2cpp_string_new:                "0xa11204c",
        il2cpp_object_get_class:          "0xa111fd8",
        il2cpp_field_static_get_value:    "0xa111d50",
        il2cpp_class_is_assignable_from:  "0xa1115f0",
        il2cpp_resolve_icall:             "0xa1115a0",
        // A descobrir:
        klass_PlayerInfo:        "0xDEADC0DEDEADC0DE",
        klass_GameController:    "0xDEADC0DEDEADC0DE",
        klass_WeaponController:  "0xDEADC0DEDEADC0DE",
        fn_GetLocalPlayer:       "0xDEADC0DEDEADC0DE",
        fn_GetAllPlayers:        "0xDEADC0DEDEADC0DE",
        fn_GameUpdate:           "0xDEADC0DEDEADC0DE",
        fn_WorldToScreen:        "0xDEADC0DEDEADC0DE",
    },
    st: {
        PlayerInfo_health:            "0xDEADC0DEDEADC0DE",
        PlayerInfo_health_max:        "0xDEADC0DEDEADC0DE",
        PlayerInfo_armor:             "0xDEADC0DEDEADC0DE",
        PlayerInfo_team_id:           "0xDEADC0DEDEADC0DE",
        PlayerInfo_is_dead:           "0xDEADC0DEDEADC0DE",
        PlayerInfo_is_local:          "0xDEADC0DEDEADC0DE",
        PlayerInfo_transform:         "0xDEADC0DEDEADC0DE",
        PlayerInfo_name:              "0xDEADC0DEDEADC0DE",
        PlayerInfo_bone_root:         "0xDEADC0DEDEADC0DE",
        PlayerInfo_weapon_controller: "0xDEADC0DEDEADC0DE",
        Transform_position:           "0x90",  // valor atual do JSON; será re-sondado
        WeaponController_currentAmmo: "0xDEADC0DEDEADC0DE",
        WeaponController_maxAmmo:     "0xDEADC0DEDEADC0DE",
        WeaponController_reserveAmmo: "0xDEADC0DEDEADC0DE",
        CharController_moveSpeed:     "0xDEADC0DEDEADC0DE",
        Camera_projMatrix:            "0xDEADC0DEDEADC0DE",
        Camera_viewMatrix:            "0xDEADC0DEDEADC0DE",
    }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────
function hex(n) { return "0x" + n.toString(16); }

function getExport(name) {
    let a = Module.findExportByName(UF_MODULE, name);
    if (a && !a.isNull()) return a;
    a = Module.findExportByName(UF_MODULE, "_" + name);
    return (a && !a.isNull()) ? a : null;
}

function readPtr(base, off) {
    try { return base.add(off).readPointer(); } catch(e) { return ptr(0); }
}

function readF32(base, off) {
    try { return base.add(off).readFloat(); } catch(e) { return NaN; }
}

function readI32(base, off) {
    try { return base.add(off).readS32(); } catch(e) { return 0x7fffffff; }
}

function readU8(base, off) {
    try { return base.add(off).readU8(); } catch(e) { return 0xff; }
}

function isPlausibleHP(v) { return !isNaN(v) && isFinite(v) && v > 0 && v <= 10000; }
function isPlausibleTeamID(v) { return v >= 0 && v <= 32; }
function isPlausibleSpeed(v) { return !isNaN(v) && v > 0.1 && v < 100; }
function isPlausibleAmmo(v) { return v > 0 && v < 10000; }

// ─── Il2CPP API setup ─────────────────────────────────────────────────────────
const mod = Process.findModuleByName(UF_MODULE);
if (!mod) { console.error("[!] " + UF_MODULE + " não encontrado"); throw new Error(); }
const moduleBase = mod.base;
const moduleSize = mod.size;
console.log("[+] " + UF_MODULE + " base=" + moduleBase + " size=" + hex(moduleSize));

// Resolve exports obrigatórios
const SYM = {};
["il2cpp_domain_get","il2cpp_domain_get_assemblies","il2cpp_assembly_get_image",
 "il2cpp_class_from_name","il2cpp_class_get_fields","il2cpp_field_get_name",
 "il2cpp_field_get_offset","il2cpp_field_get_type","il2cpp_type_get_type",
 "il2cpp_class_get_name","il2cpp_class_from_il2cpp_type","il2cpp_runtime_invoke",
 "il2cpp_object_get_class","il2cpp_class_get_method_from_name",
 "il2cpp_field_static_get_value","il2cpp_class_instance_size",
 "il2cpp_class_get_namespace","il2cpp_method_get_name","il2cpp_class_get_methods",
 "il2cpp_method_get_return_type"].forEach(name => {
    SYM[name] = getExport(name);
});

function nf(name, ret, args) {
    if (!SYM[name]) { console.warn("[!] Export não disponível: " + name); return null; }
    return new NativeFunction(SYM[name], ret, args);
}

const il2cpp_domain_get              = nf("il2cpp_domain_get", "pointer", []);
const il2cpp_domain_get_assemblies   = nf("il2cpp_domain_get_assemblies", "pointer", ["pointer","pointer"]);
const il2cpp_assembly_get_image      = nf("il2cpp_assembly_get_image", "pointer", ["pointer"]);
const il2cpp_class_from_name         = nf("il2cpp_class_from_name", "pointer", ["pointer","pointer","pointer"]);
const il2cpp_class_get_fields        = nf("il2cpp_class_get_fields", "pointer", ["pointer","pointer"]);
const il2cpp_field_get_name          = nf("il2cpp_field_get_name", "pointer", ["pointer"]);
const il2cpp_field_get_offset        = nf("il2cpp_field_get_offset", "int32", ["pointer"]);
const il2cpp_field_get_type          = nf("il2cpp_field_get_type", "pointer", ["pointer"]);
const il2cpp_type_get_type           = nf("il2cpp_type_get_type", "int", ["pointer"]);
const il2cpp_class_get_name          = nf("il2cpp_class_get_name", "pointer", ["pointer"]);
const il2cpp_class_get_namespace     = nf("il2cpp_class_get_namespace", "pointer", ["pointer"]);
const il2cpp_class_from_il2cpp_type  = nf("il2cpp_class_from_il2cpp_type", "pointer", ["pointer"]);
const il2cpp_object_get_class        = nf("il2cpp_object_get_class", "pointer", ["pointer"]);
const il2cpp_class_get_method_from_name = nf("il2cpp_class_get_method_from_name", "pointer", ["pointer","pointer","int"]);
const il2cpp_field_static_get_value  = nf("il2cpp_field_static_get_value", "void", ["pointer","pointer","pointer"]);
const il2cpp_class_instance_size     = nf("il2cpp_class_instance_size", "uint32", ["pointer"]);
const il2cpp_class_get_methods       = nf("il2cpp_class_get_methods", "pointer", ["pointer","pointer"]);
const il2cpp_method_get_name         = nf("il2cpp_method_get_name", "pointer", ["pointer"]);
const il2cpp_method_get_return_type  = nf("il2cpp_method_get_return_type", "pointer", ["pointer"]);

// ─── Enumera campos de uma classe ─────────────────────────────────────────────
function enumFields(klass) {
    const fields = [];
    if (!klass || klass.isNull()) return fields;
    const iterPtr = Memory.alloc(Process.pointerSize);
    iterPtr.writePointer(ptr(0));
    while (true) {
        const fi = il2cpp_class_get_fields(klass, iterPtr);
        if (!fi || fi.isNull()) break;
        const namePtr = il2cpp_field_get_name(fi);
        const name = namePtr && !namePtr.isNull() ? namePtr.readUtf8String() : "???";
        const offset = il2cpp_field_get_offset(fi);
        const ftype = il2cpp_field_get_type ? il2cpp_field_get_type(fi) : ptr(0);
        let fieldClassName = "";
        if (il2cpp_class_from_il2cpp_type && ftype && !ftype.isNull()) {
            try {
                const fklass = il2cpp_class_from_il2cpp_type(ftype);
                if (fklass && !fklass.isNull()) {
                    const cnPtr = il2cpp_class_get_name(fklass);
                    fieldClassName = cnPtr && !cnPtr.isNull() ? cnPtr.readUtf8String() : "";
                }
            } catch(e) {}
        }
        // Beebyte: campos com offset encoding: se A != 0xFFFFFFFF, real = A - B
        // Simplesmente imprimimos o offset raw e o nome ofuscado
        const isStatic = offset >= 0 && offset < 16;
        fields.push({ name, offset, fieldClassName, isStatic, fi });
    }
    return fields;
}

// ─── Enumera métodos de uma classe ────────────────────────────────────────────
function enumMethods(klass) {
    const methods = [];
    if (!klass || klass.isNull() || !il2cpp_class_get_methods) return methods;
    const iterPtr = Memory.alloc(Process.pointerSize);
    iterPtr.writePointer(ptr(0));
    while (true) {
        const mi = il2cpp_class_get_methods(klass, iterPtr);
        if (!mi || mi.isNull()) break;
        let name = "???";
        try {
            const np = il2cpp_method_get_name(mi);
            if (np && !np.isNull()) name = np.readUtf8String();
        } catch(e) {}
        // MethodInfo.method_pointer está no offset 0x20 (Unity 2021 arm64 community default)
        let methodPtr = ptr(0);
        try { methodPtr = mi.add(0x20).readPointer(); } catch(e) {}
        let rva = "0xDEADC0DEDEADC0DE";
        if (!methodPtr.isNull()) {
            const va = methodPtr;
            // Converte VA → RVA se estiver dentro do módulo
            if (va.compare(moduleBase) >= 0 &&
                va.compare(moduleBase.add(moduleSize)) < 0) {
                rva = hex(va.sub(moduleBase).toUInt32());
            }
        }
        methods.push({ name, mi, methodPtr, rva });
    }
    return methods;
}

// ─── Calcula RVA de um klass ptr ──────────────────────────────────────────────
function klassRva(klass) {
    if (!klass || klass.isNull()) return "0xDEADC0DEDEADC0DE";
    const va = klass;
    if (va.compare(moduleBase) >= 0 && va.compare(moduleBase.add(moduleSize)) < 0) {
        return hex(va.sub(moduleBase).toUInt32());
    }
    return hex(va.toUInt32()); // fora do módulo (improvável para klass*)
}

// ─── Encontra classes via IL2CPP API ─────────────────────────────────────────
setTimeout(function main() {
    console.log("[+] Iniciando descoberta de offsets (aguardando runtime)...");

    const domain = il2cpp_domain_get();
    if (!domain || domain.isNull()) {
        console.error("[!] IL2CPP domain NULL — runtime não pronto");
        return;
    }

    // Carrega assemblies
    const szPtr = Memory.alloc(8);
    szPtr.writeU64(0);
    const assemblies = il2cpp_domain_get_assemblies(domain, szPtr);
    const asmCount = szPtr.readU64().toNumber();
    console.log("[+] " + asmCount + " assemblies carregados");

    // Encontra imagens
    const images = [];
    for (let i = 0; i < asmCount; i++) {
        const asm = assemblies.add(i * Process.pointerSize).readPointer();
        if (asm.isNull()) continue;
        const img = il2cpp_assembly_get_image(asm);
        if (!img.isNull()) images.push(img);
    }
    console.log("[+] " + images.length + " imagens disponíveis");

    // Função helper: encontra classe por namespace + nome
    function findClass(ns, name) {
        const nsStr = Memory.allocUtf8String(ns);
        const nmStr = Memory.allocUtf8String(name);
        for (const img of images) {
            const klass = il2cpp_class_from_name(img, nsStr, nmStr);
            if (!klass.isNull()) return klass;
        }
        return ptr(0);
    }

    // ─── Encontra classes alvo ────────────────────────────────────────────────
    console.log("\n[+] Resolvendo classes...");

    const klassPlayer    = findClass(NS_GAMEPLAY, CLS_PLAYER);
    const klassAvatarMgr = findClass(NS_GAMEPLAY, CLS_AVATAR_MG);
    const klassPhysCCT   = findClass(NS_GAMEPLAY, CLS_PHYSCT);
    const klassGunCtrl   = findClass(NS_GAMEPLAY, CLS_GUN_CTRL);
    const klassPlayerData= findClass(NS_COW, CLS_PLAYERDATA);

    // Classe Camera do Unity Engine
    const klassCamera    = findClass("UnityEngine", "Camera");

    if (klassPlayer.isNull())    console.warn("[!] Player class não encontrado");
    else {
        console.log("[+] Player klass = " + klassPlayer + " → RVA=" + klassRva(klassPlayer));
        result.ga.klass_PlayerInfo = klassRva(klassPlayer);
    }
    if (klassGunCtrl.isNull())   console.warn("[!] VerticleGunController não encontrado");
    else {
        console.log("[+] VerticleGunController klass = " + klassGunCtrl);
        result.ga.klass_WeaponController = klassRva(klassGunCtrl);
    }

    // ─── Enumera campos de Player ─────────────────────────────────────────────
    if (!klassPlayer.isNull()) {
        console.log("\n[+] Campos de Player (COW.GamePlay):");
        const fields = enumFields(klassPlayer);
        const instSize = il2cpp_class_instance_size ? il2cpp_class_instance_size(klassPlayer) : 0;
        console.log("    instance_size = 0x" + instSize.toString(16));

        fields.forEach(f => {
            const tag = f.isStatic ? "(static)" : "";
            console.log("    0x" + f.offset.toString(16).padStart(4,"0") + " " +
                        tag.padEnd(8) + " " + f.name +
                        (f.fieldClassName ? (" : " + f.fieldClassName) : ""));
        });

        // Heurística de mapeamento por tipo de campo:
        //   float → candidato a health/armor/speed
        //   int32 → candidato a team_id/ammo
        //   bool  → candidato a is_dead/is_local
        //   class Transform → transform ptr
        //   string/Il2CppString → name
        fields.forEach(f => {
            if (f.isStatic) return;
            switch(f.fieldClassName) {
                case "Transform":
                    console.log("    *** PlayerInfo_transform = 0x" + f.offset.toString(16) + " ***");
                    result.st.PlayerInfo_transform = hex(f.offset);
                    break;
                case "String":
                    // Pode ser o nome do jogador — depende do nome do campo
                    // Com Beebyte os nomes são ofuscados mas registramos mesmo assim
                    console.log("    *** String field @ 0x" + f.offset.toString(16) +
                                " (candidato a name) name_raw=" + f.name + " ***");
                    break;
            }
            // Tipos primitivos — só candidatos
            if (f.fieldClassName === "Single" || f.fieldClassName === "float") {
                console.log("    float @ 0x" + f.offset.toString(16) +
                            " field=" + f.name + " (candidato hp/armor/speed)");
            }
            if (f.fieldClassName === "Int32" || f.fieldClassName === "int") {
                console.log("    int32 @ 0x" + f.offset.toString(16) +
                            " field=" + f.name + " (candidato team_id/ammo)");
            }
            if (f.fieldClassName === "Boolean" || f.fieldClassName === "bool") {
                console.log("    bool  @ 0x" + f.offset.toString(16) +
                            " field=" + f.name + " (candidato is_dead/is_local)");
            }
        });

        // Métodos de Player — procura Update/LateUpdate para fn_GameUpdate
        console.log("\n[+] Métodos de Player:");
        const methods = enumMethods(klassPlayer);
        methods.forEach(m => {
            const low = m.name.toLowerCase();
            if (low.includes("update") || low.includes("tick") || low.includes("refresh")) {
                console.log("    " + m.name + " → RVA=" + m.rva + " ptr=" + m.methodPtr);
                if (low.includes("lateupdate")) {
                    result.ga.fn_GameUpdate = m.rva;
                    console.log("    *** fn_GameUpdate (LateUpdate) = " + m.rva + " ***");
                }
            }
            if (low.includes("getlocalplayer") || low.includes("getself") ||
                low.includes("get_local") || low.includes("getmain")) {
                console.log("    *** fn_GetLocalPlayer candidate: " + m.name + " → " + m.rva + " ***");
                result.ga.fn_GetLocalPlayer = m.rva;
            }
            if (low.includes("getallplayers") || low.includes("getplayers")) {
                console.log("    *** fn_GetAllPlayers candidate: " + m.name + " → " + m.rva + " ***");
                result.ga.fn_GetAllPlayers = m.rva;
            }
        });
    }

    // ─── Enumera campos de AvatarManager ─────────────────────────────────────
    if (!klassAvatarMgr.isNull()) {
        console.log("\n[+] Campos de AvatarManager (COW.GamePlay):");
        const fields = enumFields(klassAvatarMgr);
        fields.forEach(f => {
            console.log("    0x" + f.offset.toString(16).padStart(4,"0") + " " +
                        (f.isStatic ? "(s)" : "   ") + " " + f.name +
                        (f.fieldClassName ? (" : " + f.fieldClassName) : ""));
        });

        // Métodos — procura GetLocalPlayer/GetAllPlayers
        const methods = enumMethods(klassAvatarMgr);
        console.log("[+] Métodos de AvatarManager:");
        methods.forEach(m => {
            const low = m.name.toLowerCase();
            if (low.includes("local") || low.includes("getplayer") ||
                low.includes("allplayer") || low.includes("getself")) {
                console.log("    *** " + m.name + " → RVA=" + m.rva + " ***");
                if (result.ga.fn_GetLocalPlayer === "0xDEADC0DEDEADC0DE") {
                    result.ga.fn_GetLocalPlayer = m.rva;
                }
                if ((low.includes("all") || low.includes("list")) &&
                    result.ga.fn_GetAllPlayers === "0xDEADC0DEDEADC0DE") {
                    result.ga.fn_GetAllPlayers = m.rva;
                }
            }
            if (low.includes("update") || low.includes("lateupdate")) {
                console.log("    Update/Late: " + m.name + " → RVA=" + m.rva);
            }
        });
    }

    // ─── Campos de VerticleGunController (WeaponController) ──────────────────
    if (!klassGunCtrl.isNull()) {
        console.log("\n[+] Campos de VerticleGunController:");
        const fields = enumFields(klassGunCtrl);
        fields.forEach(f => {
            const low = f.name.toLowerCase();
            const tag = f.isStatic ? "(s)" : "   ";
            console.log("    0x" + f.offset.toString(16).padStart(4,"0") + " " +
                        tag + " " + f.name + (f.fieldClassName ? " : "+f.fieldClassName : ""));
            // Int campos candidatos a ammo
            if (!f.isStatic && (f.fieldClassName === "Int32" || f.fieldClassName === "int")) {
                if (low.includes("ammo") || low.includes("bullet") || low.includes("mag")) {
                    console.log("    *** ammo candidate @ 0x" + f.offset.toString(16) + " ***");
                }
            }
        });
    }

    // ─── Campos de PhysicalCCT (CharController com moveSpeed) ────────────────
    if (!klassPhysCCT.isNull()) {
        console.log("\n[+] Campos de PhysicalCCT:");
        const fields = enumFields(klassPhysCCT);
        fields.forEach(f => {
            const low = f.name.toLowerCase();
            if (f.fieldClassName === "Single" || f.fieldClassName === "float") {
                if (low.includes("speed") || low.includes("move") || low.includes("walk")) {
                    console.log("    *** moveSpeed candidate @ 0x" + f.offset.toString(16) +
                                " field=" + f.name + " ***");
                    result.st.CharController_moveSpeed = hex(f.offset);
                }
            }
            const tag = f.isStatic ? "(s)" : "   ";
            console.log("    0x" + f.offset.toString(16).padStart(4,"0") + " " +
                        tag + " " + f.name + (f.fieldClassName ? " : "+f.fieldClassName : ""));
        });
    }

    // ─── Campos de Camera ─────────────────────────────────────────────────────
    if (klassCamera && !klassCamera.isNull()) {
        console.log("\n[+] Campos de UnityEngine.Camera:");
        const fields = enumFields(klassCamera);
        fields.forEach(f => {
            const low = f.name.toLowerCase();
            if (low.includes("proj") || low.includes("view") || low.includes("matrix")) {
                console.log("    *** camera matrix @ 0x" + f.offset.toString(16) +
                            " field=" + f.name + " ***");
            }
        });
        const methods = enumMethods(klassCamera);
        methods.forEach(m => {
            const low = m.name.toLowerCase();
            if (low.includes("worldtoscreen") || low.includes("world_to_screen")) {
                console.log("    *** WorldToScreenPoint → RVA=" + m.rva + " ***");
                result.ga.fn_WorldToScreen = m.rva;
            }
        });
    }

    // ─── Intercepta il2cpp_runtime_invoke para sondagem em runtime ────────────
    // Quando uma instância Player aparecer, fazemos sondagem dos campos
    // para confirmar os offsets por valor (health, team_id, etc.)
    let playerCaptured = false;
    let playerInst = ptr(0);

    if (SYM["il2cpp_runtime_invoke"] && !klassPlayer.isNull()) {
        console.log("\n[+] Interceptando runtime_invoke para capturar instância Player...");

        Interceptor.attach(SYM["il2cpp_runtime_invoke"], {
            onLeave: function(retval) {
                if (playerCaptured || !retval || retval.isNull()) return;
                try {
                    if (!il2cpp_object_get_class) return;
                    const klass = il2cpp_object_get_class(retval);
                    if (!klass.equals(klassPlayer)) return;
                    playerCaptured = true;
                    playerInst = retval;
                    console.log("\n[!!!] Instância Player capturada: " + playerInst);
                    probePlayerInstance(playerInst);
                } catch(e) {}
            }
        });
    }

    // ─── Intercepta MonoBehaviour.LateUpdate / Update para fn_GameUpdate ──────
    // Estratégia: hook em il2cpp_class_get_method_from_name quando busca "LateUpdate"
    // → o endereço retornado é o MethodInfo → method_pointer é o RVA que queremos.
    if (SYM["il2cpp_class_get_method_from_name"]) {
        Interceptor.attach(SYM["il2cpp_class_get_method_from_name"], {
            onEnter: function(args) {
                try {
                    const namePtr = args[1];
                    if (!namePtr || namePtr.isNull()) return;
                    const name = namePtr.readUtf8String();
                    this._name = name;
                    this._klass = args[0];
                } catch(e) {}
            },
            onLeave: function(retval) {
                if (!this._name) return;
                const low = this._name.toLowerCase();
                if (low === "lateupdate" || low === "update" || low === "tick") {
                    if (!retval || retval.isNull()) return;
                    try {
                        const methodPtr = retval.add(0x20).readPointer();
                        if (methodPtr.isNull()) return;
                        const va = methodPtr;
                        if (va.compare(moduleBase) >= 0 &&
                            va.compare(moduleBase.add(moduleSize)) < 0) {
                            const rva = hex(va.sub(moduleBase).toUInt32());
                            console.log("[hook] " + this._name + " RVA=" + rva +
                                        " klass=" + this._klass);
                            if (low === "lateupdate" &&
                                result.ga.fn_GameUpdate === "0xDEADC0DEDEADC0DE") {
                                result.ga.fn_GameUpdate = rva;
                                console.log("    *** fn_GameUpdate = " + rva + " ***");
                            }
                        }
                    } catch(e) {}
                }
            }
        });
    }

    console.log("\n[+] Setup completo. Entre em uma partida para capturar instâncias.");
    console.log("[+] JSON final será emitido após captura ou via rpc.exports.emit()");

    // ─── RPC exports ──────────────────────────────────────────────────────────
    rpc.exports = {
        // Emite o JSON com o que foi descoberto até agora
        emit: function() {
            const json = JSON.stringify(result, null, 2);
            console.log("\n=== RESULTADO FINAL (cole em offsets_E50B0E3A-....json) ===\n" + json);
            return json;
        },
        // Probe manual com Player* já conhecido
        probe: function(addrStr) {
            const obj = ptr(addrStr);
            console.log("[RPC] probe(" + obj + ")");
            probePlayerInstance(obj);
            return "done";
        },
        // Lê um campo float/int/bool de um objeto
        readField: function(addrStr, offHex, type) {
            const base = ptr(addrStr);
            const off  = parseInt(offHex, 16);
            if (type === "f32") return base.add(off).readFloat();
            if (type === "i32") return base.add(off).readS32();
            if (type === "u8")  return base.add(off).readU8();
            if (type === "ptr") return base.add(off).readPointer().toString();
            return null;
        }
    };

    // Emite automaticamente após 60s mesmo sem captura
    setTimeout(function() {
        console.log("\n[+] Timeout — emitindo resultado parcial:");
        const json = JSON.stringify(result, null, 2);
        console.log(json);
    }, 60000);

}, 4000);

// ─── Sondagem de instância Player em runtime ──────────────────────────────────
// Lê campos a partir de offsets dos campos já conhecidos + tenta floats/ints
// em toda a instância para identificar health, team_id, etc.
function probePlayerInstance(obj) {
    console.log("\n--- Sondagem de Player @ " + obj + " ---");

    const instSize = 0x200; // lê até 0x200 bytes = tamanho razoável de instância

    // Imprime todos os int32 e float com valores plausíveis
    console.log("\n  Floats plausíveis como HP/Speed (0.1 – 10000):");
    for (let off = 0x10; off < instSize; off += 4) {
        const f = readF32(obj, off);
        if (!isNaN(f) && isFinite(f) && f > 0.1 && f < 10000 && f !== 1.0) {
            console.log("    0x" + off.toString(16).padStart(3,"0") + " f32=" + f.toFixed(3));
        }
    }

    console.log("\n  Ints plausíveis como team_id (0-32):");
    for (let off = 0x10; off < instSize; off += 4) {
        const v = readI32(obj, off);
        if (v >= 0 && v <= 32 && v !== 0x7fffffff) {
            console.log("    0x" + off.toString(16).padStart(3,"0") + " i32=" + v);
        }
    }

    console.log("\n  Bools (0x00 ou 0x01) candidatos a is_dead/is_local:");
    for (let off = 0x10; off < instSize; off += 1) {
        const b = readU8(obj, off);
        if (b === 0 || b === 1) {
            // Só imprime se os vizinhos também forem 0/1 ou zero
            const prev = readU8(obj, off - 1);
            const next = readU8(obj, off + 1);
            if (prev <= 1 && next <= 1) {
                // Muitos falsos positivos — só imprime se o byte anterior é ponteiro-alinhado
            }
        }
    }

    // Ponteiros plausíveis (candidatos a transform, weapon_controller)
    console.log("\n  Ponteiros plausíveis (dentro de UnityFramework heap):");
    const heapLow  = ptr("0x100000000"); // iOS arm64 heap típico
    const heapHigh = ptr("0x200000000");
    for (let off = 0x10; off < instSize; off += 8) {
        const p = readPtr(obj, off);
        if (!p.isNull() && p.compare(heapLow) >= 0 && p.compare(heapHigh) < 0) {
            // Lê o klass ptr do objeto apontado
            const klass = readPtr(p, 0);
            let klassName = "";
            if (!klass.isNull() && il2cpp_class_get_name) {
                try {
                    const cnPtr = il2cpp_class_get_name(klass);
                    if (cnPtr && !cnPtr.isNull()) klassName = cnPtr.readUtf8String();
                } catch(e) {}
            }
            if (klassName) {
                console.log("    0x" + off.toString(16).padStart(3,"0") +
                            " ptr=" + p + " → " + klassName);
                if (klassName === "Transform") {
                    console.log("    *** PlayerInfo_transform = 0x" + off.toString(16) + " ***");
                    result.st.PlayerInfo_transform = hex(off);
                }
                if (klassName.includes("GunController") || klassName.includes("Weapon")) {
                    console.log("    *** PlayerInfo_weapon_controller = 0x" + off.toString(16) + " ***");
                    result.st.PlayerInfo_weapon_controller = hex(off);
                }
            }
        }
    }

    // Emite resultado parcial já com instância
    setTimeout(function() {
        console.log("\n=== Resultado parcial (captura Player em runtime) ===");
        console.log(JSON.stringify(result, null, 2));
    }, 2000);
}
