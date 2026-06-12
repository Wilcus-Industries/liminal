#pragma once
// GL program wrapper: loads vertex+fragment source from files, compiles,
// links, caches uniform locations. Throws std::runtime_error on compile or
// link failure (construction only — never in the hot path).

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

namespace liminal {

class Shader {
public:
    Shader(const std::string& vertPath, const std::string& fragPath);
    ~Shader();

    // Compile + link directly from GLSL source strings (no filesystem).
    // `label` only flavors error messages so a failure names its origin.
    static Shader fromSource(const std::string& vertSrc, const std::string& fragSrc,
                             const std::string& label = "<inline>");

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    void use() const;

    // Setters silently ignore unknown/optimized-out uniforms (location -1).
    void set(const char* name, int v) const;
    void set(const char* name, float v) const;
    void set(const char* name, const glm::vec2& v) const;
    void set(const char* name, const glm::vec3& v) const;
    void set(const char* name, const glm::vec4& v) const;
    void set(const char* name, const glm::mat3& v) const;
    void set(const char* name, const glm::mat4& v) const;

    unsigned int id() const { return m_program; }

private:
    Shader() = default; // used by fromSource

    int loc(const char* name) const;

    unsigned int m_program = 0;
    mutable std::unordered_map<std::string, int> m_locations;
};

} // namespace liminal
