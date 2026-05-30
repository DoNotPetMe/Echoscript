# -----------------------------------------------------------------------
#  BlacklistMod.dll
#  Requires ImGui and MinHook to be placed under thirdparty/
#
#  ImGui:    https://github.com/ocornut/imgui
#            Copy the following files into thirdparty/imgui/:
#              imgui.h  imgui.cpp  imgui_internal.h  imgui_draw.cpp
#              imgui_tables.cpp  imgui_widgets.cpp  imgui_demo.cpp
#              backends/imgui_impl_win32.{h,cpp}
#              backends/imgui_impl_dx11.{h,cpp}
#
#  MinHook:  https://github.com/TsudaKageyu/minhook
#            Copy include/MinHook.h + the relevant lib into thirdparty/minhook/
# -----------------------------------------------------------------------

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui")

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

add_library(BlacklistMod SHARED
    dllmain.cpp
    hooks/d3d11_hook.cpp
    features/freecam.cpp
    features/leveleditor.cpp
    features/engine_bridge.cpp
    features/object_dumper.cpp
    features/prop_database.cpp
    features/gizmo.cpp
    features/level_serialization.cpp
    menu/menu.cpp
    utils/cam_finder.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(BlacklistMod PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/minhook/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/json"   # nlohmann/json.hpp (header-only)
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(BlacklistMod PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

target_compile_options(BlacklistMod PRIVATE /utf-8)

target_link_libraries(BlacklistMod PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
)

set_target_properties(BlacklistMod PROPERTIES
    OUTPUT_NAME "BlacklistMod"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
