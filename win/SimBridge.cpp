// SimBridge.cpp — voir SimBridge.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <SimConnect.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "SimBridge.h"
#include "sim/SimNames.h"

namespace hands::win {

namespace {

constexpr int kRetryMs = 5000;
constexpr DWORD kMfSize = 1024;
constexpr int kMfGapMs = 35;  // le module MobiFlight ne lit son canal de commandes qu'une fois par image
constexpr SIMCONNECT_CLIENT_DATA_ID kMfCmdArea = 910001;
constexpr SIMCONNECT_CLIENT_DATA_DEFINITION_ID kMfCmdDef = 910002;
constexpr SIMCONNECT_CLIENT_DATA_ID kMfRespArea = 910003;
constexpr SIMCONNECT_CLIENT_DATA_DEFINITION_ID kMfRespDef = 910004;
constexpr SIMCONNECT_DATA_REQUEST_ID kMfRespReq = 910005;
constexpr SIMCONNECT_CLIENT_EVENT_ID kAircraftEvent = 900001;
constexpr SIMCONNECT_DATA_REQUEST_ID kAircraftStateReq = 900002;

// Résultat attendu d'une lecture : le fil appelant attend, le fil du pont renseigne.
struct Query {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    double value = 0;
    std::string error;
    std::vector<InputEventInfo> list;
    void Finish(bool success, const std::string& e = "") {
        std::lock_guard<std::mutex> l(m);
        done = true;
        ok = success;
        error = e;
        cv.notify_all();
    }
    bool Wait(int timeoutMs) {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return done; });
    }
};

}  // namespace

struct SimBridge::Impl {
    LogFn logFn;
    std::thread th;
    std::atomic<bool> stop{false};
    HANDLE evSim = nullptr;  // SimConnect : des messages attendent
    HANDLE evCmd = nullptr;  // une tâche attend dans la file

    mutable std::mutex mu;  // état, file de tâches, cache des Input Events
    SimStatus status;
    std::deque<std::function<void()>> tasks;
    std::vector<InputEventInfo> inputCache;
    bool inputCached = false;
    std::vector<std::string> lvarNames;  // liste des L: recue du module MobiFlight (sous `mu`)
    bool lvarListing = false, lvarDone = false;

    // Réservé au fil du pont (aucun verrou) :
    HANDLE h = nullptr;
    std::map<std::string, int> eventIds;
    std::map<std::string, int> writeDefs;
    std::map<std::string, int> readDefs;  // definitions de lecture par « nom|unite »
    std::map<int, std::shared_ptr<Query>> pending;      // lectures de variables / d'Input Events en cours
    std::map<int, std::shared_ptr<Query>> listPending;  // énumérations d'Input Events en cours
    std::map<int, std::vector<InputEventInfo>> listItems;
    int nextId = 1000;
    int failures = 0;
    bool mfReady = false;
    ULONGLONG mfProbeAt = 0, mfLastSend = 0;
    std::deque<std::string> mfQueue;

    void Log(const std::string& s) {
        if (logFn) logFn(s);
    }

    void SetStatus(const std::function<void(SimStatus&)>& f) {
        std::lock_guard<std::mutex> l(mu);
        f(status);
    }

