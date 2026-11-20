// C++17 console helper: capture only 010Forge's --preview windows.
// Usage: capture_ui.exe "C:\path\010Forge.exe" "C:\path\output"
// MSVC: cl /std:c++17 /EHsc /W4 /utf-8 tools\capture_ui.cpp /link user32.lib gdi32.lib
// The target must implement --preview (no installation/registry access, writes,
// application launches, or audio) and --lang=en/zh before this helper is run.
// Every HWND message below is restricted to the process created by this helper.
// No input desktop is opened/switched; no foreground/input or screen-copy API is used.
// References:
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createdesktopw
// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-printwindow
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include "../ui_text.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {
namespace fs = std::filesystem;
namespace ui = forge::ui;
constexpr wchar_t kWindowClass[] = L"010ForgeClass";
constexpr DWORD kIdleTimeoutMs = 15000;
constexpr DWORD kWindowTimeoutMs = 15000;
constexpr DWORD kMessageTimeoutMs = 2000;
constexpr DWORD kPrintTimeoutMs = 10000;
constexpr DWORD kShutdownTimeoutMs = 5000;
constexpr DWORD kTerminationTimeoutMs = 5000;
constexpr DWORD kWorkerCleanupTimeoutMs = 10000;
constexpr UINT kTimedOutExitCode = 0xE0100001u;
constexpr UINT kFailedExitCode = 0xE0100002u;
constexpr std::uint64_t kMaxBitmapBytes = 256ull * 1024 * 1024;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return "[invalid UTF-16]";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string ErrorDescription(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = count && buffer ? std::wstring(buffer, count) : L"";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return "win32=" + std::to_string(error) + (text.empty() ? "" : " (" + Utf8(text) + ")");
}
[[noreturn]] void Win32Failure(const char* operation, DWORD error = GetLastError()) {
    throw std::runtime_error(std::string(operation) + ": " + ErrorDescription(error));
}
[[noreturn]] void Timeout(const char* operation, DWORD milliseconds) {
    throw std::runtime_error(std::string(operation) + ": timeout_ms=" +
        std::to_string(milliseconds) + "; " + ErrorDescription(ERROR_TIMEOUT));
}

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{};
};
class Desktop {
public:
    ~Desktop() { if (handle) CloseDesktop(handle); }
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    Desktop() = default;
    void close() {
        if (handle && !CloseDesktop(handle)) Win32Failure("CloseDesktop");
        handle = nullptr;
    }
    HDESK handle{};
    std::wstring name;
};
struct Surface {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* pixels{};
    BITMAPINFO info{};
    DWORD byteCount{};
    LONG width{}, height{};
    ~Surface() {
        if (previous && previous != HGDI_ERROR && dc) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
};

// Standard Windows argv quoting, including trailing backslashes. Quote all args.
std::wstring Quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(ch);
        } else {
            result.append(slashes, L'\\');
            result.push_back(ch);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
void CreatePrivateDesktop(Desktop& desktop, const wchar_t* language) {
    static std::atomic<unsigned long> sequence{0};
    // PID + boot ticks + high-resolution counter + monotonic sequence prevents
    // parallel invocations or consecutive languages from sharing a desktop.
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter)) Win32Failure("QueryPerformanceCounter");
        desktop.name = L"010ForgeCapture_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter.QuadPart) + L"_" +
            std::to_wstring(++sequence) + L"_" + language;
        // No DESKTOP_SWITCHDESKTOP or hook rights are requested.
        SetLastError(ERROR_SUCCESS);
        desktop.handle = CreateDesktopW(desktop.name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE,
            nullptr);
        const DWORD error = GetLastError();
        if (!desktop.handle) Win32Failure("CreateDesktopW", error);
        if (error != ERROR_ALREADY_EXISTS) return;
        desktop.close();
    }
    throw std::runtime_error("CreateDesktopW: unique desktop name attempts exhausted");
}

