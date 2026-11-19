// overlay.m — overlay Metal via CAMetalLayer sobre a janela do Free Fire
// Estratégia: criamos uma UIWindow transparente no topo da hierarquia de janelas,
// anexamos um CAMetalLayer e renderizamos ESP + menu via Metal + Core Graphics
// (sem ImGui nesta versão — ImGui requer linkagem adicional; usamos CG/Metal direto).
// Para ImGui: adicionar imgui_impl_metal.mm + imgui_impl_ios.mm depois.

#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#import <dlfcn.h>

#include "../features/esp.h"
#include "../features/aimbot.h"
#include "../include/ff_game.h"

// ─── Forward declarations de features ────────────────────────────────────────
extern bool godmode_enabled(void);
extern void godmode_set(bool);
extern bool inf_ammo_enabled(void);
extern void inf_ammo_set(bool);
extern bool speed_enabled(void);
extern void speed_set(bool, float);
extern bool triggerbot_enabled(void);
extern void triggerbot_set(bool);

// ESP frame data declarada em esp.c
typedef struct {
    struct { float x1,y1,x2,y2; } bones[16];
    int   bone_count;
    float box_x, box_y, box_w, box_h;
    float hp_ratio, armor_ratio;
    char  label[96];
    bool  valid_snap, is_dead, is_local, valid;
    uint8_t r, g, b;
} ESP_Entity_extern;
typedef struct {
    ESP_Entity_extern entities[64];
    int count;
    float screen_w, screen_h;
} ESP_FrameData_extern;
extern ESP_FrameData_extern *ff_esp_frame_data_read(void);

// ─── Menu state ───────────────────────────────────────────────────────────────
static bool  s_menu_visible     = false;
static float s_speed_factor_ui  = 2.0f;

// ─── Helpers de desenho ───────────────────────────────────────────────────────
// Declarados antes das categories para evitar implicit-function-declaration.
static void CGContextFillRoundedRect(CGContextRef ctx, CGRect r, float radius) {
    UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:radius];
    CGContextAddPath(ctx, p.CGPath);
    CGContextFillPath(ctx);
}
static void CGContextStrokeRoundedRect(CGContextRef ctx, CGRect r, float radius) {
    UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:radius];
    CGContextAddPath(ctx, p.CGPath);
    CGContextStrokePath(ctx);
}

// ─── Overlay view ─────────────────────────────────────────────────────────────
@interface FFOverlayView : UIView
@end

@implementation FFOverlayView

// Repassa touches para a janela do jogo (overlay é transparente a toques fora do menu)
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    // Se o menu está visível e o toque está na área do menu, captura
    if (s_menu_visible) {
        CGRect menuRect = CGRectMake(10, 80, 240, 320);  // 6 items * 42 + 40 header
        if (CGRectContainsPoint(menuRect, point)) {
            return [super hitTest:point withEvent:event];
        }
    }
    return nil; // passa para baixo
}

@end

// ─── Overlay view controller ──────────────────────────────────────────────────
@interface FFOverlayVC : UIViewController
@property (nonatomic, strong) CADisplayLink *displayLink;
@property (nonatomic, strong) UILabel       *fpsLabel;
@end

@implementation FFOverlayVC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor clearColor];
    self.view.userInteractionEnabled = YES;

    // Botão toggle menu (posição fixa top-left)
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
    btn.frame = CGRectMake(10, 40, 60, 30);
    btn.backgroundColor = [UIColor colorWithRed:0.2 green:0.2 blue:0.2 alpha:0.8];
    [btn setTitle:@"MENU" forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont systemFontOfSize:11 weight:UIFontWeightBold];
    [btn addTarget:self action:@selector(toggleMenu) forControlEvents:UIControlEventTouchUpInside];
    btn.layer.cornerRadius = 4;
    [self.view addSubview:btn];

    // FPS label
    self.fpsLabel = [[UILabel alloc] initWithFrame:CGRectMake(10, 72, 100, 16)];
    self.fpsLabel.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
    self.fpsLabel.textColor = [UIColor colorWithRed:0 green:1 blue:0 alpha:0.9];
    [self.view addSubview:self.fpsLabel];

    // Display link para 60Hz
    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
    self.displayLink.preferredFramesPerSecond = 60;
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)toggleMenu {
    s_menu_visible = !s_menu_visible;
    [self.view setNeedsDisplay];
}

static CFTimeInterval s_last_frame_time = 0;
static int            s_fps_counter     = 0;
static float          s_fps_display     = 0;

- (void)renderFrame {
    // FPS counter
    CFTimeInterval now = CACurrentMediaTime();
    s_fps_counter++;
    if (now - s_last_frame_time >= 1.0) {
        s_fps_display      = (float)s_fps_counter / (float)(now - s_last_frame_time);
        s_fps_counter      = 0;
        s_last_frame_time  = now;
        self.fpsLabel.text = [NSString stringWithFormat:@"%.0f FPS", s_fps_display];
    }
    [self.view setNeedsDisplay];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // Força o superview a ser transparente
    self.view.superview.backgroundColor = [UIColor clearColor];
}

