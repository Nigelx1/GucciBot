#include "pass.hpp"
#ifdef SILICATE_PROTECT
#include "VMProtect/VMProtectSDK.h"
#endif

GLuint compileShader(GLenum type, const char* source) {
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
}

void RenderPass::initialize() {
#ifdef SILICATE_PROTECT
    VMProtectBegin("RenderPass::initialize");
#endif

    int oldTex;
    glGetIntegerv(GL_TEXTURE_2D, &oldTex);
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int oldFbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_tex, 0);

    if (m_vertexShader == 0 || m_fragmentShader == 0) {
        return;  // don't initialize if no shaders provided
    }

    GLuint vs = compileShader(GL_VERTEX_SHADER, m_vertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, m_fragmentShader);

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
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
