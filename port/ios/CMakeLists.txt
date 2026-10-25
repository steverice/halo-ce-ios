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
option(HALO_NATIVE_MAC "Native macOS AppKit host" OFF)
if(HALO_NATIVE_MAC)
    if(HALO_MAC OR NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "HALO_NATIVE_MAC requires a separate macOS SDK target")
    endif()
    if(HALO_BENCHMARK_UPLOAD OR HALO_TESTER_BUILD)
        message(FATAL_ERROR "Native macOS does not support benchmark upload or tester builds")
    endif()
endif()
# tools/ios_build.py --renderer: Metal is always present; BOTH also includes OpenGL ES.
if(HALO_NATIVE_MAC OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(HALO_RENDERER_DEFAULT "METAL")
else()
    set(HALO_RENDERER_DEFAULT "BOTH")
endif()
set(HALO_RENDERER "${HALO_RENDERER_DEFAULT}" CACHE STRING "Apple renderer backends: BOTH or METAL")
set_property(CACHE HALO_RENDERER PROPERTY STRINGS BOTH METAL)
if(NOT HALO_RENDERER STREQUAL "BOTH" AND NOT HALO_RENDERER STREQUAL "METAL")
    message(FATAL_ERROR "HALO_RENDERER must be BOTH or METAL, got '${HALO_RENDERER}'")
endif()
if(HALO_NATIVE_MAC AND NOT HALO_RENDERER STREQUAL "METAL")
    message(FATAL_ERROR "Native macOS supports only HALO_RENDERER=METAL")
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS" AND NOT HALO_RENDERER STREQUAL "METAL")
    message(FATAL_ERROR "visionOS supports only HALO_RENDERER=METAL")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/RendererBackendOptions.cmake")
# tools/ios_build.py --render-validation: every call into the GPU backend is checked first
# (port/linux/src/gpu_validate.h). Off, the app's code is what it is without the option.
option(HALO_RENDER_VALIDATION "Check every call into the GPU backend (debug.render_validation)" OFF)
# The intro and credits movies, decoded with FFmpeg's Bink decoders (port/third_party/ffmpeg-bink,
# LGPL 2.1 or later). OFF leaves every FFmpeg file out of the build: the game then skips its
# movies, as it did before there was a decoder (tools/ios_build.py --bink off).
option(HALO_BINK "Play the game's Bink movies with the vendored FFmpeg decoders (LGPL)" ON)
set(HALO_GLES_FRAMEWORKS "" CACHE PATH "Catalyst: folder with OpenGLES.framework (headers and a macCatalyst link stub)")
# iCloud saves (host/host_cloud_saves.m): off, the app has no CloudKit code, entitlement or Settings rows.
# On needs the iCloud capability with CloudKit and this container on the App ID (docs/cloud-saves.md).
option(HALO_PERF_REPORTS "Collect automatic game performance reports" OFF)
option(HALO_SHADER_UPLOAD "Contribute missing Metal compilation descriptors" OFF)
option(HALO_BENCHMARK_UPLOAD "Let the device benchmark send its report to CloudKit" OFF)
option(HALO_CLOUD_SAVES "Sync saves through the player's private iCloud database" OFF)
set(HALO_CLOUD_CONTAINER "iCloud.org.steverice.visr" CACHE STRING "iCloud container for HALO_CLOUD_SAVES")
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# The game never opens a camera. SDL's CoreMedia camera driver references AVCaptureDevice, which makes
# App Store Connect demand NSCameraUsageDescription (ITMS-90683).
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/build/third_party/SDL3" sdl3 EXCLUDE_FROM_ALL)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS" AND HALO_RENDERER STREQUAL "BOTH")
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
if(HALO_MAC AND HALO_RENDERER STREQUAL "BOTH")
    # SDL's UIKit GL files import OpenGLES headers the MacOSX SDK lacks (tools/ios_build.py opengles_framework)
    target_compile_options(SDL3-static PRIVATE "-F${HALO_GLES_FRAMEWORKS}")
    target_compile_definitions(SDL3-static PRIVATE GLES_SILENCE_DEPRECATION=1)