struct Session {
    HDESK desktop{}; // Borrowed from Desktop; closed only after this worker exits.
    std::wstring language;
    fs::path output;
    Handle process;
    DWORD pid{};
    HWND window{};
    std::mutex phaseMutex;
    const char* phase = "worker-start";
    ULONGLONG deadline{};
    DWORD phaseBudget{};
    std::atomic<bool> cancelled{false};
    bool ok = false; // Read by the supervisor only after the thread is signaled.
    std::string error;
    DWORD exitCode = STILL_ACTIVE;
    LONG width{}, height{};

    void begin(const char* nextPhase, DWORD budget) {
        if (cancelled.load()) throw std::runtime_error("supervisor timeout; owned preview was stopped");
        std::lock_guard<std::mutex> lock(phaseMutex);
        phase = nextPhase;
        phaseBudget = budget;
        deadline = GetTickCount64() + budget;
    }
    void checkAlive(const char* operation) const {
        if (cancelled.load()) throw std::runtime_error("supervisor timeout; owned preview was stopped");
        const DWORD result = WaitForSingleObject(process.get(), 0);
        if (result == WAIT_FAILED) Win32Failure("WaitForSingleObject(preview)");
        if (result == WAIT_OBJECT_0) {
            DWORD code{};
            if (!GetExitCodeProcess(process.get(), &code)) Win32Failure("GetExitCodeProcess");
            throw std::runtime_error(std::string(operation) + ": preview exited; pid=" +
                std::to_string(pid) + "; exit_code=" + std::to_string(code));
        }
    }
    void requireOwned(HWND target) const {
        checkAlive("validate HWND ownership");
        DWORD owner{};
        if (!target || !GetWindowThreadProcessId(target, &owner) || owner != pid ||
            (target != window && GetAncestor(target, GA_ROOT) != window))
            throw std::runtime_error("HWND ownership assertion failed; no message sent");
    }
    DWORD_PTR send(HWND target, UINT message, WPARAM w = 0, LPARAM l = 0) const {
        requireOwned(target);
        DWORD_PTR result{};
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(target, message, w, l,
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, kMessageTimeoutMs, &result)) {
            const DWORD code = GetLastError();
            throw std::runtime_error("SendMessageTimeoutW: message=" + std::to_string(message) +
                "; timeout_ms=" + std::to_string(kMessageTimeoutMs) + "; " + ErrorDescription(code) +
                (code == ERROR_SUCCESS ? "; API reported an unspecified failure or timeout" : ""));
        }
        return result;
    }
    HWND control(int id) const {
        const HWND result = GetDlgItem(window, id);
        if (!result) throw std::runtime_error("missing preview control; id=" + std::to_string(id));
        requireOwned(result);
        return result;
    }
    std::wstring text(int id) const {
        const HWND target = control(id);
        const DWORD_PTR length = send(target, WM_GETTEXTLENGTH);
        if (length > 16384) throw std::runtime_error("unexpected control text length; id=" + std::to_string(id));
        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 2, L'\0');
        const DWORD_PTR copied = send(target, WM_GETTEXT, buffer.size(), reinterpret_cast<LPARAM>(buffer.data()));
        if (copied >= buffer.size()) throw std::runtime_error("WM_GETTEXT returned invalid length");
        return std::wstring(buffer.data(), static_cast<size_t>(copied));
    }
    void setText(int id, const std::wstring& value) const {
        if (!send(control(id), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())))
            throw std::runtime_error("WM_SETTEXT failed; id=" + std::to_string(id));
    }
};
struct WindowSearch { DWORD pid{}; HWND window{}; unsigned count{}; };
BOOL CALLBACK FindPreviewWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != search.pid) return TRUE; // Do not inspect any other process's windows.
    wchar_t name[256]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        std::wcscmp(name, kWindowClass) == 0 && IsWindowVisible(window)) {
        search.window = window;
        ++search.count;
    }
    return TRUE;
}
HWND WaitForPreviewWindow(Session& session) {
    const ULONGLONG deadline = GetTickCount64() + kWindowTimeoutMs;
    do {
        session.checkAlive("EnumDesktopWindows");
        WindowSearch search{session.pid};
        SetLastError(ERROR_SUCCESS);
        if (!EnumDesktopWindows(session.desktop, FindPreviewWindow, reinterpret_cast<LPARAM>(&search)))
            Win32Failure("EnumDesktopWindows");
        if (search.count > 1) throw std::runtime_error("multiple visible 010ForgeClass windows for owned preview PID");
        if (search.window) return search.window;
        const DWORD result = WaitForSingleObject(session.process.get(), 50);
        if (result == WAIT_FAILED) Win32Failure("WaitForSingleObject(window readiness)");
    } while (GetTickCount64() < deadline);
    Timeout("EnumDesktopWindows(pid + 010ForgeClass)", kWindowTimeoutMs);
}
void AssertLabels(const Session& session, bool chinese) {
    for (const auto& label : ui::Labels) {
        const std::wstring expected = chinese ? label.zh : label.en;
        const std::wstring actual = session.text(label.id);
        if (actual != expected)
            throw std::runtime_error("WM_GETTEXT label assertion; id=" + std::to_string(label.id) +
                "; expected=" + Utf8(expected) + "; actual=" + Utf8(actual));
    }
    for (const int id : {ui::Activate, ui::Deploy, ui::Launch, ui::Restore, ui::Status}) {
        wchar_t name[64]{};
        if (!GetClassNameW(session.control(id), name, static_cast<int>(std::size(name))) ||
            _wcsicmp(name, L"Button") != 0)
            throw std::runtime_error("action is not a BUTTON; id=" + std::to_string(id));
    }
}
void AssertLanguagePreservesInputs(Session& session, bool chinese) {
    struct Input { int id; std::wstring original; std::wstring changed; };
    std::array<Input, 3> inputs{{
        {ui::UserName, session.text(ui::UserName), L"Preview Capture \u6D4B\u8BD5"},
        {ui::InstallPath, session.text(ui::InstallPath), L"C:\\Preview Fixture\\010 Editor"},
        {ui::ExpiryYear, session.text(ui::ExpiryYear), L"2091"}
    }};
    for (auto& input : inputs) {
        if (input.changed == input.original)
            input.changed = input.id == ui::ExpiryYear ? L"2092" : input.changed + L" 2";
        session.setText(input.id, input.changed);
    }
    const auto assertInputs = [&](bool original) {
        for (const auto& input : inputs) {
            if (session.text(input.id) != (original ? input.original : input.changed))
                throw std::runtime_error("language/input preservation assertion; id=" + std::to_string(input.id));
        }
    };
    assertInputs(false);
    // Only the preview's language button is clicked; no action button is invoked.
    session.send(session.control(ui::SwitchLanguage), BM_CLICK);
    AssertLabels(session, !chinese);
    assertInputs(false);
    session.send(session.control(ui::SwitchLanguage), BM_CLICK);
    AssertLabels(session, chinese);
    assertInputs(false);
    // Restore actual preview startup defaults, rather than assuming a real path
    // or manufacturing an installation/license status for the screenshot.
    for (const auto& input : inputs) session.setText(input.id, input.original);
    assertInputs(true);
    AssertLabels(session, chinese);
}

