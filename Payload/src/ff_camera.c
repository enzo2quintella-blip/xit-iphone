// ff_camera.c — ViewProjection matrix e world-to-screen via IL2CPP Camera.
//
// Estratégia de resolução dos métodos (todos os RVAs Beebyte = 0 no dump):
//   1. Encontra UnityEngine.Camera via il2cpp_class_from_name("UnityEngine","Camera").
//      Se "UnityEngine" não funcionar, tenta namespace vazio "" (UnityEngine é c++ side).
//   2. Faz class_get_method_from_name para:
//        "get_main"              (0 params) — Camera.main property getter
//        "get_projectionMatrix"  (0 params) — retorna Matrix4x4
//        "get_worldToCameraMatrix" (0 params) — retorna Matrix4x4
//        "WorldToScreenPoint"    (1 param: Vector3) — retorna Vector3
//   3. A cada frame (ff_camera_update):
//        cam_instance = invoke(get_main, NULL, NULL)
//        view_mat     = invoke(get_worldToCameraMatrix, cam_instance, NULL)  → Matrix4x4
//        proj_mat     = invoke(get_projectionMatrix,    cam_instance, NULL)  → Matrix4x4
//        VP = proj * view  (multiplicação manual, colunas-maior Unity)
//
// Notas sobre value types (Matrix4x4):
//   il2cpp_runtime_invoke retorna Il2CppObject*. Para value types, o runtime
//   retorna um objeto boxed: Il2CppObject header (16 bytes) seguido do valor.
//   Matrix4x4 começa em retval + 16.
//   Se a build retornar NULL para value types (algumas versões Unity IL2CPP não boxam),
//   o fallback é usar Camera.projectionMatrix via il2cpp_field_get_value se
//   o campo for exposto como campo de instância. Em último caso usamos
//   Camera.WorldToScreenPoint direto (ff_w2s_invoke).

#include "../include/ff_camera.h"
#include "../include/ff_il2cpp_resolver.h"
#include "../include/ff_math.h"
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tipos IL2CPP opacos (mesmos do resolver mas locais a este TU)
// ---------------------------------------------------------------------------
typedef void  Il2CppDomain;
typedef void  Il2CppImage;
typedef void  Il2CppClass;
typedef void  MethodInfo;

// Il2CppObject header = 16 bytes arm64
#define CAM_OBJ_HEADER 16

// ---------------------------------------------------------------------------
// Estado do módulo
// ---------------------------------------------------------------------------
static bool s_init_done       = false;
static bool s_usable          = false; // true se pelo menos um método foi resolvido

// MethodInfo* resolvidos em ff_camera_init
static MethodInfo* s_mi_get_main            = NULL;
static MethodInfo* s_mi_get_proj            = NULL; // get_projectionMatrix
static MethodInfo* s_mi_get_view            = NULL; // get_worldToCameraMatrix
static MethodInfo* s_mi_w2s                 = NULL; // WorldToScreenPoint (opcional)

// VP matrix em cache (atualizada por ff_camera_update)
static Matrix4x4   s_vp_cache;
static bool        s_vp_valid  = false;

// Posição da câmera extraída da view matrix
static Vec3        s_cam_pos   = {0};

// ---------------------------------------------------------------------------
// Helpers: lê Matrix4x4 de um retval boxed (Il2CppObject* com header 16 bytes)
// ---------------------------------------------------------------------------
static bool extract_matrix(void* obj_ptr, Matrix4x4* out) {
    if (!obj_ptr) return false;
    uintptr_t addr = (uintptr_t)obj_ptr;
    // Mínima validação: endereço alinhado, não nulo
    if (addr < 0x1000UL || (addr & 0x3) != 0) return false;

    // Value type boxed: Matrix4x4 começa em offset 16 (após klass + monitor)
    const float* src = (const float*)((uint8_t*)obj_ptr + CAM_OBJ_HEADER);
    // Matrix4x4 = 16 floats
    for (int i = 0; i < 16; i++) {
        // Rejeita NaN ou infinito — indica leitura de memória inválida
        float v = src[i];
        if (v != v || v > 1e12f || v < -1e12f) {
            // Permite zeros e números plausíveis; rejeita só valores extremos
            if (v != v) return false; // NaN explícito
        }
    }
    memcpy(out, src, sizeof(Matrix4x4));
    return true;
}

