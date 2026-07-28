#pragma once

#include <cocos2d.h>

#include <atomic>
#include <memory>

#include "colorspace/colorspace.hpp"
#include "pass.hpp"

class SLRenderTexture {
   public:
    void init(std::unique_ptr<Colorspace> colorspace);
    void destroy() const;
    void capture(uint8_t** data, std::atomic<bool>& hasDataFlag);
    void postCapture();
    void displayPreview();

   public:
    std::unique_ptr<Colorspace> m_colorspace;
    std::vector<RenderPass> m_passes;

    uint32_t m_width, m_height;
    uint32_t m_alignedWidth, m_alignedHeight;
    uint32_t m_widthOffset, m_heightOffset;

    uint32_t m_tex[2];
    uint32_t m_quadVAO, m_quadVBO;

    int m_old_fbo, m_old_rbo;
    uint32_t m_fbo[2];
    uint32_t m_pbo;

    uint32_t m_program;
};