    // --- Tâches : toutes les opérations SimConnect s'exécutent sur le fil du pont ---------------------------------------------------
    void Post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> l(mu);
            tasks.push_back(std::move(f));
        }
        SetEvent(evCmd);
    }

    bool Connected() const {
        std::lock_guard<std::mutex> l(mu);
        return status.connected;
    }

    // --- Connexion --------------------------------------------------------------------------------------------------------------------
    void Drop(const std::string& reason) {
        if (h) {
            Log("[MSFS] Deconnecte : " + reason);
            SimConnect_Close(h);
        }
        h = nullptr;
        eventIds.clear();
        writeDefs.clear();
        readDefs.clear();
        mfReady = false;
        mfQueue.clear();
        failures = 0;
        for (auto& p : pending) p.second->Finish(false, "deconnecte");
        pending.clear();
        for (auto& p : listPending) p.second->Finish(false, "deconnecte");
        listPending.clear();
        listItems.clear();
        {
            std::lock_guard<std::mutex> l(mu);
            inputCached = false;
            inputCache.clear();
            status.connected = false;
            status.simName.clear();
            status.aircraft.clear();
            status.mobiFlight = false;
            status.reason = reason;
        }
    }

    bool TryConnect() {
        HANDLE nh = nullptr;
        if (FAILED(SimConnect_Open(&nh, "HandsBridge", nullptr, 0, evSim, 0)) || !nh) {
            SetStatus([](SimStatus& s) { s.reason = "Simulateur non detecte (lancez MSFS)."; });
            return false;
        }
        h = nh;
        failures = 0;
        SimConnect_SubscribeToSystemEvent(h, kAircraftEvent, "AircraftLoaded");
        SimConnect_RequestSystemState(h, kAircraftStateReq, "AircraftLoaded");
        SetStatus([](SimStatus& s) {
            s.connected = true;
            s.reason.clear();
        });
        return true;
    }

    // --- Réception ----------------------------------------------------------------------------------------------------------------------
    void Handle(SIMCONNECT_RECV* r) {
        switch (r->dwID) {
            case SIMCONNECT_RECV_ID_OPEN: {
                const auto* o = static_cast<SIMCONNECT_RECV_OPEN*>(r);
                const std::string name = o->szApplicationName;
                SetStatus([&](SimStatus& s) { s.simName = name; });
                Log("[MSFS] Connecte a " + name + ".");
                break;
            }
            case SIMCONNECT_RECV_ID_QUIT:
                Drop("Simulateur ferme.");
                break;
            case SIMCONNECT_RECV_ID_EXCEPTION: {
                const auto* e = static_cast<SIMCONNECT_RECV_EXCEPTION*>(r);
                Log("[MSFS] Exception SimConnect " + std::to_string(e->dwException) + " (paquet " + std::to_string(e->dwSendID) + ", parametre " +
                    std::to_string(e->dwIndex) + ").");
                break;
            }
            case SIMCONNECT_RECV_ID_EVENT_FILENAME: {
                const auto* e = static_cast<SIMCONNECT_RECV_EVENT_FILENAME*>(r);
                if (e->uEventID != kAircraftEvent) break;
                const std::string a = hands::sim::AircraftFromPath(e->szFileName);
                {
                    std::lock_guard<std::mutex> l(mu);
                    status.aircraft = a;
                    inputCached = false;  // la liste des commandes de cockpit change avec l'avion
                    inputCache.clear();
                }
                Log("[MSFS] Avion charge : " + (a.empty() ? std::string("inconnu") : a) + ".");
                break;
            }
            case SIMCONNECT_RECV_ID_SYSTEM_STATE: {
                const auto* s = static_cast<SIMCONNECT_RECV_SYSTEM_STATE*>(r);
                if (s->dwRequestID != kAircraftStateReq) break;
                const std::string a = hands::sim::AircraftFromPath(s->szString);
                SetStatus([&](SimStatus& st) { st.aircraft = a; });
                break;
            }
            case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
                const auto* d = static_cast<SIMCONNECT_RECV_SIMOBJECT_DATA*>(r);
                const auto it = pending.find(static_cast<int>(d->dwRequestID));
                if (it == pending.end()) break;
                double v = 0;
                std::memcpy(&v, &d->dwData, sizeof(v));
                it->second->value = v;
                it->second->Finish(true);
                pending.erase(it);
                break;
            }
            case SIMCONNECT_RECV_ID_ENUMERATE_INPUT_EVENTS: {
                const auto* e = static_cast<SIMCONNECT_RECV_ENUMERATE_INPUT_EVENTS*>(r);
                const int id = static_cast<int>(e->dwRequestID);
                auto& items = listItems[id];
                const auto* d = reinterpret_cast<const SIMCONNECT_INPUT_EVENT_DESCRIPTOR*>(&e->rgData);
                for (DWORD i = 0; i < e->dwArraySize; ++i) {
                    InputEventInfo info;
                    info.name = d[i].Name;
                    info.hash = d[i].Hash;
                    info.isString = d[i].eType == SIMCONNECT_INPUT_EVENT_TYPE_STRING;
                    items.push_back(info);
                }
                if (e->dwEntryNumber + 1 >= e->dwOutOf) {
                    std::sort(items.begin(), items.end(), [](const InputEventInfo& a, const InputEventInfo& b) { return a.name < b.name; });
                    const auto q = listPending.find(id);
                    if (items.empty()) {
                        if (q != listPending.end()) q->second->Finish(false, "L'avion charge n'a (encore) aucune commande de cockpit.");
                    } else {
                        {
                            std::lock_guard<std::mutex> l(mu);
                            inputCache = items;
                            inputCached = true;
                        }
                        if (q != listPending.end()) {
                            q->second->list = items;
                            q->second->Finish(true);
                        }
                    }
                    if (q != listPending.end()) listPending.erase(q);
                    listItems.erase(id);
                }
                break;
            }
            case SIMCONNECT_RECV_ID_GET_INPUT_EVENT: {
                const auto* g = static_cast<SIMCONNECT_RECV_GET_INPUT_EVENT*>(r);
                const auto it = pending.find(static_cast<int>(g->dwRequestID));
                if (it == pending.end()) break;
                if (g->eType == SIMCONNECT_INPUT_EVENT_TYPE_DOUBLE) {
                    double v = 0;
                    std::memcpy(&v, &g->Value, sizeof(v));
                    it->second->value = v;
                    it->second->Finish(true);
                } else {
                    it->second->Finish(false, "cette commande est une chaine, pas un nombre");
                }
                pending.erase(it);
                break;
            }
            case SIMCONNECT_RECV_ID_CLIENT_DATA: {
                const auto* c = static_cast<SIMCONNECT_RECV_CLIENT_DATA*>(r);
                if (c->dwRequestID != kMfRespReq) break;
                const char* text = reinterpret_cast<const char*>(&c->dwData);
                if (std::strncmp(text, "MF.LVars.List.Start", 19) == 0) {
                    std::lock_guard<std::mutex> l(mu);
                    lvarNames.clear();
                    lvarListing = true;
                } else if (std::strncmp(text, "MF.LVars.List.End", 17) == 0) {
                    std::lock_guard<std::mutex> l(mu);
                    lvarListing = false;
                    lvarDone = true;
                } else if (std::strncmp(text, "MF.", 3) != 0) {
                    std::lock_guard<std::mutex> l(mu);
                    if (lvarListing) lvarNames.emplace_back(text);
                } else if (std::strncmp(text, "MF.Pong", 7) == 0) {
                    bool first = false;
                    {
                        std::lock_guard<std::mutex> l(mu);
                        first = !status.mobiFlight;
                        status.mobiFlight = true;
                    }
                    if (first) Log("[MSFS] Module MobiFlight WASM detecte.");
                }
                break;
            }
            default:
                break;
        }
    }

    void Pump() {
        for (int i = 0; i < 500 && h; ++i) {
            SIMCONNECT_RECV* r = nullptr;
            DWORD cb = 0;
            if (FAILED(SimConnect_GetNextDispatch(h, &r, &cb)) || !r) break;
            Handle(r);
        }
    }

    void NoteResult(HRESULT hr, const char* what) {
        if (SUCCEEDED(hr)) {
            failures = 0;
            return;
        }
        Log(std::string("[MSFS] Echec SimConnect : ") + what);
        if (++failures >= 3) Drop("Connexion au simulateur interrompue.");
    }

    // --- Module MobiFlight WASM ---------------------------------------------------------------------------------------------------------
    void SendMf(const std::string& text) {
        char buf[kMfSize] = {};
        std::memcpy(buf, text.data(), std::min<size_t>(text.size(), kMfSize - 1));
        NoteResult(SimConnect_SetClientData(h, kMfCmdArea, kMfCmdDef, 0, 0, kMfSize, buf), "SetClientData");
    }

    void EnsureMf() {
        if (mfReady) return;
        mfReady = true;
        SimConnect_MapClientDataNameToID(h, "MobiFlight.Command", kMfCmdArea);
        SimConnect_AddToClientDataDefinition(h, kMfCmdDef, 0, kMfSize, 0.0f, 0);
        SimConnect_MapClientDataNameToID(h, "MobiFlight.Response", kMfRespArea);
        SimConnect_AddToClientDataDefinition(h, kMfRespDef, 0, kMfSize, 0.0f, 0);
        SimConnect_RequestClientData(h, kMfRespArea, kMfRespReq, kMfRespDef, SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
                                     SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED);
        mfQueue.push_back("MF.Ping");
        mfProbeAt = GetTickCount64() + 3000;
    }

    void StepMf() {
        const ULONGLONG now = GetTickCount64();
        if (h && mfProbeAt && now > mfProbeAt) {
            mfProbeAt = 0;
            bool pong;
            {
                std::lock_guard<std::mutex> l(mu);
                pong = status.mobiFlight;
            }
            if (!pong) Log("[MSFS] Le module MobiFlight WASM ne repond pas : est-il installe dans le dossier Community ?");
        }
        if (h && !mfQueue.empty() && now - mfLastSend >= static_cast<ULONGLONG>(kMfGapMs)) {
            SendMf(mfQueue.front());
            mfQueue.pop_front();
            mfLastSend = now;
        }
    }

    // --- Boucle du fil du pont -----------------------------------------------------------------------------------------------------------
    void RunTasks() {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> l(mu);
                if (tasks.empty()) return;
                f = std::move(tasks.front());
                tasks.pop_front();
            }
            f();
        }
    }

    void Loop() {
        ULONGLONG nextTry = 0;
        while (!stop.load()) {
            if (!h) {
                const ULONGLONG now = GetTickCount64();
                if (now >= nextTry) {
                    if (!TryConnect()) nextTry = now + kRetryMs;
                }
                RunTasks();  // les tâches échouent proprement : h est nul
                const HANDLE w[1] = {evCmd};
                WaitForMultipleObjects(1, w, FALSE, 200);
                continue;
            }
            const HANDLE w[2] = {evSim, evCmd};
            WaitForMultipleObjects(2, w, FALSE, 20);
            Pump();
            RunTasks();
            StepMf();
        }
        if (h) {
            SimConnect_Close(h);
            h = nullptr;
        }
    }

    // --- Opérations (exécutées sur le fil du pont) ------------------------------------------------------------------------------------
    bool Fail(std::string* err, const std::string& msg) {
        if (err) *err = msg;
        return false;
    }
    bool CheckConnected(std::string* err) {
        if (Connected()) return true;
        std::string reason;
        {
            std::lock_guard<std::mutex> l(mu);
            reason = status.reason;
        }
        return Fail(err, "MSFS n'est pas connecte : " + (reason.empty() ? std::string("lancez le simulateur.") : reason));
    }
};

