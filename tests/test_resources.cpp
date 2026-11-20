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
static unsigned Le16(const std::uint8_t* p) {
    return unsigned(p[0]) | unsigned(p[1])<<8;
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
        // round(8 bars * 4 beats * 60 / 168 BPM * 22050 Hz) = 252000 frames.
        constexpr unsigned frames = 252000;
        constexpr unsigned channels = 2;
        constexpr unsigned sampleBytes = 2;
        constexpr unsigned frameBytes = channels * sampleBytes;
        constexpr unsigned pcmBytes = frames * frameBytes; // 1008000 bytes.
        Check(std::memcmp(music.data()+12, "fmt ", 4) == 0 && Le32(music.data()+16) == 16 &&
              Le16(music.data()+20) == 1, "BGM format is canonical PCM WAV");
        Check(Le16(music.data()+22) == channels, "BGM has two stereo channels");
        Check(Le32(music.data()+24) == 22050, "BGM sample rate is 22050 Hz");
        Check(Le16(music.data()+34) == sampleBytes * 8, "BGM is PCM16");
        Check(Le16(music.data()+32) == frameBytes && Le32(music.data()+28) == 22050 * frameBytes,
              "BGM block alignment and byte rate match stereo PCM16");
        Check(std::memcmp(music.data()+36, "data", 4) == 0 && Le32(music.data()+40) == pcmBytes,
              "BGM length matches 168 BPM, 8 bars (252000 frames / 1008000 PCM bytes)");
        Check(music.size() == 44 + pcmBytes && Le32(music.data()+4) == music.size()-8,
              "BGM RIFF size matches its PCM data");
        if (music.size() == 44 + pcmBytes) {
            Check(std::all_of(music.begin()+44, music.begin()+44+frameBytes,
                              [](std::uint8_t value) { return value == 0; }) &&
                  std::all_of(music.end()-frameBytes, music.end(),
                              [](std::uint8_t value) { return value == 0; }),
                  "both stereo loop endpoints are silent (no boundary click)");
            bool unclipped = true;
            for (std::size_t i = 44; i < music.size(); i += sampleBytes) {
                const auto sample = Le16(music.data()+i);
                if (sample == 0x7FFF || sample == 0x8000 || sample == 0x8001) unclipped = false;
            }
            Check(unclipped, "BGM PCM samples have no full-scale clipping");
        }
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
