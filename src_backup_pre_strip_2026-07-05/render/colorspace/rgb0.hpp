#pragma once

#include "colorspace.hpp"

class RGB0Colorspace : public Colorspace {
   public:
    const std::vector<RenderPass> getPasses() override {
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

                void main() {
                    vec3 rgb = texture2D(u_texture, v_texCoord).rgb;

                    gl_FragData[0] = vec4(rgb, 1.0);
                })",
                           .m_readPixels = [this](float x, float y) {
                               glReadPixels(x, y, m_alignedWidth,
                                            m_alignedHeight, GL_RGBA,
                                            GL_UNSIGNED_BYTE, nullptr);
                           }}};
    }

    size_t getBufferSize() override {
        return m_alignedWidth * m_alignedHeight * 4;
    }

    geode::Result<> prepareFrame(AVFrame* frame, uint8_t* data,
                                 size_t size) override {
        if (!frame || !data || size == 0) {
            return geode::Err("Invalid parameters");
        }

        frame->data[0] = data;

        return geode::Ok();
    }
};