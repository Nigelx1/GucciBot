#pragma once

// Video Mode: the JMF showcase video (resources/jmf_showcase.mp4, bundled with
// the mod) drawn full screen, riding the Jupiter My Favourite Trainer's click
// bar clock, for reviewing a run against the macro's timing. No level needs
// to be open. The Alignment Tool scrubs the video by hand to the frame of the
// macro's first click and sets the sync offset from it.
//
// The state lives on GucciEngine (jupiterVideoModeEnabled, jupiterVideoOffsetSec,
// jupiterVideoOpacity, jupiterVideoAlignToolActive, jupiterVideoAlignScrubSec).
// Decoding runs on a thread of its own (render/video_decoder.*), so seeks and
// decodes never hold up a frame; the main thread only uploads the newest
// finished frame. Turning the mode off stops the thread, closes the decoder
// and frees every frame and the texture.
//
// Written fresh for 2.0 (the old overlay went with build -bz). The card is
// ui/pages/videomode_card.cpp.

#include <string>

namespace gucci::videomode {

    // Saved settings (the offset and the opacity), read once. The overlay's
    // first frame does it; the card calls it too, harmlessly.
    void ensureLoaded();
    void saveSettings();

    // Once a frame from the ImGui draw callback, menu open or not. Draws the
    // video behind every ImGui window and on top of the game; starts and
    // stops the decoder as the mode is switched on and off.
    void drawOverlay();

    enum class Status { Off, Opening, Ready, Failed };
    Status status();
    // Why the video didn't open (Status::Failed).
    std::string failure();

    // The video, once it is open: decoded size, length, one frame's length.
    int width();
    int height();
    double durationSec();
    double frameSec();

    // The timestamp of the frame on screen; false before the first one.
    bool shownFrame(double& ptsSec);
    // The video time the overlay is asking for: the click bar's clock plus
    // the offset, or the scrub position while the Alignment Tool is on.
    double targetSec();

    // The macro's first press, in macro time; false when the JMF macro has
    // no clicks.
    bool firstClickSec(double& sec);

    // ---- the Alignment Tool

    // Switching it on starts the scrub where the video is now, so it doesn't jump.
    void setAlignTool(bool on);
    // Moves the scrub by whole frames from the frame on screen.
    void stepFrames(int frames);
    // Sets the offset so the frame on screen lines up with the macro's first
    // press, saves it and closes the tool. False when there is no frame on
    // screen or no first press.
    bool setOffsetFromShownFrame();

} // namespace gucci::videomode
