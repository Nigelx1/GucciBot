#include "render/video_decoder.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>

#include <cstring>

namespace gucci {

    VideoDecoder::~VideoDecoder() {
        close();
    }

    bool VideoDecoder::open(const std::filesystem::path& path) {
        close();
        auto* renderer = SLRenderer::get();
        if (!renderer->isFFmpegLoaded()) {
            geode::log::error("[VideoDecoder] FFmpeg not loaded, can't open {}", path.string());
            return false;
        }
        auto* ff = renderer->ff;

        if (ff->avformat_open_input(&m_formatCtx, path.string().c_str(), nullptr, nullptr) < 0) {
            geode::log::error("[VideoDecoder] avformat_open_input failed for {}", path.string());
            return false;
        }
        if (ff->avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
            geode::log::error("[VideoDecoder] avformat_find_stream_info failed for {}", path.string());
            close();
            return false;
        }

        for (unsigned i = 0; i < m_formatCtx->nb_streams; i++) {
            if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                m_videoStreamIndex = (int)i;
                break;
            }
        }
        if (m_videoStreamIndex < 0) {
            geode::log::error("[VideoDecoder] no video stream in {}", path.string());
            close();
            return false;
        }

        auto* stream = m_formatCtx->streams[m_videoStreamIndex];
        const AVCodec* decoder = ff->avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            geode::log::error("[VideoDecoder] no decoder for codec id {} in {}",
                              (int)stream->codecpar->codec_id,
                              path.string());
            close();
            return false;
        }

        m_codecCtx = ff->avcodec_alloc_context3(decoder);
        if (!m_codecCtx || ff->avcodec_parameters_to_context(m_codecCtx, stream->codecpar) < 0 ||
            ff->avcodec_open2(m_codecCtx, decoder, nullptr) < 0) {
            geode::log::error("[VideoDecoder] failed to open decoder for {}", path.string());
            close();
            return false;
        }

        m_width = m_codecCtx->width;
        m_height = m_codecCtx->height;
        m_timeBase = stream->time_base.den != 0
                        ? (double)stream->time_base.num / (double)stream->time_base.den
                        : 0.0;
        m_durationSec =
            m_formatCtx->duration > 0 ? (double)m_formatCtx->duration / AV_TIME_BASE : 0.0;

        m_frame = ff->av_frame_alloc();
        m_packet = ff->av_packet_alloc();
        if (!m_frame || !m_packet || m_timeBase <= 0.0) {
            geode::log::error("[VideoDecoder] setup failed for {}", path.string());
            close();
            return false;
        }

        geode::log::info("[VideoDecoder] Opened {} ({}x{}, {:.2f}s)",
                         path.string(),
                         m_width,
                         m_height,
                         m_durationSec);
        return true;
    }

    void VideoDecoder::close() {
        auto* renderer = SLRenderer::get();
        auto* ff = (renderer && renderer->isFFmpegLoaded()) ? renderer->ff : nullptr;
        if (ff) {
            if (m_swsCtx) {
                ff->sws_freeContext(m_swsCtx);
                m_swsCtx = nullptr;
            }
            if (m_rgbaFrame)
                ff->av_frame_free(&m_rgbaFrame);
            if (m_frame)
                ff->av_frame_free(&m_frame);
            if (m_packet)
                ff->av_packet_free(&m_packet);
            if (m_codecCtx)
                ff->avcodec_free_context(&m_codecCtx);
            if (m_formatCtx)
                ff->avformat_close_input(&m_formatCtx);
        }
        m_formatCtx = nullptr;
        m_codecCtx = nullptr;
        m_videoStreamIndex = -1;
        m_width = m_height = 0;
        m_durationSec = 0.0;
        m_timeBase = 0.0;
        m_lastDecodedSec = -1.0;
    }

    bool VideoDecoder::decodeNextFrame() {
        auto* ff = SLRenderer::get()->ff;
        for (;;) {
            int recv = ff->avcodec_receive_frame(m_codecCtx, m_frame);
            if (recv == 0)
                return true;
            if (recv != AVERROR(EAGAIN))
                return false; // AVERROR_EOF or a real decode error

            int readRet = ff->av_read_frame(m_formatCtx, m_packet);
            if (readRet < 0) {
                // Container exhausted -- signal EOF to the decoder and let
                // the next receive_frame drain (or report) it.
                ff->avcodec_send_packet(m_codecCtx, nullptr);
                continue;
            }
            if (m_packet->stream_index != m_videoStreamIndex) {
                ff->av_packet_unref(m_packet);
                continue;
            }
            ff->avcodec_send_packet(m_codecCtx, m_packet);
            ff->av_packet_unref(m_packet);
        }
    }

    bool VideoDecoder::convertCurrentFrameToRgba(std::vector<uint8_t>& outRgba) {
        auto* ff = SLRenderer::get()->ff;
        if (!m_swsCtx) {
            m_swsCtx = ff->sws_getContext(m_width,
                                         m_height,
                                         (AVPixelFormat)m_codecCtx->pix_fmt,
                                         m_width,
                                         m_height,
                                         AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr);
            if (!m_swsCtx)
                return false;
        }
        if (!m_rgbaFrame) {
            m_rgbaFrame = ff->av_frame_alloc();
            if (!m_rgbaFrame)
                return false;
            m_rgbaFrame->format = AV_PIX_FMT_RGBA;
            m_rgbaFrame->width = m_width;
            m_rgbaFrame->height = m_height;
            if (ff->av_frame_get_buffer(m_rgbaFrame, 0) < 0)
                return false;
        }

        ff->sws_scale(m_swsCtx,
                     m_frame->data,
                     m_frame->linesize,
                     0,
                     m_height,
                     m_rgbaFrame->data,
                     m_rgbaFrame->linesize);

        outRgba.resize((size_t)m_width * (size_t)m_height * 4);
        int lineBytes = m_width * 4;
        for (int y = 0; y < m_height; y++) {
            std::memcpy(outRgba.data() + (size_t)y * lineBytes,
                       m_rgbaFrame->data[0] + (size_t)y * m_rgbaFrame->linesize[0],
                       lineBytes);
        }
        return true;
    }

    bool VideoDecoder::getFrameAt(double seconds, std::vector<uint8_t>& outRgba) {
        if (!isOpen())
            return false;
        auto* ff = SLRenderer::get()->ff;

        // Real, traced bug (2026-08-31): the old strict `seconds <
        // m_lastDecodedSec` check had no tolerance, but "decode forward"
        // below always lands on whatever the NEXT frame's actual pts happens
        // to be -- essentially never exactly equal to the requested
        // `seconds`. With a static or near-static requested time (Video Mode
        // open but nothing actually advancing jupiterClickBarPosSec), that
        // made this function oscillate call to call: decode-forward lands
        // one frame past the target -> the following call sees `seconds <
        // m_lastDecodedSec` -> seeks backward to the target frame -> the
        // call after THAT decodes forward past it again -> repeat forever,
        // alternating between two adjacent real frames every other call.
        // Both frames decode and upload successfully every time -- that's
        // why every CPU-side log line looked clean while Nigel still saw a
        // real flicker. Fix: treat "still within about one frame's duration
        // of what's already decoded" as "nothing to do" in both directions,
        // rather than only guarding the forward-jump case.
        constexpr double kFrameTolerance = 0.06; // ~1 frame at >=16fps, with margin
        if (m_lastDecodedSec >= 0.0 && seconds >= m_lastDecodedSec - kFrameTolerance &&
            seconds <= m_lastDecodedSec + kFrameTolerance) {
            return false; // current frame is still the right one to show
        }

        bool needSeek = m_lastDecodedSec < 0.0 || seconds < m_lastDecodedSec - kFrameTolerance ||
                       (seconds - m_lastDecodedSec) > 2.0;
        if (needSeek) {
            int64_t targetTs = (int64_t)(seconds / m_timeBase);
            if (ff->av_seek_frame(m_formatCtx, m_videoStreamIndex, targetTs, AVSEEK_FLAG_BACKWARD) <
                0)
                return false;
            ff->avcodec_flush_buffers(m_codecCtx);
        }

        bool gotAny = false;
        for (;;) {
            if (!decodeNextFrame()) {
                if (!gotAny)
                    return false;
                break; // ran out of frames -- use the last good one we have
            }
            gotAny = true;
            double pts = (double)m_frame->pts * m_timeBase;
            m_lastDecodedSec = pts;
            if (pts >= seconds)
                break;
        }
        return convertCurrentFrameToRgba(outRgba);
    }

} // namespace gucci
