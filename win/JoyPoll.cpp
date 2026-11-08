// JoyPoll.cpp — voir JoyPoll.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cctype>

#include "JoyPoll.h"

namespace hands::win {

struct JoyPoll::Impl {
    struct Dev {
        IDirectInputDevice8W* dev = nullptr;
        std::string name;
        bool down[128] = {};
        bool primed = false;  // premier etat lu : sert de reference, sans evenement
    };
    IDirectInput8W* di = nullptr;
    HWND window = nullptr;
    std::vector<Dev> devs;
    ULONGLONG lastEnum = 0;

    static std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static BOOL CALLBACK EnumCb(const DIDEVICEINSTANCEW* inst, void* user) {
        Impl* self = static_cast<Impl*>(user);
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(self->di->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) || FAILED(dev->SetCooperativeLevel(self->window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            dev->Release();
            return DIENUM_CONTINUE;
        }
        char name[260] = {};
        WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, name, sizeof name - 1, nullptr, nullptr);
        Dev d;
        d.dev = dev;
        d.name = name;
        self->devs.push_back(d);
        return DIENUM_CONTINUE;
    }

    void CloseDevices() {
        for (Dev& d : devs) {
            d.dev->Unacquire();
            d.dev->Release();
        }
        devs.clear();
    }

    bool Ensure() {
        const ULONGLONG now = GetTickCount64();
        if (!di) {
            if (lastEnum && now - lastEnum < 10000) return false;
            lastEnum = now;
            if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr)))
                return false;
            window = CreateWindowExW(0, L"STATIC", L"HandsJoy", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
            lastEnum = 0;
        }
        if (devs.empty()) {
            if (lastEnum && now - lastEnum < 10000) return false;
            lastEnum = now;
            di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, this, DIEDFL_ATTACHEDONLY);
        }
        return !devs.empty();
    }
};

JoyPoll::JoyPoll() : impl_(new Impl) {}
JoyPoll::~JoyPoll() {
    Close();
    delete impl_;
}

void JoyPoll::Close() {
    impl_->CloseDevices();
    if (impl_->di) {
        impl_->di->Release();
        impl_->di = nullptr;
    }
    if (impl_->window) {
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    impl_->lastEnum = 0;
}

std::vector<JoyEvent> JoyPoll::PollAllPressed(const std::string& deviceFilter) {
    std::vector<JoyEvent> out;
    if (!impl_->Ensure()) return out;
    const std::string wanted = Impl::Lower(deviceFilter);
    bool lost = false;
    for (Impl::Dev& d : impl_->devs) {
        DIJOYSTATE2 st = {};
        d.dev->Acquire();
        d.dev->Poll();
        if (FAILED(d.dev->GetDeviceState(sizeof st, &st))) {
            lost = true;
            std::fill(std::begin(d.down), std::end(d.down), false);
            continue;
        }
        const bool match = wanted.empty() || Impl::Lower(d.name).find(wanted) != std::string::npos;
        const bool report = d.primed;
        d.primed = true;
        for (int i = 0; i < 128; ++i) {
            const bool now = (st.rgbButtons[i] & 0x80) != 0;
            if (now && !d.down[i] && match && report) {
                JoyEvent ev;
                ev.device = d.name;
                ev.button = i + 1;
                out.push_back(ev);
            }
            d.down[i] = now;
        }
    }
    if (lost) {  // peripherique debranche : on relistera dans quelques secondes
        impl_->CloseDevices();
        impl_->lastEnum = GetTickCount64();
    }
    return out;
}

bool JoyPoll::PollPressed(const std::string& deviceFilter, int buttonFilter, JoyEvent* ev) {
    for (const JoyEvent& e : PollAllPressed(deviceFilter)) {
        if (buttonFilter == 0 || buttonFilter == e.button) {
            if (ev) *ev = e;
            return true;
        }
    }
    return false;
}

std::vector<std::string> JoyPoll::Devices() {
    std::vector<std::string> out;
    impl_->Ensure();
    for (const auto& d : impl_->devs) out.push_back(d.name);
    return out;
}

}  // namespace hands::win
