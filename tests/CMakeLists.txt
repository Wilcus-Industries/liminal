# Headless tests — pure CPU paths only (no window, no GL context). The shape
# grammar's BuiltPiece carries MeshData (CPU vertex vectors), so the whole
# procgen pipeline runs without a GPU.
add_executable(test_procgen_determinism test_procgen_determinism.cpp)
target_link_libraries(test_procgen_determinism PRIVATE liminal::liminal)

add_executable(test_scene_roundtrip test_scene_roundtrip.cpp)
target_link_libraries(test_scene_roundtrip PRIVATE liminal::liminal)

add_executable(test_pak_roundtrip test_pak_roundtrip.cpp)
target_link_libraries(test_pak_roundtrip PRIVATE liminal::liminal)

# Headless cover for the editor "Build Game" pak assembly (buildGamePak). Bakes
# the sample project dir so the test can find it without a working directory.
add_executable(test_build_pak test_build_pak.cpp)
target_link_libraries(test_build_pak PRIVATE liminal::liminal)
target_compile_definitions(test_build_pak PRIVATE
    LIMINAL_SAMPLE_PROJECT_DIR="${CMAKE_SOURCE_DIR}/editor/sample_project")

add_test(NAME procgen_determinism COMMAND test_procgen_determinism)
add_test(NAME scene_roundtrip COMMAND test_scene_roundtrip)
add_test(NAME pak_roundtrip COMMAND test_pak_roundtrip)
add_test(NAME build_pak COMMAND test_build_pak)

# lm.procgen Lua bindings determinism (needs ScriptHost + sol2, always built).
# Mirrors test_procgen_determinism's link pattern.
add_executable(test_lua_procgen test_lua_procgen.cpp)
target_link_libraries(test_lua_procgen PRIVATE liminal::liminal)
add_test(NAME lua_procgen COMMAND test_lua_procgen)
