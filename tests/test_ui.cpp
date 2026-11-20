// No visible windows: checks pure label IDs/language mapping used by RefreshText.
#include "ui_text.h"
#include <cstdio>
#include <set>
#include <cwchar>
int main() {
    namespace ui = forge::ui;
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failed;
    };
    check(ui::FromLangId(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)) == ui::Language::Chinese, "simplified Chinese OS");
    check(ui::FromLangId(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)) == ui::Language::Chinese, "traditional Chinese OS selects available Chinese UI");
    check(ui::FromLangId(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) == ui::Language::English, "English OS");
    check(ui::FromLangId(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)) == ui::Language::English, "other OS falls back to English");
    std::set<int> ids;
    for (const auto& label : ui::Labels) {
        check(ids.insert(label.id).second, "label ID is unique");
        check(label.en && *label.en && label.zh && *label.zh, "both label translations populated");
        check(label.id != ui::InstallPath && label.id != ui::UserName && label.id != ui::ExpiryYear,
              "language refresh never targets user input");
        check(std::wcscmp(ui::Text(ui::Language::Chinese, label.en, label.zh), label.zh) == 0 &&
              std::wcscmp(ui::Text(ui::Language::English, label.en, label.zh), label.en) == 0,
              "EN / ZH switches use stable ID mapping");
    }
    check(ids.count(ui::Restore) == 1, "restore button translated");
    check(ids.count(ui::Deploy) == 1 && ids.count(ui::Launch) == 1 && ids.count(ui::Status) == 1, "all action buttons translated");
    return failed ? 1 : 0;
}
