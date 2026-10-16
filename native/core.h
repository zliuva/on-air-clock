#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace clockapp {
namespace fs = std::filesystem;
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
std::string Quote(const std::string& text);
std::wstring ErrorText(DWORD code);
void Check(HRESULT hr);
void Log(const std::string& text);
extern fs::path logPath;
fs::path ExecutablePath();

template<class T> class ComPtr {
    T* p = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** put() { reset(); return &p; }
    T* get() const { return p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

enum class Mode { Idle, Microphone, Camera };
struct Capture { bool camera = false, microphone = false; Mode mode() const { return camera ? Mode::Camera : microphone ? Mode::Microphone : Mode::Idle; } };
const char* ModeName(Mode mode);
struct Settings {
    std::wstring url = L"http://awtrix.local", username = L"admin";
    int pollMs = 500, retryMs = 2000, testMs = 6000, releaseMs = 800;
    std::string cameraColor = "#FF0000", micColor = "#B86000";
    void Validate() const;
    std::string Json() const;
    static Settings Parse(const std::string& json);
};
struct Store {
    fs::path directory;
    bool Exists() const;
    Settings Load() const;
    std::string Password() const;
    void Save(const Settings& settings, const std::string& password) const;
};

std::vector<std::pair<int, int>> SignPixels();
std::string Payload(const Settings& settings, Mode mode);
class ClockApi {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    ClockApi(const Settings& settings, const std::string& password);
    ~ClockApi();
    std::pair<DWORD, std::string> Request(const wchar_t* method, const std::wstring& path, const std::string& body = "");
    void Push(Mode mode);
    void Dismiss();
    std::string Device();
};

void DismissWithRetries(ClockApi& api, int attempts = 3);
class Alerts {
    Mode sent = Mode::Idle;
    ULONGLONG next = 0;
    bool owned = false, pending = false;
    int retry;
    std::function<void(Mode)> push;
    std::function<void()> dismiss;
public:
    Alerts(int retryMs, std::function<void(Mode)> p, std::function<void()> d) : retry(retryMs), push(std::move(p)), dismiss(std::move(d)) { }
    bool Tick(Mode desired, ULONGLONG now);
    void Clear();
};
struct Latch {
    int delay;
    std::optional<ULONGLONG> last;
    bool Update(bool active, ULONGLONG now) { if (active) last = now; return active || (last && now - *last < static_cast<ULONGLONG>(delay)); }
};

class Camera {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Camera();
    ~Camera();
    bool Active() const;
    unsigned Reports() const;
    std::string Error() const;
};
struct AudioSession { std::wstring device, owner; DWORD pid = 0; bool active = false, muted = false; };
std::vector<AudioSession> MicrophoneSessions(bool details = false);
std::string Diagnose(int seconds);
std::string VerifyClock(const Store& store);
int SelfTest();
}