@end

// ─── Overlay draw (Core Graphics sobre camada do UIView) ─────────────────────
@implementation FFOverlayView (Drawing)

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    CGSize screen = self.bounds.size;
    float  sw = screen.width, sh = screen.height;

    // ── ESP ──────────────────────────────────────────────────────────────────
    if (g_esp.enabled) {
        ESP_FrameData_extern *frame = ff_esp_frame_data_read();
        for (int i = 0; i < frame->count; i++) {
            ESP_Entity_extern *e = &frame->entities[i];
            if (!e->valid || e->is_dead) continue;

            float cr = e->r / 255.0f, cg = e->g / 255.0f, cb = e->b / 255.0f;
            CGContextSetRGBStrokeColor(ctx, cr, cg, cb, 0.9f);
            CGContextSetLineWidth(ctx, 1.5f);

            // Box
            if (g_esp.boxes) {
                CGRect box = CGRectMake(e->box_x, e->box_y, e->box_w, e->box_h);
                CGContextStrokeRect(ctx, box);
            }

            // Bones
            if (g_esp.bones) {
                CGContextSetRGBStrokeColor(ctx, 1.0f, 1.0f, 1.0f, 0.7f);
                CGContextSetLineWidth(ctx, 1.0f);
                for (int b = 0; b < e->bone_count; b++) {
                    CGContextMoveToPoint(ctx,    e->bones[b].x1, e->bones[b].y1);
                    CGContextAddLineToPoint(ctx, e->bones[b].x2, e->bones[b].y2);
                }
                CGContextStrokePath(ctx);
                CGContextSetRGBStrokeColor(ctx, cr, cg, cb, 0.9f);
                CGContextSetLineWidth(ctx, 1.5f);
            }

            // Health bar (lado esquerdo da caixa)
            if (g_esp.health_bar) {
                float bx = e->box_x - 6;
                float by = e->box_y;
                float bh = e->box_h;
                // Fundo cinza
                CGContextSetRGBFillColor(ctx, 0.2f, 0.2f, 0.2f, 0.8f);
                CGContextFillRect(ctx, CGRectMake(bx, by, 4, bh));
                // HP verde → amarelo → vermelho
                float hp = e->hp_ratio;
                float hr = hp < 0.5f ? 1.0f : 2.0f*(1.0f-hp);
                float hg = hp > 0.5f ? 1.0f : 2.0f*hp;
                CGContextSetRGBFillColor(ctx, hr, hg, 0.0f, 0.95f);
                CGContextFillRect(ctx, CGRectMake(bx, by + bh*(1.0f-hp), 4, bh*hp));
            }

            // Label (nome + distância)
            if (e->label[0] != '\0' && g_esp.names) {
                NSString *label = [NSString stringWithUTF8String:e->label];
                NSDictionary *attrs = @{
                    NSFontAttributeName:            [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightMedium],
                    NSForegroundColorAttributeName: [UIColor colorWithRed:cr green:cg blue:cb alpha:1.0],
                };
                CGSize sz = [label sizeWithAttributes:attrs];
                CGRect lrect = CGRectMake(e->box_x + (e->box_w - sz.width) * 0.5f,
                                         e->box_y - sz.height - 2,
                                         sz.width, sz.height);
                [label drawInRect:lrect withAttributes:attrs];
            }

            // Snap line
            if (e->valid_snap && g_esp.snap_lines) {
                CGContextSetRGBStrokeColor(ctx, 1.0f, 1.0f, 0.0f, 0.5f);
                CGContextSetLineWidth(ctx, 1.0f);
                CGContextMoveToPoint(ctx,    sw * 0.5f, sh);
                CGContextAddLineToPoint(ctx, e->box_x + e->box_w * 0.5f,
                                             e->box_y + e->box_h);
                CGContextStrokePath(ctx);
            }
        }
    }

    // ── Menu ─────────────────────────────────────────────────────────────────
    if (s_menu_visible) {
        [self drawMenu:ctx];
    }
}

