# Headless tests — pure CPU paths only (no window, no GL context). The shape
# grammar's BuiltPiece carries MeshData (CPU vertex vectors), so the whole
# procgen pipeline runs without a GPU.
add_executable(test_procgen_determinism test_procgen_determinism.cpp)
target_link_libraries(test_procgen_determinism PRIVATE liminal::liminal)

add_executable(test_scene_roundtrip test_scene_roundtrip.cpp)
target_link_libraries(test_scene_roundtrip PRIVATE liminal::liminal)

add_test(NAME procgen_determinism COMMAND test_procgen_determinism)
add_test(NAME scene_roundtrip COMMAND test_scene_roundtrip)