// ---------------------------------------------------------------------------
// Multiplicação de matrizes coluna-maior (Unity padrão): result = a * b
// Unity ordena Matrix4x4.m como m[col][row] internamente, mas no layout
// de memória os 16 floats são row0col0, row0col1, ... row3col3 (row-major
// em memória, lidos col-major pela GPU). Verificar com il2cpp_field_get_offset
// se necessário. Aqui assume row-major no array [4][4].
// ---------------------------------------------------------------------------
static void mat4_mul(const Matrix4x4 *a, const Matrix4x4 *b, Matrix4x4 *out) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a->m[row][k] * b->m[k][col];
            out->m[row][col] = s;
        }
    }
}

// ---------------------------------------------------------------------------
// Extrai posição da câmera da view matrix (worldToCamera).
// A view matrix é o inverso da model matrix da câmera.
// Para uma view matrix M (rotation+translation), a posição no espaço mundo é:
//   pos = -(R^T * T) onde R = colunas 0..2 de M, T = coluna 3 de M.
// Simplificado: cam_pos = -M^T * translation_col.
// ---------------------------------------------------------------------------
static Vec3 cam_pos_from_view(const Matrix4x4 *v) {
    // worldToCamera: coluna 3 (índice [*][3]) = -(R * camera_pos_world)
    // Para extrair camera_pos_world: pos = R^T * (-col3)
    // R^T é a transposta das 3x3 de rotação.
    // m[row][col] layout (row-major em memória):
    //   m[0][0] m[0][1] m[0][2] m[0][3]
    //   m[1][0] m[1][1] m[1][2] m[1][3]
    //   m[2][0] m[2][1] m[2][2] m[2][3]
    // Col 3 (última): m[0][3], m[1][3], m[2][3]
    float tx = v->m[0][3];
    float ty = v->m[1][3];
    float tz = v->m[2][3];
    // R^T * (-T)
    Vec3 pos;
    pos.x = -(v->m[0][0]*tx + v->m[1][0]*ty + v->m[2][0]*tz);
    pos.y = -(v->m[0][1]*tx + v->m[1][1]*ty + v->m[2][1]*tz);
    pos.z = -(v->m[0][2]*tx + v->m[1][2]*ty + v->m[2][2]*tz);
    return pos;
}

