cmake_minimum_required(VERSION 3.20)
project(SmallRender VERSION 0.1.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- FetchContent dependencies ---
include(FetchContent)

# GLFW 3.4
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ImGui (docking branch)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Native File Dialog Extended
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nfd)

# --- GLAD (pre-generated, GL 3.3 Core) ---
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# --- ImGui (manual target — no upstream CMakeLists.txt) ---
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw glad)

# --- SmallRender Monitor ---
add_executable(smallrender
    src/core/platform.cpp
    src/core/node_identity.cpp
    src/core/atomic_file_io.cpp
    src/core/ipc_server.cpp
    src/core/monitor_log.cpp
    src/core/system_tray.cpp
    src/core/single_instance.cpp
    src/core/udp_notify.cpp
    src/monitor/main.cpp
    src/monitor/monitor_app.cpp
    src/monitor/agent_supervisor.cpp
    src/monitor/heartbeat_manager.cpp
    src/monitor/farm_init.cpp
    src/monitor/template_manager.cpp
    src/monitor/job_manager.cpp
    src/monitor/dispatch_manager.cpp
    src/monitor/render_coordinator.cpp
    src/monitor/command_manager.cpp
    src/monitor/submission_manager.cpp
    src/monitor/ui_data_cache.cpp
    src/monitor/ui/dashboard.cpp
    src/monitor/ui/settings_panel.cpp
    src/monitor/ui/node_panel.cpp
    src/monitor/ui/job_list_panel.cpp
    src/monitor/ui/job_detail_panel.cpp
    src/monitor/ui/log_panel.cpp
    src/monitor/ui/farm_cleanup_dialog.cpp
    src/monitor/ui/style.cpp
    resources/smallrender.rc
)

target_include_directories(smallrender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(smallrender PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(smallrender PRIVATE
    imgui
    glad
    nlohmann_json::nlohmann_json
    nfd
    opengl32
    shell32
    ole32
    Rpcrt4
    dwmapi
    Advapi32
    Ws2_32
)

# Copy resources/ next to the executable at build time
add_custom_command(TARGET smallrender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:smallrender>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:smallrender>/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES"
        "$<TARGET_FILE_DIR:smallrender>/LICENSES"
    COMMENT "Copying resources to build output"
)

# MSVC-specific settings
if(MSVC)
    target_compile_options(smallrender PRIVATE /W4)
    # Windows subsystem (no console window)
    set_target_properties(smallrender PROPERTIES WIN32_EXECUTABLE TRUE)
    set_target_properties(smallrender PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