SimBridge::SimBridge(LogFn log) : p_(new Impl) {
    p_->logFn = std::move(log);
    p_->evSim = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    p_->evCmd = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    p_->status.reason = "Pont non demarre.";
}

SimBridge::~SimBridge() {
    Stop();
    CloseHandle(p_->evSim);
    CloseHandle(p_->evCmd);
    delete p_;
}

void SimBridge::Start() {
    if (p_->th.joinable()) return;
    p_->stop = false;
    p_->SetStatus([](SimStatus& s) { s.reason = "Recherche du simulateur..."; });
    p_->th = std::thread([this] { p_->Loop(); });
}

void SimBridge::Stop() {
    if (!p_->th.joinable()) return;
    p_->stop = true;
    SetEvent(p_->evCmd);
    p_->th.join();
}

SimStatus SimBridge::Status() const {
    std::lock_guard<std::mutex> l(p_->mu);
    return p_->status;
}

bool SimBridge::SendEvent(const std::string& nameIn, int value, std::string* err) {
    const std::string name = sim::Upper(sim::Trim(nameIn));
    if (!sim::IsValidEventName(name)) return p_->Fail(err, "Evenement MSFS invalide : '" + nameIn + "'.");
    if (!p_->CheckConnected(err)) return false;
    Impl* p = p_;
    p->Post([p, name, value] {
        if (!p->h) return;
        int id;
        const auto it = p->eventIds.find(name);
        if (it == p->eventIds.end()) {
            id = p->nextId++;
            p->NoteResult(SimConnect_MapClientEventToSimEvent(p->h, static_cast<SIMCONNECT_CLIENT_EVENT_ID>(id), name.c_str()), "MapClientEventToSimEvent");
            p->eventIds[name] = id;
        } else {
            id = it->second;
        }
        p->NoteResult(SimConnect_TransmitClientEvent(p->h, SIMCONNECT_OBJECT_ID_USER, static_cast<SIMCONNECT_CLIENT_EVENT_ID>(id), static_cast<DWORD>(value),
                                                     SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY),
                      "TransmitClientEvent");
    });
    return true;
}

