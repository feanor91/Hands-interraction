// ZoneEditor.cpp — voir ZoneEditor.h.
//
// Chaque modification relit zones.ini, change la zone concernee et reecrit le fichier : les positions apprises entre-temps par la couche
// (dans le jeu) ne sont jamais ecrasees. La couche recharge le fichier a chaud.
//
// Une zone = un type de commande (poussoir, bascule, interrupteur a N positions, rotatif, curseur) et, pour chaque emplacement de ce type,
// une sequence d'actions (evenement, variable, Input Event, code avionique, pause). Voir core/sim/ZoneControl.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ZoneEditor.h"
#include "sim/ZoneFile.h"

namespace {

namespace sim = hands::sim;
using sim::ActionSeq;
using sim::ZoneAction;
using sim::ZoneControl;
using sim::ZoneRecord;

constexpr wchar_t kClass[] = L"HandsZoneEditor";
constexpr wchar_t kDataDir[] = L"C:\\ProgramData\\HandsLayer\\data";
constexpr wchar_t kLogsDir[] = L"C:\\ProgramData\\HandsLayer\\logs";

struct LibItem {
    std::wstring label;
    ZoneControl ctl;
};

const struct {
    const wchar_t* label;
    const char* kind;
    const char* press;
    const char* inc;
    const char* dec;
} kLibrary[] = {
#include "sim/CommandLibrary.inc"
};

enum Ctl {
    IDC_LIST = 100, IDC_NEW, IDC_DELETE, IDC_CLEARPOS, IDC_NAME, IDC_FILTER, IDC_CMD, IDC_KIND, IDC_COUNT, IDC_SWMODE, IDC_STEPDEG, IDC_INVERT,
    IDC_SLMODE, IDC_NOTCHES, IDC_MIN, IDC_MAX, IDC_SLOT, IDC_STEPS, IDC_TYPE, IDC_MODE, IDC_VALUE, IDC_UNIT, IDC_TARGET, IDC_AMIN, IDC_AMAX, IDC_WRAP,
    IDC_ADDSTEP, IDC_SETSTEP, IDC_DELSTEP, IDC_ADDPAUSE, IDC_APPLY, IDC_TEST, IDC_REFRESH, IDC_CLOSE, IDC_STATUS
};

HWND g_wnd = nullptr;
HWND g_list, g_name, g_filter, g_cmd, g_kind, g_count, g_swmode, g_stepdeg, g_invert, g_slmode, g_notches, g_min, g_max, g_slot, g_steps;
HWND g_type, g_mode, g_value, g_unit, g_target, g_amin, g_amax, g_wrap, g_status;
std::vector<ZoneRecord> g_view;  // zones affichees (derniere lecture du fichier)
ZoneControl g_ctl;               // commande en cours d'edition
std::vector<LibItem> g_items;    // commandes connues (bibliotheque + Input Events lus dans le simulateur)
std::vector<LibItem> g_shown;    // sous-ensemble filtre, dans l'ordre du combo
std::vector<std::string> g_liveInputs;
std::vector<std::string> g_slots;  // emplacements du type courant

const char* const kKinds[] = {"push", "toggle", "switch", "dial", "slider"};
const wchar_t* const kKindNames[] = {L"Poussoir (un contact)", L"Bascule (2 etats)", L"Interrupteur a N positions", L"Rotatif (pincer et tourner)",
                                     L"Curseur (pincer et glisser)"};

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(static_cast<size_t>(n));
    return w;
}
std::string Text(HWND h) { return Utf8(GetText(h)); }
void SetText(HWND h, const std::string& s) { SetWindowTextW(h, Wide(s).c_str()); }
void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }
double Number(HWND h, double def) {
    const std::string t = Text(h);
    return t.empty() ? def : std::atof(t.c_str());
}

// zones.ini : data s'il existe (installation recente), sinon logs (meme regle que la couche).
std::wstring ZonesPath() {
    const DWORD a = GetFileAttributesW(kDataDir);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring(kDataDir) + L"\\zones.ini";
    return std::wstring(kLogsDir) + L"\\zones.ini";
}

