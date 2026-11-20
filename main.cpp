// 010Forge v4.0: single-file Win32 UI, verified localization deployment.
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include "deployment.h"
#include "license.h"
#include "resource.h"
#include "ui_text.h"
#include "version.h"

namespace ui = forge::ui;
namespace fs = std::filesystem;
static HINSTANCE g_instance{};
static HWND g_window{};
static HFONT g_font{};
static UINT g_dpi = 96;
static bool g_music = true;
static ui::Language g_language = ui::Language::English;

static const wchar_t* T(const wchar_t* en, const wchar_t* zh) {
    return ui::Text(g_language, en, zh);
}
static void Log(const wchar_t* en, const wchar_t* zh) {
    const HWND list = GetDlgItem(g_window, ui::Log);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(en, zh)));
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    SendMessageW(list, LB_SETCURSEL, count - 1, 0);
}
template<class... A> static void LogF(const wchar_t* en, const wchar_t* zh, A... args) {
    wchar_t buffer[2048]{};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, T(en, zh), args...);
    Log(buffer, buffer);
}
static void LogError(DWORD error) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    LogF(L"[ERROR] Windows error %lu: %s", L"[错误] Windows 错误 %lu：%s", error, text ? text : L"");
    if (text) LocalFree(text);
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
        Log(L"Save your work and close 010 Editor, then retry. No process was terminated.",
            L"请保存工作并关闭 010 Editor 后重试。本工具未终止任何进程。");
    else if (error == ERROR_ACCESS_DENIED)
        Log(L"The directory is not writable. You may run this tool as administrator.",
            L"目录写入权限不足，可手动以管理员身份运行本工具。");
}
static std::wstring ControlText(int id) {
    const HWND h = GetDlgItem(g_window, id);
    const int n = GetWindowTextLengthW(h);
    std::vector<wchar_t> text(static_cast<size_t>(n) + 1);
    GetWindowTextW(h, text.data(), n + 1);
    return text.data();
}
static bool ReadTarget(fs::path& dir) {
    std::wstring text = ControlText(ui::InstallPath);
    const auto begin = text.find_first_not_of(L" \t\r\n");
    const auto end = text.find_last_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        Log(L"[ERROR] Enter the installation directory.", L"[错误] 请填写安装目录。");
        return false;
    }
    text = text.substr(begin, end - begin + 1);
    if (text.size() > 1 && text.front() == L'\"' && text.back() == L'\"')
        text = text.substr(1, text.size() - 2);
    dir = fs::path(text).lexically_normal();
    std::error_code ec;
    if (!dir.is_absolute() || !fs::is_directory(dir, ec)) {
        Log(L"[ERROR] Select an existing absolute directory.", L"[错误] 请填写已存在的绝对目录路径。");
        return false;
    }
    return true;
}
static forge::Bytes EmbeddedProxy() {
    const HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(IDR_QUAZIP_DLL), RT_RCDATA);
    if (!resource) return {};
    const HGLOBAL loaded = LoadResource(g_instance, resource);
    const auto data = static_cast<const std::uint8_t*>(LockResource(loaded));
    const DWORD size = SizeofResource(g_instance, resource);
    return data ? forge::Bytes(data, data + size) : forge::Bytes{};
}
static forge::Deployment MakeDeployment() {
    return forge::Deployment(EmbeddedProxy(), forge::SupportedPolicy());
}
static bool RegularFile(const fs::path& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
static std::wstring Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::vector<wchar_t> result(size);
    GetEnvironmentVariableW(name, result.data(), size);
    return result.data();
}
static void OnScan() {
    std::vector<fs::path> candidates = {L"D:\\Program Files\\010 Editor", L"C:\\Program Files\\010 Editor"};
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto value = Environment(var);
        if (!value.empty()) candidates.emplace_back(fs::path(value) / L"010 Editor");
    }
    // Only local, exact registry keys. No worker thread with a stack context.
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const wchar_t* key : {
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\010 Editor",
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\010 Editor_is1",
            L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\010 Editor_is1"}) {
            wchar_t value[32768]{};
            DWORD size = sizeof(value);
            if (RegGetValueW(root, key, L"InstallLocation", RRF_RT_REG_SZ,
                             nullptr, value, &size) == ERROR_SUCCESS)
                candidates.emplace_back(value);
        }
    }
    for (const auto& dir : candidates) {
        if (RegularFile(dir / L"010Editor.exe")) {
            SetDlgItemTextW(g_window, ui::InstallPath, dir.c_str());
            LogF(L"[OK] Found directory: %s", L"[OK] 找到安装目录：%s", dir.c_str());
            return;
        }
    }
    Log(L"[NOTE] Installation not found. Enter its directory above.",
        L"[提示] 未找到安装目录，请在上方手动填写。");
}
static void OnDeploy() {
    fs::path dir;
    if (!ReadTarget(dir)) return;
    const auto result = MakeDeployment().Install(dir);
    if (!result.ok) {
        LogError(result.error);
        if (result.changed)
            Log(L"The target changed during replacement. Check status and use Restore; the verified backup is retained.",
                L"替换期间目标状态发生变化，请检查状态后使用还原；校验后的原件备份仍保留。");
        return;
    }
    Log(result.changed ? L"[OK] Proxy installed; the verified original is kept in quazip_orig.dll."
                       : L"[OK] The same localization proxy is already installed.",
        result.changed ? L"[OK] 代理已安装，原件已校验并备份为 quazip_orig.dll。"
                       : L"[OK] 已安装相同汉化代理，无需重复替换。");
    Log(L"The desktop shortcut and direct launches now use the same localized installation.",
        L"此后桌面快捷方式、文件关联及直接启动都会加载同一汉化代理。");
}
static void OnRestore() {
    fs::path dir;
    if (!ReadTarget(dir)) return;
    const auto result = MakeDeployment().Restore(dir);
    if (!result.ok) {
        LogError(result.error);
        if (result.changed)
            Log(L"The target changed during replacement. Check status and use Restore; the verified backup is retained.",
                L"替换期间目标状态发生变化，请检查状态后使用还原；校验后的原件备份仍保留。");
        return;
    }
    Log(result.changed ? L"[OK] Original DLL restored; verified backup retained."
                       : L"[OK] Original DLL already present; nothing to restore.",
        result.changed ? L"[OK] 原版 DLL 已还原，校验后的备份仍保留。"
                       : L"[OK] 当前已是原版 DLL，无需还原。");
    Log(L"License and personal settings were not changed.", L"许可证和个人配置未改动。");
}
static void OnLaunch() {
    fs::path dir;
    if (!ReadTarget(dir)) return;
    const auto inspection = MakeDeployment().Inspect(dir);
    if (!MakeDeployment().HostSupported(dir) ||
        (inspection.state != forge::PatchState::Original &&
         inspection.state != forge::PatchState::Installed && inspection.state != forge::PatchState::OlderProxy)) {
        Log(L"[ERROR] Verify or restore the installation before launch.", L"[错误] 请先检查或还原安装目录，再启动。");
        LogError(inspection.error ? inspection.error : ERROR_REVISION_MISMATCH);
        return;
    }
    const auto exe = dir / L"010Editor.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW start{}; start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, dir.c_str(), &start, &process)) {
        LogError(GetLastError()); return;
    }
    LogF(L"[OK] Process created: PID %lu. Existing windows were not terminated.",
         L"[OK] 已创建进程：PID %lu。现有窗口未被终止。", process.dwProcessId);
    Log(inspection.state == forge::PatchState::Original ? L"Original DLL installed: English UI."
                                                       : L"Verified proxy installed. A fresh process will attempt to load it; verify the UI in 010 Editor.",
        inspection.state == forge::PatchState::Original ? L"当前安装原版 DLL，将使用英文界面。"
                                                       : L"已安装校验通过的代理，新进程将尝试自动加载；请在 010 Editor 内核验界面。");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