bool SimBridge::SetVar(const std::string& nameIn, const std::string& unitIn, double value, std::string* err) {
    const std::string name = sim::NormalizeVar(nameIn);
    const std::string unit = unitIn.empty() ? "number" : sim::Trim(unitIn);
    if (!sim::IsValidSimvar(name) || !sim::IsValidUnit(unit)) return p_->Fail(err, "Variable MSFS invalide : '" + nameIn + "'.");
    if (!p_->CheckConnected(err)) return false;
    Impl* p = p_;
    p->Post([p, name, unit, value] {
        if (!p->h) return;
        const std::string key = name + "|" + unit;
        int def;
        const auto it = p->writeDefs.find(key);
        if (it == p->writeDefs.end()) {
            def = p->nextId++;
            p->NoteResult(SimConnect_AddToDataDefinition(p->h, static_cast<SIMCONNECT_DATA_DEFINITION_ID>(def), name.c_str(), unit.c_str(), SIMCONNECT_DATATYPE_FLOAT64),
                          "AddToDataDefinition");
            p->writeDefs[key] = def;
        } else {
            def = it->second;
        }
        double v = value;
        p->NoteResult(SimConnect_SetDataOnSimObject(p->h, static_cast<SIMCONNECT_DATA_DEFINITION_ID>(def), SIMCONNECT_OBJECT_ID_USER, 0, 0, sizeof(v), &v),
                      "SetDataOnSimObject");
    });
    return true;
}

