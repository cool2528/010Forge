// Inspect PE resources as data only; never execute the GUI or proxy.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "deployment.h"
#include "resource.h"
#include "license.h"

static int failures = 0;
static void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
static forge::Bytes Resource(HMODULE module, int id, LPCWSTR type) {
    const HRSRC r = FindResourceW(module, MAKEINTRESOURCEW(id), type);
    if (!r) return {};
    const auto loaded = LoadResource(module, r);
    const auto data = static_cast<const std::uint8_t*>(LockResource(loaded));
    return data ? forge::Bytes(data, data + SizeofResource(module, r)) : forge::Bytes{};
}
static unsigned Le32(const std::uint8_t* p) {
    return unsigned(p[0]) | unsigned(p[1])<<8 | unsigned(p[2])<<16 | unsigned(p[3])<<24;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return 3;
    forge::Bytes expected; DWORD error{};
    Check(forge::ReadBytes(argv[2], expected, error), "read built proxy as data");
    const auto proxy = Resource(module, IDR_QUAZIP_DLL, RT_RCDATA);
    Check(!proxy.empty() && proxy == expected, "embedded proxy bytes equal freshly built DLL");
    const auto music = Resource(module, IDR_BGM_WAV, RT_RCDATA);
    Check(music.size() > 44 && std::memcmp(music.data(), "RIFF", 4) == 0 &&
          std::memcmp(music.data()+8, "WAVE", 4) == 0, "BGM is embedded WAV");
    if (music.size() > 44) {
        Check(Le32(music.data()+24) == 22050, "BGM sample rate is 22050 Hz");
        Check(Le32(music.data()+40) == 846720, "original cyber loop is 19.2 seconds");
        Check(music[44] == 0 && music[45] == 0 && music[music.size()-1] == 0 && music[music.size()-2] == 0,
              "loop endpoints are silent (no boundary click)");
    }
    Check(!Resource(module, IDI_APP_ICON, RT_GROUP_ICON).empty(), "application icon group embedded");
    const auto manifest = Resource(module, 1, RT_MANIFEST);
    Check(!manifest.empty(), "DPI and common-controls manifest embedded");
    Check(!FindResourceW(module, MAKEINTRESOURCEW(100), RT_RCDATA), "standalone zhcn.dll is not embedded");
    FreeLibrary(module);
    // Regression only: the old C++ parser subtracted multi-character '55'/'87'.
    for (const int year : {2027, 2035, 2050, 2099}) {
        LicenseInfo info{};
        const int days = License::DaysForYear(year);
        const auto generated = License::GenSite("Test User", 500, days);
        Check(License::Validate("Test User", generated, &info) == 45 && info.supportDays == days,
              "existing codec parses its generated hexadecimal fields");
    }
    return failures ? 1 : 0;
}
