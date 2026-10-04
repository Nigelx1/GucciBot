#pragma once

#include "render/ffmpeg.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gucci {

    // Decodes a local video file and serves individual frames by timestamp,
    // for Video Mode's synced playback overlay. This class has no opinion
    // about sync -- Video Mode (trainers/videomode.*) decides which
    // timestamp to ask for from the click bar's clock; this just answers "give me the frame at
    // time T" as tightly-packed RGBA8.
    //
    // Uses SLRenderer::get()->ff for the actual FFmpeg function pointers
    // (loaded once at mod startup for the export pipeline) rather than
    // loading the DLLs a second time -- decode functions were added to that
    // same table (render/ffmpeg.hpp) specifically so this could reuse it.
    class VideoDecoder {
    public:
        ~VideoDecoder();

        // Opens `path` and probes its video stream. Returns false (and logs
        // why) on any failure -- missing file, no video stream, a codec
        // FFmpeg doesn't have a decoder for, SLRenderer's ff table not
        // loaded yet, etc.
        bool open(const std::filesystem::path& path);
        void close();
        bool isOpen() const {
            return m_formatCtx != nullptr;
        }

        // Returns the DECODE-TARGET dimensions (what getFrameAt actually
        // fills outRgba with), not necessarily the source video's native
        // resolution -- see m_outWidth/m_outHeight. Callers (texture
        // upload, letterbox aspect-ratio math) should use these, not the
        // source's own size.
        int width() const {
            return m_outWidth;
        }
        int height() const {
            return m_outHeight;
        }
        double durationSec() const {
            return m_durationSec;
        }

        // The timestamp of the frame getFrameAt last served, and the length
        // of one frame of this video.
        double framePtsSec() const {
            return m_lastDecodedSec;
        }
        double frameDurationSec() const {
            return m_frameDurationSec;
        }

        // Decodes the frame shown at `seconds` (the first frame at or after
        // it) into `outRgba` as tightly-packed RGBA8, width()*height()*4
        // bytes. Seeks first when `seconds` is behind the decoded frame or
        // more than two seconds ahead of it, otherwise decodes forward.
        // Returns false, leaving `outRgba` unchanged, when the frame already
        // decoded is still the one for `seconds` (the caller keeps showing
        // it), and when nothing could be decoded there.
        bool getFrameAt(double seconds, std::vector<uint8_t>& outRgba);

    private:
        bool decodeNextFrame();
        bool convertCurrentFrameToRgba(std::vector<uint8_t>& outRgba);

        AVFormatContext* m_formatCtx = nullptr;
        AVCodecContext* m_codecCtx = nullptr;
        SwsContext* m_swsCtx = nullptr;
        AVFrame* m_frame = nullptr;
        AVFrame* m_rgbaFrame = nullptr;
        AVPacket* m_packet = nullptr;
        int m_videoStreamIndex = -1;
        int m_width = 0, m_height = 0; // source video's native resolution
        // Decode/upload target resolution, computed at open() -- capped
        // down from the source's native size (aspect ratio preserved) to
        // cut per-frame sws_scale + RGBA-buffer + GPU-texture-upload cost.
        // Nigel reported ~5fps in Video Mode when every frame went up to
        // the GPU at full 1080p. See kMaxOutWidth in the .cpp.
        int m_outWidth = 0, m_outHeight = 0;
        double m_durationSec = 0.0;
        double m_timeBase = 0.0; // stream's time_base as num/den
        double m_lastDecodedSec = -1.0;
        // The frame decoded just before it in the same run; -1 after a seek.
        double m_prevDecodedSec = -1.0;
        // One frame of this video, from the stream's own frame rate at
        // open(). getFrameAt uses it to place the frame before the first one
        // decoded after a seek. (A fixed guess here once held 60 fps video
        // to about 16 fps: the "way less frames than mpv" report, 2026-08-31.)
        double m_frameDurationSec = 1.0 / 30.0; // safe fallback if the
                                                 // stream doesn't report one
    };

} // namespace gucci
