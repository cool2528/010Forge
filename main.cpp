// 010 Editor 一键补丁工具 (Win32 API, Unicode, 中英双语)
// 单 exe：内嵌汉化 DLL + 图标 + 背景音乐；内置注入器，零外部依赖
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>
#include "license.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

// ---------- 控件 ID ----------
#define IDC_EDIT_NAME      1001
#define IDC_EDIT_YEAR      1002
#define IDC_BTN_SCAN       1101
#define IDC_BTN_ACTIVATE   1102
#define IDC_BTN_DEPLOY     1103
#define IDC_BTN_LAUNCH     1104
#define IDC_BTN_STATUS     1105
#define IDC_BTN_EXIT       1106
#define IDC_EDIT_PATH      1004
#define IDC_LIST_LOG       1201
#define IDC_CHK_MUSIC      1401
#define IDC_BTN_LANG       1402

static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static std::wstring g_installDir;
static bool g_musicOn = true;
static int  g_lang = 0;   // 0=English, 1=中文

// ---------- 双语 ----------
static const wchar_t* T(const wchar_t* en, const wchar_t* zh) {
    return g_lang == 1 ? zh : en;
}
static void DetectLanguage() {
    LANGID lid = GetUserDefaultUILanguage();
    g_lang = (PRIMARYLANGID(lid) == LANG_CHINESE) ? 1 : 0;
}

// ---------- 工具函数 ----------
static void LogRaw(const wchar_t* s) {
    HWND hList = GetDlgItem(g_hWnd, IDC_LIST_LOG);
    if (hList) {
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(hList, LB_SETCURSEL, SendMessageW(hList, LB_GETCOUNT, 0, 0) - 1, 0);
    }
}
static void Log(const wchar_t* en, const wchar_t* zh) {
    LogRaw(T(en, zh));
}
template<typename... Args>
static void LogF(const wchar_t* enFmt, const wchar_t* zhFmt, Args... args) {
    wchar_t buf[1024];
    swprintf_s(buf, 1024, T(enFmt, zhFmt), args...);
    LogRaw(buf);
}

static std::wstring GetModuleDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p); size_t i = s.find_last_of(L"\\/");
    return i == std::wstring::npos ? s : s.substr(0, i);
}
static bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------- 资源释放 ----------
static bool ExtractResource(int resId, const wchar_t* type, const std::wstring& outPath) {
    HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(resId), type);
    if (!hRes) return false;
    HGLOBAL hGlobal = LoadResource(g_hInst, hRes);
    if (!hGlobal) return false;
    DWORD size = SizeofResource(g_hInst, hRes);
    void* data = LockResource(hGlobal);
    if (!data || size == 0) return false;
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(hFile, data, size, &written, nullptr);
    CloseHandle(hFile);
    return written == size;
}
static bool ExtractDllTo(const std::wstring& dir, std::wstring& outPath) {
    outPath = dir + L"\\zhcn.dll";
    return ExtractResource(IDR_ZHCN_DLL, RT_RCDATA, outPath);
}

// ---------- 背景音乐 ----------
static void PlayBgm() {
    if (!g_musicOn) return;
    HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_BGM_WAV), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hGlobal = LoadResource(g_hInst, hRes);
    void* data = LockResource(hGlobal);
    PlaySoundW((LPCWSTR)data, nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP);
}
static void StopBgm() { PlaySoundW(nullptr, nullptr, SND_ASYNC); }

// ---------- 查找安装目录 ----------
static std::wstring FindInstallDir() {
    const wchar_t* paths[] = {
        L"D:\\Program Files\\010 Editor",
        L"C:\\Program Files\\010 Editor",
        L"C:\\Program Files (x86)\\010 Editor",
    };
    for (auto p : paths) {
        std::wstring d(p);
        if (FileExists(d + L"\\010Editor.exe")) return d;
    }
    const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\010 Editor",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\010 Editor",
    };
    for (auto k : keys) {
        HKEY hk;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH]; DWORD sz = sizeof(buf);
            if (RegQueryValueExW(hk, L"InstallLocation", nullptr, nullptr, (BYTE*)buf, &sz) == ERROR_SUCCESS) {
                RegCloseKey(hk);
                std::wstring d(buf);
                while (!d.empty() && d.back() == L'\\') d.pop_back();
                if (FileExists(d + L"\\010Editor.exe")) return d;
            } else RegCloseKey(hk);
        }
    }
    return L"";
}

