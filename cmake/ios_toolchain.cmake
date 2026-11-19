# ios_toolchain.cmake — toolchain para iOS arm64 com Xcode/xcrun
# Uso: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/ios_toolchain.cmake ...

set(CMAKE_SYSTEM_NAME      "Darwin")
set(CMAKE_SYSTEM_PROCESSOR "arm64")
set(IOS_PLATFORM           "OS64" CACHE STRING "iOS platform")

# Detecta Xcode SDK
execute_process(
    COMMAND xcrun --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE IOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT IOS_SDK_PATH)
    message(FATAL_ERROR "Xcode iOS SDK não encontrado. Instale Xcode e rode 'xcode-select --install'")
endif()

set(CMAKE_OSX_SYSROOT       "${IOS_SDK_PATH}")
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0")

execute_process(
    COMMAND xcrun --sdk iphoneos --find clang
    OUTPUT_VARIABLE XCRUN_CLANG
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_C_COMPILER   "${XCRUN_CLANG}")
set(CMAKE_CXX_COMPILER "${XCRUN_CLANG}++")
set(CMAKE_OBJC_COMPILER "${XCRUN_CLANG}")

set(CMAKE_C_FLAGS_INIT   "-arch arm64 -miphoneos-version-min=17.0")
set(CMAKE_CXX_FLAGS_INIT "-arch arm64 -miphoneos-version-min=17.0")

# Evita cmake testar o linker com um executável host
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