static std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
static bool WriteLicense(const std::wstring& name, const std::string& serial) {
    HKEY key{};
    DWORD error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SweetScape\\010 Editor", 0,
                    nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error) { LogError(error); return false; }
    const std::wstring code(serial.begin(), serial.end());
    SYSTEMTIME now{}; GetLocalTime(&now);
    const int a = (14 - now.wMonth) / 12;
    const int y = now.wYear + 4800 - a;
    const int m = now.wMonth + 12 * a - 3;
    const int julian = now.wDay + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
    const std::wstring date = std::to_wstring(julian);
    auto put = [&](const wchar_t* field, const std::wstring& value) {
        if (error == ERROR_SUCCESS)
            error = RegSetValueExW(key, field, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    put(L"Name", name); put(L"Password", code); put(L"Hidden", L"false"); put(L"LastDate", date);
    RegCloseKey(key);
    if (error) { LogError(error); return false; }
    return true;
}
static void OnActivate() {
    fs::path dir;
    if (!ReadTarget(dir)) return;
    if (!MakeDeployment().HostSupported(dir)) { LogError(ERROR_REVISION_MISMATCH); return; }
    const auto name = ControlText(ui::UserName);
    const auto yearText = ControlText(ui::ExpiryYear);
    wchar_t* end{};
    const long year = wcstol(yearText.c_str(), &end, 10);
    if (name.empty() || name.size() > 200 || end == yearText.c_str() || *end || year < 2027 || year > 2099) {
        Log(L"[ERROR] Enter a name and an expiry year between 2027 and 2099.",
            L"[错误] 请填写用户名及 2027—2099 之间的年份。"); return;
    }
    const auto utf8 = Utf8(name);
    const int days = License::DaysForYear(static_cast<int>(year));
    const std::string serial = License::GenSite(utf8, 500, days);
    LicenseInfo info{};
    const int state = License::Validate(utf8, serial, &info);
    if (state != 45 || info.supportDays != days) {
        Log(L"[ERROR] Local verification failed; registry was not changed.",
            L"[错误] 本地校验失败，注册表未改动。"); return;
    }
    if (WriteLicense(name, serial))
        Log(L"[OK] Registry values written. Reopen 010 Editor to verify its status.",
            L"[OK] 注册表写入完成，请重新打开 010 Editor 核验状态。");
}
static void OnStatus() {
    fs::path dir;
    if (!ReadTarget(dir)) return;
    const auto inspection = MakeDeployment().Inspect(dir);
    switch (inspection.state) {
    case forge::PatchState::Original: Log(L"Localization: original DLL (English UI).", L"汉化状态：原版 DLL（英文界面）。"); break;
    case forge::PatchState::Installed: Log(L"Localization: current verified proxy and original backup.", L"汉化状态：当前代理和原件备份均已校验。"); break;
    case forge::PatchState::OlderProxy: Log(L"Localization: recognized old proxy; update is available.", L"汉化状态：已识别旧代理，可更新部署。"); break;
    case forge::PatchState::Recoverable: Log(L"Localization: damaged/missing current DLL; verified backup can restore it.", L"汉化状态：当前 DLL 缺失或损坏，可由已校验备份恢复。"); break;
    case forge::PatchState::MissingBackup: Log(L"Localization: proxy found but original backup is missing.", L"汉化状态：发现代理，但原件备份缺失。"); break;
    default: Log(L"Localization: unsupported installation or conflicting files.", L"汉化状态：安装版本不匹配或文件存在冲突。"); break;
    }
    if (!inspection.currentSha256.empty())
        LogF(L"DLL SHA-256: %s", L"DLL SHA-256：%s", inspection.currentSha256.c_str());
    if (inspection.error) LogError(inspection.error);
    wchar_t name[256]{}; DWORD length = sizeof(name);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\SweetScape\\010 Editor", L"Name",
                    RRF_RT_REG_SZ, nullptr, name, &length) == ERROR_SUCCESS)
        LogF(L"Stored license name: %s (application status is verified in 010 Editor).",
             L"已存储的许可证名称：%s（最终状态请在 010 Editor 内核验）。", name);
}
static void Music(bool enabled) {
    g_music = enabled;
    if (!enabled) { PlaySoundW(nullptr, nullptr, 0); return; }
    const HRSRC resource = FindResourceW(g_instance, MAKEINTRESOURCEW(IDR_BGM_WAV), RT_RCDATA);
    if (!resource) return;
    const HGLOBAL loaded = LoadResource(g_instance, resource);
    const void* bytes = LockResource(loaded);
    if (bytes) PlaySoundW(reinterpret_cast<LPCWSTR>(bytes), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
}
static void RefreshText() {
    const std::wstring title = L"010Forge v" + std::wstring(FORGE_VERSION_W) +
                               T(L" - Localization utility", L" - 一键补丁工具");
    SetWindowTextW(g_window, title.c_str());
    for (const auto& label : ui::Labels)
        SetDlgItemTextW(g_window, label.id, T(label.en, label.zh));
    // User name, path and year are deliberately NOT reset by a language change.
}
static int Scale(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }
static HWND Control(const wchar_t* cls, int id, DWORD style, int x, int y, int width, int height,
                    const wchar_t* value = L"") {
    const DWORD extra = wcscmp(cls, L"EDIT") == 0 || wcscmp(cls, L"LISTBOX") == 0 ? WS_EX_CLIENTEDGE : 0;
    HWND h = CreateWindowExW(extra, cls, value, WS_CHILD | WS_VISIBLE | style,
             Scale(x), Scale(y), Scale(width), Scale(height), g_window,
             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return h;
}
static LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        g_window = h;
        g_dpi = GetDpiForWindow(h);
        g_font = CreateFontW(-MulDiv(10, static_cast<int>(g_dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                              FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Control(L"STATIC", ui::Title, SS_CENTER, 16, 14, 760, 28);
        Control(L"STATIC", ui::PathLabel, 0, 16, 59, 105, 22);
        Control(L"EDIT", ui::InstallPath, ES_AUTOHSCROLL | WS_TABSTOP, 124, 55, 518, 28);
        Control(L"BUTTON", ui::Scan, BS_PUSHBUTTON | WS_TABSTOP, 654, 55, 122, 28);
        Control(L"STATIC", ui::NameLabel, 0, 16, 98, 105, 22);
        Control(L"EDIT", ui::UserName, ES_AUTOHSCROLL | WS_TABSTOP, 124, 94, 205, 28, L"Test User");
        Control(L"STATIC", ui::YearLabel, 0, 346, 98, 95, 22);
        Control(L"EDIT", ui::ExpiryYear, ES_NUMBER | WS_TABSTOP, 445, 94, 70, 28, L"2050");
        Control(L"BUTTON", ui::Music, BS_AUTOCHECKBOX | WS_TABSTOP, 532, 94, 110, 28);
        SendDlgItemMessageW(h, ui::Music, BM_SETCHECK, g_music ? BST_CHECKED : BST_UNCHECKED, 0);
        Control(L"BUTTON", ui::SwitchLanguage, BS_PUSHBUTTON | WS_TABSTOP, 654, 94, 122, 28);
        for (const auto& button : {ui::Activate, ui::Deploy, ui::Launch, ui::Restore, ui::Status}) {
            const int index = button == ui::Activate ? 0 : button == ui::Deploy ? 1 : button == ui::Launch ? 2 : button == ui::Restore ? 3 : 4;
            Control(L"BUTTON", button, BS_PUSHBUTTON | WS_TABSTOP, 16 + index * 154, 138, 144, 36);
        }
        Control(L"STATIC", ui::Hint, 0, 16, 189, 760, 40);
        Control(L"STATIC", ui::LogLabel, 0, 16, 235, 180, 22);
        Control(L"LISTBOX", ui::Log, LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 16, 260, 760, 248);
        SendDlgItemMessageW(h, ui::Log, LB_SETHORIZONTALEXTENT, Scale(1600), 0);
        Control(L"BUTTON", ui::Exit, BS_PUSHBUTTON | WS_TABSTOP, 654, 522, 122, 32);
        RefreshText();
        PostMessageW(h, WM_APP + 1, 0, 0);
        return 0;
    case WM_APP + 1:
        OnScan(); Music(g_music);
        Log(L"Ready. Deploy / Restore never force-close another application.",
            L"工具就绪。部署和还原均不会强制关闭其他程序。");
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED) {
            switch (LOWORD(w)) {
            case ui::Scan: OnScan(); break;
            case ui::Activate: OnActivate(); break;
            case ui::Deploy: OnDeploy(); break;
            case ui::Launch: OnLaunch(); break;
            case ui::Restore: OnRestore(); break;
            case ui::Status: OnStatus(); break;
            case ui::SwitchLanguage:
                g_language = g_language == ui::Language::Chinese ? ui::Language::English : ui::Language::Chinese;
                RefreshText(); break;
            case ui::Music: Music(SendDlgItemMessageW(h, ui::Music, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
            case ui::Exit: DestroyWindow(h); break;
            }
        }
        return 0;
    case WM_DESTROY:
        Music(false); DeleteObject(g_font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    g_language = ui::FromLangId(GetUserDefaultUILanguage());
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = instance;
    cls.lpszClassName = L"010ForgeClass"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) return 1;
    g_dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, Scale(792), Scale(574)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, g_dpi);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"010Forge", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG msg{}; int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return result < 0 ? 1 : static_cast<int>(msg.wParam);
}
