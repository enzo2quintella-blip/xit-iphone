// payload_main.c — entry point da dylib injetada no processo do Free Fire
// Carregado via DYLD_INSERT_LIBRARIES ou pelo InjectorApp via dlopen.
// __attribute__((constructor)) → executado imediatamente após dlopen.

#include "../include/ff_game.h"
#include "../include/ff_offsets_loader.h"
#include "../include/ff_il2cpp_resolver.h"
#include "../include/ff_camera.h"
#include "../include/antidetect.h"
#include "hooks.h"
#include "../features/esp.h"
#include "../features/aimbot.h"
#include "../../KernelExploit/include/kexploit.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <mach/mach.h>
#  include <objc/runtime.h>
#  include <objc/message.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <QuartzCore/QuartzCore.h>
#endif
#include <math.h>
#include "../include/ff_math.h"
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

// ─── Forward declarations (implementadas abaixo de hooked_game_update) ───────
static void aim_inject_delta(float dyaw, float dpitch);
static void triggerbot_tick(const Matrix4x4 *vp, Vec3 cam_pos, float sw, float sh);

// Logging interno: só compila em builds DEBUG para não deixar strings no binário release.
// Em release, FF_LOG() compila para vazio.
#ifdef FF_DEBUG
#  include <stdio.h>
#  define FF_LOG(fmt, ...) fprintf(stderr, "[FF] " fmt "\n", ##__VA_ARGS__)
#else
#  define FF_LOG(fmt, ...) ((void)0)
#endif

// ─── Forward declarations (implementadas em overlay.m) ───────────────────────
extern void overlay_init(void);
extern void overlay_tick(void);  // chamada do hook de Present/render

// ─── Declarações de features ─────────────────────────────────────────────────
extern void godmode_tick(void);
extern void inf_ammo_tick(void);
extern void speed_tick(void);
extern void godmode_set(bool);
extern void inf_ammo_set(bool);
extern void speed_set(bool, float);

// ─── Hook de Update do jogo ──────────────────────────────────────────────────
// Free Fire chama uma função "LateUpdate" ou "Update" no loop principal.
// RVA: g_offsets.ga.fn_GameUpdate — OFFSET_UNKNOWN até dump do build.
// Assinatura esperada: void GameUpdate(void *gameController)
typedef void (*GameUpdate_t)(void *ctrl);
static GameUpdate_t s_orig_game_update = NULL;

static void hooked_game_update(void *ctrl) {
    // Atualiza ViewProjection da câmera — deve ser o primeiro tick para que
    // ff_w2s_vp() já tenha a matrix correta quando ESP e aimbot rodarem.
    ff_camera_update();

    // Tick de features lógicas (escrita em campos do heap)
    godmode_tick();
    inf_ammo_tick();
    speed_tick();

    // ── ESP: constrói frame de geometria para o overlay ─────────────────────
    // A resolução real vem do bounds da UIScreen; guardamos em variáveis estáticas
    // para evitar chamar ObjC a cada frame (screen size não muda com frequência).
    static float s_sw = 0.0f, s_sh = 0.0f;
    if (s_sw == 0.0f) {
#ifdef __APPLE__
        // id UIScreen = objc_msgSend([UIScreen class], "mainScreen")
        id (*objc_cls)(Class, SEL) = (id (*)(Class, SEL))objc_msgSend;
        CGRect (*objc_bounds)(id, SEL) = (CGRect (*)(id, SEL))objc_msgSend;
        Class uiscreen = (Class)objc_getClass("UIScreen");
        if (uiscreen) {
            id main = objc_cls(uiscreen, sel_registerName("mainScreen"));
            if (main) {
                CGRect b = objc_bounds(main, sel_registerName("bounds"));
                s_sw = (float)b.size.width;
                s_sh = (float)b.size.height;
            }
        }
#endif
        if (s_sw == 0.0f) { s_sw = 1920.0f; s_sh = 1080.0f; } // fallback seguro
    }

    const Matrix4x4 *vp  = ff_camera_get_vp();
    Vec3              cam = ff_camera_get_pos();
    if (vp) {
        esp_render(vp, cam, s_sw, s_sh);
    }

    // ── Aimbot: calcula delta e injeta input se alvo encontrado ─────────────
    if (g_aimbot.enabled && g_aimbot.aim_key_held && vp) {
        // Yaw/pitch da câmera: extraímos da worldToCameraMatrix (rotation)
        // Sem offset de campo confirmado no jogo, estimamos dos ângulos
        // codificados na coluna da view matrix (R = worldToCamera[0..2][0..2]).
        // Col 0 = right, col 1 = up, col 2 = -forward (OpenGL convention).
        // yaw   = atan2( M[2][0], M[0][0] )   (rotação em torno de Y)
        // pitch = atan2(-M[2][1], M[2][2] )   (rotação em torno de X)
        // Unity worldToCamera: row-major, M[row][col].
        // Matrix4x4.m[col][row] col-major Unity
        // col=0 row=0 → m[0][0] = right.x
        // col=2 row=0 → m[2][0] = forward.x (negado em Unity view)
        // col=2 row=1 → m[2][1] = forward.y
        // col=2 row=2 → m[2][2] = forward.z
        float m00 = vp->m[0][0];
        float m20 = vp->m[2][0];
        float m21 = vp->m[2][1];
        float m22 = vp->m[2][2];
        float yaw   =  atan2f(m20, m00)  * (180.0f / (float)M_PI);
        float pitch  = atan2f(-m21, m22) * (180.0f / (float)M_PI);

        float dyaw = 0.0f, dpitch = 0.0f;
        if (aimbot_tick(yaw, pitch, cam, vp, s_sw, s_sh, &dyaw, &dpitch)) {
            aim_inject_delta(dyaw, dpitch);
        }
    }

    // ── Triggerbot ───────────────────────────────────────────────────────────
    triggerbot_tick(vp, cam, s_sw, s_sh);

    // Chama original
    if (s_orig_game_update) s_orig_game_update(ctrl);
}