// ---------------------------------------------------------------------------
// Inicialização
// ---------------------------------------------------------------------------
bool ff_camera_init(void) {
    if (s_init_done) return s_usable;
    s_init_done = true;

    // Obtém a Camera class via IL2CPP.
    // Camera está em UnityEngine — acessamos via il2cpp_class_from_name.
    // Precisamos de uma imagem que contenha UnityEngine. O UnityFramework
    // exporta os types do engine. Usamos domain_get_assemblies e procuramos
    // a imagem "UnityEngine.CoreModule" ou similar.
    //
    // Alternativa: usar il2cpp_image_get_class_count + il2cpp_image_get_class
    // para iterar todas as classes de todas as imagens. Porém isso é pesado.
    //
    // Estratégia mais direta: UnityEngine types estão em imagens cujo nome
    // começa com "UnityEngine". Iteramos assemblies procurando "Camera".

    // Pega os exports necessários do UnityFramework
    void* uf = dlopen("@rpath/UnityFramework.framework/UnityFramework",
                      RTLD_NOLOAD | RTLD_LAZY);
    if (!uf) uf = dlopen("UnityFramework", RTLD_NOLOAD | RTLD_LAZY);
    if (!uf) return false;

    typedef void*  (*pfn_domain_get)(void);
    typedef void** (*pfn_domain_assemblies)(void*, size_t*);
    typedef void*  (*pfn_asm_image)(void*);
    typedef void*  (*pfn_class_from_name)(void*, const char*, const char*);
    typedef void*  (*pfn_method_from_name)(void*, const char*, int);
    typedef void*  (*pfn_image_get_name)(void*);

    pfn_domain_get       f_domain_get  = (pfn_domain_get)dlsym(uf, "_il2cpp_domain_get");
    pfn_domain_assemblies f_dom_asm    = (pfn_domain_assemblies)dlsym(uf, "_il2cpp_domain_get_assemblies");
    pfn_asm_image        f_asm_img     = (pfn_asm_image)dlsym(uf, "_il2cpp_assembly_get_image");
    pfn_class_from_name  f_cls_name    = (pfn_class_from_name)dlsym(uf, "_il2cpp_class_from_name");
    pfn_method_from_name f_meth_name   = (pfn_method_from_name)dlsym(uf, "_il2cpp_class_get_method_from_name");
    pfn_image_get_name   f_img_name    = (pfn_image_get_name)dlsym(uf, "_il2cpp_image_get_name");

    dlclose(uf);

    if (!f_domain_get || !f_dom_asm || !f_asm_img || !f_cls_name || !f_meth_name)
        return false;

    void* domain = f_domain_get();
    if (!domain) return false;

    size_t asm_count = 0;
    void** assemblies = (void**)f_dom_asm(domain, &asm_count);
    if (!assemblies || asm_count == 0) return false;

    // Procura Camera class em qualquer imagem que contenha "UnityEngine" no nome
    // Namespaces tentados: "UnityEngine" (normal) e "" (algumas builds IL2CPP)
    Il2CppClass* cam_klass = NULL;
    for (size_t i = 0; i < asm_count && !cam_klass; i++) {
        if (!assemblies[i]) continue;
        Il2CppImage* img = (Il2CppImage*)f_asm_img(assemblies[i]);
        if (!img) continue;

        // Verifica nome da imagem (opcional, para acelerar)
        if (f_img_name) {
            const char* iname = (const char*)f_img_name(img);
            if (iname) {
                // Pula imagens claramente não-Unity (evita iterar tudo)
                if (iname[0] == 'C' && iname[1] == 'O' && iname[2] == 'W')
                    continue; // imagem do jogo, Camera não está aqui
            }
        }

        cam_klass = (Il2CppClass*)f_cls_name(img, "UnityEngine", "Camera");
        if (!cam_klass)
            cam_klass = (Il2CppClass*)f_cls_name(img, "", "Camera");
    }

    if (!cam_klass) return false;

    // Resolve métodos
    // -1 = qualquer número de parâmetros; usamos 0 para getters sem args
    s_mi_get_main = (MethodInfo*)f_meth_name(cam_klass, "get_main", 0);
    if (!s_mi_get_main)
        s_mi_get_main = (MethodInfo*)f_meth_name(cam_klass, "get_current", 0);

    s_mi_get_proj = (MethodInfo*)f_meth_name(cam_klass, "get_projectionMatrix", 0);
    s_mi_get_view = (MethodInfo*)f_meth_name(cam_klass, "get_worldToCameraMatrix", 0);
    s_mi_w2s      = (MethodInfo*)f_meth_name(cam_klass, "WorldToScreenPoint", 1);
    if (!s_mi_w2s)
        s_mi_w2s  = (MethodInfo*)f_meth_name(cam_klass, "WorldToScreenPoint", -1);

    // Usável se tivermos pelo menos get_main + uma das matrizes
    s_usable = (s_mi_get_main != NULL) &&
               (s_mi_get_proj != NULL || s_mi_get_view != NULL);

    return s_usable;
}

// ---------------------------------------------------------------------------
// Update: lê matrizes da câmera e recalcula VP
// ---------------------------------------------------------------------------
void ff_camera_update(void) {
    if (!s_usable) return;

    // Invoca Camera.main (método estático — obj=NULL)
    void* cam_inst = ff_il2cpp_invoke_method(s_mi_get_main, NULL, NULL);
    if (!cam_inst) {
        s_vp_valid = false;
        return;
    }

    Matrix4x4 proj = {0};
    Matrix4x4 view = {0};
    bool got_proj = false;
    bool got_view = false;

    // Camera.projectionMatrix (instance method)
    if (s_mi_get_proj) {
        void* ret = ff_il2cpp_invoke_method(s_mi_get_proj, cam_inst, NULL);
        got_proj = extract_matrix(ret, &proj);
    }

    // Camera.worldToCameraMatrix (instance method)
    if (s_mi_get_view) {
        void* ret = ff_il2cpp_invoke_method(s_mi_get_view, cam_inst, NULL);
        got_view = extract_matrix(ret, &view);
    }

    if (got_proj && got_view) {
        // VP = proj * view  (ordem Unity: proj aplicado após view)
        mat4_mul(&proj, &view, &s_vp_cache);
        s_vp_valid = true;
        s_cam_pos  = cam_pos_from_view(&view);
    } else if (got_view) {
        // Sem proj mas temos view: guarda a view e marca incompleto
        // (ff_w2s_vp usará apenas se proj também disponível — não faz projeção sem proj)
        s_vp_valid = false;
        s_cam_pos  = cam_pos_from_view(&view);
    } else {
        s_vp_valid = false;
    }
}

