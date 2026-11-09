// bridge_client.cpp — voir bridge_client.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "bridge_client.h"
#include "log/Logger.h"

namespace layer {

using hands::log::LogLevel;
using hands::log::Logger;

namespace {

constexpr size_t kMaxPending = 16;  // taches en attente au maximum (pont bloque : on ne s'empile pas)

struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool started = false;
};

// Jamais detruit : le fil de travail peut encore tourner quand la DLL se decharge (pas de destructeur statique a risque).
Worker& W() {
    static Worker* w = new Worker;
    return *w;
}

void Loop() {
    Worker& w = W();
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> l(w.m);
            w.cv.wait(l, [&] { return !w.q.empty(); });
            task = std::move(w.q.front());
            w.q.pop_front();
        }
        try {
            task();
        } catch (...) {
        }
    }
}

}  // namespace

std::string Bridge_Call(const std::string& request, unsigned timeoutMs) {
    std::string buf(256 * 1024, '\0');
    DWORD got = 0;
    if (!CallNamedPipeW(L"\\\\.\\pipe\\HandsBridge", const_cast<char*>(request.data()), static_cast<DWORD>(request.size()), buf.data(),
                        static_cast<DWORD>(buf.size()), &got, timeoutMs))
        return {};
    buf.resize(got);
    while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) buf.pop_back();
    return buf;
}

bool Bridge_ReadNumber(const std::string& request, double* out) {
    const std::string r = Bridge_Call(request, 6000);
    if (r.rfind("ok", 0) != 0) return false;
    const size_t t = r.find('\t');
    if (t == std::string::npos) return false;
    *out = std::atof(r.c_str() + t + 1);
    return true;
}

bool Bridge_Post(std::function<void()> task) {
    Worker& w = W();
    {
        std::lock_guard<std::mutex> l(w.m);
        if (w.q.size() >= kMaxPending) return false;
        w.q.push_back(std::move(task));
        if (!w.started) {
            w.started = true;
            std::thread(Loop).detach();
        }
    }
    w.cv.notify_one();
    return true;
}

void Bridge_RunSeqNow(const hands::sim::ActionSeq& seq, const std::string& label) {
    for (const hands::sim::ZoneAction& a : seq) {
        if (a.type == "delay") {
            Sleep(static_cast<DWORD>(a.value < 0 ? 0 : (a.value > 5000 ? 5000 : a.value)));
            continue;
        }
        std::string err;
        const std::string req = hands::sim::BuildBridgeRequest(a, &err);
        if (req.empty()) {
            Logger::Get().Log(LogLevel::Warn, "pont : '%s' : action inutilisable (%s)", label.c_str(), err.c_str());
            return;
        }
        const std::string rep = Bridge_Call(req, 8000);
        if (rep.empty()) {
            Logger::Get().Log(LogLevel::Warn, "pont : '%s' -> pont injoignable ; HandsBridge.exe est-il lance (tray) ?", label.c_str());
            return;
        }
        if (rep.rfind("ok", 0) != 0) {
            Logger::Get().Log(LogLevel::Warn, "pont : '%s' -> %s", label.c_str(), rep.c_str());
            return;
        }
    }
    Logger::Get().Log(LogLevel::Info, "pont : '%s' -> ok", label.c_str());
}

bool Bridge_RunSeq(const hands::sim::ActionSeq& seq, const std::string& label) {
    if (seq.empty()) return false;
    if (Bridge_Post([seq, label] { Bridge_RunSeqNow(seq, label); })) return true;
    Logger::Get().Log(LogLevel::Warn, "pont : '%s' ignore (file d'attente pleine)", label.c_str());
    return false;
}

}  // namespace layer
