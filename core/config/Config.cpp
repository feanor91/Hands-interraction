#include "Config.h"

#include <fstream>
#include <sstream>
#include <thread>

#include "../util/StringUtil.h"

namespace hands::config {

Config Config::FromIni(const IniFile& ini) {
    Config c;  // les valeurs par défaut sont celles des initialisateurs de la struct
    c.processAllowlist = ini.GetList("general", "process_allowlist", c.processAllowlist);
    c.enabled = ini.GetBool("general", "enabled", c.enabled);
    c.discoveryMode = ini.GetBool("general", "discovery_mode", c.discoveryMode);
    c.logLevel = hands::log::ParseLogLevel(ini.GetString("general", "log_level", "info"), c.logLevel);
    c.enableHandTrackingExtension =
        ini.GetBool("probe", "enable_hand_tracking_extension", c.enableHandTrackingExtension);
    c.probeHandTracker = ini.GetBool("probe", "probe_hand_tracker", c.probeHandTracker);
    c.logBindings = ini.GetBool("probe", "log_bindings", c.logBindings);
    c.emulationEnabled = ini.GetBool("emulation", "enabled", c.emulationEnabled);
    c.pointerMode = hands::util::ToLower(std::string(ini.GetString("emulation", "pointer_mode", c.pointerMode)));
    if (c.pointerMode != "shoulder" && c.pointerMode != "contact" && c.pointerMode != "palm" && c.pointerMode != "hand" && c.pointerMode != "finger")
        c.pointerMode = "shoulder";
    c.shoulderSideMm = ini.GetDouble("emulation", "shoulder_side_mm", c.shoulderSideMm);
    c.shoulderDropMm = ini.GetDouble("emulation", "shoulder_drop_mm", c.shoulderDropMm);
    c.shoulderBackMm = ini.GetDouble("emulation", "shoulder_back_mm", c.shoulderBackMm);
    if (c.shoulderSideMm < 50.0) c.shoulderSideMm = 50.0;
    if (c.shoulderSideMm > 400.0) c.shoulderSideMm = 400.0;
    if (c.shoulderDropMm < 0.0) c.shoulderDropMm = 0.0;
    if (c.shoulderDropMm > 500.0) c.shoulderDropMm = 500.0;
    if (c.shoulderBackMm < -200.0) c.shoulderBackMm = -200.0;
    if (c.shoulderBackMm > 300.0) c.shoulderBackMm = 300.0;
    c.shoulderSmoothHz = ini.GetDouble("emulation", "shoulder_smooth_hz", c.shoulderSmoothHz);
    c.shoulderSmoothBeta = ini.GetDouble("emulation", "shoulder_smooth_beta", c.shoulderSmoothBeta);
    if (c.shoulderSmoothHz < 0.0) c.shoulderSmoothHz = 0.0;
    if (c.shoulderSmoothHz > 20.0) c.shoulderSmoothHz = 20.0;
    if (c.shoulderSmoothBeta < 0.0) c.shoulderSmoothBeta = 0.0;
    if (c.shoulderSmoothBeta > 50.0) c.shoulderSmoothBeta = 50.0;
    c.fingerBlend = ini.GetBool("emulation", "finger_blend", c.fingerBlend);
    c.fingerBlendStartDeg = ini.GetDouble("emulation", "finger_blend_start_deg", c.fingerBlendStartDeg);
    c.fingerBlendEndDeg = ini.GetDouble("emulation", "finger_blend_end_deg", c.fingerBlendEndDeg);
    if (c.fingerBlendStartDeg < 0.0) c.fingerBlendStartDeg = 0.0;
    if (c.fingerBlendStartDeg > 120.0) c.fingerBlendStartDeg = 120.0;
    if (c.fingerBlendEndDeg < c.fingerBlendStartDeg + 5.0) c.fingerBlendEndDeg = c.fingerBlendStartDeg + 5.0;
    c.torsoFollowS = ini.GetDouble("emulation", "torso_follow_s", c.torsoFollowS);
    if (c.torsoFollowS < 0.0) c.torsoFollowS = 0.0;
    if (c.torsoFollowS > 30.0) c.torsoFollowS = 30.0;
    c.anchorSource = hands::util::ToLower(std::string(ini.GetString("emulation", "anchor_source", c.anchorSource)));
    if (c.anchorSource != "fingertip" && c.anchorSource != "bone") c.anchorSource = "fingertip";
    c.anchorForwardMm = ini.GetDouble("emulation", "anchor_forward_mm", c.anchorForwardMm);
    if (c.anchorForwardMm < 0.0) c.anchorForwardMm = 0.0;
    if (c.anchorForwardMm > 150.0) c.anchorForwardMm = 150.0;
    c.palmAim = hands::util::ToLower(std::string(ini.GetString("emulation", "palm_aim", c.palmAim)));
    if (c.palmAim != "proximal" && c.palmAim != "intermediate" && c.palmAim != "distal" && c.palmAim != "tip") c.palmAim = "intermediate";
    c.palmFollowHz = ini.GetDouble("emulation", "palm_follow_hz", c.palmFollowHz);
    if (c.palmFollowHz < 0.05) c.palmFollowHz = 0.05;
    if (c.palmFollowHz > 5.0) c.palmFollowHz = 5.0;
    c.pointerFinger = hands::util::ToLower(std::string(ini.GetString("emulation", "pointer_finger", c.pointerFinger)));
    if (c.pointerFinger == "majeur") c.pointerFinger = "middle";
    if (c.pointerFinger != "index" && c.pointerFinger != "middle") c.pointerFinger = "index";
    c.pointerPitchDeg = ini.GetDouble("emulation", "pointer_pitch_deg", c.pointerPitchDeg);
    c.pointerYawDeg = ini.GetDouble("emulation", "pointer_yaw_deg", c.pointerYawDeg);
    if (c.pointerPitchDeg < -45.0) c.pointerPitchDeg = -45.0;
    if (c.pointerPitchDeg > 45.0) c.pointerPitchDeg = 45.0;
    if (c.pointerYawDeg < -45.0) c.pointerYawDeg = -45.0;
    if (c.pointerYawDeg > 45.0) c.pointerYawDeg = 45.0;
    c.sharedPointer = ini.GetBool("emulation", "shared_pointer", c.sharedPointer);
    c.engageMm = ini.GetDouble("emulation", "engage_mm", c.engageMm);
    if (c.engageMm < 22.0) c.engageMm = 22.0;
    if (c.engageMm > 80.0) c.engageMm = 80.0;
    c.clickMode = hands::util::ToLower(std::string(ini.GetString("emulation", "click_mode", c.clickMode)));
    if (c.clickMode != "side" && c.clickMode != "pinch" && c.clickMode != "middle" && c.clickMode != "any") c.clickMode = "any";
    c.hideControllerModel = ini.GetBool("emulation", "hide_controller_model", c.hideControllerModel);
    // Anciens noms (pinch_*) acceptés : les réglages existants continuent de fonctionner.
    c.clickOnMm = ini.GetDouble("emulation", "click_on_mm", ini.GetDouble("emulation", "pinch_on_mm", c.clickOnMm));
    c.clickOffMm = ini.GetDouble("emulation", "click_off_mm", ini.GetDouble("emulation", "pinch_off_mm", c.clickOffMm));
    c.latchMs = ini.GetDouble("emulation", "latch_ms", c.latchMs);
    c.prelatchMm = ini.GetDouble("emulation", "prelatch_mm", c.prelatchMm);
    c.freezeDeadzoneDeg = ini.GetDouble("emulation", "freeze_deadzone_deg", c.freezeDeadzoneDeg);
    c.freezeDeadzoneMm = ini.GetDouble("emulation", "freeze_deadzone_mm", c.freezeDeadzoneMm);
    if (c.prelatchMm < 0.0) c.prelatchMm = 0.0;
    if (c.prelatchMm > 30.0) c.prelatchMm = 30.0;
    if (c.freezeDeadzoneDeg < 0.0) c.freezeDeadzoneDeg = 0.0;
    if (c.freezeDeadzoneDeg > 10.0) c.freezeDeadzoneDeg = 10.0;
    if (c.freezeDeadzoneMm < 0.0) c.freezeDeadzoneMm = 0.0;
    if (c.freezeDeadzoneMm > 40.0) c.freezeDeadzoneMm = 40.0;
    c.depthProbe = ini.GetBool("touch", "depth_probe", c.depthProbe);
    c.depthProbeIntervalMs = ini.GetInt("touch", "probe_interval_ms", c.depthProbeIntervalMs);
    if (c.depthProbeIntervalMs < 100) c.depthProbeIntervalMs = 100;
    if (c.depthProbeIntervalMs > 10000) c.depthProbeIntervalMs = 10000;
    c.depthProbeState = hands::util::ToLower(std::string(ini.GetString("touch", "depth_state", c.depthProbeState)));
    if (c.depthProbeState != "depth_write" && c.depthProbeState != "depth_read" && c.depthProbeState != "common" &&
        c.depthProbeState != "shader" && c.depthProbeState != "copy_source")
        c.depthProbeState = "depth_write";
    c.menuEnabled = ini.GetBool("menu", "enabled", c.menuEnabled);
    c.menuHoldMs = ini.GetInt("menu", "open_hold_ms", c.menuHoldMs);
    if (c.menuHoldMs < 400) c.menuHoldMs = 400;
    if (c.menuHoldMs > 5000) c.menuHoldMs = 5000;
    c.captureMax = ini.GetInt("touch", "capture_max", c.captureMax);
    c.captureIntervalMs = ini.GetInt("touch", "capture_interval_ms", c.captureIntervalMs);
    if (c.captureMax < 0) c.captureMax = 0;
    if (c.captureMax > 100) c.captureMax = 100;
    if (c.captureIntervalMs < 500) c.captureIntervalMs = 500;
    c.colorState = hands::util::ToLower(std::string(ini.GetString("touch", "color_state", c.colorState)));
    if (c.colorState != "render_target" && c.colorState != "common" && c.colorState != "shader" && c.colorState != "copy_source")
        c.colorState = "render_target";
    c.handsEnabled = ini.GetBool("hands", "enabled", c.handsEnabled);
    c.handsRenderScale = ini.GetDouble("hands", "render_scale", c.handsRenderScale);
    c.handsJointScale = ini.GetDouble("hands", "joint_scale", c.handsJointScale);
    c.handsBoneMm = ini.GetDouble("hands", "bone_mm", c.handsBoneMm);
    if (c.handsRenderScale < 0.25) c.handsRenderScale = 0.25;
    if (c.handsRenderScale > 1.0) c.handsRenderScale = 1.0;
    if (c.handsJointScale < 0.2) c.handsJointScale = 0.2;
    if (c.handsJointScale > 5.0) c.handsJointScale = 5.0;
    if (c.handsBoneMm < 1.0) c.handsBoneMm = 1.0;
    if (c.handsBoneMm > 30.0) c.handsBoneMm = 30.0;
    c.clickRequireTracked = ini.GetBool("emulation", "click_require_tracked", c.clickRequireTracked);
    c.requireTracked = ini.GetBool("emulation", "require_tracked", c.requireTracked);
    c.smoothing = ini.GetBool("emulation", "smoothing", c.smoothing);
    c.smoothRotHz = ini.GetDouble("emulation", "smooth_rot_hz", c.smoothRotHz);
    c.smoothRotBeta = ini.GetDouble("emulation", "smooth_rot_beta", c.smoothRotBeta);
    if (c.latchMs < 0.0) c.latchMs = 0.0;
    if (c.latchMs > 400.0) c.latchMs = 400.0;  // l'historique conservé couvre 500 ms
    if (c.smoothRotHz < 0.05) c.smoothRotHz = 0.05;
    if (c.smoothRotBeta < 0.0) c.smoothRotBeta = 0.0;
    if (c.clickOnMm < 1.0) c.clickOnMm = 1.0;
    if (c.clickOffMm <= c.clickOnMm) c.clickOffMm = c.clickOnMm + 2.0;  // l'hystérésis doit exister
    return c;
}

bool MatchesProcess(const std::vector<std::string>& allowlist, const std::string& exePath) {
    const std::string name = hands::util::FileNameOf(exePath);
    for (const std::string& allowed : allowlist) {
        if (hands::util::EqualsIgnoreCase(name, hands::util::FileNameOf(allowed))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// État partagé : survit à ConfigStore tant que le thread de surveillance tourne.
// NOTE C++ : `std::shared_ptr` = pointeur à comptage de références (comme une
// référence C# gérée par le GC, mais déterministe : l'objet est détruit quand
// la dernière copie disparaît).
struct ConfigStore::Shared {
    std::shared_ptr<const Config> current = std::make_shared<const Config>();
    std::atomic<bool> stop{false};
    std::atomic<bool> watching{false};
};

namespace {

bool ReadFile(const std::filesystem::path& file, std::string* out) {
    std::ifstream in(file, std::ios::in | std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace

ConfigStore::ConfigStore(std::filesystem::path file) : file_(std::move(file)), shared_(std::make_shared<Shared>()) {}

ConfigStore::~ConfigStore() { StopWatching(); }

std::shared_ptr<const Config> ConfigStore::Get() const {
    // Lecture atomique d'un shared_ptr (fonctions libres C++17).
    return std::atomic_load(&shared_->current);
}

bool ConfigStore::Reload(std::string* message) {
    std::string text;
    if (!ReadFile(file_, &text)) {
        if (message) *message = "fichier de configuration illisible ou absent : " + file_.string() + " (valeurs conservees)";
        return false;
    }
    std::vector<std::string> warnings;
    const IniFile ini = IniFile::Parse(text, &warnings);
    auto fresh = std::make_shared<const Config>(Config::FromIni(ini));
    std::atomic_store(&shared_->current, std::move(fresh));
    if (message) {
        *message = "configuration rechargee depuis " + file_.string();
        for (const std::string& w : warnings) *message += "\n  avertissement : " + w;
    }
    return true;
}

void ConfigStore::StartWatching(std::chrono::milliseconds interval,
                                std::function<void(const Config&, const std::string&)> onChange) {
    if (shared_->watching.exchange(true)) return;
    shared_->stop = false;
    std::shared_ptr<Shared> shared = shared_;  // copie : le thread garde l'état en vie
    const std::filesystem::path file = file_;

    // Le thread travaille sur sa propre copie de ConfigStore minimale (même état).
    std::thread([shared, file, interval, onChange = std::move(onChange)]() mutable {
        ConfigStore local(file);
        local.shared_ = shared;
        std::error_code ec;
        auto stamp = [&] {
            auto t = std::filesystem::last_write_time(file, ec);
            auto s = std::filesystem::file_size(file, ec);
            return std::make_pair(ec ? std::filesystem::file_time_type::min() : t, ec ? uintmax_t(0) : s);
        };
        auto last = stamp();
        while (!shared->stop.load()) {
            std::this_thread::sleep_for(interval);
            if (shared->stop.load()) break;
            auto now = stamp();
            if (now == last) continue;
            last = now;
            std::string msg;
            local.Reload(&msg);  // en cas d'échec, l'ancienne config est conservée
            if (onChange) onChange(*local.Get(), msg);
        }
        shared->watching = false;
        // `local` est détruit ici : son destructeur appelle StopWatching(), sans
        // effet puisque `stop` est déjà vrai.
    }).detach();
}

void ConfigStore::StopWatching() { shared_->stop = true; }

}  // namespace hands::config
