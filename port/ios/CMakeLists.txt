# Build the iPhone/iPad (or Apple TV, or Apple Vision Pro) host and SDL around the statically compiled guest image.
cmake_minimum_required(VERSION 3.24)
project(VISR LANGUAGES C CXX OBJC ASM)
# theater mode's immersive space is SwiftUI (host/Theater.swift)
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    enable_language(Swift)
endif()
set(HALO_BUNDLE_IDENTIFIER "org.steverice.visr" CACHE STRING "App bundle identifier")
# Mac Catalyst (tools/ios_build.py --mac): configured as iOS, built with -sdk macosx SUPPORTS_MACCATALYST=YES
option(HALO_MAC "Mac Catalyst build of the iOS host" OFF)
set(HALO_GLES_FRAMEWORKS "" CACHE PATH "Catalyst: folder with OpenGLES.framework (headers and a macCatalyst link stub)")
# iCloud saves (host/host_cloud_saves.m): off, the app has no CloudKit code, entitlement or Settings rows.
# On needs the iCloud capability with CloudKit and this container on the App ID (docs/cloud-saves.md).
option(HALO_CLOUD_SAVES "Sync saves through the player's private iCloud database" OFF)
set(HALO_CLOUD_CONTAINER "iCloud.org.steverice.visr" CACHE STRING "iCloud container for HALO_CLOUD_SAVES")
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/build/third_party/SDL3" sdl3 EXCLUDE_FROM_ALL)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # SDL's CMake forces SDL_OPENGLES off for tvOS, but its UIKit GL ES code is
    # shared with iOS and tvOS still ships OpenGLES.framework (SDL's own Xcode
    # config, SDL_build_config_ios.h, enables it). The sources are always
    # compiled; only these macros gate them.
    target_compile_definitions(SDL3-static PRIVATE SDL_VIDEO_OPENGL_ES=1 SDL_VIDEO_OPENGL_ES2=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    # SDL's CMake ties its virtual-joystick driver to HIDAPI, which is off on
    # visionOS, but the driver doesn't use HIDAPI. host/host_tv.m's player-one
    # pad is a virtual joystick, so compile it in as tvOS gets it.
    target_sources(SDL3-static PRIVATE "${ROOT}/build/third_party/SDL3/src/joystick/virtual/SDL_virtualjoystick.c")
    target_compile_definitions(SDL3-static PRIVATE SDL_JOYSTICK_VIRTUAL=1)
endif()
if(HALO_MAC)
    # SDL's UIKit GL files import OpenGLES headers the MacOSX SDK lacks (tools/ios_build.py opengles_framework)
    target_compile_options(SDL3-static PRIVATE "-F${HALO_GLES_FRAMEWORKS}")
    target_compile_definitions(SDL3-static PRIVATE GLES_SILENCE_DEPRECATION=1)
endif()
# iPhone and visionOS: each build a new CFBundleVersion (tools/ios_build.py), so
# installing over the app replaces it and keeps its Documents (the imported game)
set(HALO_BUILD_NUMBER 6 CACHE STRING "CFBundleVersion")
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # Controllers only, no Files app: the XISO arrives from a browser
    # (host/host_tv_import.m).
    set(HALO_TVOS ON)
    # 4K fill rate is the bottleneck in heavy fights on an Apple TV 4K (3rd gen);
    # there's no Files app to edit config.toml, so the build picks it (0 = native).
    set(HALO_RENDER_HEIGHT 1080 CACHE STRING "Internal render height (display.render_height)")
    # Brand assets (layered icon, top shelf) made from the iOS AppIcon artwork.
    set(HALO_PLATFORM_SOURCES Assets-tvOS.xcassets host/host_tv.m host/host_tv_import.m)
    set(HALO_INFO_PLIST Info-tvOS.plist)
    set(HALO_DEVICE_FAMILY 3)
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    # A window, played with a game controller (host/host_tv.m's input), the
    # game imported through Files or the Documents folder (host/host_import.m).
    # visionOS has no OpenGL ES: Metal is the only renderer.
    set(HALO_VISIONOS ON)
    # native (0): the window's drawable, about 2560x1440 at its default size,
    # following the window when it's resized (d3d8_device.c's halo_screen_commit)
    set(HALO_RENDER_HEIGHT 0 CACHE STRING "Internal render height (display.render_height)")
    option(HALO_EXTENDED_VIRTUAL_ADDRESSING "Sign with the extended virtual addressing entitlement" OFF)
    set(HALO_PLATFORM_SOURCES Assets-visionOS.xcassets host/host_tv.m host/host_import.m host/host_orientation.m
        host/host_theater.m host/Theater.swift host/host_texture_settings.m host/texture_refine.m)
    set(HALO_INFO_PLIST Info-visionOS.plist)
    set(HALO_DEVICE_FAMILY 7)
else()
    set(HALO_PLATFORM_SOURCES Assets.xcassets host/host_import.m host/host_orientation.m host/host_touch.m
        host/host_texture_settings.m host/texture_refine.m)
    if(HALO_MAC)
        # Mac Catalyst: Info.plist without the iOS-only keys (LSRequiresIPhoneOS, UIRequiresFullScreen, Files sharing)
        set(HALO_INFO_PLIST Info-macOS.plist)
    else()
        set(HALO_INFO_PLIST Info.plist)
    endif()
    set(HALO_DEVICE_FAMILY "1,2")
endif()
# the texture upscale policy (port/assets/texture-policy.json) as a C table for port/runtime/texture_policy.c
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# the app's page in the Settings app; a build with iCloud saves takes a copy with their rows added
set(HALO_SETTINGS_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle")
if(HALO_CLOUD_SAVES AND NOT HALO_TVOS)
    set(HALO_SETTINGS_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/cloud/Settings.bundle")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${ROOT}/tools/settings_bundle.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle" "${HALO_SETTINGS_BUNDLE}" COMMAND_ERROR_IS_FATAL ANY)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    COMMAND Python3::Interpreter "${ROOT}/tools/embed_texture_policy.py"
            "${ROOT}/port/assets/texture-policy.json" "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    DEPENDS "${ROOT}/tools/embed_texture_policy.py" "${ROOT}/port/assets/texture-policy.json"
    COMMENT "Embedding the texture upscale policy")
add_executable(VISR MACOSX_BUNDLE
    ${HALO_PLATFORM_SOURCES} "${ROOT}/port/runtime/xiso.c"
    "${ROOT}/port/runtime/texture_policy.c" "${ROOT}/port/runtime/texture_cache.c"
    "${CMAKE_CURRENT_BINARY_DIR}/texture_policy_table.c"
    host/host_main.m host/host_memory.c host/host_loader.c host/host_syscall.c host/guest_call.S
    host/host_thread.c host/host_sdl.c host/host_gpu.c host/host_gpu_dispatch.c host/host_frame_pacer.c host/gpu_metal.m host/host_frame_counter.m host/host_lifecycle.m
    host/host_join_link.c host/host_config.c
    host/host_stereo.m host/host_stereo_head.c host/host_stereo_hud.c
    "${ROOT}/port/linux/src/posix_files.c" host/posix_net.c
    "${ROOT}/build/ios/host/bridges.c" "${ROOT}/build/ios/embedded/guest_image.S")
target_include_directories(VISR PRIVATE host "${ROOT}/port/runtime" "${ROOT}/port/runtime/include"
    "${ROOT}/port/runtime/guest/runtime" "${ROOT}/port/linux/src" "${ROOT}/build/ios/host"
    "${ROOT}/build/ios/embedded")
# WebRTC peers use DTLS in the host; generated posix bridges translate guest buffers.
file(GLOB HALO_MBEDTLS_SOURCES CONFIGURE_DEPENDS "${ROOT}/port/third_party/mbedtls/library/*.c")
target_sources(VISR PRIVATE "${ROOT}/port/linux/src/posix_dtls.c" ${HALO_MBEDTLS_SOURCES})
target_include_directories(VISR PRIVATE "${ROOT}/port/third_party/mbedtls/include")
target_compile_definitions(VISR PRIVATE HALO_IOS=1 st_mtim=st_mtimespec st_atim=st_atimespec st_ctim=st_ctimespec)
# The Metal renderer's presentation pacing (host/gpu_metal.m: frames in flight,
# a minimum time on screen, the game's 60 Hz blank following the display link;
# tools/ios_build.py --frame-pacing). On by default except for a Vision Pro,
# whose frames the Compositor paces: its build then has none of it.
if(HALO_VISIONOS)
    option(HALO_FRAME_PACING "Compile in the Metal renderer's presentation pacing" OFF)
else()
    option(HALO_FRAME_PACING "Compile in the Metal renderer's presentation pacing" ON)
endif()
if(HALO_FRAME_PACING)
    target_compile_definitions(VISR PRIVATE HALO_FRAME_PACING=1)
else()
    target_compile_definitions(VISR PRIVATE HALO_FRAME_PACING=0)
endif()
target_compile_options(VISR PRIVATE "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wno-deprecated-declarations>")
if(NOT HALO_VISIONOS)
    # the GL backend (step 4): OpenGL ES through gl.h's pointer table, resolved by name
    target_sources(VISR PRIVATE "${ROOT}/port/linux/src/gpu_gl.c" "${ROOT}/port/linux/src/gl_functions.c")
    target_include_directories(VISR PRIVATE "${ROOT}/build/ios/gl_include")
    set_source_files_properties("${ROOT}/port/linux/src/gpu_gl.c" "${ROOT}/port/linux/src/gl_functions.c"
        PROPERTIES COMPILE_DEFINITIONS GPU_GL_HOST=1)
    target_link_libraries(VISR PRIVATE "-framework OpenGLES")
    if(HALO_MAC)
        # SDL's -framework OpenGLES resolves to the macCatalyst stub here
        target_link_options(VISR PRIVATE "-F${HALO_GLES_FRAMEWORKS}")
    endif()
endif()
target_compile_options(VISR PRIVATE "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>")
# Profile-guided optimization of the host and SDL (tools/ios_build.py --host-pgo): "use" compiles with the
# profile in HALO_HOST_PROFILE (pgo/visr_host.profdata, recorded by tools/host_pgo_train.py); "generate" builds
# the instrumented app that records one, writing raw profiles to the pattern in HALO_HOST_PROFILE when it exits.
set(HALO_HOST_PGO "off" CACHE STRING "Host profile-guided optimization: off, use or generate")
set(HALO_HOST_PROFILE "" CACHE STRING "use: the profile; generate: the raw profiles' file name pattern")
if(HALO_HOST_PGO STREQUAL "generate")
    set(HALO_HOST_PGO_FLAGS "-fprofile-instr-generate=${HALO_HOST_PROFILE}")
    target_link_options(VISR PRIVATE "-fprofile-instr-generate")
elseif(HALO_HOST_PGO STREQUAL "use")
    # a function changed since the recording, or absent from it, is compiled as without a profile
    set(HALO_HOST_PGO_FLAGS "-fprofile-instr-use=${HALO_HOST_PROFILE}" -Wno-profile-instr-out-of-date
        -Wno-profile-instr-unprofiled)
else()
    set(HALO_HOST_PGO_FLAGS "")
endif()
foreach(flag ${HALO_HOST_PGO_FLAGS})
    target_compile_options(VISR PRIVATE "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:${flag}>")
    target_compile_options(SDL3-static PRIVATE "${flag}")
endforeach()
target_link_libraries(VISR PRIVATE SDL3::SDL3-static "-framework Foundation" "-framework UIKit"
    "-framework Metal" "-framework QuartzCore" z)
# display.upscaler = "metalfx" (gpu_metal.m); the simulators have no MetalFX
if(NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    target_link_libraries(VISR PRIVATE "-weak_framework MetalFX")
endif()
if(HALO_TVOS)
    target_link_libraries(VISR PRIVATE "-framework Network" "-framework CoreImage")
    set_source_files_properties(Assets-tvOS.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(HALO_VISIONOS)
    target_link_libraries(VISR PRIVATE "-framework UniformTypeIdentifiers" "-framework CompositorServices"
        "-framework ARKit" "-framework SwiftUI")
    set_source_files_properties(Assets-visionOS.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # the app's page in the Settings app (host/host_texture_settings.m)
    target_sources(VISR PRIVATE "${HALO_SETTINGS_BUNDLE}")
    set_source_files_properties("${HALO_SETTINGS_BUNDLE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Theater.swift calls host_theater.m's C functions
    set_target_properties(VISR PROPERTIES
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/host/host_theater.h"
        XCODE_ATTRIBUTE_SWIFT_VERSION 5.0)
    # the guest pool can grow to about 1.9 GB; extended virtual addressing only
    # if a device refuses the 4 GB arena (tools/ios_build.py)
    if(HALO_EXTENDED_VIRTUAL_ADDRESSING)
        set(HALO_ENTITLEMENTS VISR-visionOS-extended.entitlements)
    else()
        set(HALO_ENTITLEMENTS VISR-visionOS.entitlements)
    endif()
    set_target_properties(VISR PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_ENTITLEMENTS}")
else()
    target_link_libraries(VISR PRIVATE "-framework UniformTypeIdentifiers")
    set_source_files_properties(Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # the app's page in the Settings app (host/host_texture_settings.m)
    target_sources(VISR PRIVATE "${HALO_SETTINGS_BUNDLE}")
    set_source_files_properties("${HALO_SETTINGS_BUNDLE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
set_target_properties(VISR PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
# internet play's MQTT brokers (network.brokers_file): host_main.m writes them
# beside config.toml at each start, as upstream's Android app does
target_sources(VISR PRIVATE "${ROOT}/port/assets/network/brokers.txt")
set_source_files_properties("${ROOT}/port/assets/network/brokers.txt" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
file(GLOB HALO_LICENSES "${ROOT}/build/ios/licenses/*.txt")
target_sources(VISR PRIVATE ${HALO_LICENSES})
set_source_files_properties(${HALO_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
# the maps' shader lists, compiled at map load (d3d8_device.c's halo_shader_list_warm)
file(GLOB HALO_SHADER_LISTS CONFIGURE_DEPENDS "${ROOT}/port/shader-lists/*.txt")
target_sources(VISR PRIVATE ${HALO_SHADER_LISTS})
set_source_files_properties(${HALO_SHADER_LISTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/shader-lists)
set_target_properties(VISR PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_INFO_PLIST}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HALO_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${HALO_DEVICE_FAMILY}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_HALO_RENDER_HEIGHT "${HALO_RENDER_HEIGHT}"
    XCODE_ATTRIBUTE_HALO_BUILD_NUMBER "${HALO_BUILD_NUMBER}")
if(HALO_DEVELOPMENT_TEAM)
    set_target_properties(VISR PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${HALO_DEVELOPMENT_TEAM}")
endif()
if(HALO_CLOUD_SAVES)
    # the sync rules (port/runtime/cloud_saves.c) behind a store interface; here the store is CloudKit
    target_sources(VISR PRIVATE "${ROOT}/port/runtime/cloud_saves.c" host/cloud_store_cloudkit.m host/host_cloud_saves.m)
    target_compile_definitions(VISR PRIVATE HALO_CLOUD_SAVES=1 "HALO_CLOUD_CONTAINER=\"${HALO_CLOUD_CONTAINER}\"")
    target_link_libraries(VISR PRIVATE "-framework CloudKit")
    # the iCloud keys, added to the target's own entitlements when it has some (visionOS)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VISR-cloud.entitlements" HALO_CLOUD_ENTITLEMENTS)
    if(HALO_ENTITLEMENTS)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_ENTITLEMENTS}" HALO_BASE_ENTITLEMENTS)
        string(REGEX REPLACE "^.*<dict>(.*)</dict>.*$" "\\1" HALO_BASE_KEYS "${HALO_BASE_ENTITLEMENTS}")
        string(REPLACE "<dict>" "<dict>${HALO_BASE_KEYS}" HALO_CLOUD_ENTITLEMENTS "${HALO_CLOUD_ENTITLEMENTS}")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/VISR-cloud.entitlements" "${HALO_CLOUD_ENTITLEMENTS}")
    set_target_properties(VISR PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_BINARY_DIR}/VISR-cloud.entitlements"
        XCODE_ATTRIBUTE_HALO_CLOUD_CONTAINER "${HALO_CLOUD_CONTAINER}")
endif()
