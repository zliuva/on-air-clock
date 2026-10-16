#include "core.h"
#include <shellapi.h>
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <set>

using namespace clockapp;
namespace {
constexpr UINT WM_TRAY = WM_APP + 1, WM_STATUS = WM_APP + 2, WM_COUNTDOWN = WM_APP + 3, WM_TEST_DONE = WM_APP + 4;
constexpr int ID_SETTINGS = 101, ID_PAUSE = 102, ID_STARTUP = 103, ID_LOG = 104, ID_QUIT = 105;
constexpr int ID_URL = 201, ID_USER = 202, ID_PASSWORD = 203, ID_TEST_CAMERA = 204, ID_TEST_MIC = 205, ID_SAVE = 206, ID_CANCEL = 207, ID_TEST_STATUS = 208;
constexpr wchar_t windowClass[] = L"OnAirClockNativeHost", settingsClass[] = L"OnAirClockNativeSettings";
HINSTANCE instance;

std::wstring Text(HWND control) { int n = GetWindowTextLengthW(control); std::wstring text(n + 1, 0); GetWindowTextW(control, text.data(), n + 1); text.resize(n); return text; }
void ErrorBox(HWND owner, const std::exception& error) { MessageBoxW(owner, Wide(error.what()).c_str(), L"ON AIR Clock", MB_OK | MB_ICONERROR); }
void Output(const std::string& text) { DWORD n = 0; HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr); }
bool StartupEnabled() { HKEY key = nullptr; if (RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_READ,&key) != ERROR_SUCCESS) return false; auto status = RegQueryValueExW(key,L"OnAirClock",nullptr,nullptr,nullptr,nullptr); RegCloseKey(key); return status == ERROR_SUCCESS; }
void SetStartup(bool enabled, const Store& store) {
    HKEY key = nullptr; LSTATUS r = RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr); if (r != ERROR_SUCCESS) throw std::runtime_error(Utf8(ErrorText(r)));
    if (enabled) { std::wstring command = L"\"" + ExecutablePath().wstring() + L"\" --data-dir \"" + store.directory.wstring() + L"\""; r = RegSetValueExW(key,L"OnAirClock",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))); }
    else { r = RegDeleteValueW(key,L"OnAirClock"); if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS; }
    RegCloseKey(key); if (r != ERROR_SUCCESS) throw std::runtime_error(Utf8(ErrorText(r)));
}
HICON CircleIcon(COLORREF color) {
    BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = 32; header.bV5Height = -32; header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS; header.bV5RedMask = 0x00FF0000; header.bV5GreenMask = 0x0000FF00; header.bV5BlueMask = 0x000000FF; header.bV5AlphaMask = 0xFF000000;
    void* data = nullptr; HDC dc = GetDC(nullptr); HBITMAP bitmap = CreateDIBSection(dc,reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS,&data,nullptr,0); ReleaseDC(nullptr,dc); if (!bitmap) return CopyIcon(LoadIconW(nullptr,IDI_APPLICATION));
    auto pixels = static_cast<DWORD*>(data); DWORD fill = 0xFF000000 | (GetRValue(color)<<16) | (GetGValue(color)<<8) | GetBValue(color);
    for (int y=0;y<32;++y) for(int x=0;x<32;++x) { double dx=x-15.5,dy=y-15.5,r=dx*dx+dy*dy; pixels[y*32+x] = r<=196 ? (r>=42 && r<=81 ? 0xFFFFFFFF : fill) : 0; }
    HBITMAP mask = CreateBitmap(32,32,1,1,nullptr); ICONINFO info{TRUE,0,0,mask,bitmap}; HICON icon = CreateIconIndirect(&info); DeleteObject(mask); DeleteObject(bitmap); return icon;
}
struct Status { Capture capture; std::string connection = "Connecting to clock", error; };
struct CleanupOnExit {
    ClockApi& api; const char* reason; bool pending=true;
    void Clear() {
        pending=false;
        try {DismissWithRetries(api);Log(std::string(reason)+": ON AIR notification dismissed.");}
        catch(const std::exception& e){Log(std::string(reason)+" could not dismiss ON AIR after retries: "+e.what());throw;}
    }
    ~CleanupOnExit(){if(pending)try{Clear();}catch(...) {}}
};
struct TestJob { Settings settings; std::string password; Mode mode; HWND target; };
struct App {
    Store store; Settings settings; std::string password;
    HWND window = nullptr, dialog = nullptr;
    HFONT font = nullptr; HICON icons[4]{};
    NOTIFYICONDATAW tray{};
    HANDLE stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread worker;
    std::atomic<bool> paused{false}, testing{false};
    std::mutex mutex;
    Status status;
    std::optional<TestJob> job;
    std::string testError;
    int dpi = 96;
    explicit App(Store value) : store(std::move(value)), settings(store.Load()) {
        try { password = store.Password(); } catch (const std::exception& e) { Log(e.what()); }
        if (!stopEvent) throw std::runtime_error("Cannot create monitor stop event.");
    }
    ~App() { Stop(); if (tray.hWnd) Shell_NotifyIconW(NIM_DELETE,&tray); for (auto i : icons) if (i) DestroyIcon(i); if(font) DeleteObject(font); CloseHandle(stopEvent); }
    void Publish(Status value) { { std::lock_guard<std::mutex> lock(mutex); status = std::move(value); } PostMessageW(window,WM_STATUS,0,0); }
    Status CurrentStatus() { std::lock_guard<std::mutex> lock(mutex); return status; }
    void Start() { if (worker.joinable()) return; ResetEvent(stopEvent); worker = std::thread([this]{ Run(); }); }
    void Stop() { if (worker.joinable()) { SetEvent(stopEvent); worker.join(); } }
    bool Wait(DWORD ms) { return WaitForSingleObject(stopEvent,ms) == WAIT_OBJECT_0; }
    void Run() {
        HRESULT com = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try {
            Check(com); ClockApi api(settings,password); CleanupOnExit cleanup{api,"Shutdown"}; Alerts alerts(settings.retryMs,[&](Mode m){api.Push(m);},[&]{api.Dismiss();}); std::unique_ptr<Camera> camera;
            bool startupCleanup=true;ULONGLONG retryStartup=0;
            Latch cameraLatch{settings.releaseMs,{}}, micLatch{settings.releaseMs,{}}; ULONGLONG retryCamera=0,retryConnection=0; std::string connection="Connecting to clock",lastLog;
            while (!Wait(0)) {
                std::optional<TestJob> nextJob;
                { std::lock_guard<std::mutex> lock(mutex); if (job) { nextJob = std::move(job); job.reset(); } }
                if (nextJob) {
                    std::string failure;
                    try {
                        alerts.Clear(); ClockApi preview(nextJob->settings,nextJob->password);CleanupOnExit previewCleanup{preview,"Preview cleanup"};
                        preview.Push(nextJob->mode);
                        ULONGLONG end=GetTickCount64()+nextJob->settings.testMs; int previous=-1;
                        while (!Wait(0) && GetTickCount64()<end) { int seconds=static_cast<int>((end-GetTickCount64()+999)/1000); if(seconds!=previous) { PostMessageW(nextJob->target,WM_COUNTDOWN,seconds,0); previous=seconds; } if(Wait(100)) break; }
                        PostMessageW(nextJob->target,WM_COUNTDOWN,0,0); Wait(300);
                        previewCleanup.Clear();
                    } catch (const std::exception& e) { failure=e.what(); }
                    { std::lock_guard<std::mutex> lock(mutex); testError=failure; }
                    PostMessageW(nextJob->target,WM_TEST_DONE,0,0);
                    if (Wait(0)) break;
                }
                Status update; ULONGLONG now=GetTickCount64();
                try { if (!camera && now>=retryCamera) camera=std::make_unique<Camera>(); if(camera) { update.capture.camera=camera->Active(); update.error=camera->Error(); } else update.error="Camera monitor unavailable; retrying"; }
                catch (const std::exception& e) { retryCamera=now+5000; update.error="Camera monitor: "+std::string(e.what()); }
                try { for(auto& s:MicrophoneSessions()) update.capture.microphone|=s.active; } catch(const std::exception& e) { if(!update.error.empty()) update.error+="; "; update.error+="Microphone monitor: "+std::string(e.what()); }
                update.capture.camera=cameraLatch.Update(update.capture.camera,now); update.capture.microphone=micLatch.Update(update.capture.microphone,now);
                Mode desired=paused ? Mode::Idle : update.capture.mode();
                try { if(connection!="Connected" && now>=retryConnection) { retryConnection=now+10000; api.Device(); connection="Connected"; } if(startupCleanup&&now>=retryStartup){retryStartup=now+settings.retryMs;api.Dismiss();startupCleanup=false;}if(!startupCleanup&&alerts.Tick(desired,now)) connection="Connected"; }
                catch(const std::exception& e) { connection=e.what(); }
                update.connection=connection; std::string line="Capture="+std::string(ModeName(update.capture.mode()))+"; "+(update.error.empty()?connection:update.error); if(line!=lastLog) {Log(line);lastLog=line;} Publish(std::move(update)); if(Wait(settings.pollMs)) break;
            }
            cleanup.Clear();
        } catch(const std::exception& e) { Publish({{},"Monitor stopped",e.what()}); Log("Monitor stopped: "+std::string(e.what())); }
        if(SUCCEEDED(com)) CoUninitialize();
    }
    void AddTray() {
        tray={}; tray.cbSize=sizeof(tray); tray.hWnd=window; tray.uID=1; tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; tray.uCallbackMessage=WM_TRAY; tray.hIcon=icons[0]; wcscpy_s(tray.szTip,L"ON AIR Clock"); Shell_NotifyIconW(NIM_ADD,&tray); tray.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&tray);
    }
    void UpdateTray() { auto s=CurrentStatus(); int index=paused?0:(!s.error.empty()||s.connection!="Connected")?3:s.capture.camera?1:s.capture.microphone?2:0; tray.hIcon=icons[index]; std::wstring tip=L"ON AIR Clock: "+(paused?std::wstring(L"Paused"):Wide(ModeName(s.capture.mode()))); wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE); tray.uFlags=NIF_ICON|NIF_TIP; Shell_NotifyIconW(NIM_MODIFY,&tray); }
    void SettingsWindow();
    void Menu() {
        auto s=CurrentStatus(); HMENU menu=CreatePopupMenu();
        std::wstring capture=s.capture.camera?(s.capture.microphone?L"Camera + microphone in use":L"Camera in use"):s.capture.microphone?L"Microphone in use":L"Camera and microphone idle";
        AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"ON AIR Clock"); AppendMenuW(menu,MF_STRING|MF_GRAYED,0,capture.c_str()); auto detail=Wide(s.error.empty()?s.connection:s.error); AppendMenuW(menu,MF_STRING|MF_GRAYED,0,detail.c_str()); AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(paused?MF_CHECKED:0),ID_PAUSE,L"Pause until resumed"); AppendMenuW(menu,MF_STRING,ID_SETTINGS,L"Settings..."); AppendMenuW(menu,MF_STRING|(StartupEnabled()?MF_CHECKED:0),ID_STARTUP,L"Start at sign-in"); AppendMenuW(menu,MF_STRING,ID_LOG,L"Open log"); AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,ID_QUIT,L"Quit");
        POINT p;GetCursorPos(&p);SetForegroundWindow(window);int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);
        try { switch(command) {case ID_SETTINGS:SettingsWindow();break;case ID_PAUSE:paused=!paused;UpdateTray();break;case ID_STARTUP:SetStartup(!StartupEnabled(),store);break;case ID_LOG:ShellExecuteW(nullptr,L"open",L"notepad.exe",(L"\""+logPath.wstring()+L"\"").c_str(),nullptr,SW_SHOWNORMAL);break;case ID_QUIT:PostMessageW(window,WM_CLOSE,0,0);break;} } catch(const std::exception& e){ErrorBox(window,e);}
    }
};
LRESULT CALLBACK HostProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));app->window=window;}
    if(!app)return DefWindowProcW(window,message,w,l);
    static UINT taskbar=RegisterWindowMessageW(L"TaskbarCreated");if(message==taskbar){app->AddTray();return 0;}
    switch(message){case WM_TRAY:if(LOWORD(l)==WM_RBUTTONUP||LOWORD(l)==WM_CONTEXTMENU)app->Menu();else if(LOWORD(l)==WM_LBUTTONDBLCLK||LOWORD(l)==NIN_KEYSELECT)app->SettingsWindow();return 0;case WM_STATUS:app->UpdateTray();return 0;case WM_QUERYENDSESSION:SetEvent(app->stopEvent);return TRUE;case WM_ENDSESSION:app->Stop();if(!w)app->Start();return 0;case WM_POWERBROADCAST:if(w==PBT_APMSUSPEND)app->Stop();else if(w==PBT_APMRESUMEAUTOMATIC)app->Start();return TRUE;case WM_CLOSE:app->Stop();if(app->dialog)DestroyWindow(app->dialog);DestroyWindow(window);return 0;case WM_DESTROY:PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
struct DialogData { App* app; int dpi=96; HFONT font=nullptr; std::vector<HWND> controls; };
int Px(int n,int dpi){return MulDiv(n,dpi,96);}
HWND Control(DialogData& d,HWND parent,const wchar_t* klass,const std::wstring& text,DWORD style,int id,int x,int y,int width,int height) {
    HWND c=CreateWindowExW(wcscmp(klass,L"EDIT")==0?WS_EX_CLIENTEDGE:0,klass,text.c_str(),WS_CHILD|WS_VISIBLE|style,Px(x,d.dpi),Px(y,d.dpi),Px(width,d.dpi),Px(height,d.dpi),parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(d.font),TRUE);d.controls.push_back(c);return c;
}
void EnableDialog(HWND window,bool enable) {for(int id:{ID_URL,ID_USER,ID_PASSWORD,ID_TEST_CAMERA,ID_TEST_MIC,ID_SAVE,ID_CANCEL})EnableWindow(GetDlgItem(window,id),enable);}
LRESULT CALLBACK SettingsProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto data=reinterpret_cast<DialogData*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){data=new DialogData{static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams)};SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));}
    if(!data)return DefWindowProcW(window,message,w,l);App& app=*data->app;
    switch(message){
        case WM_CREATE:{
            data->dpi=GetDpiForWindow(window);data->font=CreateFontW(-MulDiv(9,data->dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            RECT rect{0,0,Px(480,data->dpi),Px(330,data->dpi)};AdjustWindowRectExForDpi(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,0,WS_EX_DLGMODALFRAME,data->dpi);SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER);
            auto field=[&](const wchar_t* label,int id,const std::wstring& value,int y,DWORD extra=0){Control(*data,window,L"STATIC",label,0,0,16,y+3,105,23);Control(*data,window,L"EDIT",value,WS_TABSTOP|ES_AUTOHSCROLL|extra,id,126,y,338,24);};
            field(L"Clock URL",ID_URL,app.settings.url,16);field(L"Username",ID_USER,app.settings.username,46);field(L"Password",ID_PASSWORD,Wide(app.password),76,ES_PASSWORD);
            Control(*data,window,L"STATIC",L"Red: camera in use. Amber: microphone only.\r\nThe alert is held until capture stops or you dismiss it.\r\nPassword is encrypted for your Windows account.",0,0,16,115,448,60);
            Control(*data,window,L"BUTTON",L"Test camera (red)",WS_TABSTOP,ID_TEST_CAMERA,16,184,175,29);Control(*data,window,L"BUTTON",L"Test mic (amber)",WS_TABSTOP,ID_TEST_MIC,199,184,175,29);
            Control(*data,window,L"STATIC",L"Tests display for 6 seconds using the fields above.",0,ID_TEST_STATUS,16,222,448,25);
            Control(*data,window,L"BUTTON",L"Cancel",WS_TABSTOP,ID_CANCEL,302,272,75,29);Control(*data,window,L"BUTTON",L"Save",WS_TABSTOP|BS_DEFPUSHBUTTON,ID_SAVE,389,272,75,29);
            SetFocus(GetDlgItem(window,ID_URL));SendMessageW(GetDlgItem(window,ID_URL),EM_SETSEL,0,-1);return 0;
        }
        case WM_COMMAND:{int id=LOWORD(w);if(id==IDOK)id=ID_SAVE;else if(id==IDCANCEL)id=ID_CANCEL;if(HIWORD(w)!=BN_CLICKED)return 0;if(id==ID_CANCEL){PostMessageW(window,WM_CLOSE,0,0);return 0;}if(id!=ID_SAVE&&id!=ID_TEST_CAMERA&&id!=ID_TEST_MIC)return 0;
            try {Settings edited=app.settings;edited.url=Text(GetDlgItem(window,ID_URL));edited.username=Text(GetDlgItem(window,ID_USER));edited.Validate();std::string password=Utf8(Text(GetDlgItem(window,ID_PASSWORD)));
                if(id==ID_SAVE){app.store.Save(edited,password);app.Stop();app.settings=edited;app.password=password;app.Start();DestroyWindow(window);}
                else if(!app.testing){app.testing=true;EnableDialog(window,false);SetDlgItemTextW(window,ID_TEST_STATUS,id==ID_TEST_CAMERA?L"Showing red camera sign...":L"Showing amber microphone sign...");std::lock_guard<std::mutex> lock(app.mutex);app.testError.clear();app.job=TestJob{edited,password,id==ID_TEST_CAMERA?Mode::Camera:Mode::Microphone,window};}
            } catch(const std::exception& e){ErrorBox(window,e);}return 0;
        }
        case WM_COUNTDOWN:{auto suffix=L" ("+std::to_wstring(w)+L"s)";SetDlgItemTextW(window,ID_TEST_CAMERA,(L"Test camera (red)"+suffix).c_str());SetDlgItemTextW(window,ID_TEST_MIC,(L"Test mic (amber)"+suffix).c_str());return 0;}
        case WM_TEST_DONE:{app.testing=false;EnableDialog(window,true);SetDlgItemTextW(window,ID_TEST_CAMERA,L"Test camera (red)");SetDlgItemTextW(window,ID_TEST_MIC,L"Test mic (amber)");std::string error;{std::lock_guard<std::mutex> lock(app.mutex);error=app.testError;}SetDlgItemTextW(window,ID_TEST_STATUS,error.empty()?L"Test complete.":L"Test failed. Check the connection and credentials.");if(!error.empty())ErrorBox(window,std::runtime_error(error));return 0;}
        case WM_CLOSE:if(!app.testing)DestroyWindow(window);return 0;
        case WM_DESTROY:app.dialog=nullptr;return 0;
        case WM_NCDESTROY:if(data->font)DeleteObject(data->font);delete data;SetWindowLongPtrW(window,GWLP_USERDATA,0);return DefWindowProcW(window,message,w,l);
    }
    return DefWindowProcW(window,message,w,l);
}
void App::SettingsWindow(){if(dialog){ShowWindow(dialog,SW_RESTORE);SetForegroundWindow(dialog);return;}dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,settingsClass,L"ON AIR Clock settings",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,500,370,window,nullptr,instance,this);if(!dialog)throw std::runtime_error("Cannot open Settings.");SendMessageW(dialog,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icons[0]));RECT bounds;GetWindowRect(dialog,&bounds);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor);SetWindowPos(dialog,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-(bounds.right-bounds.left))/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-(bounds.bottom-bounds.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);ShowWindow(dialog,SW_SHOWNORMAL);if(!IsWindowVisible(dialog))ShowWindow(dialog,SW_SHOW);SetForegroundWindow(dialog);}
int RunGui(Store store,bool showSettings) {
    App app(std::move(store));WNDCLASSW klass{};klass.hInstance=instance;klass.lpfnWndProc=HostProc;klass.lpszClassName=windowClass;RegisterClassW(&klass);klass.lpfnWndProc=SettingsProc;klass.lpszClassName=settingsClass;klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&klass);
    app.icons[0]=CircleIcon(RGB(105,105,105));app.icons[1]=CircleIcon(RGB(255,0,0));app.icons[2]=CircleIcon(RGB(255,140,0));app.icons[3]=CircleIcon(RGB(65,105,225));
    if(!CreateWindowExW(0,windowClass,L"ON AIR Clock",WS_OVERLAPPED,0,0,0,0,nullptr,nullptr,instance,&app))throw std::runtime_error("Cannot create tray host.");app.AddTray();app.Start();if(showSettings||!app.store.Exists())app.SettingsWindow();
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){if(app.dialog&&IsDialogMessageW(app.dialog,&message))continue;TranslateMessage(&message);DispatchMessageW(&message);}return 0;
}
}

