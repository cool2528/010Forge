#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "deployment.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {
namespace fs = std::filesystem;
using forge::Bytes;
using forge::Deployment;
using forge::DeploymentPolicy;
using forge::PatchState;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

void Put(const fs::path& path, const Bytes& bytes) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "fixture CreateFileW");
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    Require(ok, "fixture WriteFile/FlushFileBuffers");
}

Bytes Get(const fs::path& path) {
    Bytes bytes;
    DWORD error = ERROR_SUCCESS;
    Require(forge::ReadBytes(path, bytes, error), "fixture ReadBytes");
    Require(error == ERROR_SUCCESS, "ReadBytes success error");
    return bytes;
}

template<class T>
void Store(Bytes& bytes, std::size_t offset, const T& value) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture PE bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void StoreString(Bytes& bytes, std::size_t offset, const char* text) {
    const auto length = std::strlen(text) + 1;
    Require(offset <= bytes.size() && length <= bytes.size() - offset, "fixture PE string bounds");
    std::memcpy(bytes.data() + offset, text, length);
}

// Data only: a PE header and a complete two-entry export table, never loaded.
// variant 0: all-forwarding proxy; 1: a normal vendor export; 2: second export
// targets the wrong module. Thus checking only the first forwarder is tested.
Bytes PeFixture(unsigned variant = 0) {
    Bytes bytes(0x600, 0);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    Store(bytes, 0, dos);
    const DWORD signature = IMAGE_NT_SIGNATURE;
    Store(bytes, 0x80, signature);
    IMAGE_FILE_HEADER file{};
    file.Machine = IMAGE_FILE_MACHINE_AMD64;
    file.NumberOfSections = 1;
    file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    file.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    Store(bytes, 0x84, file);
    IMAGE_OPTIONAL_HEADER64 optional{};
    optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    optional.ImageBase = 0x180000000ULL;
    optional.SectionAlignment = 0x1000;
    optional.FileAlignment = 0x200;
    optional.SizeOfHeaders = 0x200;
    optional.SizeOfImage = 0x2000;
    optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x1000, 0x180};
    Store(bytes, 0x98, optional);
    IMAGE_SECTION_HEADER section{};
    std::memcpy(section.Name, ".rdata", 6);
    section.Misc.VirtualSize = 0x400;
    section.VirtualAddress = 0x1000;
    section.PointerToRawData = 0x200;
    section.SizeOfRawData = 0x400;
    section.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    Store(bytes, 0x98 + sizeof(optional), section);
    IMAGE_EXPORT_DIRECTORY directory{};
    directory.Name = 0x1060;
    directory.Base = 1;
    directory.NumberOfFunctions = 2;
    directory.NumberOfNames = 2;
    directory.AddressOfFunctions = 0x1040;
    directory.AddressOfNames = 0x1048;
    directory.AddressOfNameOrdinals = 0x1050;
    Store(bytes, 0x200, directory);
    const DWORD functions[2] = {0x1070, variant == 1 ? 0x1300u : 0x1090u};
    const DWORD names[2] = {0x10c0, 0x10c8};
    const WORD ordinals[2] = {0, 1};
    Store(bytes, 0x240, functions);
    Store(bytes, 0x248, names);
    Store(bytes, 0x250, ordinals);
    StoreString(bytes, 0x260, "quazip.dll");
    StoreString(bytes, 0x270, "quazip_orig.first");
    StoreString(bytes, 0x290, variant == 2 ? "other_module.second" : "quazip_orig.second");
    StoreString(bytes, 0x2c0, "first");
    StoreString(bytes, 0x2c8, "second");
    return bytes;
}

