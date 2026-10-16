#include "core.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <propsys.h>
#include <fstream>
#include <iomanip>
#include <set>

namespace clockapp {
fs::path logPath;
static std::mutex logMutex;
std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr); return out;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring out(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n); return out;
}
std::string Quote(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
        else out << c;
    }
    return out.str() + '"';
}
std::wstring ErrorText(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Windows error " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error(Utf8(ErrorText(static_cast<DWORD>(hr)))); }
static void WinCheck(BOOL value) { if (!value) throw std::runtime_error(Utf8(ErrorText(GetLastError()))); }
fs::path ExecutablePath() { std::wstring p(32768, 0); DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size())); if (!n || n >= p.size()) throw std::runtime_error("Cannot locate executable."); p.resize(n); return fs::path(p); }
void Log(const std::string& text) {
    if (logPath.empty()) return;
    std::lock_guard<std::mutex> lock(logMutex);
    try {
        fs::create_directories(logPath.parent_path());
        if (fs::exists(logPath) && fs::file_size(logPath) > 512000) MoveFileExW(logPath.c_str(), fs::path(logPath.wstring() + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        SYSTEMTIME t{}; GetLocalTime(&t);
        std::ofstream out(logPath, std::ios::app); out << std::setfill('0') << t.wYear << '-' << std::setw(2) << t.wMonth << '-' << std::setw(2) << t.wDay << 'T' << std::setw(2) << t.wHour << ':' << std::setw(2) << t.wMinute << ':' << std::setw(2) << t.wSecond << ' ' << text << '\n';
    } catch (...) { }
}
const char* ModeName(Mode m) { return m == Mode::Camera ? "Camera" : m == Mode::Microphone ? "Microphone" : "Idle"; }

// Settings are a flat JSON object; preserve compatibility with the earlier app.
class JsonReader {
    const std::string& s; size_t p = 0;
    void Space() { while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) ++p; }
    char Take() { if (p == s.size()) throw std::runtime_error("Incomplete settings JSON."); return s[p++]; }
    unsigned Hex4() { unsigned value = 0; for (int i = 0; i < 4; ++i) { char c = Take(); if (!isxdigit(static_cast<unsigned char>(c))) throw std::runtime_error("Invalid JSON escape."); value = value * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10); } return value; }
public:
    explicit JsonReader(const std::string& value) : s(value) { }
    std::string String() {
        if (Take() != '"') throw std::runtime_error("Expected JSON string."); std::string value;
        for (;;) {
            char c = Take(); if (c == '"') return value;
            if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("Invalid JSON string.");
            if (c != '\\') { value += c; continue; }
            c = Take();
            switch (c) {
                case '"': case '\\': case '/': value += c; break;
                case 'b': value += '\b'; break; case 'f': value += '\f'; break; case 'n': value += '\n'; break; case 'r': value += '\r'; break; case 't': value += '\t'; break;
                case 'u': {
                    unsigned first = Hex4(); std::wstring chars(1, static_cast<wchar_t>(first));
                    if (first >= 0xD800 && first <= 0xDBFF) { if (Take() != '\\' || Take() != 'u') throw std::runtime_error("Invalid JSON surrogate."); unsigned second = Hex4(); if (second < 0xDC00 || second > 0xDFFF) throw std::runtime_error("Invalid JSON surrogate."); chars += static_cast<wchar_t>(second); }
                    value += Utf8(chars); break;
                }
                default: throw std::runtime_error("Invalid JSON escape.");
            }
        }
    }
    std::map<std::string, std::string> Object() {
        std::map<std::string, std::string> values; Space(); if (Take() != '{') throw std::runtime_error("Expected settings object."); Space();
        if (p < s.size() && s[p] == '}') { ++p; Space(); if (p != s.size()) throw std::runtime_error("Trailing settings data."); return values; }
        for (;;) {
            Space(); std::string key = String(); Space(); if (Take() != ':') throw std::runtime_error("Expected JSON colon."); Space();
            std::string value;
            if (p < s.size() && s[p] == '"') value = String();
            else { size_t begin = p; while (p < s.size() && s[p] != ',' && s[p] != '}') ++p; value = s.substr(begin, p - begin); while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) value.pop_back(); }
            values[key] = value; Space(); char c = Take(); if (c == '}') break; if (c != ',') throw std::runtime_error("Expected JSON comma.");
        }
        Space(); if (p != s.size()) throw std::runtime_error("Trailing settings data."); return values;
    }
};
static URL_COMPONENTS ParseUrl(const std::wstring& url, std::wstring& host) {
    URL_COMPONENTS c{}; c.dwStructSize = sizeof(c); c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = c.dwUserNameLength = c.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS) || c.dwUserNameLength || c.dwPasswordLength || c.dwExtraInfoLength || (c.dwUrlPathLength && std::wstring(c.lpszUrlPath, c.dwUrlPathLength) != L"/")) throw std::runtime_error("Clock URL must be an http(s) origin, for example http://awtrix.local.");
    host.assign(c.lpszHostName, c.dwHostNameLength); return c;
}
void Settings::Validate() const {
    std::wstring host; ParseUrl(url, host);
    if (username.find(L':') != std::wstring::npos) throw std::runtime_error("Username cannot contain a colon.");
    if (pollMs < 100 || pollMs > 5000 || retryMs < pollMs || retryMs > 10000 || testMs < 1000 || testMs > 30000 || releaseMs < 0 || releaseMs > 5000) throw std::runtime_error("Invalid capture or notification timing.");
    for (auto& c : { cameraColor, micColor }) if (c.size() != 7 || c[0] != '#' || !std::all_of(c.begin() + 1, c.end(), [](unsigned char ch) { return isxdigit(ch); })) throw std::runtime_error("Colors must use #RRGGBB.");
}
std::string Settings::Json() const { std::ostringstream o; o << "{\n  \"ClockUrl\": " << Quote(Utf8(url)) << ",\n  \"Username\": " << Quote(Utf8(username)) << ",\n  \"PollMs\": " << pollMs << ",\n  \"RetryMs\": " << retryMs << ",\n  \"TestMs\": " << testMs << ",\n  \"ReleaseDelayMs\": " << releaseMs << ",\n  \"CameraColor\": " << Quote(cameraColor) << ",\n  \"MicColor\": " << Quote(micColor) << "\n}\n"; return o.str(); }
Settings Settings::Parse(const std::string& json) {
    auto v = JsonReader(json).Object(); Settings s;
    if (v.count("ClockUrl")) s.url = Wide(v["ClockUrl"]); if (v.count("Username")) s.username = Wide(v["Username"]);
    auto number = [&](const char* key, int& value) { if (v.count(key)) { size_t end = 0; long long n = std::stoll(v[key], &end); if (end != v[key].size() || n < 0 || n > 1000000) throw std::runtime_error("Invalid numeric setting."); value = static_cast<int>(n); } };
    // Read the old timing keys for compatibility; these no longer renew notifications.
    number("RenewMs", s.retryMs); number("LeaseMs", s.testMs);
    number("PollMs", s.pollMs); number("RetryMs", s.retryMs); number("TestMs", s.testMs); number("ReleaseDelayMs", s.releaseMs);
    if (v.count("CameraColor")) s.cameraColor = v["CameraColor"]; if (v.count("MicColor")) s.micColor = v["MicColor"]; s.Validate(); return s;
}
static std::string Read(const fs::path& path) { std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("Cannot read " + Utf8(path.wstring())); return { std::istreambuf_iterator<char>(in), {} }; }
static void Write(const fs::path& path, const std::string& data) { fs::create_directories(path.parent_path()); auto temp = fs::path(path.wstring() + L".tmp"); { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out.write(data.data(), static_cast<std::streamsize>(data.size())); if (!out) throw std::runtime_error("Cannot save settings."); } WinCheck(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)); }
bool Store::Exists() const { return fs::exists(directory / L"settings.json"); }
Settings Store::Load() const { return Exists() ? Settings::Parse(Read(directory / L"settings.json")) : Settings{}; }
std::string Store::Password() const {
    auto path = directory / L"password.dpapi"; if (!fs::exists(path)) return {};
    std::string encrypted = Read(path); DATA_BLOB in{ static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(encrypted.data()) }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) throw std::runtime_error("Password belongs to another Windows account. Enter it again in Settings.");
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData); SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
}
void Store::Save(const Settings& settings, const std::string& password) const {
    settings.Validate(); DATA_BLOB in{ static_cast<DWORD>(password.size()), reinterpret_cast<BYTE*>(const_cast<char*>(password.data())) }, out{};
    WinCheck(CryptProtectData(&in, L"ON AIR Clock", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out));
    try { Write(directory / L"password.dpapi", std::string(reinterpret_cast<char*>(out.pbData), out.cbData)); } catch (...) { LocalFree(out.pbData); throw; } LocalFree(out.pbData);
    Write(directory / L"settings.json", settings.Json());
}

