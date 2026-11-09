// An MCP-only test launch: what it is and what stays in, core/mcp_only.hpp.

#include "core/mcp_only.hpp"

#include "hacks/safemode.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace gucci::mcponly {

    namespace {
        // Function-local: the $modify classes that fill it register during
        // static initialisation, in whatever order the linker picked.
        std::vector<geode::Hook*>& hooks() {
            static std::vector<geode::Hook*> s;
            return s;
        }
    } // namespace

    bool launch() {
        static bool const s_on = [] {
            char const* v = std::getenv("GUCCIBOT_MCP_ONLY");
            return v && *v && std::string_view(v) != "0" && safemode::testLaunch();
        }();
        return s_on;
    }

    void keepHook(geode::Hook* hook) {
        if (hook && !kept(hook))
            hooks().push_back(hook);
    }

    bool kept(geode::Hook* hook) {
        auto const& k = hooks();
        return std::find(k.begin(), k.end(), hook) != k.end();
    }

    std::vector<geode::Hook*> const& keptHooks() {
        return hooks();
    }

} // namespace gucci::mcponly