struct Fixture {
    fs::path dir;
    Bytes host = Text("fake host executable, not executable code");
    Bytes original = Text("original fixture DLL bytes 00000000000000");
    Bytes payload = Text("proxy payload fixture DLL 111111111111111");
    DeploymentPolicy policy;
    Fixture() {
        static std::atomic<unsigned> counter{0};
        dir = fs::temp_directory_path() /
            (L"forge-deployment-\x6d4b\x8bd5-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter));
        Require(fs::create_directories(dir), "fixture create directory");
        policy.hostSha256 = forge::Sha256(host);
        policy.originalDllSha256 = forge::Sha256(original);
        Put(dir / L"010Editor.exe", host);
        Put(dir / L"quazip.dll", original);
    }
    ~Fixture() {
        std::error_code ignored;
        for (fs::directory_iterator it(dir, ignored), end; !ignored && it != end; it.increment(ignored))
            if (it->is_regular_file(ignored)) SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
        fs::remove_all(dir, ignored);
    }
    Deployment deployment() const { return Deployment(payload, policy); }
    void backup() const { Put(dir / L"quazip_orig.dll", original); }
    void noTemps() const {
        for (const auto& entry : fs::directory_iterator(dir))
            Require(entry.path().filename().wstring().find(L".quazip.forge-") != 0, "temporary sibling leaked");
    }
    void originalsUnchanged(bool expectBackup = false) const {
        Require(Get(dir / L"quazip.dll") == original, "current original changed on failure");
        if (expectBackup) Require(Get(dir / L"quazip_orig.dll") == original, "verified backup changed");
        else Require(!fs::exists(dir / L"quazip_orig.dll"), "unexpected backup on preflight failure");
        noTemps();
    }
};

struct Lock {
    HANDLE handle;
    Lock(const fs::path& path, DWORD share)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr)) {
        Require(handle != INVALID_HANDLE_VALUE, "fixture lock");
    }
    ~Lock() { CloseHandle(handle); }
};

void TestHashesAndReadBytes() {
    Require(forge::Sha256({}) == L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA256 vector");
    Require(forge::Sha256(Text("abc")) == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 abc vector");
    const auto policy = forge::SupportedPolicy();
    Require(policy.hostSha256 == L"865f43ab9b0cb7fa3ce385ed82b8b11ab4ea36fb7054cf70cb136a5f4f5b0a7d", "host pin");
    Require(policy.originalDllSha256 == L"a6d829e0457ebc374e57c05cef02192b5031b1564361313e7ae86970fb8d3b20", "original pin");
    Fixture f;
    Bytes output{1, 2};
    DWORD error = 999;
    Require(forge::ReadBytes(f.dir / L"quazip.dll", output, error) && output == f.original && error == 0, "regular read");
    Require(!forge::ReadBytes(f.dir / L"absent", output, error) && error == ERROR_FILE_NOT_FOUND && output.empty(), "missing read");
    Require(!forge::ReadBytes(f.dir, output, error) && error == ERROR_DIRECTORY && output.empty(), "directory read");
    {
        Lock lock(f.dir / L"quazip.dll", 0);
        Require(!forge::ReadBytes(f.dir / L"quazip.dll", output, error) && error == ERROR_SHARING_VIOLATION, "locked read");
    }
}

void TestInstallRepeatRestore() {
    Fixture f;
    auto deployment = f.deployment();
    Require(deployment.HostSupported(f.dir), "supported fixture host");
    auto inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::Original && !inspection.backupPresent && inspection.error == 0, "initial inspection");
    const auto installed = deployment.Install(f.dir);
    if (!installed.ok || !installed.changed || installed.error)
        throw std::runtime_error("first install: error=" + std::to_string(installed.error) +
                                 ", changed=" + std::to_string(installed.changed));
    Require(Get(f.dir / L"quazip.dll") == f.payload, "installed payload content");
    Require(Get(f.dir / L"quazip_orig.dll") == f.original, "backed-up original content");
    inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::Installed && inspection.backupPresent && inspection.error == 0, "installed inspection");
    Require(inspection.currentSha256 == deployment.PayloadSha256() && inspection.backupSha256 == f.policy.originalDllSha256, "inspection hashes");
    WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
    Require(GetFileAttributesExW((f.dir / L"quazip.dll").c_str(), GetFileExInfoStandard, &before), "attributes before repeat");
    const auto repeated = deployment.Install(f.dir);
    Require(repeated.ok && !repeated.changed && repeated.error == 0, "idempotent install");
    Require(GetFileAttributesExW((f.dir / L"quazip.dll").c_str(), GetFileExInfoStandard, &after) &&
            CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0, "repeat preserves write time");
    const auto restored = deployment.Restore(f.dir);
    Require(restored.ok && restored.changed && restored.error == 0, "restore");
    f.originalsUnchanged(true);
    Require(forge::Sha256(Get(f.dir / L"quazip.dll")) == f.policy.originalDllSha256, "restored original hash");
    const auto repeatRestore = deployment.Restore(f.dir);
    Require(repeatRestore.ok && !repeatRestore.changed, "idempotent restore");
    Require(deployment.Inspect(f.dir).state == PatchState::Original, "restored inspection");
}