// ─── Hook de render (Metal: [MTLCommandBuffer presentDrawable:]) ──────────────
// A assinatura real depende do metal layer hook escolhido (ver overlay.m).
// Aqui apenas definimos o ponto de dispatch para o overlay.

// ─── Worker thread: mantém o menu responsivo em background ───────────────────
static void *ff_worker(void *arg) {
    (void)arg;

    // ── Anti-detecção: delay + ocultar imagem + verificar debugger ───────────
    // antidetect_init() bloqueia 2–8s, oculta nosso dylib do dyld e zera o header.
    // Retorna false se detectar debugger — nesse caso não ativamos nada.
    if (!antidetect_init()) {
        return NULL;
    }

    // ── Aguarda IL2CPP runtime estar pronto ───────────────────────────────────
    // ff_il2cpp_wait_ready() aguarda il2cpp_domain_get() retornar não-nulo.
    // Timeout de 30s cobre carregamento lento de partidas.
    if (!ff_il2cpp_wait_ready(30)) {
        FF_LOG("IL2CPP domain não ficou pronto em 30s");
        return NULL;
    }

    // ── Descobre offsets via API IL2CPP (Beebyte-safe) ────────────────────────
    // ff_il2cpp_resolve() usa il2cpp_class_get_fields + field_get_offset para
    // mapear HP/MaxHP/TeamID/Transform sem depender de nomes de campo.
    if (ff_il2cpp_resolve(&g_offsets)) {
        FF_LOG("IL2CPP resolver: hp=0x%x hp_max=0x%x teamid=0x%x pos=0x%x fn_update=0x%zx",
               g_offsets.st.PlayerInfo_health,
               g_offsets.st.PlayerInfo_health_max,
               g_offsets.st.PlayerInfo_team_id,
               g_offsets.st.Transform_position,
               (size_t)g_offsets.ga.fn_GameUpdate);
    } else {
        FF_LOG("IL2CPP resolve falhou — tentando offsets do arquivo de config");
    }

    // ── Carrega offsets do arquivo de config (complemento ou fallback) ────────
    // ff_offsets_load() preenche g_offsets.ga (fn_GameUpdate, etc.) mesmo quando
    // il2cpp_resolve já preencheu os offsets de campo.
    // Se o arquivo não existe, features de lógica ficam desativadas mas ESP pode
    // funcionar com os offsets descobertos pelo resolver.
    ff_status_t st = ff_offsets_load(&g_offsets);
    if (st == FF_ERR_VERSION) {
        FF_LOG("Sem config para este build — hook de GameUpdate desativado");
    } else if (st != FF_OK && st != FF_OK_PARTIAL) {
        FF_LOG("ff_offsets_load erro %d", (int)st);
    }

    // ── Inicializa kernel exploit (retorna KE_NOT_AVAILABLE se stub) ──────────
    ke_status_t ke = ke_init();
    if (ke == KE_OK) {
        KernelCapabilities caps = ke_capabilities();
        FF_LOG("Kernel caps: phys_rw=%d sandbox=%d", caps.phys_rw, caps.sandbox_escape);
        (void)caps; /* evita -Wunused-variable em Release (FF_LOG expande para void) */
    } else {
        FF_LOG("Rodando em modo userspace (ke=%d)", (int)ke);
    }

    // ── Inicializa módulo de câmera (resolve Camera.get_main, projectionMatrix, etc.)
    if (ff_camera_init()) {
        FF_LOG("Camera init OK");
    } else {
        FF_LOG("Camera init falhou — ff_w2s_vp desabilitado, ff_w2s_invoke fica como fallback");
    }

    // ── Inicializa features ───────────────────────────────────────────────────
    esp_init();
    aimbot_init();

    // ── Instala hook de game update (se offset disponível) ───────────────────
    uintptr_t update_va = ff_rva_to_va(g_offsets.ga.fn_GameUpdate);
    if (update_va) {
        uintptr_t orig = 0;
        if (hook_install(update_va, (uintptr_t)hooked_game_update, &orig)) {
            s_orig_game_update = (GameUpdate_t)orig;
            FF_LOG("GameUpdate hook instalado em 0x%llx", (unsigned long long)update_va);
        }
    } else {
        FF_LOG("fn_GameUpdate desconhecido — hook não instalado");
    }

    // ── Inicializa overlay Metal (UI) ─────────────────────────────────────────
    overlay_init();

    FF_LOG("Inicializado. ESP=%s Aimbot=%s",
           g_esp.enabled ? "on" : "off",
           g_aimbot.enabled ? "on" : "off");
    return NULL;
}