// ---------------------------------------------------------------------------
// World-to-screen via VP matrix em cache
// ---------------------------------------------------------------------------
bool ff_w2s_vp(const Vec3 *world_pos,
               float screen_w, float screen_h,
               float *out_x, float *out_y) {
    if (!s_vp_valid || !world_pos || !out_x || !out_y) return false;
    if (screen_w <= 0.0f || screen_h <= 0.0f) return false;
    return world_to_screen(&s_vp_cache, world_pos, screen_w, screen_h, out_x, out_y);
}

// ---------------------------------------------------------------------------
// World-to-screen via Camera.WorldToScreenPoint (il2cpp_runtime_invoke)
// Mais lento: uma chamada IL2CPP por ponto. Usar como fallback quando
// VP matrix não estiver disponível no frame.
// ---------------------------------------------------------------------------
bool ff_w2s_invoke(const Vec3 *world_pos,
                   float screen_w, float screen_h,
                   float *out_x, float *out_y) {
    if (!s_mi_get_main || !s_mi_w2s || !world_pos || !out_x || !out_y) return false;

    void* cam_inst = ff_il2cpp_invoke_method(s_mi_get_main, NULL, NULL);
    if (!cam_inst) return false;

    // WorldToScreenPoint recebe um Vector3. Em IL2CPP value types são passados
    // como ponteiro para o valor quando boxed. Alocamos um buffer local
    // e passamos como args[0] = &world_pos.
    //
    // NOTA: a convenção exata (boxed ou não) depende da build IL2CPP e do
    // tipo do parâmetro. Se a chamada retornar lixo, conferir com Frida:
    // il2cpp_method_get_param_type(s_mi_w2s, 0) → type enum para Vector3.
    // Se for VALUETYPE (0x11), o runtime espera Il2CppObject* (boxado).
    // Se for CLASS (0x12), espera ponteiro direto. Testamos a abordagem
    // "ponteiro direto para Vec3 no stack" primeiro — funciona na maioria
    // das builds Unity 2021 arm64.
    void* args[1];
    // Cria cópia local do Vec3 para garantir alinhamento e evitar alias
    Vec3 local_pos = *world_pos;
    args[0] = &local_pos;

    void* ret = ff_il2cpp_invoke_method(s_mi_w2s, cam_inst, args);
    if (!ret) return false;

    // Resultado é Vector3 boxed: float[3] em ret + 16
    // Vector3 screen: x = pixel, y = pixel (Unity y=0 é embaixo), z = profundidade
    const float* rv = (const float*)((uint8_t*)ret + CAM_OBJ_HEADER);
    float sx = rv[0];
    float sy = rv[1];
    // float sz = rv[2]; // profundidade — não usado aqui

    // Validação
    if (sx != sx || sy != sy) return false; // NaN
    if (sx < -screen_w * 2.0f || sx > screen_w * 3.0f) return false; // fora razoável

    // Unity: y=0 = embaixo da tela. Converte para y=0=cima (padrão ImGui/UIKit)
    *out_x = sx;
    *out_y = screen_h - sy;
    return true;
}

// ---------------------------------------------------------------------------
// Acessores
// ---------------------------------------------------------------------------
const Matrix4x4* ff_camera_get_vp(void) {
    return s_vp_valid ? &s_vp_cache : NULL;
}

Vec3 ff_camera_get_pos(void) {
    return s_cam_pos;
}