std::vector<ZoneRecord> LoadFile() {
    std::ifstream f(ZonesPath());
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return sim::ParseZones(ss.str());
}
bool SaveFile(const std::vector<ZoneRecord>& z) {
    std::ofstream f(ZonesPath(), std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << sim::SerializeZones(z);
    return static_cast<bool>(f);
}

// Requete au pont (HandsBridge.exe) ; vide si injoignable.
std::string PipeCall(const std::string& req, DWORD timeoutMs, bool* reached = nullptr) {
    std::vector<char> buf(256 * 1024);
    DWORD got = 0;
    const BOOL ok = CallNamedPipeW(L"\\\\.\\pipe\\HandsBridge", const_cast<char*>(req.data()), static_cast<DWORD>(req.size()), buf.data(),
                                   static_cast<DWORD>(buf.size()), &got, timeoutMs);
    if (reached) *reached = ok != FALSE;
    if (!ok) return {};
    std::string r(buf.data(), got);
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    return r;
}

int KindIndex(const std::string& k) {
    for (int i = 0; i < 5; ++i)
        if (k == kKinds[i]) return i;
    return 0;
}

std::wstring SlotLabel(const std::string& s) {
    if (s == "press") return L"Contact / appui";
    if (s == "inc") return L"Cran + (vers la droite)";
    if (s == "dec") return L"Cran - (vers la gauche)";
    if (s == "set") return L"Valeur (position du curseur)";
    if (s.rfind("pos", 0) == 0) return L"Position " + Wide(s.substr(3));
    return Wide(s);
}

std::wstring StepText(const ZoneAction& a) {
    if (a.type == "delay") return L"Pause " + Wide(sim::Num(a.value)) + L" ms";
    std::wstring s = Wide(a.type + " " + a.target);
    if (a.type == "var" || a.type == "input") s += L"  [" + Wide(a.mode) + L" " + Wide(sim::Num(a.value)) + L"]";
    return s;
}

std::wstring Describe(const ZoneRecord& z) {
    return Wide(z.id + "  -  " + sim::Summary(z.control)) + (sim::Complete(z) ? L"   [position apprise]" : L"   [a apprendre]");
}

void FillList(int select) {
    g_view = LoadFile();
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const ZoneRecord& z : g_view) SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Describe(z).c_str()));
    if (select >= 0 && select < static_cast<int>(g_view.size())) SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(select), 0);
}

void SelectCombo(HWND c, const char* value) {
    const LRESULT i = SendMessageW(c, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(Wide(value).c_str()));
    SendMessageW(c, CB_SETCURSEL, i == CB_ERR ? 0 : static_cast<WPARAM>(i), 0);
}

int CurSel() { return static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0)); }
std::string CurSlot() {
    const int i = static_cast<int>(SendMessageW(g_slot, CB_GETCURSEL, 0, 0));
    return i >= 0 && i < static_cast<int>(g_slots.size()) ? g_slots[static_cast<size_t>(i)] : std::string();
}

// --- Formulaire <-> commande ---------------------------------------------------------------------------------------------------------

void EnableParams() {
    const std::string& k = g_ctl.kind;
    EnableWindow(g_count, k == "switch");
    EnableWindow(g_swmode, k == "switch");
    EnableWindow(g_stepdeg, k == "dial");
    EnableWindow(g_invert, k == "dial");
    EnableWindow(g_slmode, k == "slider");
    EnableWindow(g_notches, k == "slider");
    EnableWindow(g_min, k == "slider");
    EnableWindow(g_max, k == "slider");
}

