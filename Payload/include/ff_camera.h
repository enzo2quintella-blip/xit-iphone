// ff_camera.h — obtém e mantém a ViewProjection matrix da câmera Unity via IL2CPP.
//
// Design:
//   Camera.main é obtido via il2cpp_runtime_invoke em Camera.get_main() (ou
//   "get_main" com 0 parâmetros). A partir da instância Camera:
//     VP = Camera.projectionMatrix * Camera.worldToCameraMatrix
//   Ambas são obtidas via il2cpp_runtime_invoke retornando Matrix4x4 (value type
//   boxed em Il2CppObject).
//
//   Duas funções de world-to-screen são expostas:
//     ff_w2s_vp()  — usa a VP matrix em cache (atualizada por ff_camera_update)
//     ff_w2s_invoke() — chama Camera.WorldToScreenPoint via il2cpp_runtime_invoke
//                       (mais lenta mas não depende de matrix em cache)
//
// Uso:
//   1. Chame ff_camera_init() uma vez após ff_il2cpp_resolve() ter sucedido.
//   2. Chame ff_camera_update() a cada frame (dentro do hook de LateUpdate/Present).
//   3. Chame ff_w2s_vp() para converter posições de mundo em coordenadas de tela.
//
// Limitações:
//   - fn_WorldToScreen = OFFSET_UNKNOWN neste build (Beebyte zeroed all RVAs).
//     ff_camera_init() usa scan por nome ("Camera" class + "get_main", "WorldToScreenPoint").
//   - projectionMatrix e worldToCameraMatrix são value types (Matrix4x4). O IL2CPP
//     retorna value types como pré-alocado em Il2CppObject* quando boxed.
//     ff_camera_update() usa um buffer alocado para receber o resultado.
//   - Se il2cpp_runtime_invoke não estiver disponível o módulo desabilita a si
//     mesmo e as chamadas ff_w2s_* retornam false.

#pragma once
#include "../../Shared/include/ff_types.h"
#include <stdbool.h>

// Inicializa o módulo de câmera.
// Deve ser chamado após ff_il2cpp_resolve() ter retornado true.
// Resolve:
//   UnityEngine.Camera class → get_main() MethodInfo, WorldToScreenPoint MethodInfo,
//   projectionMatrix MethodInfo, worldToCameraMatrix MethodInfo.
// Retorna true se pelo menos os métodos de matrix foram resolvidos.
// Se retornar false, ff_w2s_vp sempre retorna false.
bool ff_camera_init(void);

// Atualiza a ViewProjection matrix em cache a partir da câmera principal.
// Deve ser chamado a cada frame (hook de LateUpdate ou Metal Present).
// Se Camera.main for NULL (fora de partida), não faz nada.
void ff_camera_update(void);

// Converte uma posição de mundo em coordenadas de tela usando a VP matrix em cache.
// screen_w / screen_h: resolução atual (pixels).
// out_x, out_y: coordenadas de saída em pixels, origem canto superior esquerdo.
// Retorna false se ponto atrás da câmera, câmera não inicializada ou matrix inválida.
bool ff_w2s_vp(const Vec3 *world_pos,
               float screen_w, float screen_h,
               float *out_x, float *out_y);

// Converte usando Camera.WorldToScreenPoint via il2cpp_runtime_invoke.
// Mais lento que ff_w2s_vp() (uma chamada IL2CPP por ponto) mas não depende
// de que a VP matrix tenha sido atualizada neste frame.
// out_x, out_y: mesma convenção que ff_w2s_vp.
// Retorna false se câmera não disponível ou il2cpp_runtime_invoke não bindado.
bool ff_w2s_invoke(const Vec3 *world_pos,
                   float screen_w, float screen_h,
                   float *out_x, float *out_y);

// Retorna a VP matrix em cache (última atualizada por ff_camera_update).
// Ponteiro para dados internos — válido apenas durante o frame atual.
const Matrix4x4* ff_camera_get_vp(void);

// Retorna a posição da câmera no espaço mundo (extraída de worldToCameraMatrix).
// Válida após ff_camera_update().
Vec3 ff_camera_get_pos(void);
