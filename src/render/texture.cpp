#include "texture.hpp"
#include "renderer.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "colorspace/yuv420p.hpp"

#ifdef SILICATE_PROTECT
#include "VMProtect/VMProtectSDK.h"
#endif

namespace gucci {

    using namespace cocos2d;

    static void silentChangeSize(CCSize size, float, float) {
        auto director = CCDirector::sharedDirector();
        auto view = CCEGLView::sharedOpenGLView();
        view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
        director->updateScreenScale(size);
        director->setViewport();
        director->setProjection(kCCDirectorProjection2D);
        glViewport(0, 0, size.width, size.height);
    }

    void SLRenderTexture::init(std::unique_ptr<Colorspace> colorspace) {
#ifdef SILICATE_PROTECT
        VMProtectBegin("SLRenderTexture::init");
#endif

        m_colorspace = std::move(colorspace);
        m_colorspace->m_alignedWidth = m_alignedWidth;
        m_colorspace->m_alignedHeight = m_alignedHeight;

        m_passes = this->m_colorspace->getPasses();

        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_old_fbo);

        for (auto& pass : m_passes) {
            pass.initialize();
        }

        glGenBuffers(RING_SIZE, m_pbo);
        for (int i = 0; i < RING_SIZE; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glBufferData(
                GL_PIXEL_PACK_BUFFER, m_colorspace->getBufferSize(), nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_issued = 0;
        m_mapped = 0;
        for (int i = 0; i < RING_SIZE; i++) {
            m_fence[i] = nullptr;
            m_slotData[i] = nullptr;
            m_slotMapped[i] = false;
        }

        float vertices[] = {-1.0f,
                            -1.0f,
                            0.0f,
                            0.0f,
                            1.0f,
                            -1.0f,
                            1.0f,
                            0.0f,
                            1.0f,
                            1.0f,
                            1.0f,
                            1.0f,
                            -1.0f,
                            1.0f,
                            0.0f,
                            1.0f};

        glGenVertexArrays(1, &m_quadVAO);
        glBindVertexArray(m_quadVAO);

        glGenBuffers(1, &m_quadVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);

        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glVertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);

        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, m_old_fbo);

#ifdef SILICATE_PROTECT
        VMProtectEnd();
#endif
    }

    // Starts a readback into the next ring slot and drops a fence. Does not
    // map: mapping here is what stalled the GL thread on the GPU every frame.
    // tryHarvest picks the data up once the fence says it has landed.
    void SLRenderTexture::issue(float fadeThreshold) {
        (void)fadeThreshold;  // read by the shaders via SLRenderer::m_fadeThreshold
        int const slot = (int)(m_issued % RING_SIZE);

        // Reclaim the slot we are about to overwrite.
        if (m_slotMapped[slot]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            m_slotMapped[slot] = false;
            m_slotData[slot] = nullptr;
        }
        if (m_fence[slot]) {
            glDeleteSync(static_cast<GLsync>(m_fence[slot]));
            m_fence[slot] = nullptr;
        }

        auto director = cocos2d::CCDirector::sharedDirector();

        CCSize size = director->getOpenGLView()->getFrameSize();
        m_width = size.width;
        m_height = size.height;

        int blend;
        glGetIntegerv(GL_BLEND, &blend);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_issued % RING_SIZE]);

        for (size_t i = 0; i < m_passes.size(); ++i) {
            auto& pass = m_passes[i];
            glBindFramebuffer(GL_FRAMEBUFFER, pass.m_fbo);
            glViewport(0, 0, pass.m_width, pass.m_height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            if (i == 0) {
                silentChangeSize(
                    cocos2d::CCSize(pass.m_width, pass.m_height), m_widthOffset, m_heightOffset);

                CCDirector::get()->m_pRunningScene->visit();
            } else {
                glDisable(GL_BLEND);
            }

            if (pass.m_vertexShader && pass.m_fragmentShader) {
                glUseProgram(pass.m_program);

                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, m_passes[pass.m_sourceTex].m_tex);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, pass.m_tex);

                glUniform1i(glGetUniformLocation(pass.m_program, "u_texture"), 1);
                glUniform1f(glGetUniformLocation(pass.m_program, "u_fade"),
                            SLRenderer::get()->m_fadeThreshold);
                glUniform2f(glGetUniformLocation(pass.m_program, "u_texelSize"),
                            1.0f / pass.m_width,
                            1.0f / pass.m_height);

                glBindVertexArray(m_quadVAO);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                glBindVertexArray(0);
            }

            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            pass.m_readPixels(0, 0);
        }

        m_fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_issued++;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        silentChangeSize(cocos2d::CCSize(m_width, m_height), 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, m_old_fbo);

        glUseProgram(0);
        glEnable(GL_BLEND);
    }

    // Maps the oldest outstanding slot, but only once its fence reports the
    // copy is done -- so the map itself never waits. With block=true it will
    // wait up to a second, which is what the end-of-render drain uses.
    bool SLRenderTexture::tryHarvest(uint8_t** outData, bool block) {
        if (m_mapped >= m_issued)
            return false;

        int const slot = (int)(m_mapped % RING_SIZE);
        auto fence = static_cast<GLsync>(m_fence[slot]);
        if (!fence)
            return false;

        GLbitfield const flags = block ? GL_SYNC_FLUSH_COMMANDS_BIT : 0;
        GLuint64 const timeout = block ? 1'000'000'000ull : 0;
        GLenum const status = glClientWaitSync(fence, flags, timeout);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
            return false;  // still in flight

        glDeleteSync(fence);
        m_fence[slot] = nullptr;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[slot]);
        auto* pixelData = static_cast<uint8_t*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        if (!pixelData) {
            geode::log::error("[GucciBot] failed to map readback buffer: {}", glGetError());
            m_mapped++;
            return false;
        }

        m_slotData[slot] = pixelData;
        m_slotMapped[slot] = true;
        *outData = pixelData;
        m_mapped++;
        return true;
    }

    void SLRenderTexture::releaseSlot() {
        for (int i = 0; i < RING_SIZE; i++) {
            if (!m_slotMapped[i])
                continue;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            m_slotMapped[i] = false;
            m_slotData[i] = nullptr;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    void SLRenderTexture::displayPreview() {
        CCSize size = CCDirector::sharedDirector()->getOpenGLView()->getFrameSize();

        int blend;
        glGetIntegerv(GL_BLEND, &blend);

        glDisable(GL_BLEND);

        glClearColor(0, 0, 0, 1);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_passes[0].m_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_old_fbo);

        glBlitFramebuffer(m_widthOffset,
                          m_heightOffset,
                          m_alignedWidth,
                          m_alignedHeight,
                          0,
                          0,
                          size.width,
                          size.height,
                          GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);

        if (blend) {
            glEnable(GL_BLEND);
        }
    }

    void SLRenderTexture::destroy() {
        glDeleteTextures(2, m_tex);
        glDeleteFramebuffers(2, m_fbo);
        for (int i = 0; i < RING_SIZE; i++) {
            if (m_fence[i]) {
                glDeleteSync(static_cast<GLsync>(m_fence[i]));
                m_fence[i] = nullptr;
            }
        }
        glDeleteBuffers(RING_SIZE, m_pbo);
        glDeleteVertexArrays(1, &m_quadVAO);
        glDeleteBuffers(1, &m_quadVBO);
        glDeleteProgram(m_program);

        glBindFramebuffer(GL_FRAMEBUFFER, m_old_fbo);
    }

} // namespace gucci
