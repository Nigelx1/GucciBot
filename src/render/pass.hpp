#pragma once
#include <Geode/Geode.hpp>
#include <functional>

namespace gucci {

    GLuint compileShader(GLenum type, const char* source);

    class RenderPass {
    public:
        GLuint m_program = 0;
        GLuint m_fbo = 0;
        GLuint m_tex = 0;
        uint32_t m_width;
        uint32_t m_height;

        const char* m_vertexShader;
        const char* m_fragmentShader;
        GLuint m_sourceTex = 0;

        std::function<void(float, float)> m_readPixels;

    public:
        void initialize();
        void resize();
        void destroy();
    };

} // namespace gucci
