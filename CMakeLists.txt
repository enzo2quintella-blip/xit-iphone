cmake_minimum_required(VERSION 3.21)
project(FFCheat LANGUAGES C CXX OBJC)

# ─── Toolchain iOS arm64 ──────────────────────────────────────────────────────
# Compilar com:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/ios_toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 20)

# ─── Paths ────────────────────────────────────────────────────────────────────
set(SHARED_DIR       ${CMAKE_SOURCE_DIR}/Shared)
set(PAYLOAD_DIR      ${CMAKE_SOURCE_DIR}/Payload)
set(KEXPLOIT_DIR     ${CMAKE_SOURCE_DIR}/KernelExploit)
set(INJECTOR_DIR     ${CMAKE_SOURCE_DIR}/InjectorApp)

# ─── Flags comuns ─────────────────────────────────────────────────────────────
set(COMMON_FLAGS
    -arch arm64
    -miphoneos-version-min=17.0
    -fvisibility=hidden      # não exporta símbolos desnecessariamente
    -fstack-protector-strong
    -O2
    -Wall -Wextra
)

# Flags de release: strip total + sem info de debug no binário.
# Isso elimina nomes de funções, variáveis, caminhos de arquivo e nomes de seção.
# Em Debug compilamos com FF_DEBUG para manter os logs de stderr.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND COMMON_FLAGS
        -g0                        # sem info de debug DWARF
        -fno-unwind-tables         # sem .eh_frame / compact unwind
        -fno-asynchronous-unwind-tables
        -DNDEBUG                   # desativa assert()
    )
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND COMMON_FLAGS -DFF_DEBUG)
endif()

string(JOIN " " COMMON_FLAGS_STR ${COMMON_FLAGS})

# ─── KernelExploit — implementação real ──────────────────────────────────────
# kexploit_impl.c: socket spray + IOSurface KRW + sandbox escape (iOS 17-26.x)
# kexploit_stub.c: fallback que retorna KE_NOT_AVAILABLE (mantido para debug)
add_library(kexploit_stub STATIC
    ${KEXPLOIT_DIR}/src/kexploit_impl.c
)
target_include_directories(kexploit_stub PUBLIC ${KEXPLOIT_DIR}/include)
target_compile_options(kexploit_stub PRIVATE ${COMMON_FLAGS})
target_link_libraries(kexploit_stub PUBLIC
    "-framework IOSurface"
    "-framework CoreFoundation"
)

# ─── Payload dylib ────────────────────────────────────────────────────────────
add_library(FFPayload SHARED
    ${PAYLOAD_DIR}/src/ff_module.c
    ${PAYLOAD_DIR}/src/ff_offsets.c
    ${PAYLOAD_DIR}/src/ff_game.c
    ${PAYLOAD_DIR}/src/ff_il2cpp_resolver.c
    ${PAYLOAD_DIR}/src/ff_camera.c
    ${PAYLOAD_DIR}/src/hooks.c
    ${PAYLOAD_DIR}/src/antidetect.c
    ${PAYLOAD_DIR}/src/payload_main.m
    ${PAYLOAD_DIR}/src/overlay.m
    ${PAYLOAD_DIR}/features/esp.c
    ${PAYLOAD_DIR}/features/aimbot.c
    ${PAYLOAD_DIR}/features/godmode.c
    ${PAYLOAD_DIR}/features/ammo.c
    ${PAYLOAD_DIR}/features/speed.c
)

target_include_directories(FFPayload PRIVATE
    ${SHARED_DIR}/include
    ${PAYLOAD_DIR}/include
    ${PAYLOAD_DIR}/src
    ${KEXPLOIT_DIR}/include
)

target_compile_options(FFPayload PRIVATE ${COMMON_FLAGS})

target_link_libraries(FFPayload PRIVATE
    kexploit_stub
    "-framework UIKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework Foundation"
)

# Sem prefixo "lib" no output (queremos FFPayload.dylib, não libFFPayload.dylib)
set_target_properties(FFPayload PROPERTIES
    PREFIX ""
    OUTPUT_NAME "MetalCaptureSupport"   # nome que se passa por suporte Metal legítimo
    MACOSX_BUNDLE FALSE
    # install_name deve ser @rpath/MetalCaptureSupport.dylib para o dyld
    # resolver corretamente via LC_RPATH do executável principal.
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_NAME_DIR "@rpath"
)

# Strip de símbolos no dylib de release: -x remove símbolos locais,
# -S remove debug, -dead_strip elimina código morto.
# Em Debug mantemos tudo para símbolos no crash log.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_options(FFPayload PRIVATE
        -Wl,-x           # strip símbolos locais do output
        -Wl,-S           # strip debug symbols
        -Wl,-dead_strip  # remove código não referenciado
    )
endif()

# ─── InjectorApp (app iOS) ────────────────────────────────────────────────────
add_executable(FFCheat MACOSX_BUNDLE
    ${INJECTOR_DIR}/src/main.m
    ${INJECTOR_DIR}/src/AppDelegate.m
)

target_include_directories(FFCheat PRIVATE
    ${INJECTOR_DIR}/src
    ${KEXPLOIT_DIR}/include
    ${SHARED_DIR}/include
)

target_compile_options(FFCheat PRIVATE ${COMMON_FLAGS})

target_link_libraries(FFCheat PRIVATE
    kexploit_stub
    "-framework UIKit"
    "-framework Foundation"
    "-framework IOSurface"
    "-framework CoreFoundation"
)

# Adiciona @rpath apontando para o diretório do executável.
# Isso faz o dyld resolver @rpath/MetalCaptureSupport.dylib como
# <bundle>/FFCheat.app/MetalCaptureSupport.dylib em runtime.
target_link_options(FFCheat PRIVATE
    -Wl,-rpath,@executable_path
)

set_target_properties(FFCheat PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.nubank.mobile.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${INJECTOR_DIR}/Info.plist"
)

# Copia MetalCaptureSupport.dylib para dentro do bundle (ao lado do executável)
add_custom_command(TARGET FFCheat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:FFPayload>
        $<TARGET_BUNDLE_CONTENT_DIR:FFCheat>/MetalCaptureSupport.dylib
    COMMENT "Copiando MetalCaptureSupport.dylib para o bundle"
)
