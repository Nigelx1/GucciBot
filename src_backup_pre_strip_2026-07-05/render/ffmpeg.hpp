#ifndef FFMPEG_HPP
#define FFMPEG_HPP

#pragma once

#include <Windows.h>

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define FFMPEG_FN(ident) decltype(ident)* ident;

typedef struct {
    FFMPEG_FN(avformat_alloc_output_context2)
    FFMPEG_FN(avcodec_find_encoder_by_name)
    FFMPEG_FN(avcodec_alloc_context3)
    FFMPEG_FN(avcodec_get_supported_config)
    FFMPEG_FN(av_dict_set)
    FFMPEG_FN(avcodec_open2)
    FFMPEG_FN(av_dict_free)
    FFMPEG_FN(avformat_new_stream)
    FFMPEG_FN(avcodec_parameters_from_context)
    FFMPEG_FN(avio_open)
    FFMPEG_FN(avformat_write_header)
    FFMPEG_FN(av_frame_alloc)
    FFMPEG_FN(av_frame_get_buffer)
    FFMPEG_FN(av_packet_alloc)
    FFMPEG_FN(av_frame_unref)
    FFMPEG_FN(av_frame_make_writable)
    FFMPEG_FN(av_image_fill_linesizes)
    FFMPEG_FN(avcodec_send_frame)
    FFMPEG_FN(avcodec_receive_packet)
    FFMPEG_FN(av_packet_rescale_ts)
    FFMPEG_FN(av_interleaved_write_frame)
    FFMPEG_FN(av_packet_unref)
    FFMPEG_FN(av_channel_layout_default)
    FFMPEG_FN(swr_alloc_set_opts2)
    FFMPEG_FN(swr_init)
    FFMPEG_FN(swr_convert)
    FFMPEG_FN(av_write_trailer)
    FFMPEG_FN(avio_close)
    FFMPEG_FN(swr_free)
    FFMPEG_FN(avcodec_free_context)
    FFMPEG_FN(av_packet_free)
} ff_t;

static std::vector<std::string> DLL_FUNCTION_NAMES = {
    "avformat_alloc_output_context2",
    "avcodec_find_encoder_by_name",
    "avcodec_alloc_context3",
    "avcodec_get_supported_config",
    "av_dict_set",
    "avcodec_open2",
    "av_dict_free",
    "avformat_new_stream",
    "avcodec_parameters_from_context",
    "avio_open",
    "avformat_write_header",
    "av_frame_alloc",
    "av_frame_get_buffer",
    "av_packet_alloc",
    "av_frame_unref",
    "av_frame_make_writable",
    "av_image_fill_linesizes",
    "avcodec_send_frame",
    "avcodec_receive_packet",
    "av_packet_rescale_ts",
    "av_interleaved_write_frame",
    "av_packet_unref",
    "av_channel_layout_default",
    "swr_alloc_set_opts2",
    "swr_init",
    "swr_convert",
    "av_write_trailer",
    "avio_close",
    "swr_free",
    "avcodec_free_context",
    "av_packet_free"};

inline void* loadFunction(HMODULE* modules, size_t moduleSize,
                          const char* name) {
    void* fn = 0;

    size_t moduleIdx = 0;
    geode::log::info("[RENDERER] Resolving symbol {}...", name);
    while (fn == 0 && moduleIdx < moduleSize) {
        fn = reinterpret_cast<void*>(GetProcAddress(modules[moduleIdx], name));
        moduleIdx++;
    }

    if (fn != 0) {
        // geode::log::info("[RENDERER] Successfully loaded symbol {}", name);
    } else {
        geode::log::error("[RENDERER] Failed to load symbol {}", name);
    }

    return fn;
}

static_assert(sizeof(ff_t) == sizeof(void*) * 31);

inline bool loadFFmpegFunctions(void* ff) {
    std::vector<HMODULE> modules;
    std::vector<std::string> dlls = {
        "avutil-60.dll",   "swresample-6.dll", "swscale-9.dll",
        "avcodec-62.dll",  "avformat-62.dll",  "avfilter-11.dll",
        "avdevice-62.dll",
    };

    for (std::string& s : dlls) {
        geode::log::info(
            "[RENDERER] Loading library {}",
            geode::Mod::get()->getPersistentDir() / "libraries" / s);

        HMODULE mod = LoadLibraryW(
            (geode::Mod::get()->getPersistentDir() / "libraries" / s)
                .wstring()
                .c_str());

        if (mod == NULL) {
            DWORD errorCode = GetLastError();
            LPSTR messageBuffer = nullptr;
            FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&messageBuffer, 0, NULL);

            geode::log::error("[RENDERER] Failed to load {}: Error {} - {}", s,
                              errorCode, messageBuffer);

            LocalFree(messageBuffer);
        }

        modules.push_back(mod);
    }

    uintptr_t i = 0;
    geode::log::info("Loading {} functions...", DLL_FUNCTION_NAMES.size());
    for (std::string& s : DLL_FUNCTION_NAMES) {
        void** loc =
            reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ff) + i);
        *loc = loadFunction(modules.data(), modules.size(), s.c_str());

        if (*loc == 0) {
            return false;
        }

        i += 8;
    }

    return true;
}

#endif