std::string RectDescription(const RECT& rect) {
    return "[" + std::to_string(rect.left) + "," + std::to_string(rect.top) + "," +
        std::to_string(rect.right) + "," + std::to_string(rect.bottom) + "]";
}
RECT ControlRectInClient(const Session& session, int id) {
    const HWND child = session.control(id);
    RECT rect{};
    if (!GetWindowRect(child, &rect)) Win32Failure("GetWindowRect(preview control)");
    // GetWindowRect produces screen coordinates on our private desktop. Map
    // one RECT (exactly two points) at a time, including RTL RECT normalization.
    // A zero displacement is valid, so only a nonzero last-error means failure.
    SetLastError(ERROR_SUCCESS);
    const int mapped = MapWindowPoints(nullptr, session.window, reinterpret_cast<POINT*>(&rect), 2);
    const DWORD error = GetLastError();
    if (!mapped && error != ERROR_SUCCESS) Win32Failure("MapWindowPoints(preview control -> client)", error);
    return rect;
}
void AssertLayout(const Session& session) {
    session.requireOwned(session.window);
    RECT client{};
    if (!GetClientRect(session.window, &client)) Win32Failure("GetClientRect(owned preview)");
    if (client.right <= client.left || client.bottom <= client.top)
        throw std::runtime_error("invalid preview client rectangle=" + RectDescription(client));
    std::vector<int> ids;
    ids.reserve(std::size(ui::Labels) + 4);
    for (const auto& label : ui::Labels) ids.push_back(label.id);
    for (const int id : {ui::InstallPath, ui::UserName, ui::ExpiryYear, ui::Log}) ids.push_back(id);
    for (const int id : ids) {
        const RECT rect = ControlRectInClient(session, id);
        if (rect.right <= rect.left || rect.bottom <= rect.top ||
            rect.left < client.left || rect.top < client.top ||
            rect.right > client.right || rect.bottom > client.bottom)
            throw std::runtime_error("preview control outside client or empty; id=" + std::to_string(id) +
                "; control=" + RectDescription(rect) + "; client=" + RectDescription(client));
    }
    const RECT first = ControlRectInClient(session, ui::Activate);
    for (const int id : {ui::Deploy, ui::Launch, ui::Restore, ui::Status}) {
        const RECT rect = ControlRectInClient(session, id);
        if (rect.top != first.top || rect.right - rect.left != first.right - first.left ||
            rect.bottom - rect.top != first.bottom - first.top)
            throw std::runtime_error("preview action row requires equal y/width/height; id=" +
                std::to_string(id) + "; action=" + RectDescription(rect) +
                "; reference=" + RectDescription(first));
    }
}

