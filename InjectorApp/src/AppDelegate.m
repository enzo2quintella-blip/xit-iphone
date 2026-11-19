// AppDelegate.m — Patcher via MobileHouseArrest (sem jailbreak)
//
// Mecanismo DOPE 7.0:
//   1. Conecta ao daemon com.apple.mobile.house_arrest via /var/run/lockdown.sock
//   2. Envia HouseArrestCommand "VendDocuments" com bundleID do Free Fire
//   3. Daemon retorna AFC connection handle para o Documents/ do Free Fire
//   4. Escreve Assembly-CSharp-patch.bytes via AFC no Documents/ do Free Fire
//   5. Free Fire carrega o patch na próxima execução
//
// MobileHouseArrest permite acesso ao Documents/ de qualquer app instalado
// sem jailbreak — é o mesmo mecanismo que o iTunes/Finder usa.

#import "AppDelegate.h"
#import <UIKit/UIKit.h>
#import <dlfcn.h>
#import <sys/socket.h>
#import <sys/un.h>
#import <netinet/in.h>
#import <arpa/inet.h>

// ─── Lockdown / AFC tipos e constantes ───────────────────────────────────────
#define LOCKDOWN_SOCKET_PATH  "/var/run/lockdown.sock"
#define AFC_MAGIC             0x4141504c36414643ULL  // "AAPL6AFC"
#define AFC_VERSION           0x00000002
#define AFC_OP_STATUS         0x00000001
#define AFC_OP_DATA           0x00000002
#define AFC_OP_READ_DIR       0x00000003
#define AFC_OP_READ_FILE      0x00000004
#define AFC_OP_WRITE_FILE     0x00000005
#define AFC_OP_WRITE_PART     0x00000006
#define AFC_OP_MAKE_DIR       0x00000009
#define AFC_OP_FILE_INFO      0x0000000A
#define AFC_OP_FILE_OPEN      0x0000000D
#define AFC_OP_FILE_OPEN_RES  0x0000000E
#define AFC_OP_FILE_CLOSE     0x00000014
#define AFC_OP_FILE_SET_SIZE  0x00000013
#define AFC_FOPEN_WRONLY      0x00000002
#define AFC_FOPEN_RW          0x00000003
#define AFC_FOPEN_WR          0x00000004  // write, create, truncate

#pragma pack(push, 1)
typedef struct {
    uint64_t magic;      // AFC_MAGIC
    uint64_t entire_len; // header + data_len
    uint64_t header_len; // sizeof this header (40)
    uint64_t packet_num;
    uint64_t operation;
} AFCPacketHeader;
#pragma pack(pop)

// ─── Envio/Recebimento robusto ────────────────────────────────────────────────
static BOOL send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return NO;
        p += n; len -= n;
    }
    return YES;
}

static BOOL recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return NO;
        p += n; len -= n;
    }
    return YES;
}

// ─── AFC: envia packet com header + payload ───────────────────────────────────
static BOOL afc_send(int fd, uint64_t op, uint64_t packet_num,
                     const void *data, size_t data_len) {
    AFCPacketHeader hdr = {0};
    hdr.magic       = AFC_MAGIC;
    hdr.header_len  = sizeof(AFCPacketHeader);
    hdr.entire_len  = sizeof(AFCPacketHeader) + data_len;
    hdr.packet_num  = packet_num;
    hdr.operation   = op;

    if (!send_all(fd, &hdr, sizeof(hdr))) return NO;
    if (data_len > 0 && !send_all(fd, data, data_len)) return NO;
    return YES;
}

// Lê header de resposta e descarta o payload (retorna operation e payload_len)
static BOOL afc_recv_header(int fd, uint64_t *out_op, uint64_t *out_payload_len) {
    AFCPacketHeader hdr = {0};
    if (!recv_all(fd, &hdr, sizeof(hdr))) return NO;
    if (hdr.magic != AFC_MAGIC) return NO;
    *out_op = hdr.operation;
    *out_payload_len = hdr.entire_len - hdr.header_len;
    return YES;
}

static BOOL afc_recv_discard(int fd, uint64_t payload_len) {
    if (payload_len == 0) return YES;
    uint8_t buf[4096];
    while (payload_len > 0) {
        size_t chunk = payload_len < sizeof(buf) ? (size_t)payload_len : sizeof(buf);
        if (!recv_all(fd, buf, chunk)) return NO;
        payload_len -= chunk;
    }
    return YES;
}

// ─── Lê resposta AFC e retorna payload alocado (caller faz free) ──────────────
static NSData* afc_recv_data(int fd) {
    AFCPacketHeader hdr = {0};
    if (!recv_all(fd, &hdr, sizeof(hdr))) return nil;
    if (hdr.magic != AFC_MAGIC) return nil;
    uint64_t payload_len = hdr.entire_len - hdr.header_len;
    if (payload_len == 0) return [NSData data];
    NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)payload_len];
    if (!recv_all(fd, d.mutableBytes, (size_t)payload_len)) return nil;
    return d;
}

