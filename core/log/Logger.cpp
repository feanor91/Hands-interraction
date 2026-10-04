#include "Logger.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>

#include "../util/StringUtil.h"

namespace hands::log {

LogLevel ParseLogLevel(std::string_view text, LogLevel def) {
    const std::string t = hands::util::ToLower(hands::util::Trim(text));
    if (t == "error") return LogLevel::Error;
    if (t == "warn" || t == "warning") return LogLevel::Warn;
    if (t == "info") return LogLevel::Info;
    if (t == "debug") return LogLevel::Debug;
    return def;
}

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERREUR";
        case LogLevel::Warn:  return "ATTN  ";
        case LogLevel::Info:  return "INFO  ";
        case LogLevel::Debug: return "DEBUG ";
    }
    return "?";
}

Logger::Logger() = default;

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Logger& Logger::Get() {
    static Logger* instance = new Logger();  // jamais détruit : voir Logger.h
    return *instance;
}

bool Logger::Open(const std::filesystem::path& file) {
    std::error_code ec;  // variante sans exception des fonctions <filesystem>
    std::filesystem::create_directories(file.parent_path(), ec);
    if (std::filesystem::exists(file, ec)) {
        std::filesystem::path prev = file;
        prev.replace_extension(".prev.log");
        std::filesystem::remove(prev, ec);
        std::filesystem::rename(file, prev, ec);
    }
    // Test d'ouverture immédiat pour pouvoir répondre honnêtement.
    {
        std::ofstream probe(file, std::ios::out | std::ios::trunc);
        if (!probe.is_open()) return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = file;
        fileOpen_ = true;
        if (!workerStarted_) {
            workerStarted_ = true;
            worker_ = std::thread([this] { WorkerLoop(); });
            // Pour le journal global (voir Get()), ce thread n'est jamais joint ;
            // pour une instance de test, le destructeur le joint.
        }
    }
    wake_.notify_all();
    return true;
}

std::filesystem::path Logger::Path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

void Logger::Log(LogLevel level, const char* fmt, ...) {
    if (!IsEnabled(level)) return;
    va_list args;
    va_start(args, fmt);
    LogV(level, fmt, args);
    va_end(args);
}

void Logger::LogV(LogLevel level, const char* fmt, va_list args) {
    if (!IsEnabled(level)) return;

    char body[2048];
    const int n = std::vsnprintf(body, sizeof(body), fmt, args);
    if (n < 0) return;  // erreur de format : on ignore plutôt que de planter

    // Horodatage local HH:MM:SS.mmm
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char head[64];
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d [%s] [t%04u] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  static_cast<int>(ms), LogLevelName(level),
                  static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 10000));

    std::string line(head);
    line += body;
    if (n >= static_cast<int>(sizeof(body))) line += " [...tronque]";
    line += '\n';
    Enqueue(std::move(line));
}

void Logger::Enqueue(std::string line) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueued) {
            queue_.pop_front();  // on perd le plus ancien, on ne bloque jamais
            ++dropped_;
        }
        queue_.push_back(std::move(line));
    }
    wake_.notify_one();
}

bool Logger::Flush(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!fileOpen_) return false;
    return drained_.wait_for(lock, timeout, [this] { return queue_.empty() && !writing_; });
}

void Logger::WorkerLoop() {
    std::ofstream out;
    std::filesystem::path openedPath;
    for (;;) {
        std::deque<std::string> batch;
        size_t dropped = 0;
        std::filesystem::path path;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || (fileOpen_ && !queue_.empty()); });
            if (queue_.empty() && stop_) return;
            batch.swap(queue_);
            dropped = dropped_;
            dropped_ = 0;
            path = path_;
            writing_ = true;
        }
        if (!out.is_open() || openedPath != path) {
            if (out.is_open()) out.close();
            out.open(path, std::ios::out | std::ios::app);
            openedPath = path;
        }
        if (out.is_open()) {
            if (dropped) out << "... " << dropped << " message(s) perdu(s) (file pleine)\n";
            for (const std::string& l : batch) out << l;
            out.flush();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
        }
        drained_.notify_all();
    }
}

}  // namespace hands::log
