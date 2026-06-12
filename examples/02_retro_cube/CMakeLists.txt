add_executable(02_retro_cube main.cpp)
# glfw linked for GLFW_KEY_* constants only.
target_link_libraries(02_retro_cube PRIVATE liminal::liminal glfw)
