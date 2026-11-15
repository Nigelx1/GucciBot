#include "pass.hpp"
#ifdef SILICATE_PROTECT
#include "VMProtect/VMProtectSDK.h"
#endif

#if defined(GEODE_IS_MACOS)
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#endif

namespace gucci {

#if defined(GEODE_IS_MACOS)
    // GD runs in a legacy OpenGL 2.1 context on macOS (cocos2d-x's Mac view
    // asks for no newer profile), and that context compiles GLSL 1.20 at
    // most: the capture shaders' "#version 130" doesn't compile there. When
    // the context's GLSL is older than 1.30 each one is rewritten to 1.20
    // line by line: the version, "in"/"out" as attribute/varying, no
    // precision line (1.20 has no precision qualifiers) and no fragColor
    // output (they all write gl_FragData[0], which 1.20 has). Nothing else in
    // them is newer than 1.20.
    static bool glslBelow130() {
        static int s_below = -1;
        if (s_below < 0) {
            int major = 0, minor = 0;
            auto const* v = reinterpret_cast<char const*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
            if (v)
                std::sscanf(v, "%d.%d", &major, &minor);
            s_below = (major * 100 + minor < 130) ? 1 : 0;
            geode::log::info("[GucciBot] Render shaders: GLSL {} here, {}", v ? v : "(unknown)",
                             s_below ? "rewritten to 1.20" : "used as they are");
        }
        return s_below == 1;
    }

    static std::string toGlsl120(GLenum type, char const* source) {
        std::istringstream in(source);
        std::string out, line;
        while (std::getline(in, line)) {
            size_t const at = line.find_first_not_of(" \t");
            std::string_view const body = at == std::string::npos ? std::string_view() : std::string_view(line).substr(at);
            std::string const indent = line.substr(0, at == std::string::npos ? line.size() : at);
            if (body.starts_with("#version 130"))
                line = "#version 120";
            else if (body.starts_with("precision "))
                continue;
            else if (body.starts_with("out vec4 fragColor;"))
                continue;
            else if (body.starts_with("in "))
                line = indent + (type == GL_VERTEX_SHADER ? "attribute " : "varying ") + std::string(body.substr(3));
            else if (body.starts_with("out "))
                line = indent + "varying " + std::string(body.substr(4));
            out += line;
            out += '\n';
        }
        return out;
    }
#endif

    GLuint compileShader(GLenum type, const char* source) {
#if defined(GEODE_IS_MACOS)
        std::string translated;
        if (glslBelow130()) {
            translated = toGlsl120(type, source);
            source = translated.c_str();
        }
#endif
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);
            geode::log::error("failed to compile shader: {}", infoLog);
        }

        return shader;
    }

    void RenderPass::resize() {
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    void RenderPass::initialize() {
#ifdef SILICATE_PROTECT
        VMProtectBegin("RenderPass::initialize");
#endif

        int oldTex;
        glGetIntegerv(GL_TEXTURE_2D, &oldTex);
        glGenTextures(1, &m_tex);
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        int oldFbo;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);

        if (m_vertexShader == 0 || m_fragmentShader == 0) {
            return;
        }

        GLuint vs = compileShader(GL_VERTEX_SHADER, m_vertexShader);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, m_fragmentShader);

        m_program = glCreateProgram();
        glAttachShader(m_program, vs);
        glAttachShader(m_program, fs);
#if defined(GEODE_IS_MACOS)
        // texture.cpp feeds the quad as attributes 0 (position) and 1
        // (texture coordinates). Unbound, the linker numbers them as it likes;
        // a legacy 2.1 context also ties attribute 0 to the vertex position.
        glBindAttribLocation(m_program, 0, "a_position");
        glBindAttribLocation(m_program, 1, "a_texCoord");
#endif
        glLinkProgram(m_program);

        glDeleteShader(vs);
        glDeleteShader(fs);

        glBindFramebuffer(GL_FRAMEBUFFER, oldFbo);
        glBindTexture(GL_TEXTURE_2D, oldTex);

        geode::log::info("Initialized new render pass {}", m_program);

#ifdef SILICATE_PROTECT
        VMProtectEnd();
#endif
    }

    void RenderPass::destroy() {
        if (m_program) {
            glDeleteProgram(m_program);
        }

        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_tex);
        m_program = 0;
        m_fbo = 0;
        m_tex = 0;
    }

} // namespace gucci