void TestExistingOriginalBackupAndUppercasePolicy() {
    Fixture f;
    f.backup();
    auto policy = f.policy;
    for (auto* hash : {&policy.hostSha256, &policy.originalDllSha256})
        for (auto& c : *hash) if (c >= L'a' && c <= L'f') c -= L'a' - L'A';
    Deployment deployment(f.payload, policy);
    Require(deployment.Install(f.dir).ok && deployment.Restore(f.dir).ok, "existing backup/upper-case digest");
    f.originalsUnchanged(true);
}

void TestBadAndDirectoryBackup() {
    Fixture f;
    auto deployment = f.deployment();
    Put(f.dir / L"quazip_orig.dll", Text("damaged backup"));
    Require(deployment.Inspect(f.dir).state == PatchState::Conflict, "bad backup state");
    const auto install = deployment.Install(f.dir), restore = deployment.Restore(f.dir);
    Require(!install.ok && !install.changed && install.error == ERROR_CRC, "bad backup install CRC");
    Require(!restore.ok && !restore.changed && restore.error == ERROR_CRC, "bad backup restore CRC");
    Require(Get(f.dir / L"quazip.dll") == f.original, "bad backup keeps original");
    Put(f.dir / L"quazip_orig.dll", f.payload);
    Require(deployment.Install(f.dir).error == ERROR_CRC && deployment.Restore(f.dir).error == ERROR_CRC,
            "proxy content never qualifies as original backup");
    Require(Get(f.dir / L"quazip.dll") == f.original && Get(f.dir / L"quazip_orig.dll") == f.payload,
            "proxy backup conflict leaves files unchanged");
    Require(DeleteFileW((f.dir / L"quazip_orig.dll").c_str()), "delete damaged fixture backup");
    Require(CreateDirectoryW((f.dir / L"quazip_orig.dll").c_str(), nullptr), "directory backup fixture");
    const auto inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::Conflict && inspection.backupPresent && inspection.error == ERROR_DIRECTORY, "directory backup state/error");
    Require(deployment.Install(f.dir).error == ERROR_DIRECTORY && deployment.Restore(f.dir).error == ERROR_DIRECTORY, "directory backup blocked");
    Require(Get(f.dir / L"quazip.dll") == f.original, "directory backup keeps original");
    f.noTemps();
}

void TestUnknownSameSizeOriginal() {
    Fixture f;
    Bytes unknown = f.original;
    unknown.back() ^= 1;
    Require(unknown.size() == f.original.size(), "same-size fixture");
    Put(f.dir / L"quazip.dll", unknown);
    const auto result = f.deployment().Install(f.dir);
    Require(!result.ok && !result.changed && result.error == ERROR_CRC, "unknown same-size DLL rejected");
    Require(Get(f.dir / L"quazip.dll") == unknown && !fs::exists(f.dir / L"quazip_orig.dll"), "unknown not backed up");
    f.noTemps();
}

void TestMissingBackupAndProxyNotOriginal() {
    Fixture f;
    Put(f.dir / L"quazip.dll", f.payload);
    auto deployment = f.deployment();
    const auto inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::MissingBackup && inspection.error == ERROR_FILE_NOT_FOUND, "installed without backup");
    Require(deployment.Install(f.dir).error == ERROR_FILE_NOT_FOUND && deployment.Restore(f.dir).error == ERROR_FILE_NOT_FOUND, "no proxy-to-original backup");
    Require(!fs::exists(f.dir / L"quazip_orig.dll") && Get(f.dir / L"quazip.dll") == f.payload, "proxy kept with no backup");
    f.noTemps();
}

