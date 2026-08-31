#pragma once

#include "render/ffmpeg.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gucci {

    // Decodes a local video file and serves individual frames by timestamp,
    // for Video Mode's synced playback overlay. This class has no opinion
    // about sync -- drawJupiterClickBar's own scrub/loop/pause clock decides
    // which timestamp to ask for; this just answers "give me the frame at
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

        int width() const {
            return m_width;
        }
        int height() const {
            return m_height;
        }
        double durationSec() const {
            return m_durationSec;
        }

        // Decodes and returns the frame visible at `seconds` into the video,
        // as tightly-packed RGBA8 (width()*height()*4 bytes, resized as
        // needed). Seeks first if the requested time isn't forward-adjacent
        // to the last decoded frame -- backward seeks and big forward jumps
        // both go through av_seek_frame + decode-forward-to-target, which is
        // the real, untested performance unknown here; fine for a user
        // manually scrubbing/looping a short clip, not verified for anything
        // faster or longer. Returns false, leaving `outRgba` unchanged, if
        // nothing could be decoded at that position (e.g. past EOF or before
        // the file's first frame) -- ALSO returns false, by design, if
        // `seconds` is still within about one frame's duration of what's
        // already decoded (see kFrameTolerance in the .cpp): the caller
        // should keep showing the current frame rather than get handed a
        // "new" one, since without this a static/near-static `seconds` (e.g.
        // Video Mode sitting open with no macro actually playing, so its
        // driving clock never advances) made this oscillate between two
        // adjacent frames on alternating calls -- a real, confirmed flicker
        // bug, not a hypothetical one (2026-08-31).
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
        int m_width = 0, m_height = 0;
        double m_durationSec = 0.0;
        double m_timeBase = 0.0; // stream's time_base as num/den
        double m_lastDecodedSec = -1.0;
    };

} // namespace gucci
