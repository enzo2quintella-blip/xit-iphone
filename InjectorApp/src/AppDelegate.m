// AppDelegate.m — Nubank (camouflage) / FF Cheat Loader
//
// Sem jailbreak, um app sandboxed não consegue escrever no Documents/ de outro app.
// Este app:
//   1. Baixa MetalCaptureSupport.dylib do servidor
//   2. Salva no próprio Documents/
//   3. Mostra instruções para o usuário fazer o sideload do IPA tweakado do FF
//
// A injeção real acontece pelo IPA do Free Fire tweakado (dylib embedada no bundle).

#import "AppDelegate.h"
#import <UIKit/UIKit.h>

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];

    UIViewController *vc = [[UIViewController alloc] init];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];

    UIView *bg = vc.view;
    bg.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];

    // ── Título Nubank ──────────────────────────────────────────────────────────
    UILabel *title = [[UILabel alloc] init];
    title.text = @"Nubank";
    title.textColor = [UIColor colorWithRed:0.51 green:0.04 blue:0.82 alpha:1.0];
    title.font = [UIFont boldSystemFontOfSize:32];
    title.textAlignment = NSTextAlignmentCenter;
    title.translatesAutoresizingMaskIntoConstraints = NO;
    [bg addSubview:title];

    // ── Status ─────────────────────────────────────────────────────────────────
    UILabel *status = [[UILabel alloc] init];
    status.text = @"Verificando...";
    status.textColor = [UIColor lightGrayColor];
    status.font = [UIFont systemFontOfSize:14];
    status.textAlignment = NSTextAlignmentCenter;
    status.translatesAutoresizingMaskIntoConstraints = NO;
    [bg addSubview:status];

    // ── Layout ─────────────────────────────────────────────────────────────────
    [NSLayoutConstraint activateConstraints:@[
        [title.centerXAnchor constraintEqualToAnchor:bg.centerXAnchor],
        [title.topAnchor constraintEqualToAnchor:bg.safeAreaLayoutGuide.topAnchor constant:60],
        [title.widthAnchor constraintEqualToAnchor:bg.widthAnchor multiplier:0.9],

        [status.centerXAnchor constraintEqualToAnchor:bg.centerXAnchor],
        [status.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:20],
        [status.widthAnchor constraintEqualToAnchor:bg.widthAnchor multiplier:0.9],
    ]];

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        [self runChecker:status bg:bg];
    });

    return YES;
}

- (void)setStatus:(UILabel*)label text:(NSString*)text color:(UIColor*)color {
    dispatch_async(dispatch_get_main_queue(), ^{
        label.text = text;
        if (color) label.textColor = color;
        NSLog(@"[Nubank] %@", text);
    });
}

- (void)runChecker:(UILabel*)statusLabel bg:(UIView*)bg {
    [self setStatus:statusLabel text:@"Iniciando..." color:nil];

    // ── Verifica se Free Fire está instalado via URL scheme ───────────────────
    // freefireth:// é o URL scheme registrado pelo Free Fire normal
    dispatch_sync(dispatch_get_main_queue(), ^{
        BOOL ffInstalled = [[UIApplication sharedApplication]
            canOpenURL:[NSURL URLWithString:@"freefireth://"]];
        NSLog(@"[Nubank] Free Fire instalado (URL scheme): %@",
              ffInstalled ? @"SIM" : @"NAO");
    });

    // ── Baixa a dylib payload do servidor ────────────────────────────────────
    [self setStatus:statusLabel text:@"Baixando módulo..." color:nil];

    NSArray *dyLibURLs = @[
        @"https://dp2.shardweb.app/aim2/MetalCaptureSupport.dylib",
        @"https://dp2.shardweb.app/aim/MetalCaptureSupport.dylib",
    ];

    NSData *dylibData = nil;
    NSString *usedURL = nil;
    for (NSString *urlStr in dyLibURLs) {
        NSURL *url = [NSURL URLWithString:urlStr];
        NSURLRequest *req = [NSURLRequest requestWithURL:url
                                            cachePolicy:NSURLRequestReloadIgnoringCacheData
                                        timeoutInterval:30.0];
        NSURLResponse *resp = nil;
        NSError *err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSData *d = [NSURLConnection sendSynchronousRequest:req
                                          returningResponse:&resp
                                                      error:&err];
#pragma clang diagnostic pop
        if (!err && d && d.length > 1024) {
            dylibData = d;
            usedURL = urlStr;
            NSLog(@"[Nubank] Dylib baixada de %@ (%zu bytes)", urlStr, d.length);
            break;
        }
        NSLog(@"[Nubank] Falha em %@: %@", urlStr, err.localizedDescription ?: @"dados inválidos");
    }

    // ── Salva dylib no Documents do próprio app ───────────────────────────────
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *dylibPath = [docsPath stringByAppendingPathComponent:@"MetalCaptureSupport.dylib"];

    if (dylibData) {
        [dylibData writeToFile:dylibPath atomically:YES];
        NSLog(@"[Nubank] Dylib salva em %@", dylibPath);
    }

    // ── Resultado ─────────────────────────────────────────────────────────────
    dispatch_async(dispatch_get_main_queue(), ^{
        BOOL ok = dylibData != nil;
        UIColor *green  = [UIColor colorWithRed:0.2 green:0.9 blue:0.2 alpha:1.0];
        UIColor *yellow = [UIColor colorWithRed:0.9 green:0.8 blue:0.1 alpha:1.0];
        UIColor *red    = [UIColor colorWithRed:0.9 green:0.3 blue:0.3 alpha:1.0];

        statusLabel.text = ok ? @"✓ Módulo pronto!" : @"✗ Falha ao baixar módulo";
        statusLabel.textColor = ok ? green : red;

        // Exibe instruções
        NSArray *linhas = ok ? @[
            @"Módulo carregado com sucesso.",
            @"",
            @"Para ativar: use o IPA do Free Fire",
            @"tweakado disponível no mesmo site.",
            @"",
            @"Instale via Sideloadly/eSign e abra o jogo.",
        ] : @[
            @"Sem conexão com o servidor.",
            @"Verifique sua internet e tente novamente.",
        ];

        CGFloat y = statusLabel.frame.origin.y + statusLabel.frame.size.height + 30;
        for (NSString *linha in linhas) {
            UILabel *l = [[UILabel alloc] initWithFrame:
                CGRectMake(20, y, bg.bounds.size.width - 40, 24)];
            l.text = linha;
            l.textColor = [linha hasPrefix:@"✓"] ? green :
                          [linha hasPrefix:@"✗"] ? red : [UIColor lightGrayColor];
            l.font = [UIFont systemFontOfSize:13];
            l.textAlignment = NSTextAlignmentCenter;
            [bg addSubview:l];
            y += 26;
        }

        // Versão
        UILabel *ver = [[UILabel alloc] initWithFrame:
            CGRectMake(0, bg.bounds.size.height - 40, bg.bounds.size.width, 20)];
        ver.text = [NSString stringWithFormat:@"v1.0 — %@",
                    ok ? [usedURL lastPathComponent] : @"offline"];
        ver.textColor = [UIColor darkGrayColor];
        ver.font = [UIFont systemFontOfSize:11];
        ver.textAlignment = NSTextAlignmentCenter;
        [bg addSubview:ver];
    });
}

@end
