add_executable(03_scene_lua main.cpp)
# glfw linked for GLFW_KEY_* constants only.
target_link_libraries(03_scene_lua PRIVATE liminal::liminal glfw)
# The example registers its own source dir as an asset search path so
# Script{path = "scripts/spin.lua"} resolves — and so editing the .lua files
# in the repo hot-reloads the running example.
target_compile_definitions(03_scene_lua PRIVATE
    SCENE_LUA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
