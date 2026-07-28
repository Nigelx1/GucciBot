#pragma once
#include <Geode/Geode.hpp>
#include <functional>

GLuint compileShader(GLenum type, const char* source);

/*
 * A render pass is a single step in the rendering pipeline.
 *
 * The first render pass should be completely empty, it is used to
 * render the actual scene.
 */
class RenderPass {
   public:
    GLuint m_program = 0;
    GLuint m_fbo = 0;
    GLuint m_tex = 0;
    uint32_t m_width;
    uint32_t m_height;

    const char* m_vertexShader;
    const char* m_fragmentShader;
    GLuint m_sourceTex = 0;  // Texture to read from if shaders are present

    std::function<void(float, float)> m_readPixels;

   public:
    void initialize();
    void resize();
    void destroy();
};
