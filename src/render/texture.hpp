#pragma once

#include <cocos2d.h>

#include <atomic>
#include <memory>

#include "colorspace/colorspace.hpp"
#include "pass.hpp"

namespace gucci {

    class SLRenderTexture {
    public:
        void init(std::unique_ptr<Colorspace> colorspace);
        void destroy();
        // Async readback, ported from Silicate 2026-09-22. capture() mapped the
        // single PBO on the spot, which stalls the GL thread until the GPU has
        // finished the copy -- every frame, for the whole render.
        //
        // issue() starts a readback into the next slot of a ring and drops a
        // fence; tryHarvest() maps a slot only once its fence says the data has
        // landed. Frames in flight overlap with frames still being drawn.
        static constexpr int RING_SIZE = 8;

        void issue(float fadeThreshold);
        bool tryHarvest(uint8_t** outData, bool block);
        void releaseSlot();

        int64_t issuedCount() const {
            return m_issued;
        }
        int64_t mappedCount() const {
            return m_mapped;
        }
        void displayPreview();

    public:
        std::unique_ptr<Colorspace> m_colorspace;
        std::vector<RenderPass> m_passes;

        uint32_t m_width, m_height;
        uint32_t m_alignedWidth, m_alignedHeight;
        uint32_t m_widthOffset, m_heightOffset;
        // The window's real framebuffer size, so a render that owns the view
        // can still reason about the window it will be handed back to.
        uint32_t m_windowWidth = 0, m_windowHeight = 0;

        uint32_t m_tex[2];
        uint32_t m_quadVAO, m_quadVBO;

        int m_old_fbo, m_old_rbo;
        uint32_t m_fbo[2];
        uint32_t m_pbo[RING_SIZE] = {};
        void* m_fence[RING_SIZE] = {};
        uint8_t* m_slotData[RING_SIZE] = {};
        bool m_slotMapped[RING_SIZE] = {};
        int64_t m_issued = 0;
        int64_t m_mapped = 0;

        uint32_t m_program;
    };

} // namespace gucci
