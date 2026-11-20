#pragma once
#include <windows.h>

namespace forge::ui {
enum class Language { English, Chinese };
inline Language FromLangId(LANGID id) {
    return PRIMARYLANGID(id) == LANG_CHINESE ? Language::Chinese : Language::English;
}
inline const wchar_t* Text(Language lang, const wchar_t* en, const wchar_t* zh) {
    return lang == Language::Chinese ? zh : en;
}
enum Control {
    InstallPath = 1004, UserName = 1001, ExpiryYear = 1002,
    Scan = 1101, Activate = 1102, Deploy = 1103, Launch = 1104,
    Status = 1105, Exit = 1106, Restore = 1107,
    Log = 1201, Music = 1401, SwitchLanguage = 1402,
    Title = 1501, PathLabel = 1502, NameLabel = 1503,
    YearLabel = 1504, Hint = 1505, LogLabel = 1506
};
struct Label { int id; const wchar_t* en; const wchar_t* zh; };
inline constexpr Label Labels[] = {
    {Title, L"010Forge - 010 Editor localization utility", L"010Forge - 010 Editor 一键补丁工具"},
    {PathLabel, L"Directory:", L"安装目录："},
    {NameLabel, L"User name:", L"用户名："},
    {YearLabel, L"Year:", L"到期年份："},
    {Scan, L"Auto find", L"自动查找"},
    {Activate, L"1. Activate", L"1. 一键激活"},
    {Deploy, L"2. Deploy", L"2. 部署汉化"},
    {Launch, L"3. Launch", L"3. 启动"},
    {Restore, L"4. Restore", L"4. 还原原版"},
    {Status, L"Check status", L"查看状态"},
    {Music, L"Music", L"背景音乐"},
    {SwitchLanguage, L"Language: EN", L"语言：中文"},
    {Exit, L"Exit", L"退出"},
    {Hint, L"Close 010 Editor before Deploy / Restore. Restore keeps the license and your settings.",
           L"部署或还原前请先关闭 010 Editor。还原只恢复原版 DLL，保留许可证和个人设置。"},
    {LogLabel, L"Operation log:", L"操作日志："}
};
} // namespace forge::ui
