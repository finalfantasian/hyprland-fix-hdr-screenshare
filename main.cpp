#define WLR_USE_UNSTABLE

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

static HANDLE         g_pluginHandle             = nullptr;
static CFunctionHook* g_needsUnmodifiedCopyHook  = nullptr;
static bool           g_disableUnmodifiedCopyMRT = true;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

typedef bool (*origNeedsUnmodifiedCopy)(void*);

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
    if (g_disableUnmodifiedCopyMRT)
        return false;

    return ((origNeedsUnmodifiedCopy)g_needsUnmodifiedCopyHook->m_original)(thisptr);
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

    scheduleRefreshForAllMonitors();

    return {"fix-hdr-screenshare", "Disable the HDR unmodified-copy MRT path used for screenshare", "daniel", "0.2"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_disableUnmodifiedCopyMRT = false;

    if (g_needsUnmodifiedCopyHook) {
        g_needsUnmodifiedCopyHook->unhook();
        HyprlandAPI::removeFunctionHook(g_pluginHandle, g_needsUnmodifiedCopyHook);
        g_needsUnmodifiedCopyHook = nullptr;
    }

    scheduleRefreshForAllMonitors();
}