std::vector<std::pair<int, int>> SignPixels() {
    static const std::vector<std::vector<std::string>> glyphs = {{".##.","#..#","#..#","#..#","#..#",".##."},{"#..#","##.#","##.#","#.##","#.##","#..#"},{},{".##.","#..#","#..#","####","#..#","#..#"},{"###",".#.",".#.",".#.",".#.","###"},{"###.","#..#","#..#","###.","#.#.","#..#"}};
    std::vector<std::pair<int, int>> pixels; int x = 3;
    for (auto& glyph : glyphs) { if (glyph.empty()) { x += 3; continue; } for (size_t y = 0; y < glyph.size(); ++y) for (size_t c = 0; c < glyph[y].size(); ++c) if (glyph[y][c] == '#') pixels.emplace_back(x + static_cast<int>(c), 1 + static_cast<int>(y)); x += static_cast<int>(glyph[0].size()) + 1; }
    return pixels;
}
std::string Payload(const Settings& s, Mode mode) { if (mode == Mode::Idle) throw std::runtime_error("Idle has no notification."); std::ostringstream o; o << "{\"name\":\"on-air-clock\",\"text\":\"\",\"textColor\":\"#FFFFFF\",\"backgroundColor\":" << Quote(mode == Mode::Camera ? s.cameraColor : s.micColor) << ",\"draw\":[[\"pixels\",\"#FFFFFF\""; for (auto [x,y] : SignPixels()) o << ',' << x << ',' << y; o << "]],\"hold\":true,\"stack\":false,\"wakeup\":true}"; return o.str(); }

