#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include "render/renderer.hpp"

using namespace geode::prelude;
using namespace gucci;

// Ported from Silicate 2026-09-22. GucciBot had no CCEGLView hook at all.
//
// A render usually runs at a resolution the window is not -- 8K out of a 1080p
// window, say -- which means the view is lying about its size for the whole
// render. Anything that fires a resize while that is true (the OS, a DPI
// change, alt-tab, the user dragging the frame, GD itself) went straight
// through to cocos, which resized the view out from under the render.
//
// These three ask the renderer first. It returns true when it owns the view,
// which swallows the event and records the new WINDOW size instead, so the
// render keeps its resolution and restoreView still puts back something real
// at the end. When no render is running they pass through untouched.
struct GB7CCEGLView : Modify<GB7CCEGLView, CCEGLView> {
    void setFrameSize(float width, float height) {
        if (SLRenderer::get()->handleFrameSizeChange(width, height))
            return;
        CCEGLView::setFrameSize(width, height);
    }

    void onGLFWframebuffersize(GLFWwindow* window, int width, int height) {
        if (SLRenderer::get()->handleFramebufferSizeChange(width, height))
            return;
        CCEGLView::onGLFWframebuffersize(window, width, height);
    }

    void onGLFWWindowSizeFunCallback(GLFWwindow* window, int width, int height) {
        if (SLRenderer::get()->handleWindowSizeChange())
            return;
        CCEGLView::onGLFWWindowSizeFunCallback(window, width, height);
    }
};