void ReadParams() {
    g_ctl.count = std::max(2, std::min(12, static_cast<int>(Number(g_count, 3))));
    g_ctl.switchMode = Text(g_swmode) == "cycle" ? "cycle" : "bounce";
    g_ctl.stepDeg = std::max(5.0, std::min(90.0, Number(g_stepdeg, 25)));
    g_ctl.invert = SendMessageW(g_invert, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_ctl.sliderMode = Text(g_slmode) == "value" ? "value" : "steps";
    g_ctl.notches = std::max(1, std::min(100, static_cast<int>(Number(g_notches, 10))));
    g_ctl.min = Number(g_min, 0);
    g_ctl.max = Number(g_max, 16383);
}

void ShowSteps() {
    SendMessageW(g_steps, LB_RESETCONTENT, 0, 0);
    for (const ZoneAction& a : sim::Slot(g_ctl, CurSlot())) SendMessageW(g_steps, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(StepText(a).c_str()));
}

void FillSlots(const std::string& keep) {
    g_slots = sim::SlotNames(g_ctl);
    SendMessageW(g_slot, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t i = 0; i < g_slots.size(); ++i) {
        SendMessageW(g_slot, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(SlotLabel(g_slots[i]).c_str()));
        if (g_slots[i] == keep) sel = static_cast<int>(i);
    }
    SendMessageW(g_slot, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    ShowSteps();
}

void ShowControl(const ZoneControl& c) {
    g_ctl = c;
    SendMessageW(g_kind, CB_SETCURSEL, static_cast<WPARAM>(KindIndex(g_ctl.kind)), 0);
    SetText(g_count, std::to_string(g_ctl.count));
    SelectCombo(g_swmode, g_ctl.switchMode.c_str());
    SetText(g_stepdeg, sim::Num(g_ctl.stepDeg));
    SendMessageW(g_invert, BM_SETCHECK, g_ctl.invert ? BST_CHECKED : BST_UNCHECKED, 0);
    SelectCombo(g_slmode, g_ctl.sliderMode.c_str());
    SetText(g_notches, std::to_string(g_ctl.notches));
    SetText(g_min, sim::Num(g_ctl.min));
    SetText(g_max, sim::Num(g_ctl.max));
    EnableParams();
    FillSlots("");
}

void ShowStepFields(const ZoneAction* a) {
    ZoneAction d;
    if (a) d = *a;
    else d.type = "event", d.mode = "press";
    SelectCombo(g_type, d.type.empty() ? "event" : d.type.c_str());
    SetText(g_target, d.target);
    SelectCombo(g_mode, d.mode.c_str());
    SetText(g_value, sim::Num(d.value));
    SetText(g_unit, d.unit);
    SetText(g_amin, d.hasMin ? sim::Num(d.min) : "");
    SetText(g_amax, d.hasMax ? sim::Num(d.max) : "");
    SendMessageW(g_wrap, BM_SETCHECK, d.wrap ? BST_CHECKED : BST_UNCHECKED, 0);
}

ZoneAction FormStep() {
    ZoneAction a;
    a.type = Text(g_type);
    a.target = Text(g_target);
    a.mode = Text(g_mode);
    a.value = Number(g_value, 1);
    a.unit = Text(g_unit);
    if (a.unit.empty()) a.unit = "number";
    const std::string mn = Text(g_amin), mx = Text(g_amax);
    if (!mn.empty()) { a.hasMin = true; a.min = std::atof(mn.c_str()); }
    if (!mx.empty()) { a.hasMax = true; a.max = std::atof(mx.c_str()); }
    a.wrap = SendMessageW(g_wrap, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (a.type == "event" || a.type == "code") a.mode = "press";
    return a;
}

// --- Commandes connues --------------------------------------------------------------------------------------------------------------

void RebuildItems() {
    g_items.clear();
    for (const auto& l : kLibrary) {
        LibItem it;
        it.label = l.label;
        it.ctl.kind = l.kind;
        if (l.press[0]) it.ctl.slots["press"] = sim::ParseSeq(l.press);
        if (l.inc[0]) it.ctl.slots["inc"] = sim::ParseSeq(l.inc);
        if (l.dec[0]) it.ctl.slots["dec"] = sim::ParseSeq(l.dec);
        g_items.push_back(std::move(it));
    }
    for (const std::string& n : g_liveInputs) {
        LibItem it;
        it.label = L"Input Event : " + Wide(n);
        it.ctl.kind = "toggle";
        ZoneAction a;
        a.type = "input";
        a.target = n;
        a.mode = "toggle";
        it.ctl.slots["press"] = {a};
        g_items.push_back(std::move(it));
    }
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

void FillCombo() {
    const std::wstring f = Lower(GetText(g_filter));
    g_shown.clear();
    SendMessageW(g_cmd, CB_RESETCONTENT, 0, 0);
    for (const LibItem& it : g_items) {
        if (!f.empty() && Lower(it.label).find(f) == std::wstring::npos) continue;
        g_shown.push_back(it);
        SendMessageW(g_cmd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it.label.c_str()));
    }
    wchar_t t[64];
    std::swprintf(t, 64, L"%zu commande(s) proposee(s)", g_shown.size());
    SetStatus(t);
}

// --- Actions ------------------------------------------------------------------------------------------------------------------------

void Apply() {
    ReadParams();
    const std::string id = sim::SanitizeZoneId(Text(g_name));
    if (id.empty()) {
        SetStatus(L"Donnez un nom a la zone (lettres, chiffres, _).");
        return;
    }
    const std::string err = sim::Validate(g_ctl);
    if (!err.empty()) {
        SetStatus(L"Commande incomplete : " + Wide(err));
        return;
    }
    std::vector<ZoneRecord> disk = LoadFile();
    const int sel = CurSel();
    const std::string oldId = (sel >= 0 && sel < static_cast<int>(g_view.size())) ? g_view[static_cast<size_t>(sel)].id : std::string();
    ZoneRecord* rec = nullptr;
    for (auto& z : disk)
        if (z.id == oldId) rec = &z;
    for (auto& z : disk)
        if (z.id == id && &z != rec) {
            SetStatus(L"Une autre zone porte deja ce nom.");
            return;
        }
    if (!rec) {
        disk.push_back(ZoneRecord{});
        rec = &disk.back();
    }
    rec->id = id;
    rec->control = g_ctl;
    if (!SaveFile(disk)) {
        SetStatus(L"Ecriture de zones.ini impossible (droits ? reinstallez la couche).");
        return;
    }
    int idx = 0;
    for (size_t i = 0; i < disk.size(); ++i)
        if (disk[i].id == id) idx = static_cast<int>(i);
    FillList(idx);
    SetStatus(sim::Complete(disk[static_cast<size_t>(idx)]) ? L"Enregistre." : L"Enregistre. Appuyez sur le bouton de memorisation en vol (ou choisissez la zone dans le menu VR) pour apprendre sa position.");
}

// Envoie la sequence de l'emplacement choisi (pauses comprises) ; les rotatifs et curseurs se testent emplacement par emplacement.
void Test() {
    ReadParams();
    const ActionSeq seq = sim::Slot(g_ctl, CurSlot());
    if (seq.empty()) {
        SetStatus(L"Cet emplacement ne contient aucune action.");
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (const ZoneAction& a : seq) {
        if (a.type == "delay") {
            Sleep(static_cast<DWORD>(std::max(0.0, std::min(5000.0, a.value))));
            continue;
        }
        std::string err;
        const std::string req = sim::BuildBridgeRequest(a, &err);
        if (req.empty()) {
            SetStatus(L"Action invalide : " + Wide(err));
            break;
        }
        bool reached = false;
        const std::string r = PipeCall(req, 8000, &reached);
        if (!reached) {
            SetStatus(L"Pont injoignable : HandsBridge.exe ne tourne pas (il est lance par la tray).");
            break;
        }
        if (r.rfind("ok", 0) != 0) {
            SetStatus(L"Refusee : " + Wide(r));
            break;
        }
        SetStatus(L"Commande envoyee au simulateur.");
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

void RefreshInputs() {
    bool reached = false;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::string r = PipeCall("inputs", 12000, &reached);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!reached) {
        SetStatus(L"Pont injoignable : HandsBridge.exe ne tourne pas.");
        return;
    }
    if (r.rfind("ok", 0) != 0) {
        SetStatus(L"Lecture impossible : " + Wide(r));
        return;
    }
    g_liveInputs.clear();
    size_t p = r.find('\t');
    while (p != std::string::npos) {
        const size_t q = r.find('\t', p + 1);
        const std::string n = r.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
        if (!n.empty()) g_liveInputs.push_back(n);
        p = q;
    }
    std::sort(g_liveInputs.begin(), g_liveInputs.end());
    RebuildItems();
    FillCombo();
    wchar_t t[96];
    std::swprintf(t, 96, L"%zu Input Events lus dans l'avion charge.", g_liveInputs.size());
    SetStatus(t);
}

// --- Fenetre ----------------------------------------------------------------------------------------------------------------------------

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}
HWND Label(const wchar_t* text, int x, int y, int w) { return Make(L"STATIC", text, 0, x, y + 3, w, 18, 0); }
HWND Edit(int x, int y, int w, int id) { return Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x, y, w, 22, id); }
HWND Combo(int x, int y, int w, int id) { return Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 220, id); }
HWND Button(const wchar_t* text, int x, int y, int w, int id, DWORD style = BS_PUSHBUTTON) { return Make(L"BUTTON", text, style, x, y, w, 26, id); }

void Build() {
    const int rx = 285;
    Label(L"Zones (dans l'ordre d'apprentissage)", 10, 5, 265);
    g_list = Make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 10, 28, 265, 440, IDC_LIST);
    Button(L"Nouvelle", 10, 476, 80, IDC_NEW);
    Button(L"Supprimer", 95, 476, 80, IDC_DELETE);
    Button(L"Oublier position", 180, 476, 95, IDC_CLEARPOS);

    Label(L"Nom", rx, 8, 100);
    g_name = Edit(rx + 115, 8, 220, IDC_NAME);
    Label(L"Chercher une commande", rx, 38, 130);
    g_filter = Edit(rx + 135, 38, 200, IDC_FILTER);
    Label(L"Commande connue", rx, 68, 110);
    g_cmd = Combo(rx + 115, 68, 390, IDC_CMD);
    Label(L"Type de commande", rx, 100, 110);
    g_kind = Combo(rx + 115, 100, 250, IDC_KIND);

    Make(L"BUTTON", L"Reglages du type", BS_GROUPBOX, rx, 128, 510, 100, 0);
    Label(L"Positions", rx + 10, 148, 70);
    g_count = Edit(rx + 85, 148, 40, IDC_COUNT);
    Label(L"Parcours", rx + 140, 148, 60);
    g_swmode = Combo(rx + 205, 148, 100, IDC_SWMODE);
    Label(L"Degres/cran", rx + 10, 176, 75);
    g_stepdeg = Edit(rx + 85, 176, 40, IDC_STEPDEG);
    g_invert = Make(L"BUTTON", L"Inverser le sens", BS_AUTOCHECKBOX, rx + 140, 176, 130, 22, IDC_INVERT);
    Label(L"Curseur", rx + 10, 202, 70);
    g_slmode = Combo(rx + 85, 202, 80, IDC_SLMODE);
    Label(L"Crans", rx + 175, 202, 40);
    g_notches = Edit(rx + 215, 202, 40, IDC_NOTCHES);
    Label(L"Min", rx + 265, 202, 30);
    g_min = Edit(rx + 295, 202, 65, IDC_MIN);
    Label(L"Max", rx + 370, 202, 30);
    g_max = Edit(rx + 400, 202, 65, IDC_MAX);

    Label(L"Emplacement", rx, 238, 100);
    g_slot = Combo(rx + 115, 238, 220, IDC_SLOT);
    g_steps = Make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, rx, 266, 510, 84, IDC_STEPS);

    Label(L"Type", rx, 360, 40);
    g_type = Combo(rx + 45, 360, 90, IDC_TYPE);
    Label(L"Mode", rx + 145, 360, 40);
    g_mode = Combo(rx + 185, 360, 80, IDC_MODE);
    Label(L"Valeur", rx + 275, 360, 45);
    g_value = Edit(rx + 320, 360, 65, IDC_VALUE);
    Label(L"Unite", rx + 395, 360, 40);
    g_unit = Edit(rx + 435, 360, 75, IDC_UNIT);
    Label(L"Cible", rx, 390, 40);
    g_target = Edit(rx + 45, 390, 465, IDC_TARGET);
    Label(L"Borne min", rx, 420, 60);
    g_amin = Edit(rx + 65, 420, 60, IDC_AMIN);
    Label(L"max", rx + 135, 420, 30);
    g_amax = Edit(rx + 165, 420, 60, IDC_AMAX);
    g_wrap = Make(L"BUTTON", L"Boucler (ex. cap 0-360)", BS_AUTOCHECKBOX, rx + 240, 420, 200, 22, IDC_WRAP);
    Button(L"Ajouter l'etape", rx, 448, 110, IDC_ADDSTEP);
    Button(L"Remplacer", rx + 115, 448, 85, IDC_SETSTEP);
    Button(L"Supprimer", rx + 205, 448, 80, IDC_DELSTEP);
    Button(L"+ Pause", rx + 290, 448, 70, IDC_ADDPAUSE);

    Button(L"Appliquer", rx, 482, 90, IDC_APPLY, BS_DEFPUSHBUTTON);
    Button(L"Tester", rx + 95, 482, 80, IDC_TEST);
    Button(L"Lire les Input Events", rx + 180, 482, 150, IDC_REFRESH);
    Button(L"Fermer", rx + 425, 482, 85, IDC_CLOSE);
    g_status = Make(L"STATIC", L"", SS_LEFT, 10, 518, 780, 22, IDC_STATUS);

    for (int i = 0; i < 5; ++i) SendMessageW(g_kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kKindNames[i]));
    for (const wchar_t* t : {L"bounce", L"cycle"}) SendMessageW(g_swmode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const wchar_t* t : {L"steps", L"value"}) SendMessageW(g_slmode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const wchar_t* t : {L"event", L"var", L"input", L"code", L"delay"}) SendMessageW(g_type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const wchar_t* t : {L"press", L"set", L"toggle", L"add"}) SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        g_wnd, [](HWND c, LPARAM f) -> BOOL { SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(f), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
}

