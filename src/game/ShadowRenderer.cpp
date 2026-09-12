#include "game/ShadowRenderer.hpp"

#include <vector>

#include "app/Application.hpp"
#include "core/Log.hpp"
#include "formats/ImageIO.hpp"  // decodeImage: WebP shadow when the pack ships no .spr
#include "formats/Spr.hpp"
#include "render/Shader.hpp"
#include "resource/Vfs.hpp"

namespace uaro {

namespace {
struct SVertex {
    f32 x, y, z, u, v;
    u32 abgr;
};
constexpr float kShadowAlpha = 0.42f;  // oval translucency (RO's shadow is a soft dark blob)
// MUST match CharacterActor's kWorldPerPx (kBodyHeightCells / kRefBodyPx = 1/92) so the
// shadow is sized in the same world-per-pixel space as the body standing on it.
constexpr float kWorldPerPx = 1.0f / 92.0f;
// The 35x17 sprite is a top-down oval; on the ground viewed at the ~50deg RO camera the
// depth (Z) axis is foreshortened, so stretch it to read as a round-ish shadow.
constexpr float kDepthStretch = 1.9f;
}  // namespace

bool ShadowRenderer::load(Application& app) {
    u16 texW = 0, texH = 0;
    std::vector<u8> rgba;
    if (auto bytes = app.vfs().read("data/sprite/shadow.spr")) {  // classic .spr
        auto spr = Sprite::parse(*bytes);
        if (spr && !spr->indexedFrames().empty()) {
            const SprFrame& fr = spr->indexedFrames()[0];
            auto px = spr->indexedToRgba(0);  // index 0 -> transparent
            if (fr.width && fr.height && px.size() >= static_cast<usize>(fr.width) * fr.height * 4) {
                texW = fr.width;
                texH = fr.height;
                rgba = std::move(px);
            }
        }
    }
    if (rgba.empty()) {  // .spr-less WebP pack: data/sprite/shadow.png.d/0.webp is the shadow oval
        auto wb = app.vfs().readQuiet("data/sprite/shadow.png.d/0.webp");
        if (!wb) wb = app.vfs().readQuiet("data/sprite/shadow.png.d/0.png");
        if (wb)
            if (auto img = decodeImage(*wb); img && img->valid()) {
                texW = static_cast<u16>(img->width);
                texH = static_cast<u16>(img->height);
                rgba = std::move(img->rgba);
            }
    }
    if (rgba.empty() || texW == 0 || texH == 0) {
        log::warn("ShadowRenderer: data/sprite/shadow.(spr|png.d) missing; no character shadows");
        return false;
    }
    // 2k/4k WebP packs author the oval K× bigger; size the ground quad at the 1k logical size (÷K)
    // while the texture stays full-res (same rule as the char sprites). Classic .spr = 1k = K 1.
    const std::string sq = app.spriteQuality();
    const float kScale = (sq == "4k") ? 4.0f : (sq == "2k") ? 2.0f : 1.0f;
    const float logicalW = static_cast<float>(texW) / kScale;
    const float logicalH = static_cast<float>(texH) / kScale;

    // vs_sprite3d, NOT vs_model: vs_model emits normal/tangent varyings fs_sprite3d doesn't read, so
    // that program fails to LINK on DX11 and shadows silently vanished (S. log). vs_sprite3d does the
    // same transform + exactly {v_texcoord0,v_color0}; its u_spriteBias nudge is zeroed at draw.
    program_ = load_program(app.assetDir(), "vs_sprite3d", "fs_sprite3d");
    if (!bgfx::isValid(program_)) {
        log::warn("ShadowRenderer: shader unavailable; no character shadows");
        return false;
    }
    sampler_ = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    fade_ = bgfx::createUniform("u_spriteFade", bgfx::UniformType::Vec4);
    bias_ = bgfx::createUniform("u_spriteBias", bgfx::UniformType::Vec4);  // vs_sprite3d depth nudge (kept 0)
    // Clamp + linear so the oval edge stays soft and never wraps.
    tex_ = bgfx::createTexture2D(
        texW, texH, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(rgba.data(), static_cast<u32>(rgba.size())));
    if (!bgfx::isValid(tex_)) {
        destroy();
        return false;
    }

    // 0.75 = half * 1.5: the oval is sized 50% bigger than the raw sprite (S. request).
    halfW_ = 0.75f * logicalW * kWorldPerPx;
    halfD_ = 0.75f * logicalH * kWorldPerPx * kDepthStretch;
    layout_.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    ready_ = true;
    log::info("ShadowRenderer: shadow {}x{} loaded", texW, texH);
    return true;
}

void ShadowRenderer::destroy() {
    if (bgfx::isValid(tex_)) bgfx::destroy(tex_);
    if (bgfx::isValid(sampler_)) bgfx::destroy(sampler_);
    if (bgfx::isValid(fade_)) bgfx::destroy(fade_);
    if (bgfx::isValid(bias_)) bgfx::destroy(bias_);
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    tex_ = BGFX_INVALID_HANDLE;
    sampler_ = BGFX_INVALID_HANDLE;
    fade_ = BGFX_INVALID_HANDLE;
    bias_ = BGFX_INVALID_HANDLE;
    program_ = BGFX_INVALID_HANDLE;
    ready_ = false;
}

void ShadowRenderer::draw(bgfx::ViewId view, float cx, float groundY, float cz, float scale) const {
    if (!ready_) return;
    if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6)
        return;
    const float hw = halfW_ * scale, hd = halfD_ * scale;
    const float y = groundY + 0.04f;  // tiny lift so it doesn't z-fight the terrain

    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    SVertex* vd = reinterpret_cast<SVertex*>(tvb.data);
    vd[0] = {cx - hw, y, cz - hd, 0.0f, 0.0f, 0xffffffffu};
    vd[1] = {cx + hw, y, cz - hd, 1.0f, 0.0f, 0xffffffffu};
    vd[2] = {cx + hw, y, cz + hd, 1.0f, 1.0f, 0xffffffffu};
    vd[3] = {cx - hw, y, cz + hd, 0.0f, 1.0f, 0xffffffffu};
    u16* id = reinterpret_cast<u16*>(tib.data);
    id[0] = 0; id[1] = 1; id[2] = 2; id[3] = 0; id[4] = 2; id[5] = 3;

    const float fade[4] = {kShadowAlpha, 0.0f, 0.0f, 0.0f};
    const float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // flat oval on the ground -> no depth bias
    // Translucent, depth-TESTED against the terrain (so it hugs the ground and a higher
    // floor in front hides it) but does NOT write depth — the billboards drawn after it
    // stay visible. Same recipe as the water plane.
    const u64 state = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA;
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, sampler_, tex_);
    bgfx::setUniform(fade_, fade);
    bgfx::setUniform(bias_, bias);
    bgfx::setState(state);
    bgfx::submit(view, program_);
}

} // namespace uaro
