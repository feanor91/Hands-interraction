// bridge_client.cpp — voir bridge_client.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "bridge_client.h"
#include "log/Logger.h"

namespace layer {

using hands::log::LogLevel;
using hands::log::Logger;

void Bridge_Send(const std::string& request, const std::string& label) {
    static std::atomic<int> inFlight{0};
    if (inFlight.fetch_add(1) >= 3) {  // pont bloque : on ne fait pas s'empiler les fils
        --inFlight;
        Logger::Get().Log(LogLevel::Warn, "pont : '%s' ignore (trop de requetes en attente)", label.c_str());
        return;
    }
    std::thread([request, label] {
        char reply[1024] = {};
        DWORD got = 0;
        // Si le tube n'existe pas encore, on attend brievement (la tray relance le pont) ; reponse attendue en moins de 6 s (lecture d'etat).
        const BOOL ok = CallNamedPipeW(L"\\\\.\\pipe\\HandsBridge", const_cast<char*>(request.data()), static_cast<DWORD>(request.size()), reply,
                                       sizeof(reply) - 1, &got, 6000);
        std::string rep(reply, ok ? got : 0);
        while (!rep.empty() && (rep.back() == '\n' || rep.back() == '\r')) rep.pop_back();
        if (!ok)
            Logger::Get().Log(LogLevel::Warn, "pont : '%s' -> pont injoignable (erreur %lu) ; HandsBridge.exe est-il lance (tray) ?", label.c_str(),
                              static_cast<unsigned long>(GetLastError()));
        else if (rep.rfind("ok", 0) == 0)
            Logger::Get().Log(LogLevel::Info, "pont : '%s' -> ok", label.c_str());
        else
            Logger::Get().Log(LogLevel::Warn, "pont : '%s' -> %s", label.c_str(), rep.c_str());
        --inFlight;
    }).detach();
}

}  // namespace layer