endif()
# Each build a new CFBundleVersion (tools/ios_build.py), so installing over the
# app replaces it and keeps its Documents, or on tvOS its Caches (the imported game)
set(HALO_BUILD_NUMBER 6 CACHE STRING "CFBundleVersion")
set(HALO_MARKETING_VERSION "0.1.2" CACHE STRING "CFBundleShortVersionString supplied by the CI release tag")
if(HALO_NATIVE_MAC)
    set(HALO_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../macos")
    set(HALO_PLATFORM_SOURCES "${HALO_NATIVE_DIR}/host_macos_services.m"
        "${HALO_NATIVE_DIR}/host_macos_import.m")
    set(HALO_INFO_PLIST "${HALO_NATIVE_DIR}/Info.plist")
    set(HALO_RENDER_HEIGHT 0 CACHE STRING "Internal render height")
    set(HALO_DEVICE_FAMILY "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # Controllers only, no Files app: the XISO arrives from a browser
    # (host/host_tv_import.m).
    set(HALO_TVOS ON)
    # The render height is the device profile's (profiles/apple-tv.toml: 1080, since 4K fill
    # rate is the bottleneck in heavy fights on an Apple TV 4K, 3rd gen). A build made with
    # tools/ios_build.py --render-height N overrides every profile (0 = the profile's).
    set(HALO_RENDER_HEIGHT 0 CACHE STRING "Internal render height (display.render_height); 0 leaves it to the device profile")
    # Brand assets (layered icon, top shelf) made from the iOS AppIcon artwork.
    set(HALO_PLATFORM_SOURCES Assets-tvOS.xcassets host/host_tv.m host/host_tv_import.m)
    set(HALO_INFO_PLIST Info-tvOS.plist)
    set(HALO_DEVICE_FAMILY 3)
    option(HALO_EXTENDED_VIRTUAL_ADDRESSING "Sign with the extended virtual addressing entitlement" OFF)
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
        host/host_theater.m host/Theater.swift host/host_texture_settings.m host/texture_refine.m
        host/host_spatial_input.m host/host_spatial_gamepad.c)
    set(HALO_INFO_PLIST Info-visionOS.plist)
    set(HALO_DEVICE_FAMILY 7)
else()
    option(HALO_EXTENDED_VIRTUAL_ADDRESSING "Sign with the extended virtual addressing entitlement" OFF)
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
# Settings rows follow enabled build capabilities; plain builds use the original bundle.
set(HALO_SETTINGS_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle")
if(HALO_CLOUD_SAVES OR HALO_SHADER_UPLOAD)
    set(HALO_SETTINGS_CLOUD_SAVES off)
    set(HALO_SETTINGS_SHADER_UPLOAD off)
    if(HALO_CLOUD_SAVES)
        set(HALO_SETTINGS_CLOUD_SAVES on)
    endif()
    if(HALO_SHADER_UPLOAD)
        set(HALO_SETTINGS_SHADER_UPLOAD on)
    endif()
    set(HALO_SETTINGS_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/cloud/Settings.bundle")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${ROOT}/tools/settings_bundle.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle" "${HALO_SETTINGS_BUNDLE}"
        --cloud-saves "${HALO_SETTINGS_CLOUD_SAVES}" --shader-upload "${HALO_SETTINGS_SHADER_UPLOAD}"
        COMMAND_ERROR_IS_FATAL ANY)
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
    host/host_main.m host/host_crash.m host/host_memory.c host/host_loader.c host/host_syscall.c host/guest_call.S
    host/host_thread.c host/host_sdl.c host/host_audio_output.m host/host_gpu.c host/host_external.m host/host_external_mode.c host/host_gpu_dispatch.c host/host_frame_pacer.c host/gpu_metal.m host/host_frame_counter.m host/host_frame_counter_stats.c host/host_lifecycle.m
    host/host_join_link.c host/host_config.c host/host_bink.c host/host_bonjour.m host/host_https.m host/host_share.m host/host_lan.c host/host_profile.c host/host_profile_catalog.c host/host_profile_delivery.c host/host_device.m host/host_memory_pressure.m
    "${ROOT}/port/third_party/tomlc17/tomlc17.c"
    host/host_shader_pack.c host/host_data_pack.m host/host_data_updates.m host/host_recording_pack.c host/host_benchmark_snapshot.m host/host_benchmark_catalog.m
    "${ROOT}/port/third_party/monocypher/monocypher.c"
    "${ROOT}/port/third_party/monocypher/monocypher-ed25519.c"
    host/host_shader_upload.m host/host_map_identity.m "${ROOT}/port/runtime/shader_observations.c"
    host/host_perf_reports.m host/host_perf_hooks.c host/host_perf_metal.m host/host_perf_validation.m host/host_report_delivery.m "${ROOT}/port/runtime/game_perf.c"
    host/host_benchmark.m host/host_benchmark_outbox.m host/host_benchmark_presentation.m host/host_benchmark_page.m "${ROOT}/port/runtime/benchmark_metrics.c" "${ROOT}/port/runtime/benchmark_presentation.c"
    "${ROOT}/port/runtime/benchmark_plan.c" "${ROOT}/port/runtime/benchmark_journal.c"
    host/host_stereo.m host/host_stereo_head.c host/host_stereo_hud.c
    host/host_spatial.m host/host_spatial_pose.c host/host_spatial_record.c
    "${ROOT}/port/linux/src/posix_files.c" host/posix_net.c host/host_socket_revival.c host/host_natpmp.c host/host_natpmp_packets.c
    "${ROOT}/build/ios/host/bridges.c" "${ROOT}/build/ios/embedded/guest_image.S")
if(HALO_NATIVE_MAC)
    get_target_property(HALO_NATIVE_SOURCES VISR SOURCES)
    list(REMOVE_ITEM HALO_NATIVE_SOURCES host/host_external.m host/host_share.m host/host_lifecycle.m
        host/host_frame_counter.m host/host_benchmark_page.m)
    set_property(TARGET VISR PROPERTY SOURCES "${HALO_NATIVE_SOURCES}")
    target_compile_definitions(VISR PRIVATE HALO_NATIVE_MAC=1)
    target_include_directories(VISR PRIVATE "${HALO_NATIVE_DIR}")
endif()
# astc-encoder detects NEON on every arm64 Apple target. Keep its C++ flags
# separate from the host and guest translation units.
file(GLOB HALO_ASTCENC_SOURCES CONFIGURE_DEPENDS "${ROOT}/port/third_party/astcenc/*.cpp")
add_library(halo_astcenc STATIC ${HALO_ASTCENC_SOURCES})
target_compile_features(halo_astcenc PRIVATE cxx_std_14)
target_compile_definitions(halo_astcenc PRIVATE ASTCENC_BLOCK_MAX_TEXELS=16)
target_include_directories(halo_astcenc PUBLIC "${ROOT}/port/third_party/astcenc")
target_sources(VISR PRIVATE "${ROOT}/port/linux/src/astc_encode.cpp"
    "${ROOT}/port/linux/src/astc_rewrite.c" "${ROOT}/port/runtime/astc_store.c"
    host/host_astc_store.m host/host_astc_texture.c)
target_sources(VISR PRIVATE host/host_texture_upscale.m host/texture_upscale_generate.m host/texture_upscale_pixels.c
    host/texture_refine_bounded.c host/texture_upscale_encode.cpp "${ROOT}/port/third_party/bc7enc/bc7enc.cpp"
    "${ROOT}/port/third_party/bc7enc16/bc7decomp.c")
target_include_directories(VISR PRIVATE "${ROOT}/port/third_party/bc7enc" "${ROOT}/port/third_party/bc7enc16")
target_link_libraries(VISR PRIVATE "-framework Accelerate")
if(HALO_TVOS OR HALO_NATIVE_MAC)
    target_sources(VISR PRIVATE host/texture_refine.m)
endif()
target_compile_features(VISR PRIVATE cxx_std_14)
target_link_libraries(VISR PRIVATE halo_astcenc)
# Compile the CC0 model on the build host; inference never compiles a package on the device.
# tvOS and native macOS do not run the phase-2 generator, but share its unavailable-service behavior.
target_sources(VISR PRIVATE host/texture_upscale_model.m)
target_link_libraries(VISR PRIVATE "-framework CoreML")
if(NOT HALO_TVOS AND NOT HALO_NATIVE_MAC)
    set(HALO_UPSCALE_MODEL_PACKAGE "${ROOT}/port/assets/models/S4g.mlpackage")
    set(HALO_UPSCALE_MODEL_COMPILED "${CMAKE_CURRENT_BINARY_DIR}/models/S4g.mlmodelc")
    file(GLOB_RECURSE HALO_UPSCALE_MODEL_INPUTS CONFIGURE_DEPENDS "${HALO_UPSCALE_MODEL_PACKAGE}/*")
    add_custom_command(
        OUTPUT "${HALO_UPSCALE_MODEL_COMPILED}/coremldata.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/models"
        COMMAND xcrun coremlcompiler compile "${HALO_UPSCALE_MODEL_PACKAGE}" "${CMAKE_CURRENT_BINARY_DIR}/models"
        DEPENDS ${HALO_UPSCALE_MODEL_INPUTS}
        COMMENT "Compiling the S4g texture upscale model"
        VERBATIM)
    add_custom_target(halo_upscale_model DEPENDS "${HALO_UPSCALE_MODEL_COMPILED}/coremldata.bin")
    add_dependencies(VISR halo_upscale_model)
    set_source_files_properties("${HALO_UPSCALE_MODEL_COMPILED}" PROPERTIES
        GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
    target_sources(VISR PRIVATE "${HALO_UPSCALE_MODEL_COMPILED}")
endif()
target_include_directories(VISR PRIVATE host "${ROOT}/port/runtime" "${ROOT}/port/runtime/include"
    "${ROOT}/port/runtime/guest/runtime" "${ROOT}/port/linux/src" "${ROOT}/build/ios/host"
    "${ROOT}/build/ios/embedded" "${ROOT}/port/third_party/tomlc17")
# WebRTC peers use DTLS in the host; generated posix bridges translate guest buffers.
file(GLOB HALO_MBEDTLS_SOURCES CONFIGURE_DEPENDS "${ROOT}/port/third_party/mbedtls/library/*.c")
target_sources(VISR PRIVATE "${ROOT}/port/linux/src/posix_dtls.c" ${HALO_MBEDTLS_SOURCES})
target_include_directories(VISR PRIVATE "${ROOT}/port/third_party/mbedtls/include")
target_compile_definitions(VISR PRIVATE HALO_IOS=1 st_mtim=st_mtimespec st_atim=st_atimespec st_ctim=st_ctimespec)
if(HALO_RENDERER STREQUAL "METAL")
    target_compile_definitions(VISR PRIVATE HALO_METAL_ONLY=1)
endif()
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
if(HALO_RENDER_VALIDATION)
    target_sources(VISR PRIVATE host/gpu_validate.c host/gpu_capture.c host/gpu_replay.c host/host_render_replay.c)
    target_compile_definitions(VISR PRIVATE HALO_RENDER_VALIDATION=1)
endif()
if(NOT HALO_VISIONOS AND HALO_RENDERER STREQUAL "BOTH")
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
if(HALO_BINK)
    # FFmpeg's files as its own build compiles them, a static library for each of its libraries
    # (their BUILDING_ macros differ); the list is port/third_party/ffmpeg-bink/SOURCES.txt
    set(HALO_FFMPEG "${ROOT}/port/third_party/ffmpeg-bink")
    file(STRINGS "${HALO_FFMPEG}/SOURCES.txt" HALO_FFMPEG_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HALO_FFMPEG}/SOURCES.txt")
    foreach(library avformat avcodec avutil)
        set(sources ${HALO_FFMPEG_SOURCES})
        list(FILTER sources INCLUDE REGEX "^lib${library}/")
        list(TRANSFORM sources PREPEND "${HALO_FFMPEG}/")
        add_library(ffmpeg_bink_${library} STATIC ${sources})
        target_include_directories(ffmpeg_bink_${library} PRIVATE "${HALO_FFMPEG}")
        target_compile_definitions(ffmpeg_bink_${library} PRIVATE HAVE_AV_CONFIG_H _ISOC11_SOURCE BUILDING_${library})
        target_compile_options(ffmpeg_bink_${library} PRIVATE -std=c17 -w -fno-math-errno -fno-signed-zeros)
        target_link_libraries(VISR PRIVATE ffmpeg_bink_${library})
    endforeach()
    target_sources(VISR PRIVATE "${ROOT}/port/runtime/bink_decoder.c")
    set_source_files_properties("${ROOT}/port/runtime/bink_decoder.c" PROPERTIES
        INCLUDE_DIRECTORIES "${HALO_FFMPEG}")
    target_compile_definitions(VISR PRIVATE HALO_BINK=1)
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
if(HALO_NATIVE_MAC)
    set(HALO_UI_FRAMEWORK "-framework AppKit")
else()
    set(HALO_UI_FRAMEWORK "-framework UIKit")
    target_link_libraries(VISR PRIVATE "-framework AVKit")
endif()
target_link_libraries(VISR PRIVATE SDL3::SDL3-static "-framework Foundation" "${HALO_UI_FRAMEWORK}"
    "-framework Metal" "-framework AVFAudio" "-framework QuartzCore" "-framework Network" "-framework CoreImage" z)
# display.upscaler = "metalfx" (gpu_metal.m); the simulators have no MetalFX
if(NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    target_link_libraries(VISR PRIVATE "-weak_framework MetalFX")
endif()
if(HALO_NATIVE_MAC)
    target_link_libraries(VISR PRIVATE "-framework UniformTypeIdentifiers")
    target_sources(VISR PRIVATE "${HALO_NATIVE_DIR}/Assets-macOS.xcassets")
    set_source_files_properties("${HALO_NATIVE_DIR}/Assets-macOS.xcassets" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(HALO_TVOS)
    target_link_libraries(VISR PRIVATE "-framework Network" "-framework CoreImage")
    set_source_files_properties(Assets-tvOS.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # An Apple TV HD (A8, 2 GB) has no free 4 GB at a multiple of 4 GB for the
    # guest arena; the entitlement asks for a larger address space
    # (tools/ios_build.py --tvos --extended-virtual-addressing)
    if(HALO_EXTENDED_VIRTUAL_ADDRESSING)
        # (named, so the iCloud keys are added to it and do not replace it)
        set(HALO_ENTITLEMENTS VISR-tvOS-extended.entitlements)
        set_target_properties(VISR PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/VISR-tvOS-extended.entitlements")
    endif()
elseif(HALO_VISIONOS)
    target_link_libraries(VISR PRIVATE "-framework UniformTypeIdentifiers" "-framework CompositorServices"
        "-framework ARKit" "-framework SwiftUI" "-framework GameController" "-framework CoreHaptics")
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
    # An iPad Pro (first generation, A9X, 4 GB) has no free 4 GB at a multiple of 4 GB for the guest arena; the
    # entitlement asks for a larger address space (tools/ios_build.py --extended-virtual-addressing). The iCloud
    # keys are added to this file's, not in place of them
    if(HALO_EXTENDED_VIRTUAL_ADDRESSING)
        set_target_properties(VISR PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/VISR-iOS-extended.entitlements")
    endif()
    # the app's page in the Settings app (host/host_texture_settings.m)
    target_sources(VISR PRIVATE "${HALO_SETTINGS_BUNDLE}")
    set_source_files_properties("${HALO_SETTINGS_BUNDLE}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
if(NOT HALO_NATIVE_MAC)
    if(HALO_TVOS)
        set(HALO_LAUNCH_STORYBOARD LaunchScreen-tvOS.storyboard)
    elseif(HALO_VISIONOS)
        set(HALO_LAUNCH_STORYBOARD LaunchScreen-visionOS.storyboard)
    else()
        set(HALO_LAUNCH_STORYBOARD LaunchScreen.storyboard)
    endif()
    target_sources(VISR PRIVATE ${HALO_LAUNCH_STORYBOARD})
    set_source_files_properties(${HALO_LAUNCH_STORYBOARD} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
set_target_properties(VISR PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
# An archive (tools/ios_build.py --app-store-package) holds only what its targets install. CMake's targets skip the
# install step, which leaves an archive with no app in it that no distribution method accepts; the app alone is
# installed, so the archive is an app archive and not one with a static library beside the app.
set_target_properties(VISR PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL NO XCODE_ATTRIBUTE_INSTALL_PATH "/Applications")
# internet play's MQTT brokers (network.brokers_file): host_main.m writes them
# beside config.toml at each start, as upstream's Android app does
target_sources(VISR PRIVATE "${ROOT}/port/assets/network/brokers.txt")
set_source_files_properties("${ROOT}/port/assets/network/brokers.txt" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_link_libraries(VISR PRIVATE "-framework CoreText" "-framework CoreGraphics")
target_sources(VISR PRIVATE "${ROOT}/port/assets/brand/Orbitron.ttf" "${ROOT}/port/assets/brand/VISRIcon.png"
    "${ROOT}/port/assets/brand/OFL-Orbitron.txt")
set_source_files_properties("${ROOT}/port/assets/brand/Orbitron.ttf" "${ROOT}/port/assets/brand/VISRIcon.png"
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties("${ROOT}/port/assets/brand/OFL-Orbitron.txt" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
file(GLOB HALO_LICENSES "${ROOT}/build/ios/licenses/*.txt")
target_sources(VISR PRIVATE ${HALO_LICENSES})
set_source_files_properties(${HALO_LICENSES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
target_sources(VISR PRIVATE Licenses/PBRify-CC0-1.0.txt Licenses/BC7-MIT-ISC.txt)
set_source_files_properties(Licenses/PBRify-CC0-1.0.txt Licenses/BC7-MIT-ISC.txt PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
# CloudKit upload capabilities select linked diagnostic disclosure independently of the tester interface.
option(HALO_TESTER_BUILD "A tester build: the Run benchmark row, and the privacy manifest that declares its report" OFF)
if(HALO_TESTER_BUILD)
    target_sources(VISR PRIVATE host/host_benchmark_menu.m)
    target_compile_definitions(VISR PRIVATE HALO_TESTER_BUILD=1)
endif()
if(HALO_TESTER_BUILD OR HALO_BENCHMARK_UPLOAD OR HALO_PERF_REPORTS)
    set(HALO_PRIVACY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/privacy/tester/PrivacyInfo.xcprivacy")
elseif(HALO_SHADER_UPLOAD)
    set(HALO_PRIVACY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/privacy/shader/PrivacyInfo.xcprivacy")
else()
    set(HALO_PRIVACY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/privacy/plain/PrivacyInfo.xcprivacy")
endif()
set(HALO_DATA_PACK_PUBLIC_KEY "" CACHE STRING "Pinned Ed25519 public key (64 hex digits)")
set(HALO_PROFILE_FEED_URL "" CACHE STRING "Device-keyed signed profile HTTPS directory")
set(HALO_SHADER_FEED_URL "" CACHE STRING "Signed additive shader envelope HTTPS URL")
set(HALO_RECORDING_FEED_URL "" CACHE STRING "Signed recording envelope HTTPS URL")
# tools/ios_build.py --data-pack-channel; empty follows the tester build. The native Mac app has no tester build,
# and its TestFlight build names the tester channel
set(HALO_DATA_PACK_CHANNEL_NAME "" CACHE STRING "Signed update channel: stable, tester, or empty for the build's own")
if(HALO_DATA_PACK_CHANNEL_NAME STREQUAL "tester" OR HALO_DATA_PACK_CHANNEL_NAME STREQUAL "stable")
    set(HALO_DATA_PACK_CHANNEL "${HALO_DATA_PACK_CHANNEL_NAME}")
elseif(HALO_DATA_PACK_CHANNEL_NAME)
    message(FATAL_ERROR "HALO_DATA_PACK_CHANNEL_NAME must be stable or tester, got '${HALO_DATA_PACK_CHANNEL_NAME}'")
elseif(HALO_TESTER_BUILD)
    set(HALO_DATA_PACK_CHANNEL "tester")
else()
    set(HALO_DATA_PACK_CHANNEL "stable")
endif()
foreach(setting HALO_DATA_PACK_PUBLIC_KEY HALO_PROFILE_FEED_URL HALO_RECORDING_FEED_URL HALO_SHADER_FEED_URL HALO_DATA_PACK_CHANNEL)
    set_target_properties(VISR PROPERTIES "XCODE_ATTRIBUTE_${setting}" "${${setting}}")
endforeach()
target_sources(VISR PRIVATE "${HALO_PRIVACY_MANIFEST}")
set_source_files_properties("${HALO_PRIVACY_MANIFEST}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
# The device benchmark's recorded routes (port/benchmark/README.md), copied into the data folder when a suite
# starts. Off, the suite has only the scenes that play by themselves and the app carries no recording: the first
# tester builds are made so, until recorded routes are shown to hold on other devices
# On with routes off (the native Mac development build): the app carries no recordings but still runs the unsigned
# benchmark/local-routes catalog that tools/replay_check.py and step-tools/watch-windows.py write
option(HALO_BENCHMARK_LOCAL_ROUTES "Run benchmark/local-routes without bundling recordings" OFF)
option(HALO_BENCHMARK_ROUTES "Bundle the device benchmark's recorded routes and run them as scenes" ON)
if(HALO_BENCHMARK_ROUTES)
    file(GLOB HALO_BENCHMARK_RECORDINGS CONFIGURE_DEPENDS "${ROOT}/port/benchmark/recordings/*.input"
        "${ROOT}/port/benchmark/recordings/*.actions" "${ROOT}/port/benchmark/recordings/*.route"
        "${ROOT}/port/benchmark/recordings/*.checks" "${ROOT}/port/benchmark/recordings/*.seeds")
    target_sources(VISR PRIVATE ${HALO_BENCHMARK_RECORDINGS})
    set_source_files_properties(${HALO_BENCHMARK_RECORDINGS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/benchmark-recordings)
elseif(NOT HALO_BENCHMARK_LOCAL_ROUTES)
    target_compile_definitions(VISR PRIVATE HALO_BENCHMARK_NO_ROUTES=1)
endif()
# the device profiles (profiles/README.md): host/host_profile.c picks this device's at start
file(GLOB HALO_DEVICE_PROFILES CONFIGURE_DEPENDS "${ROOT}/profiles/*.toml")
list(FILTER HALO_DEVICE_PROFILES EXCLUDE REGEX "TEMPLATE\\.toml$")
target_sources(VISR PRIVATE ${HALO_DEVICE_PROFILES})
set_source_files_properties(${HALO_DEVICE_PROFILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/profiles)
set(HALO_HARDWARE_CATALOG "${ROOT}/profiles/hardware/catalog.toml")
target_sources(VISR PRIVATE ${HALO_HARDWARE_CATALOG})
set_source_files_properties(${HALO_HARDWARE_CATALOG} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/profiles/hardware)
# the maps' shader lists, compiled at map load (d3d8_device.c's halo_shader_list_warm)
file(GLOB HALO_SHADER_LISTS CONFIGURE_DEPENDS "${ROOT}/port/shader-lists/*.txt")
target_sources(VISR PRIVATE ${HALO_SHADER_LISTS})
set_source_files_properties(${HALO_SHADER_LISTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/shader-lists)
if(NOT IS_ABSOLUTE "${HALO_INFO_PLIST}")
    set(HALO_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_INFO_PLIST}")
endif()
set_target_properties(VISR PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${HALO_INFO_PLIST}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HALO_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${HALO_DEVICE_FAMILY}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    # a dSYM beside the app, which an App Store archive carries to App Store Connect for symbolicated crash reports
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym"
    # (CMake's Release flags have no -g, so its Xcode project turns debug symbols off and no dSYM is made; this
    # gives the compiler -g back without changing the code it generates)
    XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS YES
    XCODE_ATTRIBUTE_HALO_RENDER_HEIGHT "${HALO_RENDER_HEIGHT}"
    XCODE_ATTRIBUTE_HALO_BUILD_NUMBER "${HALO_BUILD_NUMBER}"
    XCODE_ATTRIBUTE_HALO_MARKETING_VERSION "${HALO_MARKETING_VERSION}")
if(HALO_DEVELOPMENT_TEAM)
    set_target_properties(VISR PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${HALO_DEVELOPMENT_TEAM}")
endif()
# Automatic benchmark report delivery (host/host_benchmark_send.m): one record in the public
# database of the same container. It needs the iCloud entitlement as cloud saves do, and nothing else of theirs
set(HALO_BENCHMARK_APS_ENVIRONMENT "development" CACHE STRING "APS environment authorized by the benchmark upload signing profile")
if(HALO_BENCHMARK_UPLOAD OR HALO_SHADER_UPLOAD OR HALO_PERF_REPORTS)
    if(NOT HALO_BENCHMARK_APS_ENVIRONMENT STREQUAL "development" AND
       NOT HALO_BENCHMARK_APS_ENVIRONMENT STREQUAL "production")
        message(FATAL_ERROR "HALO_BENCHMARK_APS_ENVIRONMENT must match the selected signing profile")
    endif()
endif()
if(HALO_BENCHMARK_UPLOAD)
    target_sources(VISR PRIVATE host/host_benchmark_send.m)
    target_compile_definitions(VISR PRIVATE
        "HALO_BENCHMARK_CLOUD=\"${HALO_CLOUD_CONTAINER}\""
        HALO_BENCHMARK_UPLOAD=1)
endif()
if(HALO_SHADER_UPLOAD)
    target_link_libraries(VISR PRIVATE sqlite3)
    target_compile_definitions(VISR PRIVATE HALO_SHADER_UPLOAD=1
        "HALO_SHADER_CLOUD=\"${HALO_CLOUD_CONTAINER}\"")
endif()
if(HALO_PERF_REPORTS)
    target_compile_definitions(VISR PRIVATE HALO_PERF_REPORTS=1
        "HALO_PERF_CLOUD=\"${HALO_CLOUD_CONTAINER}\"")
endif()
if(HALO_CLOUD_SAVES)
    # the sync rules (port/runtime/cloud_saves.c) behind a store interface; here the store is CloudKit
    target_sources(VISR PRIVATE "${ROOT}/port/runtime/cloud_saves.c" host/cloud_store_cloudkit.m host/host_cloud_saves.m)
    target_compile_definitions(VISR PRIVATE HALO_CLOUD_SAVES=1 "HALO_CLOUD_CONTAINER=\"${HALO_CLOUD_CONTAINER}\"")
endif()
if(HALO_CLOUD_SAVES OR HALO_BENCHMARK_UPLOAD OR HALO_SHADER_UPLOAD OR HALO_PERF_REPORTS)
    target_link_libraries(VISR PRIVATE "-framework CloudKit")
    # the iCloud keys, added to whatever entitlements the target already signs with (visionOS's, or the
    # extended virtual addressing entitlement of a visionOS or tvOS build), which this block must run after
    include("${CMAKE_CURRENT_LIST_DIR}/CloudUploadEntitlements.cmake")
    get_target_property(HALO_BASE_ENTITLEMENTS_FILE VISR XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS)
    halo_cloud_upload_entitlements("${CMAKE_CURRENT_BINARY_DIR}/VISR-cloud.entitlements"
        "${HALO_BASE_ENTITLEMENTS_FILE}")
    set_target_properties(VISR PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_BINARY_DIR}/VISR-cloud.entitlements"
        XCODE_ATTRIBUTE_HALO_CLOUD_CONTAINER "${HALO_CLOUD_CONTAINER}")
else()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/VISR-cloud.entitlements")
endif()
# The native Mac app's App Sandbox (port/macos/VISR-macOS.entitlements). A team-signed build, the one that goes to
# TestFlight, always runs sandboxed and also declares the iCloud container, so the App Store profile that signs it
# already carries the container when CloudKit saves arrive on the Mac. An ad hoc build signs after the build
# (tools/macos_build.py) and leaves the iCloud keys out: macOS does not launch an ad hoc app that claims them.
# This replaces the generated VISR-cloud.entitlements on the Mac, which therefore signs CloudKit without push.
if(HALO_NATIVE_MAC AND HALO_DEVELOPMENT_TEAM)
    file(READ "${HALO_NATIVE_DIR}/VISR-macOS.entitlements" HALO_NATIVE_ENTITLEMENTS)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VISR-cloud.entitlements" HALO_NATIVE_CLOUD)
    string(REGEX REPLACE "^.*<dict>(.*)</dict>.*$" "\\1" HALO_NATIVE_CLOUD_KEYS "${HALO_NATIVE_CLOUD}")
    string(REPLACE "$(HALO_CLOUD_CONTAINER)" "${HALO_CLOUD_CONTAINER}" HALO_NATIVE_CLOUD_KEYS "${HALO_NATIVE_CLOUD_KEYS}")
    string(REPLACE "</dict>" "${HALO_NATIVE_CLOUD_KEYS}</dict>" HALO_NATIVE_ENTITLEMENTS "${HALO_NATIVE_ENTITLEMENTS}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/VISR-macOS.entitlements" "${HALO_NATIVE_ENTITLEMENTS}")
    set_target_properties(VISR PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_BINARY_DIR}/VISR-macOS.entitlements")
endif()
# The multicast entitlement (spec section 2): with it in the chosen
# entitlements file, iOS and visionOS may broadcast, and network.lan_broadcast
# = "auto" does (host/host_bonjour.m)
if(HALO_ENTITLEMENTS)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${HALO_ENTITLEMENTS}" HALO_ENTITLEMENTS_TEXT)
    if(HALO_ENTITLEMENTS_TEXT MATCHES "com\\.apple\\.developer\\.networking\\.multicast")
        target_compile_definitions(VISR PRIVATE HALO_MULTICAST_ENTITLED=1)
        message(STATUS "the entitlements include multicast: broadcasts on")
    endif()
endif()
