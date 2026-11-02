// The menu art's textures (ui/art.hpp).

#include "render/gl_compat.hpp"
#include "ui/art.hpp"
#include "ui/png.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace gucci::ui::art {

    Texture::~Texture() {
        release();
    }

    bool Texture::load(std::string_view file, std::string& error) {
        release();
        auto const path = Mod::get()->getResourcesDir() / std::string(file);
        auto bytes = geode::utils::file::readBinary(path);
        if (!bytes) {
            error = fmt::format("{}: {}", file, bytes.unwrapErr());
            return false;
        }
        auto const& got = bytes.unwrap();
        png::Pixels px;
        if (!png::decode(got.data(), got.size(), px, error)) {
            error = fmt::format("{}: {}", file, error);
            return false;
        }

        // The upload reads the pixels straight from memory: nothing bound
        // for unpacking (Video Mode puts its binding back, but be sure), no
        // row length left over, and the alignment put back afterwards, as
        // Video Mode's upload does.
        GLint alignment = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
#if GB_DESKTOP_GL
        GLint unpackBuffer = 0, rowLength = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        if (unpackBuffer)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
        auto* texture = new CCTexture2D();
        bool const made = texture->initWithData(px.rgba.data(), kCCTexture2DPixelFormat_RGBA8888,
                                                static_cast<unsigned>(px.width), static_cast<unsigned>(px.height),
                                                CCSize(static_cast<float>(px.width), static_cast<float>(px.height)));
        if (made) {
            ccTexParams params = {GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
            texture->setTexParameters(&params);
        }
#if GB_DESKTOP_GL
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        if (unpackBuffer)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
#endif
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        if (!made) {
            texture->release();
            error = fmt::format("{}: the texture couldn't be made", file);
            return false;
        }
        m_texture = texture;
        m_width = px.width;
        m_height = px.height;
#if defined(GEODE_IS_ANDROID)
        m_keep = std::move(px.rgba);
#endif
        return true;
    }

    void Texture::release() {
        if (m_texture) {
            m_texture->release();
            m_texture = nullptr;
        }
        m_width = m_height = 0;
        std::vector<uint8_t>().swap(m_keep);
    }

    ImTextureID Texture::id() const {
        return m_texture ? static_cast<ImTextureID>(m_texture->getName()) : ImTextureID{};
    }

} // namespace gucci::ui::art
