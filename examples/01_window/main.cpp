// 01_window: minimal liminal::Window with a GL clear loop. ESC quits.
#include <liminal/core/window.hpp>
#include <liminal/version.hpp>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <cstdio>

int main() {
    liminal::Window window(1280, 720, "liminal — 01_window");
    std::printf("liminal %s\n", liminal::kVersionString);

    while (!window.shouldClose()) {
        window.pollEvents();
        if (window.keyPressed(GLFW_KEY_ESCAPE)) window.requestClose();

        glClearColor(0.09f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        window.swapBuffers();
    }
    return 0;
}
