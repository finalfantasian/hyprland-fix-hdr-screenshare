#define WLR_USE_UNSTABLE

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

static HANDLE         g_pluginHandle             = nullptr;
static CFunctionHook* g_needsUnmodifiedCopyHook  = nullptr;
static CFunctionHook* g_saveBufferForMirrorHook  = nullptr;
static bool           g_disableUnmodifiedCopyMRT = true;
static SP<CShader>     g_captureShader;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

typedef bool (*origNeedsUnmodifiedCopy)(void*);
typedef void (*origSaveBufferForMirror)(void*, const CBox&);

static constexpr float CAPTURE_EXPOSURE_AT_REF_LUMINANCE = 0.78125F;
static constexpr float CAPTURE_REFERENCE_LUMINANCE       = 80.0F;
static constexpr float CAPTURE_SATURATION                = 0.86F;
static constexpr float CAPTURE_DEFAULT_MIN_LUMINANCE      = 0.2F;

static void scheduleRefreshForAllMonitors() {
    if (!g_pCompositor)
        return;

    for (const auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor)
            continue;

        monitor->m_forceFullFrames = std::max(monitor->m_forceFullFrames, 1);
        g_pCompositor->scheduleFrameForMonitor(monitor, Aquamarine::IOutput::AQ_SCHEDULE_RENDER_MONITOR);
    }
}

static bool hkNeedsUnmodifiedCopy(void* thisptr) {
    const auto monitor = static_cast<CMonitor*>(thisptr);

    if (g_disableUnmodifiedCopyMRT && monitor && monitor->inHDR())
        return false;

    return ((origNeedsUnmodifiedCopy)g_needsUnmodifiedCopyHook->m_original)(thisptr);
}

static NColorManagement::PImageDescription captureImageDescription() {
    return NColorManagement::DEFAULT_SRGB_IMAGE_DESCRIPTION;
}

static bool ensureCaptureShader() {
    if (g_captureShader)
        return true;

    if (!Render::GL::g_pHyprOpenGL || !Render::GL::g_pHyprOpenGL->m_shadersInitialized || !Render::GL::g_pHyprOpenGL->m_shaders)
        return false;

    static const std::string FRAG = R"SHADER(
#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform float exposure;
uniform float saturation;
uniform float blackLift;
uniform float inverseSdrBrightness;
uniform float inverseSdrSaturation;
layout(location = 0) out vec4 fragColor;

vec3 acesApprox(vec3 v) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((v * (a * v + b)) / (v * (c * v + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 v) {
    bvec3 cutoff = lessThanEqual(v, vec3(0.0031308));
    vec3  lower  = v * 12.92;
    vec3  higher = 1.055 * pow(max(v, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(higher, lower, cutoff);
}

vec3 applySaturation(vec3 color, float saturation) {
    float y = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(y), color, saturation);
}

void main() {
    vec4 src = texture(tex, v_texcoord);
    vec3 rgb = max(src.rgb, vec3(0.0));

    rgb *= inverseSdrBrightness;
    rgb = applySaturation(rgb, inverseSdrSaturation);

    rgb *= exposure;
    rgb = acesApprox(rgb);

    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb        = mix(vec3(luma), rgb, saturation);

    float shadowMask = 1.0 - smoothstep(0.0, 0.25, max(max(rgb.r, rgb.g), rgb.b));
    rgb += vec3(blackLift) * shadowMask;

    rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));

    fragColor = vec4(rgb, 1.0);
}
)SHADER";

    g_captureShader = makeShared<CShader>();
    if (!g_captureShader->createProgram(Render::GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC, FRAG, true, true)) {
        g_captureShader.reset();
        return false;
    }

    return true;
}