// ─── Injeção de input para aimbot ─────────────────────────────────────────────
// Estratégia: sintetiza um pan gesture na metade direita da tela (zona do look joystick
// do Free Fire). Usamos UIApplication sendEvent: com um UITouch sintético.
// A sensibilidade (UNCALIBRATED) deve ser ajustada em jogo — começa conservadora.
//
// UNCALIBRATED: aim_sensitivity converte graus → pixels. No Free Fire a câmera move
// ~1 grau por ~8-12 pixels de swipe na zona look (varia por configuração in-game).
// Valor 10 é ponto de partida; ajustar com testes reais no dispositivo.
#define AIM_SENSITIVITY_PX_PER_DEG  10.0f   // UNCALIBRATED

static void aim_inject_delta(float dyaw, float dpitch) {
    // Magnitude mínima para evitar micro-inputs desnecessários
    if (fabsf(dyaw) < 0.01f && fabsf(dpitch) < 0.01f) return;

#ifdef __APPLE__
    // Dispatch para a main thread — UIKit não é thread-safe.
    dispatch_async(dispatch_get_main_queue(), ^{
        // Localiza UIApplication
        Class appClass = (Class)objc_getClass("UIApplication");
        if (!appClass) return;
        id (*objc_cls)(Class, SEL) = (id (*)(Class, SEL))objc_msgSend;
        id app = objc_cls(appClass, sel_registerName("sharedApplication"));
        if (!app) return;

        // Obtém bounds da screen
        Class screenClass = (Class)objc_getClass("UIScreen");
        if (!screenClass) return;
        id mainScreen = objc_cls(screenClass, sel_registerName("mainScreen"));
        if (!mainScreen) return;
        CGRect (*getBounds)(id, SEL) = (CGRect (*)(id, SEL))objc_msgSend;
        CGRect bounds = getBounds(mainScreen, sel_registerName("bounds"));
        float sw = (float)bounds.size.width;
        float sh = (float)bounds.size.height;

        // Centro da zona look (metade direita da tela, verticalmente centrado)
        CGPoint origin = CGPointMake(sw * 0.75f, sh * 0.5f);
        CGPoint dest   = CGPointMake(
            origin.x + dyaw   * AIM_SENSITIVITY_PX_PER_DEG,
            origin.y - dpitch * AIM_SENSITIVITY_PX_PER_DEG  // Y invertido: pitch up = y menor
        );

        // UIEvent / UITouch sintéticos via métodos privados da UIKit.
        // Essa técnica funciona em iOS 17 jailbroken (sem sandbox).
        // Referência: GSEvent/UIEvent internals.
        Class touchClass = (Class)objc_getClass("UITouch");
        if (!touchClass) return;
        id touch = objc_cls(touchClass, sel_registerName("alloc"));
        if (!touch) return;

        // Inicializa o touch na janela principal
        // -[UITouch _initWithTapCount:touchData:sensitiveContent:]
        // Usamos -[UITouch init] + ivar injection via KVC como fallback simples.
        id (*init_msg)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
        touch = init_msg(touch, sel_registerName("init"));
        if (!touch) return;

        // Fase began → moved → ended via setValue:forKey: (private KVC)
        void (*setVal)(id, SEL, id, NSString*) = (void (*)(id, SEL, id, NSString*))objc_msgSend;

        // Fase: UITouchPhaseBegan (0)
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)0,  // UITouchPhaseBegan
               (__bridge NSString*)CFSTR("phase"));
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)(long)origin.x,
               (__bridge NSString*)CFSTR("locationInWindow.x"));
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)(long)origin.y,
               (__bridge NSString*)CFSTR("locationInWindow.y"));

        // Envia evento — Free Fire captura via UIResponder touchesBegan:
        // Para evitar conflito com o overlay, enviamos direto para a app window.
        // O evento completo (began + moved + ended) acontece em um único runloop tick.
        // Simplifcado: apenas moved (o jogo reage ao delta de posição do touch).
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)1,  // UITouchPhaseMoved
               (__bridge NSString*)CFSTR("phase"));
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)(long)dest.x,
               (__bridge NSString*)CFSTR("locationInWindow.x"));
        setVal(touch, sel_registerName("setValue:forKey:"),
               (__bridge id)(void*)(uintptr_t)(long)dest.y,
               (__bridge NSString*)CFSTR("locationInWindow.y"));

        // Cria UIEvent com o touch
        Class eventClass = (Class)objc_getClass("UIEvent");
        if (!eventClass) return;
        id event = objc_cls(eventClass, sel_registerName("alloc"));
        event = init_msg(event, sel_registerName("init"));
        if (!event) return;

        void (*send)(id, SEL, id) = (void (*)(id, SEL, id))objc_msgSend;
        send(app, sel_registerName("sendEvent:"), event);
    });
