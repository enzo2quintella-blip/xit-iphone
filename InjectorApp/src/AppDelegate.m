// AppDelegate.m — Patcher via metadata scan (sem jailbreak, sem API privada)
// Mecanismo:
//   1. Varre /private/var/mobile/Containers/Data/Application/*/
//      lendo .com.apple.mobile_container_manager.metadata.plist
//      para encontrar o container de dados do Free Fire pelo bundle ID
//   2. Baixa Assembly-CSharp-patch.bytes do servidor
//   3. Escreve dentro de Documents/ do container do Free Fire
//   4. Free Fire carrega o patch na próxima execução
//
// NOTA: container_query API privada foi removida — retorna xpc_object_t em iOS 26
// que não é CFDictionaryRef, causando SIGTRAP em CFDictionaryGetValue.
// O scan de metadata é mais robusto e não depende de API que muda entre versões.

#import "AppDelegate.h"
#import <UIKit/UIKit.h>

// ─── Resolve container de dados via metadata scan ────────────────────────────
// Varre /private/var/mobile/Containers/Data/Application/*/
// lendo .com.apple.mobile_container_manager.metadata.plist
// procurando MCMMetadataIdentifier == bundleID
// Retorna o path raiz do container (ex: /private/var/mobile/Containers/Data/Application/<UUID>)
static NSString* resolve_container_path_scan(NSString *bundleID) {
    // Container de dados (onde fica Documents/, Library/, etc.)
    NSString *dataRoot = @"/private/var/mobile/Containers/Data/Application";
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *listErr = nil;

    NSArray *uuids = [fm contentsOfDirectoryAtPath:dataRoot error:&listErr];
    if (!uuids || uuids.count == 0) {
        NSLog(@"[Nubank] Falha ao listar %@: %@", dataRoot,
              listErr.localizedDescription ?: @"nil");
        return nil;
    }

    for (NSString *uuid in uuids) {
        NSString *meta = [NSString stringWithFormat:
            @"%@/%@/.com.apple.mobile_container_manager.metadata.plist",
            dataRoot, uuid];

        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:meta];
        if (!d) continue;

        NSString *identifier = d[@"MCMMetadataIdentifier"];
        if ([identifier isEqualToString:bundleID]) {
            NSString *result = [NSString stringWithFormat:@"%@/%@", dataRoot, uuid];
            NSLog(@"[Nubank] Container encontrado: %@", result);
            return result;
        }
    }

    NSLog(@"[Nubank] Bundle ID %@ não encontrado em %@", bundleID, dataRoot);
    return nil;
}

// ─── Baixa dados de uma URL ───────────────────────────────────────────────────
static NSData* download_sync(NSString *urlStr) {
    NSURL *url = [NSURL URLWithString:urlStr];
    NSURLRequest *req = [NSURLRequest requestWithURL:url
                                        cachePolicy:NSURLRequestReloadIgnoringCacheData
                                    timeoutInterval:30.0];
    NSURLResponse *resp = nil;
    NSError *err = nil;
    NSData *data = [NSURLConnection sendSynchronousRequest:req
                                         returningResponse:&resp
                                                     error:&err];
    if (err || !data || data.length < 100) return nil;
    return data;
}

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    // ── UI mínima ────────────────────────────────────────────────────────────
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];
    self.window.rootViewController = [[UIViewController alloc] init];
    [self.window makeKeyAndVisible];

    UIView *bg = self.window.rootViewController.view;
    bg.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];

    // Logo / título
    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(0, 120, bg.bounds.size.width, 50)];
    title.text = @"Nubank";
    title.textColor = [UIColor colorWithRed:0.51 green:0.04 blue:0.82 alpha:1.0];
    title.font = [UIFont boldSystemFontOfSize:28];
    title.textAlignment = NSTextAlignmentCenter;
    [bg addSubview:title];

    // Status label
    UILabel *status = [[UILabel alloc] initWithFrame:CGRectMake(20, 200, bg.bounds.size.width - 40, 40)];
    status.text = @"Inicializando...";
    status.textColor = [UIColor whiteColor];
    status.font = [UIFont systemFontOfSize:14];
    status.textAlignment = NSTextAlignmentCenter;
    status.tag = 1001;
    [bg addSubview:status];

    // Roda o patch em background
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        [self runPatcher:status];
    });

    return YES;
}

