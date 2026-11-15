#pragma once

#include "colorspace.hpp"

namespace gucci {

    // Planar YUV with full-resolution chroma (4:4:4): yuv420p's conversion
    // (BT.709 coefficients, limited range: Y 16-235, U and V 16-240) without
    // the 2x2 chroma average. Three planes of width x height bytes each, in
    // the order FFmpeg's AV_PIX_FMT_YUV444P lays them out: Y, then U (Cb),
    // then V (Cr). Each plane is its own pass and its own glReadPixels into
    // the slot's PBO, at the plane's offset, as yuv420p does.
    //
    // For showcase renders (Nigel's TCBot setup: libx264, yuv444p, -qp 0).
    // With x264 at qp 0 the stream is lossless, so the colors in the file are
    // exactly these: thin colored edges (wave trails, hitboxes, text) keep
    // their color, which 4:2:0's quarter-size chroma smears.
    class YUV444PColorspace : public Colorspace {
    public:
        const std::vector<RenderPass> getPasses() override {
            const uintptr_t planeSize = (uintptr_t)m_alignedWidth * m_alignedHeight;

            const char* vertexShader = R"(#version 130
        in vec4 a_position;
        in vec2 a_texCoord;

        out vec2 v_texCoord;

        void main() {
            gl_Position = vec4(a_position.xy, 0.0, 1.0);
            v_texCoord = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
        }
        )";

            return {RenderPass{.m_width = m_alignedWidth,
                               .m_height = m_alignedHeight,
                               .m_vertexShader = nullptr,
                               .m_fragmentShader = nullptr,
                               .m_readPixels = [](float, float) {}},
                    RenderPass{.m_width = m_alignedWidth,
                               .m_height = m_alignedHeight,
                               .m_vertexShader = vertexShader,
                               .m_fragmentShader = R"(#version 130
                precision highp float;

                in vec2 v_texCoord;
                out vec4 fragColor;

                uniform sampler2D u_texture;
                uniform float u_fade;

                const vec3 coeffY = vec3(0.2126, 0.7152, 0.0722);

                void main() {
                    float gamma = 1.6;
                    float corrected = pow(u_fade, 1.0 / gamma);
                    vec3 rgb = texture2D(u_texture, v_texCoord).rgb * corrected;

                    float y = dot(rgb, coeffY);

                    // convert range to bt709
                    y = y * 219.0 / 255.0 + 16.0 / 255.0;

                    gl_FragData[0] = vec4(y, 0.0, 0.0, 1.0);
                })",
                               .m_readPixels =
                                   [this](float x, float y) {
                                       glReadPixels(x,
                                                    y,
                                                    m_alignedWidth,
                                                    m_alignedHeight,
                                                    GL_RED,
                                                    GL_UNSIGNED_BYTE,
                                                    nullptr);
                                   }},
                    RenderPass{.m_width = m_alignedWidth,
                               .m_height = m_alignedHeight,
                               .m_vertexShader = vertexShader,
                               .m_fragmentShader = R"(#version 130
                precision highp float;

                in vec2 v_texCoord;
                out vec4 fragColor;

                uniform sampler2D u_texture;
                uniform float u_fade;

                const vec3 coeffU = vec3(-0.11457, -0.38543, 0.5);

                void main() {
                    float gamma = 1.6;
                    float corrected = pow(u_fade, 1.0 / gamma);
                    vec3 rgb = texture2D(u_texture, v_texCoord).rgb * corrected;

                    float u = dot(rgb, coeffU) + 0.5;

                    // convert range to bt709
                    u = u * 224.0 / 255.0 + 16.0 / 255.0;

                    gl_FragData[0] = vec4(u, 0.0, 0.0, 1.0);
                })",
                               .m_readPixels =
                                   [this, planeSize](float x, float y) {
                                       glReadPixels(x,
                                                    y,
                                                    m_alignedWidth,
                                                    m_alignedHeight,
                                                    GL_RED,
                                                    GL_UNSIGNED_BYTE,
                                                    reinterpret_cast<void*>(planeSize));
                                   }},
                    RenderPass{.m_width = m_alignedWidth,
                               .m_height = m_alignedHeight,
                               .m_vertexShader = vertexShader,
                               .m_fragmentShader = R"(#version 130
                precision highp float;

                in vec2 v_texCoord;
                out vec4 fragColor;

                uniform sampler2D u_texture;
                uniform float u_fade;

                const vec3 coeffV = vec3(0.5, -0.45415, -0.04585);

                void main() {
                    float gamma = 1.6;
                    float corrected = pow(u_fade, 1.0 / gamma);
                    vec3 rgb = texture2D(u_texture, v_texCoord).rgb * corrected;

                    float v = dot(rgb, coeffV) + 0.5;

                    // convert range to bt709
                    v = v * 224.0 / 255.0 + 16.0 / 255.0;

                    gl_FragData[0] = vec4(v, 0.0, 0.0, 1.0);
                })",
                               .m_readPixels = [this, planeSize](float x, float y) {
                                   glReadPixels(x,
                                                y,
                                                m_alignedWidth,
                                                m_alignedHeight,
                                                GL_RED,
                                                GL_UNSIGNED_BYTE,
                                                reinterpret_cast<void*>(planeSize * 2));
                               }}};
        }

        size_t getBufferSize() override {
            return (size_t)m_alignedWidth * m_alignedHeight * 3;
        }

        geode::Result<> prepareFrame(AVFrame* frame, uint8_t* data, size_t size) override {
            if (!frame || !data || size == 0) {
                return geode::Err("Invalid parameters");
            }

            const uintptr_t planeSize = (uintptr_t)m_alignedWidth * m_alignedHeight;

            frame->data[0] = data;
            frame->data[1] = data + planeSize;
            frame->data[2] = data + planeSize * 2;

            return geode::Ok();
        }
    };

} // namespace gucci
