// ff_il2cpp_resolver.h
// Discovers game field offsets at runtime using IL2CPP exported API.
// Writes results directly into g_offsets.st (FF_StructOffsets).
//
// All IL2CPP API addresses are resolved at runtime via dlsym against
// UnityFramework — the binary exports them publicly.  Confirmed RVAs
// (measured from the actual IPA binary, symtab scan):
//
//   _il2cpp_domain_get              @ 0xA111B28
//   _il2cpp_domain_get_assemblies   @ 0xA111B34
//   _il2cpp_assembly_get_image      @ (confirmed present)
//   _il2cpp_image_get_name          @ 0xA1122EC
//   _il2cpp_image_get_class         @ 0xA112310
//   _il2cpp_image_get_class_count   @ 0xA1122F8
//   _il2cpp_class_get_name          @ 0xA111634
//   _il2cpp_class_get_namespace     @ 0xA111638
//   _il2cpp_class_get_fields        @ 0xA111610
//   _il2cpp_class_instance_size     @ 0xA111644
//   _il2cpp_field_get_name          @ 0xA111D28
//   _il2cpp_field_get_type          @ 0xA111D38
//   _il2cpp_field_get_offset        @ 0xA111D34
//   _il2cpp_type_get_type           @ (confirmed present)
//   _il2cpp_runtime_invoke          @ (confirmed present)
//
// No jailbreak API is used here.

#pragma once
#include "../../Shared/include/ff_offsets.h"
#include <stdbool.h>
#include <stddef.h>

// Waits up to timeout_sec for il2cpp_domain_get() to return non-null.
// Returns false on timeout.
bool ff_il2cpp_wait_ready(unsigned timeout_sec);

// Resolves game field offsets and fills offsets->st (struct fields) and
// offsets->ga.fn_GameUpdate (LateUpdate method function pointer stored as a
// module-relative RVA, so hooks.c can call ff_rva_to_va() on it).
// Returns true when at least HP + position offsets were discovered.
// fn_GameUpdate may remain 0 on return if the Player class has no LateUpdate
// method; the config-file fallback in payload_main.c covers that case.
// Safe to call multiple times; returns cached result after first success.
bool ff_il2cpp_resolve(FF_Offsets *offsets);

// ─── Cached IL2CPP objects (available after ff_il2cpp_resolve succeeds) ──────

// MethodInfo* for GetLocalPlayer / GetAllPlayers — NULL if not found.
// Pass to ff_il2cpp_invoke_method(). obj=NULL for static methods.
void* ff_il2cpp_get_mi_local_player(void);
void* ff_il2cpp_get_mi_all_players(void);

// Pointers to IL2CPP class descriptors (Il2CppClass*).
void* ff_il2cpp_get_player_class(void);
void* ff_il2cpp_get_avmgr_class(void);

// AvatarManager singleton instance — may be NULL if static field scan failed.
// Updated on first valid read; re-call after each game phase transition.
void* ff_il2cpp_get_avmgr_singleton(void);

// Thin wrapper around il2cpp_runtime_invoke.
// method_info : MethodInfo* obtained from class_get_method_from_name.
// obj         : instance object, or NULL for static methods.
// args        : array of void* arguments (boxed value types), or NULL.
// Returns the return value as an Il2CppObject*, or NULL on exception / unavailable.
void* ff_il2cpp_invoke_method(void* method_info, void* obj, void** args);