struct HttpHandle { HINTERNET h = nullptr; HttpHandle() = default; explicit HttpHandle(HINTERNET value) : h(value) { if (!h) throw std::runtime_error(Utf8(ErrorText(GetLastError()))); } ~HttpHandle() { if (h) WinHttpCloseHandle(h); } };
struct ClockApi::Impl {
    Settings settings; std::wstring host, authorization; INTERNET_PORT port; bool secure;
    HttpHandle session;
    Impl(const Settings& s, const std::string& password) : settings(s), session(WinHttpOpen(L"OnAirClock/2.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) {
        s.Validate(); auto c = ParseUrl(s.url, host); port = c.nPort; secure = c.nScheme == INTERNET_SCHEME_HTTPS;
        WinCheck(WinHttpSetTimeouts(session.h, 1000, 1000, 1500, 1500));
        if (!s.username.empty()) { auto value = Utf8(s.username) + ':' + password; DWORD n = 0; WinCheck(CryptBinaryToStringW(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)); std::wstring encoded(n, 0); WinCheck(CryptBinaryToStringW(reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &n)); while (!encoded.empty() && encoded.back() == 0) encoded.pop_back(); authorization = L"Authorization: Basic " + encoded + L"\r\n"; }
    }
};
ClockApi::ClockApi(const Settings& s, const std::string& password) : impl(std::make_unique<Impl>(s, password)) { }
ClockApi::~ClockApi() = default;
std::pair<DWORD, std::string> ClockApi::Request(const wchar_t* method, const std::wstring& path, const std::string& body) {
    HttpHandle connection(WinHttpConnect(impl->session.h, impl->host.c_str(), impl->port, 0));
    HttpHandle request(WinHttpOpenRequest(connection.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, impl->secure ? WINHTTP_FLAG_SECURE : 0));
    DWORD disable = WINHTTP_DISABLE_REDIRECTS; WinCheck(WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)));
    std::wstring headers = impl->authorization + L"Content-Type: application/json\r\n";
    WinCheck(WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(headers.size()), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)); WinCheck(WinHttpReceiveResponse(request.h, nullptr));
    DWORD status = 0, n = sizeof(status); WinCheck(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &n, WINHTTP_NO_HEADER_INDEX));
    std::string result; DWORD available = 0;
    do { WinCheck(WinHttpQueryDataAvailable(request.h, &available)); if (!available) break; if (result.size() + available > 1024 * 1024) throw std::runtime_error("Clock response too large."); size_t before = result.size(); result.resize(before + available); DWORD read = 0; WinCheck(WinHttpReadData(request.h, result.data() + before, available, &read)); result.resize(before + read); } while (available);
    return {status, result};
}
static void CheckHttp(const std::pair<DWORD, std::string>& response) { if (response.first >= 200 && response.first < 300) return; if (response.first == 401) throw std::runtime_error("Clock authentication failed. Check username and password in Settings."); throw std::runtime_error("Clock returned HTTP " + std::to_string(response.first) + ": " + response.second.substr(0, 240)); }
void ClockApi::Push(Mode mode) { CheckHttp(Request(L"POST", L"/api/v1/notifications", Payload(impl->settings, mode))); }
void ClockApi::Dismiss() { auto r = Request(L"DELETE", L"/api/v1/notifications/on-air-clock"); if (r.first != 404) CheckHttp(r); }
std::string ClockApi::Device() { auto r = Request(L"GET", L"/api/v1/device"); CheckHttp(r); return r.second; }
void DismissWithRetries(ClockApi& api, int attempts) { for (int i=0; i<attempts; ++i) { try { api.Dismiss(); return; } catch (...) { if(i+1==attempts)throw; Sleep(200); } } }
bool Alerts::Tick(Mode desired, ULONGLONG now) {
    if(desired!=sent){sent=desired;pending=true;next=0;}
    if(!pending)return false;
    if(desired==Mode::Idle&&!owned){pending=false;return false;}
    if(now<next)return false;
    next=now+retry;
    if(desired==Mode::Idle){dismiss();owned=false;}else{owned=true;push(desired);}
    pending=false;return true;
}
void Alerts::Clear() { if(owned){dismiss();owned=false;sent=Mode::Idle;pending=false;} }

