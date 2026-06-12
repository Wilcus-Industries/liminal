// Shader.cpp — GL program wrapper implementation.
//
// Lifecycle notes for the curious:
//   - Shader *objects* (the compiled .vert/.frag stages) only exist to feed
//     the link step. Once the program is linked, GL keeps its own copy of the
//     binary, so we detach + delete the stages immediately. Keeping them
//     around just leaks driver memory.
//   - Compile/link errors throw from the constructor only. After construction
//     the object is immutable GPU state, so the per-frame path (use()/set())
//     never throws.

#include <liminal/render/shader.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

namespace liminal {

namespace {

// Slurp a whole file into a string. GLSL sources are tiny (a few KB) so the
// stringstream copy is irrelevant; what matters is failing loudly with the
// path, because "shader compile error at line 12" is useless without knowing
// *which* file line 12 belongs to.
std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Shader: cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Compile one stage. Returns the GL shader object on success; throws with the
// driver's info log (and the offending path) on failure. The info log is the
// only diagnostic GL gives us — Apple's GLSL compiler messages are terse but
// they do include line numbers.
unsigned int compileStage(unsigned int type, const std::string& source,
                          const std::string& path) {
    unsigned int shader = glCreateShader(type);

    // glShaderSource takes an array of strings (historically used to prepend
    // #defines). We pass one string; length nullptr means "NUL-terminated".
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        int logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen > 1 ? static_cast<size_t>(logLen) : 1, '\0');
        glGetShaderInfoLog(shader, static_cast<int>(log.size()), nullptr, log.data());
        glDeleteShader(shader); // don't leak the failed stage
        throw std::runtime_error("Shader: compile failed: " + path + "\n" + log.data());
    }
    return shader;
}

// Compile both stages and link, returning the program object. The labels are
// whatever names make the error message useful — file paths or a pack label.
unsigned int buildProgram(const std::string& vertSrc, const std::string& fragSrc,
                          const std::string& vertLabel, const std::string& fragLabel) {
    unsigned int vert = compileStage(GL_VERTEX_SHADER, vertSrc, vertLabel);
    unsigned int frag = 0;
    try {
        frag = compileStage(GL_FRAGMENT_SHADER, fragSrc, fragLabel);
    } catch (...) {
        // compileStage cleaned up `frag`'s own object; we still own `vert`.
        glDeleteShader(vert);
        throw;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    // Whatever happens next, the stage objects are no longer needed: the
    // linked program (or the link error) is all we care about.
    glDetachShader(program, vert);
    glDetachShader(program, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    int ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        int logLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        std::vector<char> log(logLen > 1 ? static_cast<size_t>(logLen) : 1, '\0');
        glGetProgramInfoLog(program, static_cast<int>(log.size()), nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("Shader: link failed: " + vertLabel + " + " + fragLabel +
                                 "\n" + log.data());
    }
    return program;
}

} // namespace

Shader::Shader(const std::string& vertPath, const std::string& fragPath) {
    m_program = buildProgram(readFile(vertPath), readFile(fragPath), vertPath, fragPath);
}

Shader Shader::fromSource(const std::string& vertSrc, const std::string& fragSrc,
                          const std::string& label) {
    Shader s;
    s.m_program = buildProgram(vertSrc, fragSrc, label + " (vert)", label + " (frag)");
    return s;
}

Shader::~Shader() {
    // glDeleteProgram(0) is technically a silent no-op, but moved-from
    // objects are the common case here and being explicit costs nothing.
    if (m_program != 0) {
        glDeleteProgram(m_program);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_program(other.m_program),
      m_locations(std::move(other.m_locations)) {
    // Zero the source so its destructor doesn't delete the program we now own.
    other.m_program = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_program != 0) {
            glDeleteProgram(m_program);
        }
        m_program = other.m_program;
        m_locations = std::move(other.m_locations);
        other.m_program = 0;
    }
    return *this;
}

void Shader::use() const {
    glUseProgram(m_program);
}

int Shader::loc(const char* name) const {
    // Uniform locations never change after link, so we look them up once and
    // cache forever. Crucially we also cache -1 (uniform missing or optimized
    // out by the GLSL compiler) — otherwise every frame would pay a string
    // lookup *plus* a driver call for a uniform that doesn't exist.
    auto it = m_locations.find(name);
    if (it != m_locations.end()) {
        return it->second;
    }
    int location = glGetUniformLocation(m_program, name);
    m_locations.emplace(name, location);
    return location;
}

// All setters assume use() has been called — glUniform* writes to the
// *currently bound* program. They silently no-op on -1 so callers can set
// uniforms speculatively (e.g. fog params on a shader that ignores fog).

void Shader::set(const char* name, int v) const {
    int l = loc(name);
    if (l != -1) glUniform1i(l, v);
}

void Shader::set(const char* name, float v) const {
    int l = loc(name);
    if (l != -1) glUniform1f(l, v);
}

void Shader::set(const char* name, const glm::vec2& v) const {
    int l = loc(name);
    if (l != -1) glUniform2fv(l, 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::vec3& v) const {
    int l = loc(name);
    if (l != -1) glUniform3fv(l, 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::vec4& v) const {
    int l = loc(name);
    if (l != -1) glUniform4fv(l, 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::mat3& v) const {
    int l = loc(name);
    // GL_FALSE: glm matrices are already column-major, same as GL expects.
    if (l != -1) glUniformMatrix3fv(l, 1, GL_FALSE, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::mat4& v) const {
    int l = loc(name);
    if (l != -1) glUniformMatrix4fv(l, 1, GL_FALSE, glm::value_ptr(v));
}

} // namespace liminal