void PrepareSurface(const Session& session, Surface& surface) {
    session.requireOwned(session.window);
    RECT rect{};
    if (!GetWindowRect(session.window, &rect)) Win32Failure("GetWindowRect(owned preview)");
    surface.width = rect.right - rect.left;
    surface.height = rect.bottom - rect.top;
    if (surface.width <= 0 || surface.height <= 0 || surface.width > 16384 || surface.height > 16384)
        throw std::runtime_error("invalid preview window dimensions");
    const std::uint64_t bytes = static_cast<std::uint64_t>(surface.width) * surface.height * 4;
    if (bytes > kMaxBitmapBytes || bytes > std::numeric_limits<DWORD>::max() - 54ull)
        throw std::runtime_error("preview bitmap exceeds the 256 MiB capture budget");
    surface.byteCount = static_cast<DWORD>(bytes);
    auto& header = surface.info.bmiHeader;
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = surface.width;
    header.biHeight = -surface.height; // Top-down 32-bit BI_RGB; rows need no padding.
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = surface.byteCount;
    // This is only a memory DC: no GetDC(NULL), desktop pixels, or screen blit.
    surface.dc = CreateCompatibleDC(nullptr);
    if (!surface.dc) Win32Failure("CreateCompatibleDC(memory)");
    surface.bitmap = CreateDIBSection(surface.dc, &surface.info, DIB_RGB_COLORS,
        &surface.pixels, nullptr, 0);
    if (!surface.bitmap || !surface.pixels) Win32Failure("CreateDIBSection");
    surface.previous = SelectObject(surface.dc, surface.bitmap);
    if (!surface.previous || surface.previous == HGDI_ERROR) Win32Failure("SelectObject(DIB)");
    std::memset(surface.pixels, 0xFF, surface.byteCount);
}
void WriteAll(HANDLE file, const void* source, DWORD length) {
    const auto bytes = static_cast<const BYTE*>(source);
    DWORD offset = 0;
    while (offset < length) {
        DWORD written{};
        if (!WriteFile(file, bytes + offset, length - offset, &written, nullptr)) Win32Failure("WriteFile(BMP)");
        if (!written) Win32Failure("WriteFile(BMP zero progress)", ERROR_WRITE_FAULT);
        offset += written;
    }
}
void WriteBitmap(const fs::path& output, const Surface& surface) {
    static_assert(sizeof(BITMAPFILEHEADER) == 14, "BMP requires its packed 14-byte header");
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + surface.byteCount;
    const fs::path temporary = output.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    struct RemoveTemporary {
        fs::path path;
        bool created = false;
        ~RemoveTemporary() { if (created) DeleteFileW(path.c_str()); }
    } remove{temporary};
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) Win32Failure("CreateFileW(temporary BMP)");
    remove.created = true;
    WriteAll(file.get(), &fileHeader, sizeof(fileHeader));
    WriteAll(file.get(), &surface.info.bmiHeader, sizeof(BITMAPINFOHEADER));
    WriteAll(file.get(), surface.pixels, surface.byteCount);
    if (!FlushFileBuffers(file.get())) Win32Failure("FlushFileBuffers(BMP)");
    file.reset();
    if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        Win32Failure("MoveFileExW(completed BMP)");
    remove.created = false;
}
void Capture(Session& session) {
    session.begin("assert-preview-client-layout", 5000);
    AssertLayout(session); // Fail before rendering if DPI/layout clips any control.
    Surface surface;
    session.begin("prepare-memory-DIB", 5000);
    PrepareSurface(session, surface);
    // PrintWindow is synchronous. The supervisor watches this exact phase and
    // ends only this owned preview process if its WM_PRINT handler times out.
    session.begin("PrintWindow", kPrintTimeoutMs);
    session.requireOwned(session.window);
    SetLastError(ERROR_SUCCESS);
    if (!PrintWindow(session.window, surface.dc, PW_RENDERFULLCONTENT)) Win32Failure("PrintWindow");
    if (!GdiFlush()) Win32Failure("GdiFlush");
    session.checkAlive("PrintWindow completion");
    const auto pixels = static_cast<const DWORD*>(surface.pixels);
    const size_t count = surface.byteCount / sizeof(DWORD);
    const DWORD first = pixels[0] & 0x00FFFFFFu;
    bool varied = false;
    for (size_t i = 1; i < count; ++i) {
        if ((pixels[i] & 0x00FFFFFFu) != first) { varied = true; break; }
    }
    if (!varied) throw std::runtime_error("PrintWindow returned a uniform/blank image; screenshot not published");
    session.begin("write-BMP", 10000);
    WriteBitmap(session.output, surface);
    session.width = surface.width;
    session.height = surface.height;
}