// ABI definitions mirror the Windows sensor activity monitor interfaces.
struct SensorProcess : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetProcessId(ULONG*) = 0; virtual HRESULT STDMETHODCALLTYPE GetStreamingState(BOOL*) = 0; virtual HRESULT STDMETHODCALLTYPE GetStreamingMode(int*) = 0; virtual HRESULT STDMETHODCALLTYPE GetReportTime(FILETIME*) = 0; };
struct SensorReport : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetFriendlyName(LPWSTR,ULONG,ULONG*) = 0; virtual HRESULT STDMETHODCALLTYPE GetSymbolicLink(LPWSTR,ULONG,ULONG*) = 0; virtual HRESULT STDMETHODCALLTYPE GetProcessCount(ULONG*) = 0; virtual HRESULT STDMETHODCALLTYPE GetProcessActivity(ULONG,SensorProcess**) = 0; };
struct SensorReports : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetCount(ULONG*) = 0; virtual HRESULT STDMETHODCALLTYPE GetActivityReport(ULONG,SensorReport**) = 0; virtual HRESULT STDMETHODCALLTYPE GetActivityReportByDeviceName(LPCWSTR,SensorReport**) = 0; };
struct SensorCallback : IUnknown { virtual HRESULT STDMETHODCALLTYPE OnActivitiesReport(SensorReports*) = 0; };
struct SensorMonitor : IUnknown { virtual HRESULT STDMETHODCALLTYPE Start() = 0; virtual HRESULT STDMETHODCALLTYPE Stop() = 0; };
static const GUID iidCallback = {0xDE5072EE,0xDBE3,0x46DC,{0x8A,0x87,0xB6,0xF6,0x31,0x19,0x47,0x51}};
class CameraCallback final : public SensorCallback {
    std::atomic<ULONG> refs{1};
public:
    mutable std::mutex mutex; std::map<std::wstring, bool> devices; unsigned reports = 0; std::string error;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override { if (!value) return E_POINTER; *value = nullptr; if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, iidCallback)) { *value = static_cast<SensorCallback*>(this); AddRef(); return S_OK; } return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnActivitiesReport(SensorReports* input) override {
        try { ULONG count = 0; Check(input->GetCount(&count)); for (ULONG i = 0; i < count; ++i) { ComPtr<SensorReport> device; Check(input->GetActivityReport(i, device.put())); wchar_t name[4096]{}; ULONG written = 0, processes = 0; Check(device->GetSymbolicLink(name,4096,&written)); Check(device->GetProcessCount(&processes)); bool active = false; for (ULONG j = 0; j < processes; ++j) { ComPtr<SensorProcess> p; Check(device->GetProcessActivity(j,p.put())); BOOL streaming = FALSE; Check(p->GetStreamingState(&streaming)); active |= streaming != FALSE; } std::lock_guard<std::mutex> lock(mutex); devices[name] = active; } std::lock_guard<std::mutex> lock(mutex); ++reports; error.clear(); }
        catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); error = e.what(); } catch (...) { }
        return S_OK;
    }
};
struct Camera::Impl {
    HMODULE platform = nullptr, group = nullptr; CameraCallback* callback = nullptr; ComPtr<SensorMonitor> monitor; bool started = false;
    using Startup = HRESULT(WINAPI*)(ULONG,DWORD); using Shutdown = HRESULT(WINAPI*)(); using Create = HRESULT(WINAPI*)(SensorCallback*,SensorMonitor**);
    ~Impl() { if (monitor) { monitor->Stop(); monitor.reset(); } if (callback) callback->Release(); if (started) { auto f = reinterpret_cast<Shutdown>(GetProcAddress(platform,"MFShutdown")); if (f) f(); } if (group) FreeLibrary(group); if (platform) FreeLibrary(platform); }
};
Camera::Camera() : impl(std::make_unique<Impl>()) {
    impl->platform = LoadLibraryExW(L"mfplat.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32); impl->group = LoadLibraryExW(L"mfsensorgroup.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!impl->platform || !impl->group) throw std::runtime_error("Windows camera activity monitor is unavailable.");
    auto startup = reinterpret_cast<Impl::Startup>(GetProcAddress(impl->platform,"MFStartup")); auto create = reinterpret_cast<Impl::Create>(GetProcAddress(impl->group,"MFCreateSensorActivityMonitor"));
    if (!startup || !create) throw std::runtime_error("Windows camera activity monitor is unavailable.");
    Check(startup(0x20070,1)); impl->started = true; impl->callback = new CameraCallback(); Check(create(impl->callback,impl->monitor.put())); Check(impl->monitor->Start());
}
Camera::~Camera() = default;
bool Camera::Active() const { std::lock_guard<std::mutex> lock(impl->callback->mutex); for (auto& v : impl->callback->devices) if (v.second) return true; return false; }
unsigned Camera::Reports() const { std::lock_guard<std::mutex> lock(impl->callback->mutex); return impl->callback->reports; }
std::string Camera::Error() const { std::lock_guard<std::mutex> lock(impl->callback->mutex); return impl->callback->error; }