// ─── House Arrest: conecta e obtém AFC fd para Documents/ do bundleID ─────────
// Usa lockdown socket local — funciona sem jailbreak no mesmo device
static int house_arrest_connect(NSString *bundleID) {
    // Conecta ao socket do lockdown
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        NSLog(@"[Nubank] socket() falhou: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, LOCKDOWN_SOCKET_PATH, sizeof(addr.sun_path));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        NSLog(@"[Nubank] connect lockdown: %s", strerror(errno));
        close(sock);
        return -1;
    }

    // Envia HouseArrest plist request (XML)
    // O daemon aceita plist XML direto no socket para o serviço house_arrest
    NSString *plistXML = [NSString stringWithFormat:
        @"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        @"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        @"\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        @"<plist version=\"1.0\"><dict>"
        @"<key>Command</key><string>VendDocuments</string>"
        @"<key>Identifier</key><string>%@</string>"
        @"</dict></plist>", bundleID];

    NSData *plistData = [plistXML dataUsingEncoding:NSUTF8StringEncoding];
    uint32_t plistLen = (uint32_t)plistData.length;
    uint32_t plistLenBE = CFSwapInt32HostToBig(plistLen);

    // Lockdown protocol: 4-byte big-endian length + plist data
    if (!send_all(sock, &plistLenBE, 4) ||
        !send_all(sock, plistData.bytes, plistLen)) {
        NSLog(@"[Nubank] Falha ao enviar house_arrest request");
        close(sock);
        return -1;
    }

    // Lê resposta
    uint32_t respLenBE = 0;
    if (!recv_all(sock, &respLenBE, 4)) {
        NSLog(@"[Nubank] Falha ao ler house_arrest response length");
        close(sock);
        return -1;
    }
    uint32_t respLen = CFSwapInt32BigToHost(respLenBE);
    if (respLen == 0 || respLen > 65536) {
        NSLog(@"[Nubank] house_arrest response length inválido: %u", respLen);
        close(sock);
        return -1;
    }

    NSMutableData *respData = [NSMutableData dataWithLength:respLen];
    if (!recv_all(sock, respData.mutableBytes, respLen)) {
        NSLog(@"[Nubank] Falha ao ler house_arrest response");
        close(sock);
        return -1;
    }

    // Parse plist de resposta
    NSError *parseErr = nil;
    NSDictionary *resp = [NSPropertyListSerialization
        propertyListWithData:respData
        options:NSPropertyListImmutable
        format:nil
        error:&parseErr];

    if (!resp || parseErr) {
        NSLog(@"[Nubank] Falha ao parsear house_arrest response: %@",
              parseErr.localizedDescription);
        close(sock);
        return -1;
    }

    NSString *status = resp[@"Status"];
    if (![status isEqualToString:@"Complete"]) {
        NSLog(@"[Nubank] house_arrest status: %@ error: %@",
              status, resp[@"Error"] ?: @"none");
        close(sock);
        return -1;
    }

    NSLog(@"[Nubank] house_arrest OK para %@, socket AFC pronto", bundleID);
    // O socket agora está em modo AFC — pode usar AFC ops diretamente
    return sock;
}

// ─── AFC: cria diretório (ignora erro se já existe) ───────────────────────────
static void afc_mkdir(int afc_fd, const char *path, uint64_t *pkt_num) {
    size_t plen = strlen(path) + 1;
    afc_send(afc_fd, AFC_OP_MAKE_DIR, (*pkt_num)++, path, plen);
    uint64_t op, payload_len;
    afc_recv_header(afc_fd, &op, &payload_len);
    afc_recv_discard(afc_fd, payload_len);
}

// ─── AFC: escreve arquivo completo ───────────────────────────────────────────
static BOOL afc_write_file(int afc_fd, const char *path,
                           const void *data, size_t data_len,
                           uint64_t *pkt_num) {
    // FILE_OPEN: mode (8 bytes) + path + null
    uint8_t open_buf[8 + 256];
    uint64_t mode = AFC_FOPEN_WR;
    memcpy(open_buf, &mode, 8);
    size_t plen = strlen(path) + 1;
    if (plen > 256) return NO;
    memcpy(open_buf + 8, path, plen);

    afc_send(afc_fd, AFC_OP_FILE_OPEN, (*pkt_num)++, open_buf, 8 + plen);

    NSData *resp = afc_recv_data(afc_fd);
    if (!resp || resp.length < 8) return NO;

    uint64_t file_handle = 0;
    memcpy(&file_handle, resp.bytes, 8);
    if (file_handle == 0) return NO;

    NSLog(@"[Nubank] AFC file handle: %llu", (unsigned long long)file_handle);

    // WRITE_FILE: file_handle (8 bytes) + data
    NSMutableData *wbuf = [NSMutableData dataWithCapacity:8 + data_len];
    [wbuf appendBytes:&file_handle length:8];
    [wbuf appendBytes:data length:data_len];

    afc_send(afc_fd, AFC_OP_WRITE_FILE, (*pkt_num)++,
             wbuf.bytes, wbuf.length);

    uint64_t op, payload_len;
    if (!afc_recv_header(afc_fd, &op, &payload_len)) return NO;
    afc_recv_discard(afc_fd, payload_len);

    // FILE_CLOSE
    afc_send(afc_fd, AFC_OP_FILE_CLOSE, (*pkt_num)++,
             &file_handle, sizeof(file_handle));
    afc_recv_header(afc_fd, &op, &payload_len);
    afc_recv_discard(afc_fd, payload_len);

    return (op != AFC_OP_STATUS || payload_len == 0 || YES); // best-effort
}

