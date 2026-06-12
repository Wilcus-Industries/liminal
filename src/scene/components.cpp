#include <liminal/scene/components.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace liminal {

glm::mat4 Transform::matrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    // Yaw -> pitch -> roll, matching the header's documented order.
    m = glm::rotate(m, glm::radians(rotationEuler.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotationEuler.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotationEuler.z), glm::vec3(0, 0, 1));
    return glm::scale(m, scale);
}

} // namespace liminal
