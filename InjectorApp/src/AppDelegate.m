// AppDelegate.m — Patcher via container_query (sem jailbreak)
// Mecanismo idêntico ao DOPE:
//   1. container_query_* resolve o path do container do Free Fire pelo bundle ID
//   2. Baixa Assembly-CSharp-patch.bytes do servidor
//   3. Escreve dentro de Documents/ do container do Free Fire
//   4. Free Fire carrega o patch na próxima execução

#import "AppDelegate.h"
#import <UIKit/UIKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <dlfcn.h>

// ─── container_query API privada ─────────────────────────────────────────────
// Tipos opacos C — NÃO são objetos ObjC. Usar CFTypeRef/CFArrayRef, nunca NSArray*.
// ARM64e tem PAC: passar NSArray* onde a função espera xpc_object_t/CFArrayRef
// corrompe ponteiros e causa EXC_BAD_ACCESS com "pointer authentication failure".
typedef void* MCMContainerQuery;

#define CONTAINER_CLASS_APP  2
#define CONTAINER_FLAGS_PATH (1 << 0)

static MCMContainerQuery (*fn_query_create)(void)                              = NULL;
static void              (*fn_query_set_class)(MCMContainerQuery, int)         = NULL;
// set_identifiers recebe CFArrayRef (não NSArray* — são bridge-free em C puro)
static void              (*fn_query_set_ids)(MCMContainerQuery, CFArrayRef)    = NULL;
static void              (*fn_query_op_flags)(MCMContainerQuery, int, int)     = NULL;
// get_single_result retorna CFDictionaryRef (xpc dict), não NSDictionary*
static CFDictionaryRef   (*fn_query_single)(MCMContainerQuery, int*)           = NULL;
static void              (*fn_query_free)(MCMContainerQuery)                   = NULL;

static BOOL load_container_api(void) {
    void *lib = dlopen("/usr/lib/system/libsystem_containermanager.dylib",
                       RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) lib = dlopen("/usr/lib/system/libsystem_containermanager.dylib", RTLD_LAZY);
    if (!lib) return NO;

    fn_query_create   = dlsym(lib, "container_query_create");
    fn_query_set_class= dlsym(lib, "container_query_set_class");
    fn_query_set_ids  = dlsym(lib, "container_query_set_identifiers");
    fn_query_op_flags = dlsym(lib, "container_query_operation_set_flags");
    fn_query_single   = dlsym(lib, "container_query_get_single_result");
    fn_query_free     = dlsym(lib, "container_query_free");

    return (fn_query_create && fn_query_set_class && fn_query_set_ids &&
            fn_query_op_flags && fn_query_single && fn_query_free);
}

// ─── Resolve container path via metadata scan ────────────────────────────────
// Varre /private/var/containers/Bundle/Application/*/
// procurando o .com.apple.mobile_container_manager.metadata.plist
// com CFBundleIdentifier = bundleID
static NSString* resolve_container_path_scan(NSString *bundleID) {
    NSString *appsRoot = @"/private/var/containers/Bundle/Application";
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *err = nil;
    NSArray *uuids = [fm contentsOfDirectoryAtPath:appsRoot error:&err];
    if (!uuids) return nil;

    for (NSString *uuid in uuids) {
        NSString *meta = [NSString stringWithFormat:
            @"%@/%@/.com.apple.mobile_container_manager.metadata.plist",
            appsRoot, uuid];
        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:meta];
        if ([d[@"MCMMetadataIdentifier"] isEqualToString:bundleID]) {
            // Container de dados fica em /private/var/mobile/Containers/Data/Application/<uuid>
            // Precisa buscar nessa árvore também
            break;
        }
    }

    // Busca no container de dados
    NSString *dataRoot = @"/private/var/mobile/Containers/Data/Application";
    NSArray *dataUUIDs = [fm contentsOfDirectoryAtPath:dataRoot error:&err];
    if (!dataUUIDs) return nil;

    for (NSString *uuid in dataUUIDs) {
        NSString *meta = [NSString stringWithFormat:
            @"%@/%@/.com.apple.mobile_container_manager.metadata.plist",
            dataRoot, uuid];
        NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:meta];
        if ([d[@"MCMMetadataIdentifier"] isEqualToString:bundleID]) {
            return [NSString stringWithFormat:@"%@/%@", dataRoot, uuid];
        }
    }
    return nil;
}

// ─── Resolve via container_query API (C puro, sem ObjC messaging no resultado)
static NSString* resolve_container_path_api(NSString *bundleID) {
    if (!load_container_api()) return nil;

    MCMContainerQuery q = fn_query_create();
    if (!q) return nil;

    // Passa CFArrayRef com CFStringRef — nunca NSArray* aqui
    CFStringRef cfBundleID = (__bridge CFStringRef)bundleID;
    CFArrayRef  cfIDs      = CFArrayCreate(NULL, (const void **)&cfBundleID, 1,
                                           &kCFTypeArrayCallBacks);
    fn_query_set_class(q, CONTAINER_CLASS_APP);
    fn_query_set_ids(q, cfIDs);
    CFRelease(cfIDs);
    fn_query_op_flags(q, 0, CONTAINER_FLAGS_PATH);

    int err = 0;
    CFDictionaryRef result = fn_query_single(q, &err);
    fn_query_free(q);

    if (!result || err != 0) return nil;

    // resultado é CFDictionaryRef — acessa via CF, não ObjC subscript
    NSString *path = nil;
    CFStringRef cfPath = CFDictionaryGetValue(result, CFSTR("path"));
    if (!cfPath) cfPath = CFDictionaryGetValue(result, CFSTR("containerPath"));
    if (cfPath && CFGetTypeID(cfPath) == CFStringGetTypeID()) {
        path = (__bridge NSString*)cfPath;
    }
    CFRelease(result);
    return path;
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

    // 1. container_query API (C puro, CFArrayRef) com fallback para metadata scan
    NSString *containerPath = resolve_container_path_api(ffBundleID);
    if (!containerPath) {
        containerPath = resolve_container_path_scan(ffBundleID);
    }

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