void SelectZone(int s) {
    if (s >= 0 && s < static_cast<int>(g_view.size())) {
        SetText(g_name, g_view[static_cast<size_t>(s)].id);
        ShowControl(g_view[static_cast<size_t>(s)].control);
    } else {
        SetText(g_name, "");
        ShowControl(ZoneControl{});
    }
    ShowStepFields(nullptr);
}

void OnStepSelected() {
    const int i = static_cast<int>(SendMessageW(g_steps, LB_GETCURSEL, 0, 0));
    const ActionSeq& q = sim::Slot(g_ctl, CurSlot());
    if (i >= 0 && i < static_cast<int>(q.size())) ShowStepFields(&q[static_cast<size_t>(i)]);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case IDC_LIST:
                    if (code == LBN_SELCHANGE) SelectZone(CurSel());
                    break;
                case IDC_NEW:
                    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                    SelectZone(-1);
                    SetFocus(g_name);
                    SetStatus(L"Nouvelle zone : nom, type, commande (ou etapes), puis Appliquer.");
                    break;
                case IDC_DELETE: {
                    const int s = CurSel();
                    if (s < 0 || s >= static_cast<int>(g_view.size())) break;
                    if (MessageBoxW(hwnd, L"Supprimer cette zone ?", L"Zones", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                    std::vector<ZoneRecord> disk = LoadFile();
                    const std::string gone = g_view[static_cast<size_t>(s)].id;
                    disk.erase(std::remove_if(disk.begin(), disk.end(), [&](const ZoneRecord& z) { return z.id == gone; }), disk.end());
                    SaveFile(disk);
                    FillList(-1);
                    SelectZone(-1);
                    SetStatus(L"Zone supprimee.");
                    break;
                }
                case IDC_CLEARPOS: {
                    const int s = CurSel();
                    if (s < 0 || s >= static_cast<int>(g_view.size())) break;
                    std::vector<ZoneRecord> disk = LoadFile();
                    for (auto& z : disk)
                        if (z.id == g_view[static_cast<size_t>(s)].id) z.hasPos = z.hasPos2 = false;
                    SaveFile(disk);
                    FillList(s);
                    SetStatus(L"Position oubliee : la zone sera reapprise au prochain appui sur le bouton de memorisation.");
                    break;
                }
                case IDC_FILTER:
                    if (code == EN_CHANGE) FillCombo();
                    break;
                case IDC_CMD:
                    if (code == CBN_SELCHANGE) {
                        const int i = static_cast<int>(SendMessageW(g_cmd, CB_GETCURSEL, 0, 0));
                        if (i >= 0 && i < static_cast<int>(g_shown.size())) {
                            const LibItem& it = g_shown[static_cast<size_t>(i)];
                            ZoneControl keep = g_ctl;  // les reglages (positions, crans...) du type precedent ne servent plus
                            (void)keep;
                            ShowControl(it.ctl);
                            ShowStepFields(nullptr);
                            if (GetText(g_name).empty()) {
                                std::string nm;
                                for (const std::string& s : sim::SlotNames(it.ctl)) {
                                    const ActionSeq& q = sim::Slot(it.ctl, s);
                                    if (!q.empty()) { nm = sim::SanitizeZoneId(q[0].target); break; }
                                }
                                if (nm.size() > 40) nm.resize(40);
                                SetText(g_name, nm);
                            }
                            SetStatus(L"Commande choisie : verifiez les etapes de chaque emplacement, puis Appliquer.");
                        }
                    }
                    break;
                case IDC_KIND:
                    if (code == CBN_SELCHANGE) {
                        ReadParams();
                        const int i = static_cast<int>(SendMessageW(g_kind, CB_GETCURSEL, 0, 0));
                        g_ctl.kind = kKinds[std::max(0, std::min(4, i))];
                        EnableParams();
                        FillSlots("");
                    }
                    break;
                case IDC_COUNT:
                case IDC_SLMODE:
                    if ((id == IDC_COUNT && code == EN_KILLFOCUS) || (id == IDC_SLMODE && code == CBN_SELCHANGE)) {
                        ReadParams();
                        FillSlots(CurSlot());
                    }
                    break;
                case IDC_SLOT:
                    if (code == CBN_SELCHANGE) {
                        ShowSteps();
                        ShowStepFields(nullptr);
                    }
                    break;
                case IDC_STEPS:
                    if (code == LBN_SELCHANGE) OnStepSelected();
                    break;
                case IDC_ADDSTEP:
                case IDC_ADDPAUSE:
                case IDC_SETSTEP:
                case IDC_DELSTEP: {
                    const std::string slot = CurSlot();
                    if (slot.empty()) break;
                    ActionSeq& q = g_ctl.slots[slot];
                    const int i = static_cast<int>(SendMessageW(g_steps, LB_GETCURSEL, 0, 0));
                    if (id == IDC_ADDSTEP || id == IDC_ADDPAUSE) {
                        ZoneAction a;
                        if (id == IDC_ADDPAUSE) {
                            a.type = "delay";
                            a.value = 300;
                        } else {
                            a = FormStep();
                            std::string err;
                            if (a.type != "delay" && sim::BuildBridgeRequest(a, &err).empty()) {
                                SetStatus(L"Etape invalide : " + Wide(err));
                                break;
                            }
                        }
                        q.push_back(a);
                    } else if (i >= 0 && i < static_cast<int>(q.size())) {
                        if (id == IDC_DELSTEP) {
                            q.erase(q.begin() + i);
                        } else {
                            const ZoneAction a = FormStep();
                            std::string err;
                            if (a.type != "delay" && sim::BuildBridgeRequest(a, &err).empty()) {
                                SetStatus(L"Etape invalide : " + Wide(err));
                                break;
                            }
                            q[static_cast<size_t>(i)] = a;
                        }
                    }
                    ShowSteps();
                    break;
                }
                case IDC_APPLY: Apply(); break;
                case IDC_TEST: Test(); break;
                case IDC_REFRESH: RefreshInputs(); break;
                case IDC_CLOSE: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: g_wnd = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ZoneEditor_Show(HWND, HINSTANCE inst) {
    if (g_wnd) {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    RECT r{0, 0, 810, 548};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(0, kClass, L"Zones du pochoir", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                            nullptr);
    if (!g_wnd) return;
    Build();
    RebuildItems();
    FillCombo();
    FillList(0);
    SelectZone(g_view.empty() ? -1 : 0);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}
