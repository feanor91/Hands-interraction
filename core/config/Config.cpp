#include "Config.h"

#include <fstream>
#include <sstream>
#include <thread>

#include "../util/StringUtil.h"

namespace hands::config {

Config Config::FromIni(const IniFile& ini) {
    Config c;  // les valeurs par défaut sont celles des initialisateurs de la struct
    c.processAllowlist = ini.GetList("general", "process_allowlist", c.processAllowlist);
    c.discoveryMode = ini.GetBool("general", "discovery_mode", c.discoveryMode);
    c.logLevel = hands::log::ParseLogLevel(ini.GetString("general", "log_level", "info"), c.logLevel);
    c.enableHandTrackingExtension =
        ini.GetBool("probe", "enable_hand_tracking_extension", c.enableHandTrackingExtension);
    c.probeHandTracker = ini.GetBool("probe", "probe_hand_tracker", c.probeHandTracker);
    c.logBindings = ini.GetBool("probe", "log_bindings", c.logBindings);
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
