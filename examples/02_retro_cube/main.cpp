// 02_retro_cube: a spinning procedural box through the PS1-style Renderer
// with the embedded default "retro" shader pack. ESC quits.
#include <liminal/core/window.hpp>
#include <liminal/render/mesh.hpp>
#include <liminal/render/renderer.hpp>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

int main() {
    liminal::Window window(1280, 720, "liminal — 02_retro_cube");
    liminal::Renderer renderer; // embedded retro pack; no files touched

    liminal::Mesh cube = liminal::Mesh::box(); // 1x1x1, base on y=0

    while (!window.shouldClose()) {
        window.pollEvents();
        if (window.keyPressed(GLFW_KEY_ESCAPE)) window.requestClose();

        const float t = float(window.time());

        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.4f, 3.0f),
                                           glm::vec3(0.0f, 0.5f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        renderer.beginFrame(view);

        liminal::DrawItem item;
        item.mesh = &cube;
        item.model = glm::rotate(glm::mat4(1.0f), t * 0.8f,
                                 glm::vec3(0.0f, 1.0f, 0.0f));
        item.color = {0.65f, 0.5f, 0.75f};
        item.color2 = {0.9f, 0.85f, 0.7f};
        renderer.draw(item);

        int fbw = 0, fbh = 0;
        window.framebufferSize(fbw, fbh);
        renderer.endFrame(fbw, fbh);

        window.swapBuffers();
    }
    return 0;
}