void TestMissingAndDamagedCurrentRecovery() {
    Fixture f;
    f.backup();
    Require(DeleteFileW((f.dir / L"quazip.dll").c_str()), "remove current fixture");
    auto deployment = f.deployment();
    auto inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::Recoverable && inspection.error == 0, "missing current recoverable");
    const auto install = deployment.Install(f.dir);
    Require(!install.ok && !install.changed && install.error == ERROR_INVALID_DATA, "missing current requires restore first");
    Require(!fs::exists(f.dir / L"quazip.dll"), "failed install keeps missing current");
    Require(deployment.Restore(f.dir).ok, "missing current restore");
    f.originalsUnchanged(true);
    Put(f.dir / L"quazip.dll", Text("broken/truncated proxy bytes"));
    Require(deployment.Inspect(f.dir).state == PatchState::Recoverable, "damaged current recoverable");
    Require(deployment.Restore(f.dir).ok, "damaged current restore");
    f.originalsUnchanged(true);
}

void TestUnknownVendorProtected() {
    Fixture f;
    f.backup();
    const auto vendor = PeFixture(1);
    Put(f.dir / L"quazip.dll", vendor);
    auto deployment = f.deployment();
    const auto inspection = deployment.Inspect(f.dir);
    Require(inspection.state == PatchState::Conflict && inspection.error == ERROR_REVISION_MISMATCH, "unknown valid PE conflict");
    Require(deployment.Restore(f.dir).error == ERROR_REVISION_MISMATCH &&
            deployment.Install(f.dir).error == ERROR_REVISION_MISMATCH, "no vendor downgrade");
    Require(Get(f.dir / L"quazip.dll") == vendor && Get(f.dir / L"quazip_orig.dll") == f.original, "unknown vendor retained");
    auto unsupportedLayout = vendor;
    const DWORD directoryCount = IMAGE_NUMBEROF_DIRECTORY_ENTRIES + 1;
    Store(unsupportedLayout, 0x98 + offsetof(IMAGE_OPTIONAL_HEADER64, NumberOfRvaAndSizes), directoryCount);
    Put(f.dir / L"quazip.dll", unsupportedLayout);
    Require(deployment.Restore(f.dir).error == ERROR_REVISION_MISMATCH, "unsupported PE layout is protected rather than recovery");
    Require(Get(f.dir / L"quazip.dll") == unsupportedLayout, "unsupported layout retained");
    f.noTemps();
}

void TestLegacyProxyFullForwardersAndPins() {
    Fixture f;
    f.backup();
    const auto old = PeFixture();
    f.policy.olderProxySha256.push_back(forge::Sha256(old));
    Put(f.dir / L"quazip.dll", old);
    auto deployment = f.deployment();
    Require(deployment.Inspect(f.dir).state == PatchState::OlderProxy, "pinned older proxy state");
    Require(deployment.Install(f.dir).ok, "older proxy upgrade");
    Require(Get(f.dir / L"quazip_orig.dll") == f.original, "upgrade retained original");
    Require(deployment.Restore(f.dir).ok, "upgraded proxy restore");
    Put(f.dir / L"quazip.dll", old);
    Require(deployment.Restore(f.dir).ok, "older proxy direct restore");
    f.originalsUnchanged(true);
    Put(f.dir / L"quazip.dll", old);
    auto noPins = f.policy;
    noPins.olderProxySha256.clear();
    Require(Deployment(f.payload, noPins).Inspect(f.dir).state == PatchState::Conflict, "unknown forwarder PE is not trusted");
    Require(Deployment(f.payload, noPins).Install(f.dir).error == ERROR_REVISION_MISMATCH, "forwarder structure alone not identity");
    const auto partial = PeFixture(2);
    f.policy.olderProxySha256.push_back(forge::Sha256(partial));
    Put(f.dir / L"quazip.dll", partial);
    auto invalid = f.deployment();
    Require(invalid.Inspect(f.dir).state == PatchState::Conflict && invalid.Install(f.dir).error == ERROR_BAD_EXE_FORMAT,
            "all forwarders checked including second");
    Require(Get(f.dir / L"quazip.dll") == partial && Get(f.dir / L"quazip_orig.dll") == f.original, "partial forwarder unchanged");
    f.noTemps();
}