- (void)setStatus:(UILabel*)label text:(NSString*)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        label.text = text;
        NSLog(@"[Nubank] %@", text);
    });
}

- (void)runPatcher:(UILabel*)statusLabel {
    NSString *ffBundleID = @"com.dts.freefireth";

    [self setStatus:statusLabel text:@"Localizando Free Fire..."];

    // 1. Localiza container de dados via metadata scan
    NSString *containerPath = resolve_container_path_scan(ffBundleID);

    if (!containerPath) {
        [self setStatus:statusLabel text:@"Free Fire não encontrado. Instale o jogo primeiro."];
        return;
    }

    NSLog(@"[Nubank] Container: %@", containerPath);
    [self setStatus:statusLabel text:@"Free Fire encontrado! Baixando patch..."];

    // 2. Baixa o patch — tenta os dois endpoints como o DOPE faz
    NSArray *patchURLs = @[
        @"https://dp2.shardweb.app/aim2/Assembly-CSharp-patch.bytes",
        @"https://dp2.shardweb.app/aim/Assembly-CSharp-patch.bytes",
    ];

    NSData *patchData = nil;
    for (NSString *url in patchURLs) {
        patchData = download_sync(url);
        if (patchData) {
            NSLog(@"[Nubank] Patch baixado de %@ (%zu bytes)", url, patchData.length);
            break;
        }
    }

    if (!patchData) {
        [self setStatus:statusLabel text:@"Falha ao baixar patch. Verifique a conexão."];
        return;
    }

    [self setStatus:statusLabel text:@"Aplicando patch..."];

    // 3. Escreve em Documents/ do container do Free Fire
    NSString *docsPath = [containerPath stringByAppendingPathComponent:@"Documents"];
    NSFileManager *fm = [NSFileManager defaultManager];

    NSError *err = nil;
    if (![fm fileExistsAtPath:docsPath]) {
        [fm createDirectoryAtPath:docsPath
      withIntermediateDirectories:YES
                       attributes:nil
                            error:&err];
    }

    NSString *patchDest = [docsPath stringByAppendingPathComponent:@"Assembly-CSharp-patch.bytes"];
    BOOL ok = [patchData writeToFile:patchDest options:NSDataWritingAtomic error:&err];

    if (!ok || err) {
        NSString *msg = [NSString stringWithFormat:@"Falha ao escrever patch: %@",
                         err.localizedDescription ?: @"sem permissão"];
        [self setStatus:statusLabel text:msg];
        return;
    }

    NSLog(@"[Nubank] Patch escrito em: %@", patchDest);

    // 4. Baixa e aplica plist de config se disponível
    NSArray *plistURLs = @[
        @"https://dp2.shardweb.app/aim2/com.dts.freefireth.plist",
        @"https://dp2.shardweb.app/aim/com.dts.freefireth.plist",
    ];

    for (NSString *url in plistURLs) {
        NSData *plistData = download_sync(url);
        if (plistData) {
            NSString *prefPath = [containerPath
                stringByAppendingPathComponent:@"Library/Preferences/com.dts.freefireth.plist"];
            [plistData writeToFile:prefPath options:NSDataWritingAtomic error:nil];
            NSLog(@"[Nubank] Plist de config escrita em: %@", prefPath);
            break;
        }
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        statusLabel.text = @"✓ Patch aplicado! Abra o Free Fire.";
        statusLabel.textColor = [UIColor colorWithRed:0.2 green:0.9 blue:0.2 alpha:1.0];

        UIView *bg = self.window.rootViewController.view;
        UILabel *note = [[UILabel alloc] initWithFrame:
            CGRectMake(20, 260, bg.bounds.size.width - 40, 60)];
        note.text = @"Feche este app e abra o Free Fire para ativar.";
        note.textColor = [UIColor lightGrayColor];
        note.font = [UIFont systemFontOfSize:13];
        note.textAlignment = NSTextAlignmentCenter;
        note.numberOfLines = 2;
        [bg addSubview:note];
    });
}

@end
