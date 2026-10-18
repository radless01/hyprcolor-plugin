
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IValue.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <wayland-server-core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

constexpr float RED_WEIGHT   = 0.2126F;
constexpr float GREEN_WEIGHT = 0.7152F;
constexpr float BLUE_WEIGHT  = 0.0722F;

constexpr float SATURATION_MIN = 0.0F;
constexpr float SATURATION_MAX = 3.0F;
constexpr float BRIGHTNESS_MIN = 0.0F;
constexpr float BRIGHTNESS_MAX = 2.0F;

constexpr int UPDATE_INTERVAL_MS = 1000;

static SP<Config::Values::CFloatValue> CFG_SATURATION;
static SP<Config::Values::CFloatValue> CFG_BRIGHTNESS;

static wl_event_source* g_timer = nullptr;
static HANDLE g_handle = nullptr;
static bool g_stopping = false;

static double config_float(
    const char* name,
    double fallback)
{
    if (std::string{name} == "plugin:hyprcolor:value") {
        return CFG_SATURATION
            ? static_cast<double>(CFG_SATURATION->value())
            : fallback;
    }

    if (std::string{name} == "plugin:hyprcolor:brightness") {
        return CFG_BRIGHTNESS
            ? static_cast<double>(CFG_BRIGHTNESS->value())
            : fallback;
    }

    return fallback;
}

/*
 * Build the saturation matrix and incorporate brightness
 * into the same matrix.
 *
 * This matrix is handed to Hyprland itself through
 * CMonitor::setCTM(). No direct DRM commits are performed.
 */
static Mat3x3 make_saturation_matrix(
    double saturation,
    double brightness)
{
    saturation = std::clamp(
        std::isfinite(saturation) ? saturation : 1.0,
        static_cast<double>(SATURATION_MIN),
        static_cast<double>(SATURATION_MAX));

    brightness = std::clamp(
        std::isfinite(brightness) ? brightness : 1.0,
        static_cast<double>(BRIGHTNESS_MIN),
        static_cast<double>(BRIGHTNESS_MAX));

    const double inverse = 1.0 - saturation;

    const auto f = [brightness](double value) -> float {
        return static_cast<float>(brightness * value);
    };

    return Mat3x3{std::array<float, 9>{
        f(inverse * RED_WEIGHT + saturation),
        f(inverse * GREEN_WEIGHT),
        f(inverse * BLUE_WEIGHT),

        f(inverse * RED_WEIGHT),
        f(inverse * GREEN_WEIGHT + saturation),
        f(inverse * BLUE_WEIGHT),

        f(inverse * RED_WEIGHT),
        f(inverse * GREEN_WEIGHT),
        f(inverse * BLUE_WEIGHT + saturation)
    }};
}

static void apply_all()
{
    if (!g_pCompositor)
        return;

    const double saturation = std::clamp(
        config_float("plugin:hyprcolor:value", 1.0),
        static_cast<double>(SATURATION_MIN),
        static_cast<double>(SATURATION_MAX));

    const double brightness_input = std::clamp(
        config_float("plugin:hyprcolor:brightness", 1.0),
        static_cast<double>(BRIGHTNESS_MIN),
        static_cast<double>(BRIGHTNESS_MAX));

    // Preserve the brightness scaling used by the original plugin.
    const double brightness =
        1.0 + ((brightness_input - 1.0) * 0.10);

    const Mat3x3 matrix =
        make_saturation_matrix(saturation, brightness);

    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || !monitor->m_enabled || !monitor->m_output)
            continue;

        /*
         * Update Hyprland's own monitor state. Its renderer/output
         * commit path now owns applying this CTM, rather than having
         * this plugin race Hyprland with independent DRM commits.
         */
        monitor->setCTM(matrix);
    }
}

static int timer_callback(void*)
{
    if (g_stopping)
        return 0;

    apply_all();

    if (g_timer)
        wl_event_source_timer_update(
            g_timer,
            UPDATE_INTERVAL_MS);

    return 0;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    g_handle = handle;
    g_stopping = false;

    if (std::string{__hyprland_api_get_hash()} !=
        std::string{__hyprland_api_get_client_hash()}) {
        HyprlandAPI::addNotification(
            g_handle,
            "[hw-saturation] Hyprland headers do not match the running compositor.",
            CHyprColor{1.0, 0.2, 0.2, 1.0},
            5000);

        throw std::runtime_error(
            "Hyprland header/API version mismatch");
    }

    CFG_SATURATION = makeShared<Config::Values::CFloatValue>(
        "plugin:hyprcolor:value",
        "Hardware saturation",
        1.0F,
        Config::Values::SFloatValueOptions{
            .min = 0.0F,
            .max = 3.0F
        });

    CFG_BRIGHTNESS = makeShared<Config::Values::CFloatValue>(
        "plugin:hyprcolor:brightness",
        "Hardware brightness",
        1.0F,
        Config::Values::SFloatValueOptions{
            .min = 0.0F,
            .max = 2.0F
        });

    if (!HyprlandAPI::addConfigValueV2(g_handle, CFG_SATURATION))
        throw std::runtime_error(
            "Failed to register saturation config");

    if (!HyprlandAPI::addConfigValueV2(g_handle, CFG_BRIGHTNESS))
        throw std::runtime_error(
            "Failed to register brightness config");

    if (!g_pCompositor || !g_pCompositor->m_wlEventLoop)
        throw std::runtime_error(
            "Hyprland Wayland event loop is unavailable");

    g_timer = wl_event_loop_add_timer(
        g_pCompositor->m_wlEventLoop,
        timer_callback,
        nullptr);

    if (!g_timer)
        throw std::runtime_error(
            "Failed to create hw-saturation timer");

    apply_all();

    wl_event_source_timer_update(
        g_timer,
        UPDATE_INTERVAL_MS);

    return {
        "hyprcolor",
        "Hardware saturation and brightness controls",
        "radless",
        "1.0"
    };
}

APICALL EXPORT void PLUGIN_EXIT()
{
    g_stopping = true;

    if (g_timer) {
        wl_event_source_remove(g_timer);
        g_timer = nullptr;
    }

    /*
     * Do not restore stale DRM values here. The plugin no longer
     * owns an independent DRM state; Hyprland owns the monitor CTM.
     */
}
