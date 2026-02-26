cmake_minimum_required(VERSION 3.20)
project(AE_YOLO VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

# === Paths ===
if(WIN32)
    set(AESDK_ROOT "D:/Work/Coding/AE_webgpu/AfterEffectsSDK/Examples" CACHE PATH "AE SDK root")
else()
    set(AESDK_ROOT "/Users/eric/Documents/coding/AfterEffectsSDK/Examples" CACHE PATH "AE SDK root")
endif()

set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/onnxruntime/onnxruntime-directml" CACHE PATH "ONNX Runtime DirectML root")

# === Verify paths ===
if(NOT EXISTS "${AESDK_ROOT}/Headers")
    message(FATAL_ERROR "AE SDK not found at: ${AESDK_ROOT}/Headers")
endif()
if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime not found at: ${ONNXRUNTIME_ROOT}")
endif()

# === Sources ===
set(SOURCES
    src/AE_YOLO.cpp
    src/YoloEngine.cpp
    src/YoloPostprocess.cpp
    src/FrameAnalyzer.cpp
    src/FileDialog.cpp
    ${AESDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${AESDK_ROOT}/Util/MissingSuiteError.cpp
)

set(HEADERS
    src/AE_YOLO.h
    src/YoloEngine.h
    src/YoloPostprocess.h
    src/FrameAnalyzer.h
    src/FileDialog.h
    src/Letterbox.h
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

# === Windows ===
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
        ole32.lib
        shell32.lib
        shlwapi.lib
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

message(STATUS "=== AE_YOLO Configuration ===")
message(STATUS "  AE SDK:        ${AESDK_ROOT}")
message(STATUS "  ONNX Runtime:  ${ONNXRUNTIME_ROOT}")
message(STATUS "  CRT:           /MD (dynamic, matches ONNX Runtime)")