static bool renderCaptureShader(const SP<Render::ITexture>& sourceTex, const SP<Render::IFramebuffer>& mirrorFB, const CBox& box, const float exposure, const float blackLift, const float inverseSdrBrightness, const float inverseSdrSaturation) {
    if (!sourceTex || !mirrorFB || !ensureCaptureShader())
        return false;

    auto guard = g_pHyprRenderer->bindTempFB(mirrorFB);

    Render::GL::g_pHyprOpenGL->blend(false);

    const auto& glMatrix = g_pHyprRenderer->projectBoxToTarget(box);

    glActiveTexture(GL_TEXTURE0);
    sourceTex->bind();
    sourceTex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    sourceTex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    sourceTex->setTexParameter(GL_TEXTURE_MAG_FILTER, sourceTex->magFilter);
    sourceTex->setTexParameter(GL_TEXTURE_MIN_FILTER, sourceTex->minFilter);

    auto shader = Render::GL::g_pHyprOpenGL->useShader(g_captureShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1f(glGetUniformLocation(shader->program(), "exposure"), exposure);
    glUniform1f(glGetUniformLocation(shader->program(), "saturation"), CAPTURE_SATURATION);
    glUniform1f(glGetUniformLocation(shader->program(), "blackLift"), blackLift);
    glUniform1f(glGetUniformLocation(shader->program(), "inverseSdrBrightness"), inverseSdrBrightness);
    glUniform1f(glGetUniformLocation(shader->program(), "inverseSdrSaturation"), inverseSdrSaturation);

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindBuffer(GL_ARRAY_BUFFER, shader->getUniformLocation(SHADER_SHADER_VBO));
    glBufferData(GL_ARRAY_BUFFER, sizeof(Render::GL::fullVerts), nullptr, GL_DYNAMIC_DRAW);

    auto verts = Render::GL::fullVerts;
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts.data());

    g_pHyprRenderer->m_renderData.finalDamage.forEachRect([](const auto& rect) {
        Render::GL::g_pHyprOpenGL->scissor(&rect, g_pHyprRenderer->m_renderData.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    sourceTex->unbind();
    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    Render::GL::g_pHyprOpenGL->blend(true);

    return true;
}

static void hkSaveBufferForMirror(void* thisptr, const CBox& box) {
    const auto monitorRef = g_pHyprRenderer ? g_pHyprRenderer->m_renderData.pMonitor : PHLMONITORREF{};
    const auto monitor    = monitorRef ? monitorRef.lock() : PHLMONITOR{};

    if (!monitor || !monitor->inHDR()) {
        ((origSaveBufferForMirror)g_saveBufferForMirrorHook->m_original)(thisptr, box);
        return;
    }

    const auto sourceTex = g_pHyprRenderer && g_pHyprRenderer->m_renderData.currentFB ? g_pHyprRenderer->m_renderData.currentFB->getTexture() : nullptr;
    const auto mirrorFB  = monitor && monitor->resources() ? monitor->resources()->mirrorFB() : nullptr;

    if (mirrorFB)
        mirrorFB->setImageDescription(captureImageDescription());

    const float sdrMaxLuminance = monitor ? std::max<float>(1.0F, monitor->m_sdrMaxLuminance) : CAPTURE_REFERENCE_LUMINANCE;
    const float sdrMinLuminance = monitor ? std::max<float>(0.0F, monitor->m_sdrMinLuminance) : CAPTURE_DEFAULT_MIN_LUMINANCE;
    const float exposure             = CAPTURE_EXPOSURE_AT_REF_LUMINANCE * CAPTURE_REFERENCE_LUMINANCE / sdrMaxLuminance;
    const float blackLift            = sdrMinLuminance / CAPTURE_REFERENCE_LUMINANCE;
    const float inverseSdrBrightness = monitor && monitor->m_sdrBrightness > 0.0F ? 1.0F / monitor->m_sdrBrightness : 1.0F;
    const float inverseSdrSaturation = monitor && monitor->m_sdrSaturation > 0.0F ? 1.0F / monitor->m_sdrSaturation : 1.0F;

    if (renderCaptureShader(sourceTex, mirrorFB, box, exposure, blackLift, inverseSdrBrightness, inverseSdrSaturation))
        return;

    ((origSaveBufferForMirror)g_saveBufferForMirrorHook->m_original)(thisptr, box);
}

static void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles) {
    auto fns = HyprlandAPI::findFunctionsByName(g_pluginHandle, name);
    if (fns.empty())
        throw std::runtime_error(std::format("[fix-hdr-screenshare] No functions found for {}", name));

    if (demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        for (const auto& needle : demangledNeedles) {
            if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                matches.push_back(fn);
                break;
            }
        }
    }

    if (matches.empty())
        throw std::runtime_error(std::format("[fix-hdr-screenshare] No matching overload for {}", name));

    if (matches.size() > 1)
        throw std::runtime_error(std::format("[fix-hdr-screenshare] Ambiguous overload for {}", name));

    return matches[0].address;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;
    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH)
        throw std::runtime_error("Version mismatch: headers version is not equal to running Hyprland version");

    g_disableUnmodifiedCopyMRT = true;
    g_needsUnmodifiedCopyHook  = HyprlandAPI::createFunctionHook(
        g_pluginHandle,
        findFnOrThrow("needsUnmodifiedCopy", {"CMonitor::needsUnmodifiedCopy("}),
        (void*)hkNeedsUnmodifiedCopy);

    if (!g_needsUnmodifiedCopyHook || !g_needsUnmodifiedCopyHook->hook())
        throw std::runtime_error("[fix-hdr-screenshare] Failed to hook CMonitor::needsUnmodifiedCopy");

    g_saveBufferForMirrorHook = HyprlandAPI::createFunctionHook(
        g_pluginHandle,
        findFnOrThrow("saveBufferForMirror", {"CHyprOpenGLImpl::saveBufferForMirror("}),
        (void*)hkSaveBufferForMirror);

    if (!g_saveBufferForMirrorHook || !g_saveBufferForMirrorHook->hook())
        throw std::runtime_error("[fix-hdr-screenshare] Failed to hook CHyprOpenGLImpl::saveBufferForMirror");

    scheduleRefreshForAllMonitors();

    return {"fix-hdr-screenshare", "Disable the HDR unmodified-copy MRT path used for screenshare", "daniel", "0.4"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_disableUnmodifiedCopyMRT = false;

    if (g_needsUnmodifiedCopyHook) {
        g_needsUnmodifiedCopyHook->unhook();
        HyprlandAPI::removeFunctionHook(g_pluginHandle, g_needsUnmodifiedCopyHook);
        g_needsUnmodifiedCopyHook = nullptr;
    }

    if (g_saveBufferForMirrorHook) {
        g_saveBufferForMirrorHook->unhook();
        HyprlandAPI::removeFunctionHook(g_pluginHandle, g_saveBufferForMirrorHook);
        g_saveBufferForMirrorHook = nullptr;
    }

    g_captureShader.reset();

    scheduleRefreshForAllMonitors();
}