// ---------- 注册表许可 ----------
static bool WriteLicense(const std::string& nameUtf8, const std::string& serial) {
    HKEY hk;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SweetScape\\010 Editor", 0, KEY_SET_VALUE, &hk);
    if (r != ERROR_SUCCESS)
        r = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SweetScape\\010 Editor", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wn(n); MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, wn.data(), n);
    int m = MultiByteToWideChar(CP_UTF8, 0, serial.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> ws(m); MultiByteToWideChar(CP_UTF8, 0, serial.c_str(), -1, ws.data(), m);
    RegSetValueExW(hk, L"Name", 0, REG_SZ, (BYTE*)wn.data(), n * sizeof(wchar_t));
    RegSetValueExW(hk, L"Password", 0, REG_SZ, (BYTE*)ws.data(), m * sizeof(wchar_t));
    RegSetValueExW(hk, L"Hidden", 0, REG_SZ, (BYTE*)L"false", 6 * sizeof(wchar_t));
    std::time_t now = std::time(nullptr);
    long long days = (now / 86400) + 2440588 + 1;
    wchar_t jd[32]; swprintf_s(jd, 32, L"%lld", days);
    RegSetValueExW(hk, L"LastDate", 0, REG_SZ, (BYTE*)jd, (DWORD)((wcslen(jd)+1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return true;
}
static bool ReadLicense(std::wstring& name, std::wstring& serial) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SweetScape\\010 Editor", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    auto rd = [&](const wchar_t* k, std::wstring& o) {
        wchar_t b[512]; DWORD sz = sizeof(b); o.clear();
        if (RegQueryValueExW(hk, k, nullptr, nullptr, (BYTE*)b, &sz) == ERROR_SUCCESS) o = b;
    };
    rd(L"Name", name); rd(L"Password", serial);
    RegCloseKey(hk);
    return !serial.empty();
}
static void KillAll() { system("taskkill /F /IM 010Editor.exe >nul 2>&1"); Sleep(300); }

// ---------- 内置注入启动器 ----------
static bool LaunchWithDll() {
    if (g_installDir.empty()) { Log(L"[ERROR] Install dir not found", L"[错误] 未找到安装目录"); return false; }
    KillAll();
    std::wstring dllPath;
    if (!ExtractDllTo(g_installDir, dllPath)) { Log(L"[ERROR] Failed to extract zhcn.dll", L"[错误] 释放 zhcn.dll 失败"); return false; }
    Log(L"[OK] zhcn.dll extracted", L"[OK] 已释放 zhcn.dll 到安装目录");
    std::wstring exe = g_installDir + L"\\010Editor.exe";
    if (!FileExists(exe)) { LogF(L"[ERROR] Not found: %s", L"[错误] 找不到: %s", exe.c_str()); return false; }
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = exe; std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, g_installDir.c_str(), &si, &pi)) {
        LogF(L"[ERROR] CreateProcess failed: %lu", L"[错误] 创建进程失败: %lu", GetLastError()); return false;
    }
    SIZE_T bytes = (dllPath.size()+1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) { TerminateProcess(pi.hProcess,1); Log(L"[ERROR] VirtualAllocEx failed", L"[错误] VirtualAllocEx 失败"); return false; }
    WriteProcessMemory(pi.hProcess, remote, dllPath.c_str(), bytes, nullptr);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoad = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(pi.hProcess, nullptr, 0, pLoad, remote, 0, nullptr);
    if (!th) { VirtualFreeEx(pi.hProcess,remote,0,MEM_RELEASE); TerminateProcess(pi.hProcess,1); Log(L"[ERROR] Inject failed", L"[错误] 注入失败"); return false; }
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    ResumeThread(pi.hThread);
    LogF(L"[OK] Started PID=%lu (Chinese patch injected)", L"[OK] 已启动 PID=%lu（汉化已注入）", pi.dwProcessId);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// ---------- 按钮处理 ----------
static void OnScan() {
    g_installDir = FindInstallDir();
    SetDlgItemTextW(g_hWnd, IDC_EDIT_PATH, g_installDir.c_str());
    if (!g_installDir.empty()) LogF(L"[OK] Install dir: %s", L"[OK] 找到安装目录: %s", g_installDir.c_str());
    else Log(L"[ERROR] Install dir not found, fill it manually", L"[错误] 未找到安装目录，请手动填写");
}
static void OnActivate() {
    wchar_t wn[256], wy[32];
    GetDlgItemTextW(g_hWnd, IDC_EDIT_NAME, wn, 256);
    GetDlgItemTextW(g_hWnd, IDC_EDIT_YEAR, wy, 32);
    int year = _wtoi(wy); if (year < 2026) year = 2050;
    int n = WideCharToMultiByte(CP_UTF8, 0, wn, -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> u8(n); WideCharToMultiByte(CP_UTF8, 0, wn, -1, u8.data(), n, nullptr, nullptr);
    std::string name(u8.data());
    int days = License::DaysForYear(year);
    std::string serial = License::GenSite(name, 500, days);
    LicenseInfo info; int st = License::Validate(name, serial, &info);
    LogF(L"Serial: %S", L"生成: %S", serial.c_str());
    LogF(L"Name=%s Expiry=%d-01-01 Verify=%d", L"用户名=%s 到期=%d-01-01 校验=%d", wn, year, st);
    if (WriteLicense(name, serial)) Log(L"[OK] Written to registry (effective after restart)", L"[OK] 已写入注册表（重启程序生效）");
    else Log(L"[ERROR] Failed to write registry", L"[错误] 写注册表失败");
}
static void OnDeploy() {
    if (g_installDir.empty()) { Log(L"[ERROR] Install dir not found", L"[错误] 未找到安装目录"); return; }
    KillAll();
    std::wstring dst;
    if (ExtractDllTo(g_installDir, dst)) LogF(L"[OK] zhcn.dll deployed to %s", L"[OK] zhcn.dll 已部署到 %s", dst.c_str());
    else Log(L"[ERROR] Deploy failed", L"[错误] 部署失败");
}
static void OnStatus() {
    std::wstring name, serial;
    if (!ReadLicense(name, serial)) { Log(L"Status: no license found", L"当前状态: 未找到许可证"); return; }
    LogF(L"License: %s / %s", L"授权: %s / %s", name.c_str(), serial.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> u(n); WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, u.data(), n, nullptr, nullptr);
    int m = WideCharToMultiByte(CP_UTF8, 0, serial.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> s(m); WideCharToMultiByte(CP_UTF8, 0, serial.c_str(), -1, s.data(), m, nullptr, nullptr);
    LicenseInfo info; int st = License::Validate(std::string(u.data()), std::string(s.data()), &info);
    LogF(L"Verify: status=%d users=%d num=%d supportDays=%d", L"校验: 状态=%d 用户=%d 编号=%d 支持天数=%d", st, info.users, info.licnum, info.supportDays);
}
static void ToggleMusic(HWND hChk) {
    g_musicOn = (SendMessageW(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (g_musicOn) { PlayBgm(); Log(L"[OK] BGM on", L"[OK] 背景音乐已开启"); }
    else { StopBgm(); Log(L"[OK] BGM off", L"[OK] 背景音乐已关闭"); }
}

// 刷新所有控件文本（切换语言时）
static void RefreshUI() {
    HICON hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (hIcon) { SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon); SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon); }
    SetWindowTextW(g_hWnd, T(L"010Forge v2.0 - One-Click Patcher (Single-File)", L"010Forge v2.0 - 一键补丁工具 (单文件)"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_SCAN,     T(L"Auto Find", L"自动查找"));
    SetDlgItemTextW(g_hWnd, IDC_EDIT_NAME,    L"Test User");
    SetDlgItemTextW(g_hWnd, IDC_BTN_ACTIVATE, T(L"1. Activate", L"1. 一键激活"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_DEPLOY,   T(L"2. Deploy CN Patch", L"2. 部署汉化"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_LAUNCH,   T(L"3. Launch (CN)", L"3. 启动(汉化)"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_STATUS,   T(L"Check Status", L"查看状态"));
    SetDlgItemTextW(g_hWnd, IDC_CHK_MUSIC,    T(L"BGM", L"背景音乐"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_LANG,     T(L"语言: EN", L"语言: 中文"));
    SetDlgItemTextW(g_hWnd, IDC_BTN_EXIT,     T(L"Exit", L"退出"));
    // 静态标签：用子窗口枚举按原文本识别较麻烦，这里重建时统一设置（通过查找子窗口）
    // 为简化，静态文本用位置固定，切换语言时直接遍历设置
    int i = 0;
    const wchar_t* enLabels[] = { L"010Forge - 010 Editor One-Click Patcher", L"Install Dir:", L"Username:", L"Expiry Year:", L"Tip: Single-file, DLL embedded; Activate writes registry, CN patch via injection", L"Log:" };
    const wchar_t* zhLabels[] = { L"010Forge - 010 Editor 一键补丁工具", L"安装目录:", L"用户名:", L"到期年份:", L"提示: 单文件版, DLL 已内嵌; 激活写注册表, 汉化靠注入", L"操作日志:" };
    HWND child = GetWindow(g_hWnd, GW_CHILD);
    while (child && i < 6) {
        wchar_t cls[64]; GetClassNameW(child, cls, 64);
        if (wcscmp(cls, L"Static") == 0) {
            SetWindowTextW(child, T(enLabels[i], zhLabels[i]));
            i++;
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
}
static void ToggleLanguage() {
    g_lang = g_lang ? 0 : 1;
    RefreshUI();
    Log(L"Language switched to English", L"语言已切换为中文");
}

// ---------- 窗口过程 ----------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = h;
        CreateWindowExW(0, L"Static", L"010Forge - 010 Editor One-Click Patcher", WS_CHILD|WS_VISIBLE|SS_CENTER, 20, 10, 560, 26, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(0, L"Static", L"", WS_CHILD|WS_VISIBLE, 20, 46, 80, 20, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 100, 44, 440, 22, h, (HMENU)IDC_EDIT_PATH, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 550, 43, 90, 24, h, (HMENU)IDC_BTN_SCAN, g_hInst, nullptr);
        CreateWindowExW(0, L"Static", L"", WS_CHILD|WS_VISIBLE, 20, 80, 80, 20, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"Test User", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 100, 78, 200, 22, h, (HMENU)IDC_EDIT_NAME, g_hInst, nullptr);
        CreateWindowExW(0, L"Static", L"", WS_CHILD|WS_VISIBLE, 320, 80, 80, 20, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"2050", WS_CHILD|WS_VISIBLE|ES_NUMBER, 400, 78, 60, 22, h, (HMENU)IDC_EDIT_YEAR, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_CHECKBOX|BS_AUTOCHECKBOX, 465, 78, 80, 24, h, (HMENU)IDC_CHK_MUSIC, g_hInst, nullptr);
        SendMessageW(GetDlgItem(h,IDC_CHK_MUSIC), BM_SETCHECK, BST_CHECKED, 0);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 550, 78, 90, 24, h, (HMENU)IDC_BTN_LANG, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 20, 114, 140, 30, h, (HMENU)IDC_BTN_ACTIVATE, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 172, 114, 140, 30, h, (HMENU)IDC_BTN_DEPLOY, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 324, 114, 140, 30, h, (HMENU)IDC_BTN_LAUNCH, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 476, 114, 160, 30, h, (HMENU)IDC_BTN_STATUS, g_hInst, nullptr);
        CreateWindowExW(0, L"Static", L"", WS_CHILD|WS_VISIBLE, 20, 154, 620, 20, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(0, L"Static", L"", WS_CHILD|WS_VISIBLE, 20, 184, 100, 20, h, nullptr, g_hInst, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"", WS_CHILD|WS_VISIBLE|LBS_STANDARD, 20, 206, 610, 240, h, (HMENU)IDC_LIST_LOG, g_hInst, nullptr);
        CreateWindowExW(0, L"Button", L"", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 550, 456, 80, 26, h, (HMENU)IDC_BTN_EXIT, g_hInst, nullptr);
        RefreshUI();
        OnScan();
        PlayBgm();
        Log(L"Ready. Steps: 1) Activate  2) Deploy  3) Launch  (BGM toggle above)",
            L"工具就绪。步骤: 1)一键激活  2)部署汉化  3)启动  (背景音乐可在上方关闭)");
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_SCAN:     OnScan(); break;
        case IDC_BTN_ACTIVATE: OnActivate(); break;
        case IDC_BTN_DEPLOY:   OnDeploy(); break;
        case IDC_BTN_LAUNCH:   LaunchWithDll(); break;
        case IDC_BTN_STATUS:   OnStatus(); break;
        case IDC_CHK_MUSIC:    ToggleMusic((HWND)lp); break;
        case IDC_BTN_LANG:     ToggleLanguage(); break;
        case IDC_BTN_EXIT:     DestroyWindow(h); break;
        }
        break;
    case WM_DESTROY: StopBgm(); PostQuitMessage(0); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd) {
    g_hInst = hInst;
    DetectLanguage();  // 按系统语言自动选择
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = L"010ForgeClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!RegisterClassW(&wc)) return 1;
    HWND h = CreateWindowExW(0, L"010ForgeClass", L"010Forge",
                             WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 660, 530, nullptr, nullptr, hInst, nullptr);
    if (!h) return 1;
    ShowWindow(h, nCmd); UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
