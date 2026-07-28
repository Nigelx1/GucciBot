#pragma once

#include "colorspace.hpp"

class NV12Colorspace : public Colorspace {
   public:
    const std::vector<RenderPass> getPasses() override {
        const uintptr_t yPlaneSize = m_alignedWidth * m_alignedHeight;

        const char* vertexShader = R"(#version 130
        in vec4 a_position;
        in vec2 a_texCoord;

        out vec2 v_texCoord;

        void main() {
            gl_Position = vec4(a_position.xy, 0.0, 1.0);
            v_texCoord = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
        }
        )";

        return {
            RenderPass{
                .m_width = m_alignedWidth,
                .m_height = m_alignedHeight,
                .m_vertexShader = nullptr,
                .m_fragmentShader = nullptr,
                .m_readPixels = [](float, float) {},
            },
            RenderPass{
                .m_width = m_alignedWidth,
                .m_height = m_alignedHeight,
                .m_vertexShader = vertexShader,
                .m_fragmentShader = R"(#version 130
                precision highp float;

                in vec2 v_texCoord;
                out vec4 fragColor;

                uniform sampler2D u_texture;

                const vec3 coeffY = vec3(0.2126, 0.7152, 0.0722);

                void main() {
                    vec3 rgb = texture2D(u_texture, v_texCoord).rgb;

                    float y = dot(rgb, coeffY);

                    // convert range to bt709
                    y = y * 219.0 / 255.0 + 16.0 / 255.0;

                    gl_FragData[0] = vec4(y, 0.0, 0.0, 1.0);
                })",
                .m_readPixels =
                    [this](float x, float y) {
                        glReadPixels(x, y, m_alignedWidth, m_alignedHeight,
                                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
                    },
            },
            RenderPass{
                .m_width = m_alignedWidth / 2,
                .m_height = m_alignedHeight / 2,
                .m_vertexShader = vertexShader,
                .m_fragmentShader = R"(#version 130
            precision highp float;

            in vec2 v_texCoord;
            out vec4 fragColor;

            uniform sampler2D u_texture;
            uniform vec2 u_texelSize;

            const vec3 coeffU = vec3(-0.11457, -0.38543, 0.5);
            const vec3 coeffV = vec3(0.5, -0.45415, -0.04585);

            void main() {
                vec3 rgb00 = texture2D(u_texture, v_texCoord).rgb;
                vec3 rgb01 = texture2D(u_texture, v_texCoord + vec2(u_texelSize.x, 0)).rgb;
                vec3 rgb10 = texture2D(u_texture, v_texCoord + vec2(0, u_texelSize.y)).rgb;
                vec3 rgb11 = texture2D(u_texture, v_texCoord + u_texelSize).rgb;

                vec3 rgb = (rgb00 + rgb01 + rgb10 + rgb11) / 4.0;

                float u = dot(rgb, coeffU) + 0.5;
                float v = dot(rgb, coeffV) + 0.5;

                // convert range to bt709
                u = u * 224.0 / 255.0 + 16.0 / 255.0;
                v = v * 224.0 / 255.0 + 16.0 / 255.0;
            
                gl_FragData[0] = vec4(u, v, 0.0, 1.0);
            })",
                .m_readPixels =
                    [this, yPlaneSize](float x, float y) {
                        glReadPixels(x, y, m_alignedWidth / 2,
                                     m_alignedHeight / 2, GL_RG,
                                     GL_UNSIGNED_BYTE,
                                     reinterpret_cast<void*>(yPlaneSize));
                    },
            },
        };
    }

    size_t getBufferSize() override {
        const uintptr_t yPlaneSize = m_alignedWidth * m_alignedHeight;
        const uintptr_t uvPlaneSize = yPlaneSize / 4;
        return yPlaneSize + uvPlaneSize * 2;
    }

    geode::Result<> prepareFrame(AVFrame* frame, uint8_t* data,
                                 size_t size) override {
        if (!frame || !data || size == 0) {
            return geode::Err("Invalid parameters");
        }

        const uintptr_t yPlaneSize = m_alignedWidth * m_alignedHeight;

        frame->data[0] = data;
        frame->data[1] = data + yPlaneSize;

        return geode::Ok();
    }
};