bool SimBridge::ExecCode(const std::string& codeIn, std::string* err) {
    const std::string code = sim::NormalizeCode(codeIn);
    if (code.empty()) return p_->Fail(err, "Aucun code avionique.");
    if (sim::IsCodeTooLong(code)) return p_->Fail(err, "Code avionique trop long.");
    if (!p_->CheckConnected(err)) return false;
    Impl* p = p_;
    p->Post([p, code] {
        if (!p->h) return;
        p->EnsureMf();
        // Chaque commande est suivie d'une commande neutre : le module ne voit pas deux fois de suite le même contenu (crans d'une molette).
        p->mfQueue.push_back("MF.SimVars.Set." + code);
        p->mfQueue.push_back("MF.DummyCmd");
    });
    return true;
}

bool SimBridge::ListLVars(std::vector<std::string>* out, int timeoutMs, std::string* err) {
    if (!p_->CheckConnected(err)) return false;
    Impl* p = p_;
    {
        std::lock_guard<std::mutex> l(p->mu);
        p->lvarNames.clear();
        p->lvarListing = false;
        p->lvarDone = false;
    }
    p->Post([p] {
        if (!p->h) return;
        p->EnsureMf();
        p->mfQueue.push_back("MF.LVars.List");
    });
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> l(p->mu);
            if (p->lvarDone) {
                if (out) *out = p->lvarNames;
                return true;
            }
        }
        if (GetTickCount64() > until) return p->Fail(err, "le module MobiFlight WASM n'a pas repondu (est-il dans le dossier Community ?)");
        Sleep(50);
    }
}

bool SimBridge::ReadVar(const std::string& nameIn, const std::string& unitIn, double* out, int timeoutMs, std::string* err) {
    const std::string name = sim::NormalizeVar(nameIn);
    const std::string unit = unitIn.empty() ? "number" : sim::Trim(unitIn);
    if (!sim::IsValidSimvar(name) || !sim::IsValidUnit(unit)) return p_->Fail(err, "Variable MSFS invalide : '" + nameIn + "'.");
    if (!p_->CheckConnected(err)) return false;
    auto q = std::make_shared<Query>();
    Impl* p = p_;
    p->Post([p, q, name, unit] {
        if (!p->h) return q->Finish(false, "deconnecte");
        // Une definition par variable (reutilisee) : en relevant des centaines de variables en boucle, en creer une a chaque lecture saturait SimConnect.
        const int id = p->nextId++;
        const std::string key = name + "|" + unit;
        auto it = p->readDefs.find(key);
        HRESULT hr1 = S_OK;
        if (it == p->readDefs.end()) {
            const int def = p->nextId++;
            hr1 = SimConnect_AddToDataDefinition(p->h, static_cast<SIMCONNECT_DATA_DEFINITION_ID>(def), name.c_str(), unit.c_str(), SIMCONNECT_DATATYPE_FLOAT64);
            if (SUCCEEDED(hr1)) it = p->readDefs.emplace(key, def).first;
        }
        const HRESULT hr2 = it == p->readDefs.end() ? E_FAIL : SimConnect_RequestDataOnSimObject(p->h, static_cast<SIMCONNECT_DATA_REQUEST_ID>(id), static_cast<SIMCONNECT_DATA_DEFINITION_ID>(it->second),
                                                              SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_ONCE);
        if (FAILED(hr1) || FAILED(hr2)) return q->Finish(false, "requete refusee");
        p->pending[id] = q;
    });
    if (!q->Wait(timeoutMs)) return p_->Fail(err, "Pas de reponse du simulateur (variable inconnue ?).");
    if (!q->ok) return p_->Fail(err, q->error);
    if (out) *out = q->value;
    return true;
}