void TestMalformedPinnedProxyAndMarker() {
    Fixture f;
    f.backup();
    Bytes truncated = PeFixture();
    truncated.resize(0x250);
    f.policy.olderProxySha256.push_back(forge::Sha256(truncated));
    Put(f.dir / L"quazip.dll", truncated);
    Require(f.deployment().Install(f.dir).error == ERROR_BAD_EXE_FORMAT, "truncated pinned PE rejected");
    const auto marker = Text("010Forge marker quazip_orig. quazip proxy version 4.0");
    Put(f.dir / L"quazip.dll", marker);
    const auto result = f.deployment().Install(f.dir);
    Require(!result.ok && !result.changed && result.error == ERROR_INVALID_DATA, "marker is not trust");
    Require(Get(f.dir / L"quazip.dll") == marker && Get(f.dir / L"quazip_orig.dll") == f.original, "marker not installed or backed up");
    f.noTemps();
}

void TestCurrentAndBackupLocks() {
    Fixture f;
    auto deployment = f.deployment();
    {
        Lock lock(f.dir / L"quazip.dll", FILE_SHARE_READ);
        Require(deployment.Inspect(f.dir).state == PatchState::Original, "read-only inspection with reader");
        const auto result = deployment.Install(f.dir);
        Require(!result.ok && !result.changed && result.error == ERROR_SHARING_VIOLATION, "current delete-sharing lock");
    }
    f.originalsUnchanged();
    Require(deployment.Install(f.dir).ok, "install after lock released");
    {
        Lock lock(f.dir / L"quazip.dll", FILE_SHARE_READ);
        const auto result = deployment.Restore(f.dir);
        Require(!result.ok && !result.changed && result.error == ERROR_SHARING_VIOLATION, "restore current locked");
    }
    Require(Get(f.dir / L"quazip.dll") == f.payload && Get(f.dir / L"quazip_orig.dll") == f.original, "current lock preserves both");
    {
        Lock lock(f.dir / L"quazip_orig.dll", 0);
        Require(deployment.Inspect(f.dir).error == ERROR_SHARING_VIOLATION, "backup locked inspection");
        const auto install = deployment.Install(f.dir), restore = deployment.Restore(f.dir);
        Require(!install.ok && !install.changed && install.error == ERROR_SHARING_VIOLATION, "locked backup repeat install");
        Require(!restore.ok && !restore.changed && restore.error == ERROR_SHARING_VIOLATION, "locked backup restore");
    }
    Require(Get(f.dir / L"quazip.dll") == f.payload && Get(f.dir / L"quazip_orig.dll") == f.original, "backup lock preserves both");
    f.noTemps();
    Require(deployment.Restore(f.dir).ok, "restore after backup lock released");
    f.originalsUnchanged(true);
}

void TestReadonlyAttributes() {
    Fixture f;
    auto deployment = f.deployment();
    Require(SetFileAttributesW((f.dir / L"quazip.dll").c_str(), FILE_ATTRIBUTE_READONLY), "set read-only current");
    const auto denied = deployment.Install(f.dir);
    Require(!denied.ok && !denied.changed && denied.error == ERROR_ACCESS_DENIED, "read-only current install");
    Require(GetFileAttributesW((f.dir / L"quazip.dll").c_str()) & FILE_ATTRIBUTE_READONLY, "failed install preserves read-only bit");
    f.originalsUnchanged();
    Require(SetFileAttributesW((f.dir / L"quazip.dll").c_str(), FILE_ATTRIBUTE_NORMAL), "clear fixture current read-only");
    Require(deployment.Install(f.dir).ok, "install writable current");
    Require(!(GetFileAttributesW((f.dir / L"quazip.dll").c_str()) & FILE_ATTRIBUTE_TEMPORARY), "no temporary attribute on deployed file");
    Require(SetFileAttributesW((f.dir / L"quazip.dll").c_str(), FILE_ATTRIBUTE_READONLY), "set proxy read-only");
    const auto restore = deployment.Restore(f.dir);
    Require(!restore.ok && !restore.changed && restore.error == ERROR_ACCESS_DENIED, "read-only current restore");
    Require(Get(f.dir / L"quazip.dll") == f.payload && Get(f.dir / L"quazip_orig.dll") == f.original, "read-only failure preserves both");
    Require(SetFileAttributesW((f.dir / L"quazip.dll").c_str(), FILE_ATTRIBUTE_NORMAL), "clear proxy read-only");
    Require(SetFileAttributesW((f.dir / L"quazip_orig.dll").c_str(), FILE_ATTRIBUTE_READONLY), "set backup read-only");
    Require(deployment.Restore(f.dir).ok, "restore can copy read-only backup");
    Require(GetFileAttributesW((f.dir / L"quazip_orig.dll").c_str()) & FILE_ATTRIBUTE_READONLY, "backup attributes retained");
    f.originalsUnchanged(true);
}