// ─── Baixa dados de uma URL ───────────────────────────────────────────────────
static NSData* download_sync(NSString *urlStr) {
    NSURL *url = [NSURL URLWithString:urlStr];
    NSURLRequest *req = [NSURLRequest requestWithURL:url
                                        cachePolicy:NSURLRequestReloadIgnoringCacheData
                                    timeoutInterval:30.0];
    NSURLResponse *resp = nil;
    NSError *err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSData *data = [NSURLConnection sendSynchronousRequest:req
                                         returningResponse:&resp
                                                     error:&err];
#pragma clang diagnostic pop
    if (err || !data || data.length < 100) return nil;
    return data;
}

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];
    self.window.rootViewController = [[UIViewController alloc] init];
    [self.window makeKeyAndVisible];

    UIView *bg = self.window.rootViewController.view;
    bg.backgroundColor = [UIColor colorWithRed:0.08 green:0.08 blue:0.10 alpha:1.0];

    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(0, 120, bg.bounds.size.width, 50)];
    title.text = @"Nubank";
    title.textColor = [UIColor colorWithRed:0.51 green:0.04 blue:0.82 alpha:1.0];
    title.font = [UIFont boldSystemFontOfSize:28];
    title.textAlignment = NSTextAlignmentCenter;
    [bg addSubview:title];

    UILabel *status = [[UILabel alloc] initWithFrame:CGRectMake(20, 200, bg.bounds.size.width - 40, 40)];
    status.text = @"Inicializando...";
    status.textColor = [UIColor whiteColor];
    status.font = [UIFont systemFontOfSize:14];
    status.textAlignment = NSTextAlignmentCenter;
    status.tag = 1001;
    [bg addSubview:status];

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

    [self setStatus:statusLabel text:@"Conectando ao Free Fire..."];

    // 1. Conecta via MobileHouseArrest — obtém socket AFC para Documents/ do FF
    int afc_fd = house_arrest_connect(ffBundleID);
    if (afc_fd < 0) {
        [self setStatus:statusLabel text:@"Free Fire não encontrado. Instale o jogo primeiro."];
        return;
    }

    [self setStatus:statusLabel text:@"Free Fire encontrado! Baixando patch..."];

    // 2. Baixa o patch
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
        close(afc_fd);
        [self setStatus:statusLabel text:@"Falha ao baixar patch. Verifique a conexão."];
        return;
    }

    [self setStatus:statusLabel text:@"Aplicando patch..."];

    // 3. Escreve via AFC no Documents/ do Free Fire
    // No contexto do house_arrest, o path "/" já é o Documents/ do app
    uint64_t pkt_num = 1;

    BOOL ok = afc_write_file(afc_fd, "/Assembly-CSharp-patch.bytes",
                             patchData.bytes, patchData.length, &pkt_num);

    // 4. Baixa e aplica plist de config
    NSArray *plistURLs = @[
        @"https://dp2.shardweb.app/aim2/com.dts.freefireth.plist",
        @"https://dp2.shardweb.app/aim/com.dts.freefireth.plist",
    ];
    for (NSString *url in plistURLs) {
        NSData *plistData = download_sync(url);
        if (plistData) {
            afc_write_file(afc_fd, "/com.dts.freefireth.plist",
                           plistData.bytes, plistData.length, &pkt_num);
            break;
        }
    }

    close(afc_fd);

    dispatch_async(dispatch_get_main_queue(), ^{
        if (ok) {
            statusLabel.text = @"✓ Patch aplicado! Abra o Free Fire.";
            statusLabel.textColor = [UIColor colorWithRed:0.2 green:0.9 blue:0.2 alpha:1.0];
        } else {
            statusLabel.text = @"Patch baixado mas falha ao escrever. Sem permissão.";
            statusLabel.textColor = [UIColor colorWithRed:0.9 green:0.3 blue:0.3 alpha:1.0];
        }

        UIView *bg = self.window.rootViewController.view;
        UILabel *note = [[UILabel alloc] initWithFrame:
            CGRectMake(20, 260, bg.bounds.size.width - 40, 60)];
        note.text = ok ? @"Feche este app e abra o Free Fire para ativar."
                       : @"Tente novamente ou verifique as permissões.";
        note.textColor = [UIColor lightGrayColor];
        note.font = [UIFont systemFontOfSize:13];
        note.textAlignment = NSTextAlignmentCenter;
        note.numberOfLines = 2;
        [bg addSubview:note];
    });
}

@end