bool SimBridge::ListInputs(std::vector<InputEventInfo>* out, bool force, int timeoutMs, std::string* err) {
    if (!p_->CheckConnected(err)) return false;
    if (!force) {
        std::lock_guard<std::mutex> l(p_->mu);
        if (p_->inputCached) {
            if (out) *out = p_->inputCache;
            return true;
        }
    }
    auto q = std::make_shared<Query>();
    Impl* p = p_;
    p->Post([p, q] {
        if (!p->h) return q->Finish(false, "deconnecte");
        const int id = p->nextId++;
        if (FAILED(SimConnect_EnumerateInputEvents(p->h, static_cast<SIMCONNECT_DATA_REQUEST_ID>(id)))) return q->Finish(false, "requete refusee");
        p->listPending[id] = q;
    });
    if (!q->Wait(timeoutMs)) return p_->Fail(err, "Le simulateur n'a renvoye aucune commande de cockpit (avion en chargement ?).");
    if (!q->ok) return p_->Fail(err, q->error);
    if (out) *out = q->list;
    return true;
}

bool SimBridge::SetInput(const std::string& nameIn, double value, std::string* err) {
    const std::string name = sim::Trim(nameIn);
    if (!sim::IsValidInputEvent(name)) return p_->Fail(err, "Nom d'Input Event invalide : '" + nameIn + "'.");
    std::vector<InputEventInfo> list;
    if (!ListInputs(&list, false, 6000, err)) return false;
    const InputEventInfo* found = nullptr;
    for (const auto& i : list)
        if (i.name == name) found = &i;
    if (!found)
        for (const auto& i : list)
            if (sim::Upper(i.name) == sim::Upper(name)) found = &i;
    if (!found) return p_->Fail(err, "L'avion charge n'a pas de commande '" + name + "'.");
    if (found->isString) return p_->Fail(err, "'" + name + "' attend une chaine, pas un nombre.");
    const uint64_t hash = found->hash;
    Impl* p = p_;
    p->Post([p, hash, value] {
        if (!p->h) return;
        double v = value;
        p->NoteResult(SimConnect_SetInputEvent(p->h, hash, sizeof(v), &v), "SetInputEvent");
    });
    return true;
}

bool SimBridge::ReadInput(const std::string& nameIn, double* out, int timeoutMs, std::string* err) {
    const std::string name = sim::Trim(nameIn);
    if (!sim::IsValidInputEvent(name)) return p_->Fail(err, "Nom d'Input Event invalide : '" + nameIn + "'.");
    std::vector<InputEventInfo> list;
    if (!ListInputs(&list, false, 6000, err)) return false;
    const InputEventInfo* found = nullptr;
    for (const auto& i : list)
        if (i.name == name || sim::Upper(i.name) == sim::Upper(name)) found = &i;
    if (!found) return p_->Fail(err, "L'avion charge n'a pas de commande '" + name + "'.");
    auto q = std::make_shared<Query>();
    Impl* p = p_;
    const uint64_t hash = found->hash;
    p->Post([p, q, hash] {
        if (!p->h) return q->Finish(false, "deconnecte");
        const int id = p->nextId++;
        if (FAILED(SimConnect_GetInputEvent(p->h, static_cast<SIMCONNECT_DATA_REQUEST_ID>(id), hash))) return q->Finish(false, "requete refusee");
        p->pending[id] = q;
    });
    if (!q->Wait(timeoutMs)) return p_->Fail(err, "Pas de reponse du simulateur.");
    if (!q->ok) return p_->Fail(err, q->error);
    if (out) *out = q->value;
    return true;
}

}  // namespace hands::win
