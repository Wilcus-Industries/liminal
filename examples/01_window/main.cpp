// 01_window: minimal raw-GLFW window with a GL clear loop. ESC quits.
// Intentionally uses glfwCreateWindow directly — the liminal Window class
// arrives in Phase 1.
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <liminal/liminal.hpp>

#include <cstdio>

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, LIMINAL_GL_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, LIMINAL_GL_MINOR);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "liminal — 01_window", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    glfwSwapInterval(1);

    std::printf("liminal %s — GL %s\n", liminal::version(),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        glClearColor(0.09f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
