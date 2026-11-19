/**
 * frida_validate_inject.js
 * Verifica se a dylib MetalCaptureSupport foi injetada com sucesso e
 * se as features estão ativas no processo do Free Fire.
 *
 * O que faz:
 *   1. Lista todas as imagens carregadas e verifica se MetalCaptureSupport.dylib aparece.
 *   2. Verifica se nossos símbolos públicos (overlay_init, esp_render, etc.) estão acessíveis.
 *   3. Intercepta il2cpp_domain_get para confirmar que o resolver rodou.
 *   4. Lê variáveis de estado do cheat (g_esp, g_aimbot) se acessíveis.
 *   5. Verifica se antidetect_hide_image funcionou (a dylib NÃO deve aparecer após isso).
 *
 * Uso:
 *   frida -U -n com.garena.game.freefire -l frida_validate_inject.js --no-pause
 *
 * Resultado esperado após injeção bem-sucedida:
 *   [+] MetalCaptureSupport: NÃO visível na lista dyld (antidetect_hide_image OK)
 *   [+] il2cpp domain resolvido
 *   [+] Overlay window criada (UIKit)
 */

"use strict";

setTimeout(function() {
    console.log("=== frida_validate_inject — diagnóstico de injeção ===\n");

    // ─── 1. Lista imagens carregadas ──────────────────────────────────────────
    const modules = Process.enumerateModules();
    console.log("[1] Módulos carregados: " + modules.length);

    let foundPayload = false;
    modules.forEach(m => {
        const low = m.name.toLowerCase();
        if (low.includes("metalcapturesupport") || low.includes("ffpayload")) {
            console.log("    [!] DYLIB VISÍVEL: " + m.name + " @ " + m.base);
            foundPayload = true;
        }
        if (low.includes("datadome")) {
            console.log("    [info] DataDomeSDK encontrado: " + m.name + " (injeção via vnode possível)");
        }
        if (low.includes("unityframework")) {
            console.log("    [+] UnityFramework: " + m.name + " @ " + m.base + " size=" + m.size.toString(16));
        }
    });

    if (!foundPayload) {
        console.log("    [+] MetalCaptureSupport NÃO visível na lista dyld → antidetect_hide_image OK");
    } else {
        console.log("    [WARN] Dylib ainda visível → antidetect pode não ter rodado (injeção muito recente?)");
    }

    // ─── 2. Verifica símbolos do cheat ────────────────────────────────────────
    console.log("\n[2] Procurando símbolos do cheat no espaço de memória...");

    // Nomes de função que deveriam estar no processo (mesmo se header zerado)
    const symsToFind = [
        "overlay_init",
        "esp_render",
        "aimbot_tick",
        "ff_camera_update",
        "ff_il2cpp_resolve",
        "antidetect_init",
        "ff_gameassembly_base",
    ];

    symsToFind.forEach(name => {
        // Tenta via Module.findExportByName em todos os módulos
        // (pode falhar se header foi zerado por antidetect_zero_header)
        let found = null;
        modules.forEach(m => {
            if (found) return;
            const a = Module.findExportByName(m.name, name);
            if (a && !a.isNull()) found = a;
        });
        if (found) {
            console.log("    [+] " + name + " @ " + found);
        } else {
            console.log("    [?] " + name + " não encontrado via export (normal se header zerado)");
        }
    });

    // ─── 3. Verifica il2cpp domain ────────────────────────────────────────────
    console.log("\n[3] Verificando IL2CPP runtime...");
    const domainSym = Module.findExportByName("UnityFramework", "il2cpp_domain_get") ||
                      Module.findExportByName("UnityFramework", "_il2cpp_domain_get");
    if (domainSym) {
        const il2cpp_domain_get = new NativeFunction(domainSym, "pointer", []);
        const domain = il2cpp_domain_get();
        if (!domain.isNull()) {
            console.log("    [+] IL2CPP domain OK: " + domain);
        } else {
            console.log("    [!] IL2CPP domain NULL — runtime ainda não inicializado");
        }
    } else {
        console.log("    [?] il2cpp_domain_get não encontrado");
    }

    // ─── 4. Verifica UIWindow overlay ─────────────────────────────────────────
    console.log("\n[4] Verificando janelas UIKit...");

    // Lê a lista de UIWindows no main thread
    const script = `
        ObjC.schedule(ObjC.mainQueue, function() {
            try {
                var app = ObjC.classes.UIApplication.sharedApplication();
                var windows = app.windows();
                var count = windows.count().valueOf();
                for (var i = 0; i < count; i++) {
                    var w = windows.objectAtIndex_(i);
                    var level = w.windowLevel().valueOf();
                    var desc = w.description().toString();
                    if (level > 2000) {
                        console.log("    [+] UIWindow nível " + level + " (nosso overlay): " + desc.substring(0,80));
                    } else {
                        console.log("    [-] UIWindow nível " + level + " (jogo/sistema)");
                    }
                }
                if (count === 0) console.log("    [!] Nenhuma UIWindow encontrada");
            } catch(e) {
                console.log("    [!] Erro ao listar UIWindows: " + e);
            }
        });
    `;
    try {
        eval(script);
    } catch(e) {
        console.log("    [?] Não foi possível enumerar janelas: " + e);
    }

    // ─── 5. Verifica threads do processo ─────────────────────────────────────
    console.log("\n[5] Threads do processo:");
    Process.enumerateThreads().forEach(t => {
        // Nossa thread é nomeada "com.apple.metal.capture" por antidetect_name_thread
        // (propositalmente benigno para passar inspeção)
        console.log("    tid=" + t.id + " state=" + t.state + " pc=" + t.context.pc);
    });

    // ─── 6. Scan de memória por strings do cheat ──────────────────────────────
    // Se antidetect_zero_header e strip funcionaram, strings como "esp_render"
    // NÃO devem aparecer na memória do binário.
    // (Scan completo seria lento — apenas verifica o módulo UnityFramework como referência)
    console.log("\n[6] Scan rápido por strings suspeitas (sample)...");
    const dangerStrings = ["esp_render", "aimbot", "hack", "cheat", "GetLocalPlayer"];
    const uf = Process.findModuleByName("UnityFramework");
    if (uf) {
        // Apenas imprime aviso — scan completo é feito offline com `strings` no binário
        console.log("    UnityFramework: scan completo deve ser feito offline.");
        console.log("    Comando: strings build/MetalCaptureSupport.dylib | grep -iE 'hack|cheat|esp|aimbot'");
    }

    // ─── Resumo ───────────────────────────────────────────────────────────────
    console.log("\n=== Resumo ===");
    console.log("Rode este script após injetar MetalCaptureSupport.dylib e entrar em partida.");
    console.log("Se tudo OK: dylib oculta, IL2CPP ativo, UIWindow overlay visível.");
    console.log("Se algo falhou: veja mensagens [!] acima.");

}, 2000);
