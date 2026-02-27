cmake_minimum_required(VERSION 3.20)
project(AE_YOLO VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

# === Paths ===
if(WIN32)
    set(AESDK_ROOT "D:/Work/Coding/AE_webgpu/AfterEffectsSDK/Examples" CACHE PATH "AE SDK root")
    set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime/onnxruntime-directml" CACHE PATH "ONNX Runtime root")
elseif(APPLE)
    set(AESDK_ROOT "/Users/eric/Documents/ae25.6_61.64bit.AfterEffectsSDK/Examples" CACHE PATH "AE SDK root")
    set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime/onnxruntime-osx" CACHE PATH "ONNX Runtime root")
endif()

# === Verify paths ===
if(NOT EXISTS "${AESDK_ROOT}/Headers")
    message(FATAL_ERROR "AE SDK not found at: ${AESDK_ROOT}/Headers")
endif()
if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime not found at: ${ONNXRUNTIME_ROOT}")
endif()

# === Sources (platform-independent) ===
set(SOURCES
    src/AE_YOLO.cpp
    src/YoloEngine.cpp
    src/YoloPostprocess.cpp
    src/FrameAnalyzer.cpp
    ${AESDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${AESDK_ROOT}/Util/MissingSuiteError.cpp
)

set(HEADERS
    src/AE_YOLO.h
    src/YoloEngine.h
    src/YoloPostprocess.h
    src/FrameAnalyzer.h
    src/Letterbox.h
    src/SavGolSmooth.h
)

# === Plugin target ===
add_library(AE_YOLO MODULE ${SOURCES} ${HEADERS})

target_include_directories(AE_YOLO PRIVATE
    ${AESDK_ROOT}/Headers
    ${AESDK_ROOT}/Headers/SP
    ${AESDK_ROOT}/Util
    ${ONNXRUNTIME_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# =============================================================================
# Windows
# =============================================================================
if(WIN32)
    target_include_directories(AE_YOLO PRIVATE
        ${AESDK_ROOT}/Headers/Win
    )

    target_compile_definitions(AE_YOLO PRIVATE
        WIN32 _WINDOWS NDEBUG _USRDLL
        PF_DEEP_COLOR_AWARE=1
        MSWindows=1
        _CRT_SECURE_NO_WARNINGS
        UNICODE _UNICODE
    )

    # /MD to match ONNX Runtime (NOT /MT)
    if(MSVC)
        target_compile_options(AE_YOLO PRIVATE
            /W3 /O2 /Oi /EHsc /fp:fast
        )
        set_property(TARGET AE_YOLO PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    endif()

    set_target_properties(AE_YOLO PROPERTIES
        SUFFIX ".aex"
        PREFIX ""
        OUTPUT_NAME "AE_YOLO"
    )

    target_link_options(AE_YOLO PRIVATE
        "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/AE_YOLO.def"
    )

    target_link_libraries(AE_YOLO PRIVATE
        "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        delayimp.lib
        ole32.lib
        shell32.lib
        shlwapi.lib
    )

    # Delay-load onnxruntime.dll so the plugin can set DLL search path
    # (via PreloadDlls) before the library is actually resolved.
    target_link_options(AE_YOLO PRIVATE
        "/DELAYLOAD:onnxruntime.dll"
    )

    # === PiPL resource compilation ===
    set(PIPL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/AE_YOLOPiPL.r")
    set(PIPL_DIR "${CMAKE_CURRENT_BINARY_DIR}/pipl")
    file(MAKE_DIRECTORY ${PIPL_DIR})

    add_custom_command(
        OUTPUT "${PIPL_DIR}/AE_YOLOPiPL.rr"
        COMMAND ${CMAKE_COMMAND} -E copy "${PIPL_SRC}" "${PIPL_DIR}/AE_YOLOPiPL.r"
        COMMAND cl
            /I "${AESDK_ROOT}/Headers"
            /I "${AESDK_ROOT}/Headers/SP"
            /I "${AESDK_ROOT}/Util"
            /EP "${PIPL_DIR}/AE_YOLOPiPL.r"
            > "${PIPL_DIR}/AE_YOLOPiPL.rr"
        DEPENDS "${PIPL_SRC}"
        COMMENT "PiPL Step 1: Preprocessing .r"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PIPL_DIR}/AE_YOLOPiPL.rrc"
        COMMAND "${AESDK_ROOT}/Resources/PiPLtool.exe"
            "${PIPL_DIR}/AE_YOLOPiPL.rr"
            "${PIPL_DIR}/AE_YOLOPiPL.rrc"
        DEPENDS "${PIPL_DIR}/AE_YOLOPiPL.rr"
        COMMENT "PiPL Step 2: PiPLtool"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${PIPL_DIR}/AE_YOLOPiPL.rc"
        COMMAND cl /D "MSWindows" /EP
            "${PIPL_DIR}/AE_YOLOPiPL.rrc"
            > "${PIPL_DIR}/AE_YOLOPiPL.rc"
        DEPENDS "${PIPL_DIR}/AE_YOLOPiPL.rrc"
        COMMENT "PiPL Step 3: Creating .rc"
        VERBATIM
    )

    target_sources(AE_YOLO PRIVATE "${PIPL_DIR}/AE_YOLOPiPL.rc")

    # === Post-build: copy ONNX Runtime DLL alongside .aex ===
    add_custom_command(TARGET AE_YOLO POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:AE_YOLO>/onnxruntime.dll"
        COMMENT "Copying onnxruntime.dll"
    )

    # === Install target ===
    set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore" CACHE PATH "AE plugins directory")
    add_custom_target(install_plugin
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:AE_YOLO>" "${AE_PLUGINS_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "${AE_PLUGINS_DIR}/onnxruntime.dll"
        DEPENDS AE_YOLO
        COMMENT "Installing AE_YOLO plugin to ${AE_PLUGINS_DIR}"
    )
endif()

# =============================================================================
# macOS
# =============================================================================
if(APPLE)
    target_include_directories(AE_YOLO PRIVATE
        ${AESDK_ROOT}/Headers/Mac
    )

    target_compile_definitions(AE_YOLO PRIVATE
        __MACH__
        PF_DEEP_COLOR_AWARE=1
    )

    target_compile_options(AE_YOLO PRIVATE
        -Wall -O2 -ffast-math
    )

    # Universal binary: Intel + Apple Silicon
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

    # Output as .plugin bundle
    set_target_properties(AE_YOLO PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        PREFIX ""
        OUTPUT_NAME "AE_YOLO"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
    )

    # Link ONNX Runtime dylib
    find_library(ONNXRUNTIME_LIB onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT ONNXRUNTIME_LIB)
        # Fallback: link directly
        set(ONNXRUNTIME_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib")
    endif()

    target_link_libraries(AE_YOLO PRIVATE
        ${ONNXRUNTIME_LIB}
        "-framework Cocoa"
        "-framework CoreML"
        "-framework Foundation"
    )

    # Set rpath so the plugin can find libonnxruntime.dylib next to it
    set_target_properties(AE_YOLO PROPERTIES
        INSTALL_RPATH "@loader_path"
        BUILD_WITH_INSTALL_RPATH TRUE
    )

    # === PiPL resource compilation via Rez ===
    set(PIPL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/resources/AE_YOLOPiPL.r")
    set(PIPL_RSRC "${CMAKE_CURRENT_BINARY_DIR}/AE_YOLOPiPL.rsrc")

    find_program(REZ_TOOL Rez PATHS
        "/Developer/Tools"
        "/Applications/Xcode.app/Contents/Developer/usr/bin"
        "/usr/bin"
    )

    if(REZ_TOOL)
        add_custom_command(
            OUTPUT "${PIPL_RSRC}"
            COMMAND ${REZ_TOOL}
                "${PIPL_SRC}"
                -o "${PIPL_RSRC}"
                -useDF
                -i "${AESDK_ROOT}/Headers"
                -i "${AESDK_ROOT}/Headers/SP"
                -i "${AESDK_ROOT}/Util"
                -i "${AESDK_ROOT}/Resources"
            DEPENDS "${PIPL_SRC}"
            COMMENT "Compiling PiPL resource via Rez"
            VERBATIM
        )

        add_custom_command(TARGET AE_YOLO POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:AE_YOLO>/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy "${PIPL_RSRC}" "$<TARGET_BUNDLE_DIR:AE_YOLO>/Contents/Resources/AE_YOLOPiPL.rsrc"
            COMMENT "Embedding PiPL resource in plugin bundle"
        )

        # Ensure rsrc is built before the plugin
        target_sources(AE_YOLO PRIVATE "${PIPL_RSRC}")
        set_source_files_properties("${PIPL_RSRC}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
            HEADER_FILE_ONLY TRUE
        )
    else()
        message(WARNING "Rez tool not found — PiPL resource will not be compiled. "
                        "Install Xcode Command Line Tools or set REZ_TOOL manually.")
    endif()

    # === Post-build: copy ONNX Runtime dylib into plugin bundle ===
    add_custom_command(TARGET AE_YOLO POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:AE_YOLO>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.dylib"
            "$<TARGET_BUNDLE_DIR:AE_YOLO>/Contents/Frameworks/libonnxruntime.dylib"
        COMMENT "Copying libonnxruntime.dylib into plugin bundle"
    )

    # === Install target ===
    set(AE_PLUGINS_DIR "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore" CACHE PATH "AE plugins directory")
    add_custom_target(install_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:AE_YOLO>" "${AE_PLUGINS_DIR}/AE_YOLO.plugin"
        DEPENDS AE_YOLO
        COMMENT "Installing AE_YOLO plugin to ${AE_PLUGINS_DIR}"
    )
endif()

message(STATUS "=== AE_YOLO Configuration ===")
message(STATUS "  Platform:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "  AE SDK:        ${AESDK_ROOT}")
message(STATUS "  ONNX Runtime:  ${ONNXRUNTIME_ROOT}")
if(WIN32)
    message(STATUS "  CRT:           /MD (dynamic, matches ONNX Runtime)")
endif()
if(APPLE)
    message(STATUS "  Architectures: ${CMAKE_OSX_ARCHITECTURES}")
    message(STATUS "  Min macOS:     ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
