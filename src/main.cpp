#include "GucciBot.hpp"
#include "render/renderer.hpp"
#include <Geode/Geode.hpp>
using namespace geode::prelude;

$on_mod(Loaded) {
    GucciEngine::get()->initialize();
    SLRenderer::get()->loadFFmpeg();
}
