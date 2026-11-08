// HandsBridge.exe — pont SimConnect resident (sans fenetre). La couche OpenXR (dans le jeu) et les outils lui envoient leurs commandes par un
// tube nomme ; lui parle a MSFS. Lance par la tray ; compilé a part car la bibliotheque SimConnect du SDK impose la CRT dynamique.
//
// Tube : \\.\pipe\HandsBridge (ouvert a tous les utilisateurs et aux applications Store, dont MSFS). Protocole : voir core/sim/ZoneAction.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SimBridge.h"
#include "sim/ZoneAction.h"

using hands::win::SimBridge;

namespace {

constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\HandsBridge";
std::atomic<bool> g_quit{false};
std::mutex g_logMutex;
std::wstring g_logPath;

void Log(const std::string& s) {
    std::lock_guard<std::mutex> l(g_logMutex);
    if (g_logPath.empty()) return;
    // Journal tronque quand il depasse 512 Ko.
    WIN32_FILE_ATTRIBUTE_DATA d;
    DWORD disp = OPEN_ALWAYS;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &d) && d.nFileSizeLow > 512 * 1024) disp = CREATE_ALWAYS;
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const std::string line = std::string(b) + s + "\r\n";
    DWORD w = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
    CloseHandle(h);
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        const size_t q = s.find('\t', p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

// Execute une requete ; renvoie la reponse (sans fin de ligne).
std::string Handle(SimBridge& sim, const std::string& line) {
    const std::vector<std::string> f = Split(line);
    const std::string& c = f[0];
    std::string err;
    auto bad = [&](const std::string& m) { return "err\t" + m; };
    auto arg = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
    if (c == "status") {
        const auto s = sim.Status();
        return std::string("ok\tconnected=") + (s.connected ? "1" : "0") + ";sim=" + s.simName + ";aircraft=" + s.aircraft +
               ";mobiflight=" + (s.mobiFlight ? "1" : "0") + ";reason=" + s.reason;
    }
    if (c == "quit") {
        g_quit = true;
        return "ok";
    }
    if (c == "event") {
        if (!sim.SendEvent(arg(1), std::atoi(arg(2).c_str()), &err)) return bad(err);
        return "ok";
    }
    if (c == "setvar" || c == "togglevar") {
        const std::string unit = arg(2).empty() ? "number" : arg(2);
        double v = std::atof(arg(3).c_str());
        if (c == "togglevar") {
            double cur = 0;
            if (!sim.ReadVar(arg(1), unit, &cur, 2000, &err)) return bad(err);
            v = (cur != 0.0) ? 0.0 : v;
        }
        if (!sim.SetVar(arg(1), unit, v, &err)) return bad(err);
        return "ok";
    }
    if (c == "setinput" || c == "toggleinput") {
        double v = std::atof(arg(2).c_str());
        if (c == "toggleinput") {
            double cur = 0;
            if (!sim.ReadInput(arg(1), &cur, 4000, &err)) return bad(err);
            v = (cur != 0.0) ? 0.0 : v;
        }
        if (!sim.SetInput(arg(1), v, &err)) return bad(err);
        return "ok";
    }
    if (c == "code") {
        if (!sim.ExecCode(arg(1), &err)) return bad(err);
        return "ok";
    }
    return bad("commande inconnue");
}

void Serve(SimBridge* sim, HANDLE pipe) {
    char buf[2048];
    DWORD n = 0;
    std::string req;
    if (ReadFile(pipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        req.assign(buf, n);
        while (!req.empty() && (req.back() == '\n' || req.back() == '\r' || req.back() == '\0')) req.pop_back();
        const std::string rep = Handle(*sim, req) + "\n";
        if (req != "status") Log(req + "  ->  " + rep.substr(0, rep.size() - 1));
        DWORD w = 0;
        WriteFile(pipe, rep.data(), static_cast<DWORD>(rep.size()), &w, nullptr);
        FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\HandsBridge");
    if (!once || GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // deja lance

    wchar_t pd[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) g_logPath = std::wstring(pd) + L"\\HandsLayer\\logs\\bridge.log";

    // Tube ouvert aux utilisateurs et aux applications Store (MSFS), meme en integrite basse.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;

    SimBridge sim([](const std::string& s) { Log(s); });
    sim.Start();
    Log("HandsBridge demarre");

    bool first = true;
    while (!g_quit) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 4096,
                                       4096, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (first) break;  // un autre exemplaire tient deja le tube
            Sleep(500);
            continue;
        }
        first = false;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
            std::thread(Serve, &sim, pipe).detach();
        else
            CloseHandle(pipe);
    }
    sim.Stop();
    Log("HandsBridge arrete");
    return 0;
}