void TestUnsupportedHostAndInvalidPolicy() {
    Fixture f;
    auto deployment = f.deployment();
    auto otherHost = f.host;
    otherHost[0] ^= 1;
    Put(f.dir / L"010Editor.exe", otherHost);
    Require(!deployment.HostSupported(f.dir) && GetLastError() == ERROR_REVISION_MISMATCH, "same-size unsupported host");
    Require(deployment.Inspect(f.dir).state == PatchState::Conflict && deployment.Inspect(f.dir).error == ERROR_REVISION_MISMATCH, "unsupported host state/error");
    Require(deployment.Install(f.dir).error == ERROR_REVISION_MISMATCH && deployment.Restore(f.dir).error == ERROR_REVISION_MISMATCH, "unsupported host fail closed");
    f.originalsUnchanged();
    Put(f.dir / L"010Editor.exe", f.host);
    auto invalid = f.policy;
    invalid.originalDllSha256 = L"not-a-sha256";
    Require(Deployment(f.payload, invalid).Install(f.dir).error == ERROR_INVALID_PARAMETER, "invalid policy fail closed");
    Require(Deployment({}, f.policy).Install(f.dir).error == ERROR_INVALID_DATA, "empty payload fail closed");
    Require(Deployment(f.original, f.policy).Install(f.dir).error == ERROR_INVALID_DATA, "payload cannot become original");
    Require(DeleteFileW((f.dir / L"010Editor.exe").c_str()), "remove host fixture");
    Require(deployment.Install(f.dir).error == ERROR_FILE_NOT_FOUND, "missing host fail closed");
    f.originalsUnchanged();
}

void TestCurrentDirectoryAndMissingBoth() {
    Fixture f;
    Require(DeleteFileW((f.dir / L"quazip.dll").c_str()), "remove original fixture");
    auto deployment = f.deployment();
    Require(deployment.Inspect(f.dir).state == PatchState::MissingBackup && deployment.Restore(f.dir).error == ERROR_FILE_NOT_FOUND, "both DLLs missing");
    Require(CreateDirectoryW((f.dir / L"quazip.dll").c_str(), nullptr), "current directory fixture");
    Require(deployment.Install(f.dir).error == ERROR_DIRECTORY && deployment.Restore(f.dir).error == ERROR_DIRECTORY, "directory current preserved");
    Require(fs::is_directory(f.dir / L"quazip.dll") && !fs::exists(f.dir / L"quazip_orig.dll"), "directory remains untouched");
    f.noTemps();
}

} // namespace

int wmain() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"hashes and regular-file reads", TestHashesAndReadBytes},
        {"install -> repeat -> restore -> original hash", TestInstallRepeatRestore},
        {"existing verified backup / uppercase policy", TestExistingOriginalBackupAndUppercasePolicy},
        {"corrupt or directory backup", TestBadAndDirectoryBackup},
        {"unknown same-size original", TestUnknownSameSizeOriginal},
        {"missing backup / proxy is never original", TestMissingBackupAndProxyNotOriginal},
        {"missing or damaged current recovery", TestMissingAndDamagedCurrentRecovery},
        {"unknown valid vendor downgrade protection", TestUnknownVendorProtected},
        {"pinned legacy proxy / every forwarder parsed", TestLegacyProxyFullForwardersAndPins},
        {"malformed PE and untrusted marker", TestMalformedPinnedProxyAndMarker},
        {"current and backup sharing locks", TestCurrentAndBackupLocks},
        {"read-only attributes / temporary cleanup", TestReadonlyAttributes},
        {"unsupported host / malformed policy", TestUnsupportedHostAndInvalidPolicy},
        {"directory current / missing both", TestCurrentDirectoryAndMissingBoth},
    };
    unsigned failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "[PASS] " << test.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.first << ": " << error.what() << ", Win32=" << GetLastError() << '\n';
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0]) - failures) << '/' << sizeof(tests) / sizeof(tests[0]) << " tests passed\n";
    return failures ? 1 : 0;
}