- (void)drawMenu:(CGContextRef)ctx {
    // Painel semi-transparente
    CGRect panel = CGRectMake(10, 80, 240, 320);  // 6 items * 42 + 40 header + 10 padding
    CGContextSetRGBFillColor(ctx, 0.05f, 0.05f, 0.05f, 0.88f);
    CGContextFillRoundedRect(ctx, panel, 8);
    CGContextSetRGBStrokeColor(ctx, 0.4f, 0.4f, 0.4f, 1.0f);
    CGContextSetLineWidth(ctx, 1.0f);
    CGContextStrokeRoundedRect(ctx, panel, 8);

    // Título
    NSString *title = @"FF CHEAT";
    NSDictionary *titleAttrs = @{
        NSFontAttributeName:            [UIFont systemFontOfSize:13 weight:UIFontWeightBold],
        NSForegroundColorAttributeName: [UIColor whiteColor],
    };
    [title drawAtPoint:CGPointMake(panel.origin.x + 10, panel.origin.y + 10)
        withAttributes:titleAttrs];

    // Botões toggle (6 itens — panel height ajustado para 320 em overlay_init)
    struct { const char *name; } items[] = {
        { "ESP"        },
        { "Aimbot"     },
        { "Triggerbot" },
        { "God Mode"   },
        { "Inf Ammo"   },
        { "Speed x2"   },
    };

    bool  dyn_flags[6];
    dyn_flags[0] = g_esp.enabled;
    dyn_flags[1] = g_aimbot.enabled;
    dyn_flags[2] = triggerbot_enabled();
    dyn_flags[3] = godmode_enabled();
    dyn_flags[4] = inf_ammo_enabled();
    dyn_flags[5] = speed_enabled();

    for (int i = 0; i < 6; i++) {
        // y = panel.origin.y + 40 + i*42
        CGRect row = CGRectMake(panel.origin.x + 10,
                                panel.origin.y + 40 + i * 42,
                                220, 32);
        bool on = dyn_flags[i];
        // Fundo do botão
        CGContextSetRGBFillColor(ctx,
            on ? 0.0f : 0.2f,
            on ? 0.6f : 0.2f,
            on ? 0.0f : 0.2f,
            0.9f);
        CGContextFillRoundedRect(ctx, row, 4);

        // Texto
        NSString *label = [NSString stringWithFormat:@"%s  [%s]",
                           items[i].name, on ? "ON" : "OFF"];  // items[i].name ok
        NSDictionary *attrs = @{
            NSFontAttributeName:            [UIFont systemFontOfSize:12 weight:UIFontWeightMedium],
            NSForegroundColorAttributeName: on ? [UIColor colorWithRed:0.3 green:1 blue:0.3 alpha:1]
                                               : [UIColor colorWithRed:0.7 green:0.7 blue:0.7 alpha:1],
        };
        [label drawAtPoint:CGPointMake(row.origin.x + 8, row.origin.y + 10) withAttributes:attrs];
    }
}

@end

// ─── Touch handler para o menu ────────────────────────────────────────────────
@implementation FFOverlayView (Touch)

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!s_menu_visible) return;
    UITouch *t = touches.anyObject;
    CGPoint p = [t locationInView:self];

    // Detecta toque nos botões (6 items, row height = 32, gap = 42)
    for (int i = 0; i < 6; i++) {
        CGRect row = CGRectMake(20, 120 + i * 42, 220, 32);
        if (CGRectContainsPoint(row, p)) {
            switch (i) {
                case 0: g_esp.enabled       = !g_esp.enabled;             break;
                case 1: g_aimbot.enabled    = !g_aimbot.enabled;          break;
                case 2: triggerbot_set(!triggerbot_enabled());             break;
                case 3: godmode_set(!godmode_enabled());                   break;
                case 4: inf_ammo_set(!inf_ammo_enabled());                 break;
                case 5: speed_set(!speed_enabled(), s_speed_factor_ui);   break;
            }
            [self setNeedsDisplay];
            break;
        }
    }
}

@end

// ─── overlay_init / overlay_tick ─────────────────────────────────────────────
static UIWindow *s_overlay_window = nil;

void overlay_init(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        // Janela transparente no topo de todas as outras
        CGRect screen = [UIScreen mainScreen].bounds;
        s_overlay_window = [[UIWindow alloc] initWithFrame:screen];
        s_overlay_window.windowLevel = UIWindowLevelAlert + 100;
        s_overlay_window.backgroundColor = [UIColor clearColor];
        s_overlay_window.opaque = NO;
        s_overlay_window.userInteractionEnabled = YES;

        FFOverlayVC *vc = [[FFOverlayVC alloc] init];
        // A view precisa ser FFOverlayView para o hitTest correto
        // Substituímos a view do VC após loadView
        [vc loadViewIfNeeded];
        // Recria com a subclasse correta
        FFOverlayView *ov = [[FFOverlayView alloc] initWithFrame:screen];
        ov.backgroundColor = [UIColor clearColor];
        ov.opaque = NO;

        // Passa subviews do VC para a FFOverlayView
        for (UIView *sub in [vc.view.subviews copy]) {
            [ov addSubview:sub];
        }
        vc.view = ov;

        s_overlay_window.rootViewController = vc;
        [s_overlay_window makeKeyAndVisible];
    });
}

void overlay_tick(void) {
    // Chamado do hook de render; apenas sinaliza redraw se necessário.
    // O CADisplayLink já agenda o redraw a 60Hz — não precisamos chamar aqui.
    // Mantemos para compatibilidade com payload_main.c.
}