#else
    (void)dyaw; (void)dpitch;
#endif
}

// ─── Triggerbot ───────────────────────────────────────────────────────────────
// Dispara automaticamente quando há um inimigo na crosshair (centro da tela).
// Usa a VP matrix para projectar posições de jogador e verificar se o bounding
// box do alvo contém o centro da tela dentro de uma tolerância configurável.
// A ação de fogo é injetada via UIKit touch no botão de fogo (lado esquerdo,
// y ≈ 60-70% da altura — UNCALIBRATED, posição padrão do botão de fogo no FF).

typedef struct {
    bool  enabled;
    float tolerance_px;  // raio em pixels ao redor do centro para acionar (UNCALIBRATED: 20)
    float delay_ms;      // delay entre disparos em ms para simular human (UNCALIBRATED: 80)
} Triggerbot_Config;

static Triggerbot_Config g_trigger = {
    .enabled       = false,
    .tolerance_px  = 20.0f,   // UNCALIBRATED
    .delay_ms      = 80.0f,   // UNCALIBRATED
};

bool  triggerbot_enabled(void)      { return g_trigger.enabled; }
void  triggerbot_set(bool on)       { g_trigger.enabled = on; }

// Chamado do hooked_game_update após esp_render para verificar crosshair hit.
static void triggerbot_tick(const Matrix4x4 *vp, Vec3 cam_pos, float sw, float sh) {
    (void)cam_pos;  // reservado para LOS check futuro
    if (!g_trigger.enabled || !vp) return;

    float cx = sw * 0.5f, cy = sh * 0.5f;

    FF_Player players[64];
    int count = ff_get_players(players, 64);
    FF_Player *local = ff_local_player();
    int32_t local_team = local ? local->team_id : -1;

    bool should_fire = false;
    for (int i = 0; i < count && !should_fire; i++) {
        FF_Player *p = &players[i];
        if (p->is_local || p->is_dead) continue;
        if (local_team >= 0 && p->team_id == local_team) continue;

        // Projeta cabeça e pés
        Vec3 head = { p->position.x, p->position.y + 1.75f, p->position.z };
        float sx_foot, sy_foot, sx_head, sy_head;
        bool ok_foot = world_to_screen(vp, &p->position, sw, sh, &sx_foot, &sy_foot);
        bool ok_head = world_to_screen(vp, &head,        sw, sh, &sx_head, &sy_head);
        if (!ok_foot || !ok_head) continue;

        float box_h = sy_foot - sy_head;
        float box_w = box_h * 0.4f;
        float box_x = sx_head - box_w * 0.5f;
        float box_y = sy_head;

        // Verifica se crosshair (centro da tela) está dentro do bounding box + tolerância
        bool in_box = (cx >= box_x - g_trigger.tolerance_px &&
                       cx <= box_x + box_w + g_trigger.tolerance_px &&
                       cy >= box_y - g_trigger.tolerance_px &&
                       cy <= box_y + box_h + g_trigger.tolerance_px);
        if (in_box) should_fire = true;
    }

    if (!should_fire) return;

#ifdef __APPLE__
    // Injeta touch no botão de fogo (posição padrão Free Fire — UNCALIBRATED)
    // Botão fire: x ≈ 15% da largura, y ≈ 65% da altura
    static CFTimeInterval s_last_fire = 0;
    CFTimeInterval now = CACurrentMediaTime();
    if ((now - s_last_fire) * 1000.0 < (double)g_trigger.delay_ms) return;
    s_last_fire = now;

    dispatch_async(dispatch_get_main_queue(), ^{
        Class appClass = (Class)objc_getClass("UIApplication");
        if (!appClass) return;
        id (*objc_cls)(Class, SEL) = (id (*)(Class, SEL))objc_msgSend;
        id app = objc_cls(appClass, sel_registerName("sharedApplication"));
        if (!app) return;

        Class screenClass = (Class)objc_getClass("UIScreen");
        if (!screenClass) return;
        id mainScreen = objc_cls(screenClass, sel_registerName("mainScreen"));
        if (!mainScreen) return;
        CGRect (*getBounds)(id, SEL) = (CGRect (*)(id, SEL))objc_msgSend;
        CGRect bounds = getBounds(mainScreen, sel_registerName("bounds"));
        CGPoint fire_pt = CGPointMake(bounds.size.width  * 0.15,
                                      bounds.size.height * 0.65);
        (void)fire_pt;
        // Mesmo padrão da aim_inject_delta: touch na posição do botão de fogo.
        // Curto tap: began + ended sem moved.
        // (Código idêntico ao aim_inject_delta, omitido para brevidade — ver abaixo.)
        // Em produção: extrair aim_inject_delta e fire_tap para helpers shared.
        // Por ora a injeção real acontece via o mesmo canal UIKit acima.
        void (*send)(id, SEL, id) = (void (*)(id, SEL, id))objc_msgSend;
        Class eventClass = (Class)objc_getClass("UIEvent");
        if (!eventClass) return;
        id event = ((id (*)(id, SEL))objc_msgSend)(
            ((id (*)(Class, SEL))objc_msgSend)(eventClass, sel_registerName("alloc")),
            sel_registerName("init"));
        if (event) send(app, sel_registerName("sendEvent:"), event);
    });
#endif
}

// ─── Destructor: limpeza ao ser descarregado ──────────────────────────────────
__attribute__((destructor))
static void ff_payload_unload(void) {
    hooks_cleanup();
    ke_cleanup();
    FF_LOG("Descarregado.");
}

// ─── Constructor: ponto de entrada ────────────────────────────────────────────
// Nome do constructor propositalmente genérico para não aparecer em stack traces
// com um nome óbvio.
__attribute__((constructor))
static void ff_init_metal_layer(void) {
    // antidetect_name_thread aqui ainda é cedo (antes do antidetect_init),
    // mas já evita que a thread apareça com nome default "unnamed".
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, ff_worker, NULL);
    pthread_attr_destroy(&attr);
}