// The handle was returned by this helper's one explicit CreateProcessW call.
// Never search by process name, open arbitrary PIDs, kill a tree, or use taskkill.
void TerminateOwned(Session& session, UINT exitCode) {
    const DWORD state = WaitForSingleObject(session.process.get(), 0);
    if (state == WAIT_FAILED) Win32Failure("WaitForSingleObject(before owned termination)");
    if (state != WAIT_OBJECT_0 && !TerminateProcess(session.process.get(), exitCode)) {
        const DWORD error = GetLastError();
        if (WaitForSingleObject(session.process.get(), 0) != WAIT_OBJECT_0)
            Win32Failure("TerminateProcess(owned preview)", error);
    }
    const DWORD result = WaitForSingleObject(session.process.get(), kTerminationTimeoutMs);
    if (result == WAIT_TIMEOUT) Timeout("owned preview termination", kTerminationTimeoutMs);
    if (result != WAIT_OBJECT_0) Win32Failure("WaitForSingleObject(owned termination)");
}
void ClosePreview(Session& session) {
    const DWORD state = WaitForSingleObject(session.process.get(), 0);
    if (state == WAIT_FAILED) Win32Failure("WaitForSingleObject(before WM_CLOSE)");
    if (state != WAIT_OBJECT_0) {
        if (!session.window) {
            TerminateOwned(session, kFailedExitCode);
            return;
        }
        session.requireOwned(session.window);
        if (!PostMessageW(session.window, WM_CLOSE, 0, 0)) Win32Failure("PostMessageW(WM_CLOSE)");
        const DWORD result = WaitForSingleObject(session.process.get(), kShutdownTimeoutMs);
        if (result == WAIT_TIMEOUT) {
            TerminateOwned(session, kTimedOutExitCode);
            Timeout("WM_CLOSE process exit", kShutdownTimeoutMs);
        }
        if (result != WAIT_OBJECT_0) Win32Failure("WaitForSingleObject(WM_CLOSE)");
    }
    if (!GetExitCodeProcess(session.process.get(), &session.exitCode)) Win32Failure("GetExitCodeProcess");
}
unsigned __stdcall DesktopWorker(void* parameter) {
    auto& session = *static_cast<Session*>(parameter);
    try {
        // A fresh thread has no HWNDs or hooks. Its desktop association ends
        // with the thread, before the supervisor calls CloseDesktop.
        session.begin("SetThreadDesktop", 5000);
        if (!SetThreadDesktop(session.desktop)) Win32Failure("SetThreadDesktop(private worker)");
        // PrintWindow renders physical pixels. Its DIB must use physical bounds,
        // not GetWindowRect's DPI-virtualized coordinates for an unaware caller.
        if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            Win32Failure("SetThreadDpiAwarenessContext(private capture worker)");
        session.begin("WaitForInputIdle", kIdleTimeoutMs + 500);
        const DWORD idle = WaitForInputIdle(session.process.get(), kIdleTimeoutMs);
        if (idle == WAIT_TIMEOUT) Timeout("WaitForInputIdle", kIdleTimeoutMs);
        if (idle == WAIT_FAILED) Win32Failure("WaitForInputIdle");
        if (idle != 0) throw std::runtime_error("WaitForInputIdle: unexpected return=" + std::to_string(idle));
        session.begin("EnumDesktopWindows", kWindowTimeoutMs + 500);
        session.window = WaitForPreviewWindow(session);
        session.send(session.window, WM_NULL); // Bounded responsiveness barrier.
        const bool chinese = session.language == L"zh";
        session.begin("assert-preview-labels", 15000);
        AssertLabels(session, chinese);
        session.begin("assert-language-input-roundtrip", 15000);
        AssertLanguagePreservesInputs(session, chinese);
        session.send(session.window, WM_NULL);
        Capture(session);
        session.begin("WM_CLOSE", kShutdownTimeoutMs + kTerminationTimeoutMs + 500);
        ClosePreview(session);
        if (session.exitCode != 0)
            throw std::runtime_error("preview normal exit_code=" + std::to_string(session.exitCode));
        session.ok = true;
    } catch (const std::exception& error) {
        session.error = error.what();
    } catch (...) {
        session.error = "unexpected C++ exception in private desktop worker";
    }
    if (!session.ok) {
        try {
            if (session.cancelled.load()) TerminateOwned(session, kTimedOutExitCode);
            else {
                session.begin("failure-cleanup-WM_CLOSE", kShutdownTimeoutMs + kTerminationTimeoutMs + 500);
                ClosePreview(session);
            }
        } catch (const std::exception& cleanup) {
            session.error += "; cleanup: ";
            session.error += cleanup.what();
            try { TerminateOwned(session, kFailedExitCode); }
            catch (const std::exception& stop) { session.error += "; "; session.error += stop.what(); }
        }
    }
    return 0;
}