std::vector<AudioSession> MicrophoneSessions(bool details) {
    ComPtr<IMMDeviceEnumerator> enumerator; Check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(enumerator.put())));
    ComPtr<IMMDeviceCollection> devices; Check(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,devices.put())); UINT count = 0; Check(devices->GetCount(&count)); std::vector<AudioSession> result; unsigned inspected = 0; std::string failure;
    for (UINT i = 0; i < count; ++i) {
        try { ComPtr<IMMDevice> device; Check(devices->Item(i,device.put())); std::wstring deviceName;
            if (details) { ComPtr<IPropertyStore> properties; Check(device->OpenPropertyStore(STGM_READ,properties.put())); PROPERTYKEY key{{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}},14}; PROPVARIANT value{}; Check(properties->GetValue(key,&value)); if (value.vt == VT_LPWSTR && value.pwszVal) deviceName = value.pwszVal; PropVariantClear(&value); }
            ComPtr<IAudioSessionManager2> manager; Check(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.put()))); ComPtr<IAudioSessionEnumerator> sessions; Check(manager->GetSessionEnumerator(sessions.put())); int n = 0; Check(sessions->GetCount(&n));
            for (int j = 0; j < n; ++j) { ComPtr<IAudioSessionControl> control; Check(sessions->GetSession(j,control.put())); AudioSession value; value.device = deviceName; AudioSessionState state; Check(control->GetState(&state)); value.active = state == AudioSessionStateActive;
                if (details) { ComPtr<ISimpleAudioVolume> volume; Check(control->QueryInterface(__uuidof(ISimpleAudioVolume),reinterpret_cast<void**>(volume.put()))); BOOL muted = FALSE; Check(volume->GetMute(&muted)); value.muted = muted != FALSE; ComPtr<IAudioSessionControl2> info; Check(control->QueryInterface(__uuidof(IAudioSessionControl2),reinterpret_cast<void**>(info.put()))); Check(info->GetProcessId(&value.pid)); HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,value.pid); if (process) { wchar_t path[32768]; DWORD size = 32768; if (QueryFullProcessImageNameW(process,0,path,&size)) value.owner = fs::path(std::wstring(path,size)).stem().wstring(); CloseHandle(process); } }
                result.push_back(std::move(value)); }
            ++inspected;
        } catch (const std::exception& e) { failure = e.what(); }
    }
    if (count && !inspected && !failure.empty()) throw std::runtime_error(failure); return result;
}
std::string Diagnose(int seconds) { if (seconds < 1 || seconds > 300) throw std::runtime_error("Diagnostic seconds must be 1-300."); Camera camera; std::ostringstream output; for (int i = 0; i < seconds * 2; ++i) { Sleep(500); auto sessions = MicrophoneSessions(true); bool active = std::any_of(sessions.begin(),sessions.end(),[](auto& v){return v.active;}); output << "{\"camera\":" << (camera.Active()?"true":"false") << ",\"microphone\":" << (active?"true":"false") << ",\"cameraReports\":" << camera.Reports() << ",\"error\":" << Quote(camera.Error()) << ",\"sessions\":["; bool first = true; for (auto& v : sessions) { if (!first) output << ','; first = false; output << "{\"DeviceName\":" << Quote(Utf8(v.device)) << ",\"ProcessId\":" << v.pid << ",\"ProcessName\":" << Quote(Utf8(v.owner)) << ",\"Active\":" << (v.active?"true":"false") << ",\"Muted\":" << (v.muted?"true":"false") << '}'; } output << "]}\n"; } return output.str(); }
std::string VerifyClock(const Store& store) {
    Settings s=store.Load();ClockApi api(s,store.Password());std::string report;
    auto screen=[&](const char* name,const std::string& color){Sleep(150);auto response=api.Request(L"GET",L"/api/v1/display/screen");CheckHttp(response);auto p=response.second.find("\"pixels\"");if(p==std::string::npos)throw std::runtime_error("No screen pixels returned.");p=response.second.find('[',p);auto end=response.second.find(']',p);if(p==std::string::npos||end==std::string::npos)throw std::runtime_error("Invalid screen response.");std::string numbers=response.second.substr(p+1,end-p-1);std::replace(numbers.begin(),numbers.end(),',',' ');std::istringstream input(numbers);std::vector<unsigned> pixels;unsigned value;while(input>>value)pixels.push_back(value);if(pixels.size()!=256)throw std::runtime_error("Expected a 32x8 display.");std::vector<unsigned> expected(256,static_cast<unsigned>(std::stoul(color.substr(1),nullptr,16)));for(auto [x,y]:SignPixels())expected[y*32+x]=0xFFFFFF;if(pixels!=expected)throw std::runtime_error(std::string("Unexpected pixels during ")+name);report+="PASS "+std::string(name)+" (6 pixel letters, blank top/bottom, equal 3 pixel side margins)\n";};
    try {
        api.Push(Mode::Camera);screen("red camera",s.cameraColor);Sleep(s.testMs+700);screen("held alert persists without renewal",s.cameraColor);
        api.Push(Mode::Microphone);screen("amber microphone",s.micColor);api.Dismiss();if(api.Request(L"DELETE",L"/api/v1/notifications/on-air-clock").first!=404)throw std::runtime_error("Named clear failed.");report+="PASS named dismissal\n";
        api.Push(Mode::Camera);DismissWithRetries(api);if(api.Request(L"DELETE",L"/api/v1/notifications/on-air-clock").first!=404)throw std::runtime_error("Shutdown-style held alert cleanup failed.");report+="PASS shutdown-style held alert cleanup\n";
        Write(store.directory/L"native-clock-verification.txt",report);return report;
    } catch(...) {try{DismissWithRetries(api);}catch(...){}throw;}
}
}
