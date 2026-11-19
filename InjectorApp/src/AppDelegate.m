// AppDelegate.m — InjectorApp: inicia, aguarda o Free Fire e injeta a dylib
// Mecanismo: DYLD_INSERT_LIBRARIES via relaunch (se jailbreak disponível)
// ou dlopen direto se rodando in-process (modo sideload com entitlements).
// Com kernel exploit: vnode_redirect para apontar uma dylib do jogo para a nossa.

#import "AppDelegate.h"
#import <dlfcn.h>
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>  // _dyld_image_count, _dyld_get_image_name

#include "../../KernelExploit/include/kexploit.h"

// Caminho da dylib payload dentro do bundle
static NSString *payload_dylib_path(void) {
    NSString *bundle = [[NSBundle mainBundle] bundlePath];
    return [bundle stringByAppendingPathComponent:@"FFPayload.dylib"];
}

// ─── Injeção via dlopen (modo in-process) ────────────────────────────────────
// Funciona quando o InjectorApp e o payload rodam no mesmo processo,
// ou quando o Free Fire foi relanado com DYLD_INSERT_LIBRARIES apontando para
// o payload (requer sandbox escape + exec do jogo de dentro do nosso app).
static BOOL inject_dlopen(void) {
    NSString *path = payload_dylib_path();
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        NSLog(@"[Injector] Dylib não encontrada em: %@", path);
        return NO;
    }
    void *handle = dlopen(path.UTF8String, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        NSLog(@"[Injector] dlopen falhou: %s", dlerror());
        return NO;
    }
    NSLog(@"[Injector] Dylib carregada: %@", path);
    return YES;
}

// ─── Injeção via vnode_redirect (requer kernel exploit) ──────────────────────
// Redireciona uma dylib que o Free Fire já carrega para a nossa dylib payload.
// Transparente ao DYLD: o jogo carrega normalmente, mas recebe nosso código.
//
// Alvo escolhido: DataDomeSDK.framework/DataDomeSDK
//   • É o único @rpath/... não-system carregado pelo UnityFramework.
//   • Não é uma biblioteca crítica de engine nem de sistema.
//   • Mapeado via LC_LOAD_DYLIB com @rpath, logo carregado antes da main().
//
// O rpath resolvido em tempo de execução é:
//   <bundle>/Frameworks/DataDomeSDK.framework/DataDomeSDK
// A nossa dylib substitui esse vnode via ke_vnode_redirect():
//   o kernel aponta o inode alvo para o arquivo da nossa dylib.
//
// VERIFICAÇÃO: confirme que DataDomeSDK ainda está presente na versão atual:
//   otool -L <IPA>/Payload/FreeFire.app/Frameworks/UnityFramework.framework/UnityFramework
//   | grep -i datadome
// Se não estiver mais: use qualquer outro @rpath/... listado que não seja
//   libswift* nem UnityEngine.
static BOOL inject_vnode(void) {
    KernelCapabilities caps = ke_capabilities();
    if (!caps.vnode_redirect) return NO;

    // Caminho resolvido do framework alvo dentro do bundle do Free Fire.
    // ke_vnode_redirect() recebe caminhos absolutos.
    // O bundle do Free Fire fica em:
    //   /private/var/containers/Bundle/Application/<UUID>/FreeFire.app/
    // Localize via /proc/self/maps ou NSBundle(bundleIdentifier).
    NSString *ff_bundle_path = nil;
    // Heurística: se rodando via jailbreak, o Free Fire provavelmente está instalado.
    // Detecta o bundle procurando pelo processo ou pelo path de um módulo do jogo.
    uint32_t img_count = _dyld_image_count();
    for (uint32_t i = 0; i < img_count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, "FreeFire") && strstr(name, "Frameworks")) {
            // name é algo como /.../FreeFire.app/Frameworks/UnityFramework.../
            NSString *path = [NSString stringWithUTF8String:name];
            // Vai para <bundle>.app/
            NSRange r = [path rangeOfString:@".app/"];
            if (r.location != NSNotFound) {
                ff_bundle_path = [path substringToIndex:r.location + r.length - 1];
                // ff_bundle_path = /.../FreeFire.app
            }
            break;
        }
    }

    if (!ff_bundle_path) {
        NSLog(@"[Injector] vnode_redirect: bundle do FreeFire não encontrado");
        return NO;
    }

    NSString *target = [ff_bundle_path
        stringByAppendingPathComponent:
            @"Frameworks/DataDomeSDK.framework/DataDomeSDK"];
    const char *target_dylib = target.UTF8String;
    const char *our_dylib    = payload_dylib_path().UTF8String;

    if (![[NSFileManager defaultManager] fileExistsAtPath:target]) {
        NSLog(@"[Injector] vnode_redirect: alvo '%@' não existe — DataDomeSDK removido neste build?", target);
        return NO;
    }

    ke_status_t st = ke_vnode_redirect(target_dylib, our_dylib);
    if (st != KE_OK) {
        NSLog(@"[Injector] ke_vnode_redirect falhou: %d", (int)st);
        return NO;
    }
    NSLog(@"[Injector] vnode_redirect: '%@' → nossa dylib", target);
    return YES;
}

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    NSLog(@"[Injector] Iniciando...");

    // 1. Tenta kernel exploit
    ke_status_t ke = ke_init();
    NSLog(@"[Injector] Kernel exploit: %d", (int)ke);

    if (ke == KE_OK) {
        // Sandbox escape primeiro
        ke_sandbox_escape();
        // Tenta vnode redirect
        if (inject_vnode()) {
            NSLog(@"[Injector] Injeção via vnode_redirect agendada.");
        }
    }

    // 2. Fallback: dlopen direto (funciona se rodando no mesmo processo ou com entitlements)
    if (!inject_dlopen()) {
        NSLog(@"[Injector] Injeção direta falhou — verifique entitlements e caminho da dylib.");
    }

    // Cria janela mínima para o app não crashar imediatamente
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[UIViewController alloc] init];
    self.window.rootViewController.view.backgroundColor = [UIColor blackColor];
    [self.window makeKeyAndVisible];

    // Label de status
    UILabel *lbl = [[UILabel alloc] initWithFrame:CGRectMake(20, 100, 280, 60)];
    lbl.text = @"FF Cheat Injector\nAguardando jogo...";
    lbl.numberOfLines = 2;
    lbl.textColor = [UIColor greenColor];
    lbl.textAlignment = NSTextAlignmentCenter;
    lbl.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightBold];
    [self.window.rootViewController.view addSubview:lbl];

    return YES;
}

@end