int WINAPI wWinMain(HINSTANCE hInstance,HINSTANCE,PWSTR,int) {
    instance=hInstance;int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::vector<std::wstring> args;for(int i=1;i<argc;++i)args.emplace_back(argv[i]);LocalFree(argv);
    auto has=[&](const wchar_t* name){return std::find(args.begin(),args.end(),name)!=args.end();};auto argument=[&](const wchar_t* name)->std::optional<std::wstring>{auto i=std::find(args.begin(),args.end(),name);if(i!=args.end()&&++i!=args.end())return *i;return {};};
    bool cli=has(L"--diagnose")||has(L"--probe")||has(L"--pulse")||has(L"--clear")||has(L"--set-password-stdin")||has(L"--self-test")||has(L"--verify-clock")||has(L"--quit");
    HRESULT com=CoInitializeEx(nullptr,cli?COINIT_MULTITHREADED:COINIT_APARTMENTTHREADED);int result=0;
    try {
        Check(com);auto base=ExecutablePath().parent_path();fs::path data=base/L"data";
        if(auto value=argument(L"--data-dir"))data=fs::absolute(*value);Store store{data};logPath=data/L"on-air-clock.log";
        if(has(L"--quit")){HWND host=FindWindowW(windowClass,nullptr);if(host){DWORD pid=0;GetWindowThreadProcessId(host,&pid);HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);PostMessageW(host,WM_CLOSE,0,0);if(process){DWORD waited=WaitForSingleObject(process,15000);CloseHandle(process);if(waited==WAIT_TIMEOUT)throw std::runtime_error("Host is still attempting clock cleanup.");}}Output("Host quit; shutdown cleanup completed.\n");}
        else if(has(L"--self-test")){result=SelfTest();}
        else if(has(L"--diagnose")){Output(Diagnose(argument(L"--seconds")?std::stoi(*argument(L"--seconds")):3));}
        else if(has(L"--verify-clock")){Output(VerifyClock(store));}
        else if(has(L"--set-password-stdin")){std::string password;std::getline(std::cin,password);if(!password.empty()&&password.back()=='\r')password.pop_back();store.Save(store.Load(),password);Output("Credentials encrypted and saved.\n");}
        else if(has(L"--probe")||has(L"--pulse")||has(L"--clear")){ClockApi api(store.Load(),store.Password());if(has(L"--probe"))Output(api.Device()+"\n");else if(has(L"--clear")){DismissWithRetries(api);Output("Cleared ON AIR notification.\n");}else{auto mode=argument(L"--pulse");if(!mode||(*mode!=L"camera"&&*mode!=L"mic"))throw std::runtime_error("Use --pulse camera or --pulse mic.");api.Push(*mode==L"camera"?Mode::Camera:Mode::Microphone);Output("Sent a held notification; use --clear to dismiss it.\n");}}
        else {HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\OnAirClock-Host");if(!mutex)throw std::runtime_error("Cannot create host mutex.");if(GetLastError()!=ERROR_ALREADY_EXISTS){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);result=RunGui(store,has(L"--settings"));}CloseHandle(mutex);}
    }catch(const std::exception& e){Log(e.what());if(cli)Output(std::string(e.what())+"\n");else ErrorBox(nullptr,e);result=1;}
    if(SUCCEEDED(com))CoUninitialize();return result;
}
