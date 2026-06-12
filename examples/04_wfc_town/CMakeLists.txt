add_executable(04_wfc_town main.cpp)
# glfw linked for GLFW_KEY_* constants only.
target_link_libraries(04_wfc_town PRIVATE liminal::liminal glfw)
target_compile_definitions(04_wfc_town PRIVATE
    WFC_TOWN_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