void RunLanguage(const fs::path& executable, const fs::path& outputDirectory, const wchar_t* language) {
    Desktop desktop;
    CreatePrivateDesktop(desktop, language);
    Session session;
    session.desktop = desktop.handle;
    session.language = language;
    session.output = outputDirectory / (L"gui-tool-" + session.language + L".bmp");
    std::wstring command = Quote(executable.wstring()) + L" " + Quote(L"--preview") +
        L" " + Quote(L"--lang=" + session.language);
    if (command.size() >= 32767) throw std::runtime_error("preview command line exceeds CreateProcessW limit");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktop.name.data();
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL; // Visible only on the new, never-switched desktop.
    PROCESS_INFORMATION process{};
    const fs::path workingDirectory = executable.parent_path();
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr,
            workingDirectory.c_str(), &startup, &process)) Win32Failure("CreateProcessW(--preview)");
    session.process.reset(process.hProcess);
    session.pid = process.dwProcessId;
    Handle initialThread(process.hThread);
    initialThread.reset();
    std::cout << "PREVIEW lang=" << Utf8(session.language) << " pid=" << session.pid
              << " desktop=" << Utf8(desktop.name) << "; installation/license state not examined\n";
    session.begin("worker-start", 5000);
    const uintptr_t thread = _beginthreadex(nullptr, 0, DesktopWorker, &session, 0, nullptr);
    if (!thread) {
        const int error = errno;
        TerminateOwned(session, kFailedExitCode);
        throw std::runtime_error("_beginthreadex: errno=" + std::to_string(error));
    }
    Handle worker(reinterpret_cast<HANDLE>(thread));
    std::string watchdogError;
    while (true) {
        const DWORD wait = WaitForSingleObject(worker.get(), 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            watchdogError = "WaitForSingleObject(worker): " + ErrorDescription(GetLastError());
        } else {
            std::lock_guard<std::mutex> lock(session.phaseMutex);
            if (GetTickCount64() >= session.deadline)
                watchdogError = std::string(session.phase) + ": supervisor timeout_ms=" +
                    std::to_string(session.phaseBudget) + "; " + ErrorDescription(ERROR_TIMEOUT);
        }
        if (watchdogError.empty()) continue;
        session.cancelled.store(true);
        std::cerr << "ERROR lang=" << Utf8(session.language) << " pid=" << session.pid
                  << " " << watchdogError << '\n';
        try { TerminateOwned(session, kTimedOutExitCode); }
        catch (const std::exception& error) { watchdogError += "; "; watchdogError += error.what(); }
        const DWORD drained = WaitForSingleObject(worker.get(), kWorkerCleanupTimeoutMs);
        if (drained != WAIT_OBJECT_0) {
            // A pathological GDI/driver stall must not strand this helper in an
            // unbounded join. Preview termination was requested only via our
            // owned handle; ending this helper releases all its remaining GDI,
            // thread and desktop handles through process teardown.
            std::cerr << "FATAL private worker did not drain after owned preview termination; "
                      << watchdogError << "; wait_result=" << drained << '\n';
            std::cerr.flush();
            std::cout.flush();
            ExitProcess(4);
        }
        break;
    }
    worker.reset(); // DesktopWorker has exited and released its desktop association.
    desktop.close();
    if (!watchdogError.empty()) throw std::runtime_error(watchdogError);
    if (!session.ok) throw std::runtime_error("lang=" + Utf8(session.language) + "; " + session.error);
    std::cout << "PASS lang=" << Utf8(session.language) << " actions=5 labels=" << std::size(ui::Labels)
              << " input_roundtrip=preserved defaults=restored client_layout=contained action_row=equal dimensions=" << session.width << "x" << session.height
              << " preview_exit=" << session.exitCode << " desktop_closed=yes bmp=" << Utf8(session.output.wstring()) << '\n';
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "Usage: capture_ui.exe <010Forge.exe> <outputDir>\n";
        return 2;
    }
    // Error dialogs belong neither on the user's desktop nor in a stalled run.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        const fs::path executable = fs::canonical(fs::path(argv[1]));
        if (!fs::is_regular_file(executable)) throw std::runtime_error("exePath is not a regular file");
        if (_wcsicmp(executable.filename().c_str(), L"010Forge.exe") != 0)
            throw std::runtime_error("exePath must name 010Forge.exe (only the preview helper target is accepted)");
        const fs::path outputDirectory = fs::absolute(fs::path(argv[2])).lexically_normal();
        fs::create_directories(outputDirectory);
        if (!fs::is_directory(outputDirectory)) throw std::runtime_error("outputDir is not a directory");
        for (const wchar_t* language : {L"en", L"zh"}) RunLanguage(executable, outputDirectory, language);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n';
        return 1;
    }
}
