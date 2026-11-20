#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "deployment.h"
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

namespace forge {
namespace {
namespace fs = std::filesystem;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE release() { HANDLE h = h_; h_ = INVALID_HANDLE_VALUE; return h; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (valid()) CloseHandle(h_);
        h_ = h;
    }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

DWORD CryptoError(NTSTATUS status) {
    using Convert = ULONG (WINAPI*)(NTSTATUS);
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return convert ? convert(status) : ERROR_GEN_FAILURE;
}

bool IsMissing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool IsDigest(const std::wstring& digest) {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
               (c >= L'A' && c <= L'F');
    });
}

std::wstring LowerDigest(std::wstring digest) {
    for (auto& c : digest) if (c >= L'A' && c <= L'F') c += L'a' - L'A';
    return digest;
}

bool ValidPolicy(const DeploymentPolicy& policy) {
    if (!IsDigest(policy.hostSha256) || !IsDigest(policy.originalDllSha256)) return false;
    for (const auto& hash : policy.olderProxySha256) {
        if (!IsDigest(hash) || hash == policy.originalDllSha256) return false;
    }
    return true;
}

bool CheckDirectory(const fs::path& dir, DWORD& error) {
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { error = GetLastError(); return false; }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { error = ERROR_DIRECTORY; return false; }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = ERROR_CANT_ACCESS_FILE; return false; }
    error = ERROR_SUCCESS;
    return true;
}

struct Snapshot {
    Handle handle;
    bool present = false;
    DWORD attributes = 0;
    BY_HANDLE_FILE_INFORMATION identity{};
    Bytes bytes;
    std::wstring hash;
};

// Deny writers while hashing. The transaction's current-file handle additionally
// requests DELETE and shares DELETE, to preflight sharing locks before any write.
// The verified backup and host deny both writers and deletion for the whole run.
bool OpenSnapshot(const fs::path& path, Snapshot& file, DWORD& error,
                  DWORD access = GENERIC_READ, DWORD share = FILE_SHARE_READ) {
    file = Snapshot{};
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if (IsMissing(error)) { error = ERROR_SUCCESS; return true; }
        return false;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { error = ERROR_DIRECTORY; return false; }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = ERROR_CANT_ACCESS_FILE; return false; }
    file.handle.reset(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.handle.valid()) { error = GetLastError(); return false; }
    if (!GetFileInformationByHandle(file.handle.get(), &file.identity)) {
        error = GetLastError(); return false;
    }
    file.attributes = file.identity.dwFileAttributes;
    if (file.attributes & FILE_ATTRIBUTE_DIRECTORY) { error = ERROR_DIRECTORY; return false; }
    if (file.attributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = ERROR_CANT_ACCESS_FILE; return false; }
    if (GetFileType(file.handle.get()) != FILE_TYPE_DISK) { error = ERROR_INVALID_HANDLE; return false; }
    ULARGE_INTEGER size{};
    size.HighPart = file.identity.nFileSizeHigh;
    size.LowPart = file.identity.nFileSizeLow;
    if (size.QuadPart > static_cast<ULONGLONG>(file.bytes.max_size())) {
        error = ERROR_FILE_TOO_LARGE; return false;
    }
    try { file.bytes.resize(static_cast<std::size_t>(size.QuadPart)); }
    catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
    std::size_t offset = 0;
    while (offset < file.bytes.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(file.bytes.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file.handle.get(), file.bytes.data() + offset, requested, &read, nullptr)) {
            error = GetLastError(); return false;
        }
        if (!read) { error = ERROR_HANDLE_EOF; return false; }
        offset += read;
    }
    file.hash = Sha256(file.bytes);
    if (file.hash.empty()) { error = GetLastError(); return false; }
    file.present = true;
    error = ERROR_SUCCESS;
    return true;
}

bool SameIdentity(const Snapshot& a, const Snapshot& b) {
    return a.present == b.present && (!a.present ||
        (a.identity.dwVolumeSerialNumber == b.identity.dwVolumeSerialNumber &&
         a.identity.nFileIndexHigh == b.identity.nFileIndexHigh &&
         a.identity.nFileIndexLow == b.identity.nFileIndexLow && a.hash == b.hash));
}

bool StillCurrent(const fs::path& path, const Snapshot& file, DWORD& error) {
    Snapshot now;
    if (!OpenSnapshot(path, now, error, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE)) return false;
    if (!SameIdentity(now, file)) { error = ERROR_FILE_INVALID; return false; }
    return true;
}

template<class T>
bool At(const Bytes& bytes, std::size_t offset, T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

// Keep even an unsupported/compact PE layout out of the recovery path. A
// parser limitation must not authorize downgrading an unknown vendor image.
bool HasPeHeader(const Bytes& bytes) {
    IMAGE_DOS_HEADER dos{};
    DWORD signature = 0;
    IMAGE_FILE_HEADER file{};
    WORD magic = 0;
    if (!At(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
    const std::size_t nt = static_cast<std::size_t>(dos.e_lfanew);
    return At(bytes, nt, signature) && signature == IMAGE_NT_SIGNATURE &&
        At(bytes, nt + sizeof(signature), file) &&
        At(bytes, nt + sizeof(signature) + sizeof(file), magic) &&
        (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC || magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);
}

struct PeImage {
    const Bytes& bytes;
    DWORD headers = 0;
    DWORD imageSize = 0;
    WORD machine = 0;
    WORD characteristics = 0;
    IMAGE_DATA_DIRECTORY exports{};
    std::vector<IMAGE_SECTION_HEADER> sections;

    bool Parse() {
        IMAGE_DOS_HEADER dos{};
        if (!At(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
        const std::size_t nt = static_cast<std::size_t>(dos.e_lfanew);
        DWORD signature = 0;
        IMAGE_FILE_HEADER file{};
        if (!At(bytes, nt, signature) || signature != IMAGE_NT_SIGNATURE ||
            !At(bytes, nt + sizeof(signature), file) || !file.NumberOfSections ||
            file.NumberOfSections > 96 || !(file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)) return false;
        const std::size_t opt = nt + sizeof(signature) + sizeof(file);
        if (opt > bytes.size() || file.SizeOfOptionalHeader > bytes.size() - opt) return false;
        WORD magic = 0;
        if (!At(bytes, opt, magic)) return false;
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            IMAGE_OPTIONAL_HEADER64 header{};
            if (file.SizeOfOptionalHeader < sizeof(header) || !At(bytes, opt, header) ||
                !header.SectionAlignment || !header.FileAlignment) return false;
            headers = header.SizeOfHeaders; imageSize = header.SizeOfImage;
            if (header.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return false;
            if (header.NumberOfRvaAndSizes) exports = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_OPTIONAL_HEADER32 header{};
            if (file.SizeOfOptionalHeader < sizeof(header) || !At(bytes, opt, header) ||
                !header.SectionAlignment || !header.FileAlignment) return false;
            headers = header.SizeOfHeaders; imageSize = header.SizeOfImage;
            if (header.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return false;
            if (header.NumberOfRvaAndSizes) exports = header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        } else return false;
        const std::size_t table = opt + file.SizeOfOptionalHeader;
        const std::size_t end = table + static_cast<std::size_t>(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (headers < end || headers > bytes.size() || imageSize < headers) return false;
        for (WORD i = 0; i < file.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            if (!At(bytes, table + i * sizeof(section), section)) return false;
            const auto rawEnd = static_cast<std::uint64_t>(section.PointerToRawData) + section.SizeOfRawData;
            const auto virtualEnd = static_cast<std::uint64_t>(section.VirtualAddress) +
                std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if ((section.SizeOfRawData && (section.PointerToRawData < headers || rawEnd > bytes.size())) ||
                virtualEnd > imageSize) return false;
            for (const auto& prior : sections) {
                if (section.SizeOfRawData && prior.SizeOfRawData &&
                    section.PointerToRawData < static_cast<std::uint64_t>(prior.PointerToRawData) + prior.SizeOfRawData &&
                    prior.PointerToRawData < rawEnd) return false;
                if (std::max(section.Misc.VirtualSize, section.SizeOfRawData) &&
                    std::max(prior.Misc.VirtualSize, prior.SizeOfRawData) &&
                    section.VirtualAddress < static_cast<std::uint64_t>(prior.VirtualAddress) +
                        std::max(prior.Misc.VirtualSize, prior.SizeOfRawData) &&
                    prior.VirtualAddress < virtualEnd) return false;
            }
            sections.push_back(section);
        }
        machine = file.Machine;
        characteristics = file.Characteristics;
        return true;
    }

    bool Map(DWORD rva, std::size_t length, std::size_t& offset) const {
        if (rva < headers && length <= static_cast<std::uint64_t>(headers) - rva) {
            offset = rva; return true;
        }
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const auto delta = static_cast<std::uint64_t>(rva) - section.VirtualAddress;
            if (delta <= section.SizeOfRawData && length <= section.SizeOfRawData - delta) {
                offset = static_cast<std::size_t>(section.PointerToRawData + delta); return true;
            }
        }
        return false;
    }

    bool String(DWORD rva, std::string& text, std::uint64_t rvaEnd = 0) const {
        std::size_t offset = 0;
        if (!Map(rva, 1, offset)) return false;
        // Strings may not run out of their mapped region or export directory.
        text.clear();
        for (std::size_t i = 0; i < bytes.size() - offset; ++i) {
            const auto address = static_cast<std::uint64_t>(rva) + i;
            if (address > std::numeric_limits<DWORD>::max() || (rvaEnd && address >= rvaEnd)) return false;
            std::size_t mapped = 0;
            if (!Map(static_cast<DWORD>(address), 1, mapped) || mapped != offset + i) return false;
            const auto ch = bytes[mapped];
            if (!ch) return !text.empty();
            if (ch < 0x21 || ch > 0x7e) return false;
            text.push_back(static_cast<char>(ch));
        }
        return false;
    }

    bool AllOriginalForwarders() const {
        if (!(characteristics & IMAGE_FILE_DLL) ||
            (machine != IMAGE_FILE_MACHINE_AMD64 && machine != IMAGE_FILE_MACHINE_I386) ||
            !exports.VirtualAddress || exports.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return false;
        const auto exportEnd = static_cast<std::uint64_t>(exports.VirtualAddress) + exports.Size;
        if (exportEnd > imageSize) return false;
        std::size_t directoryOffset = 0;
        IMAGE_EXPORT_DIRECTORY directory{};
        if (!Map(exports.VirtualAddress, exports.Size, directoryOffset) ||
            !At(bytes, directoryOffset, directory) || !directory.NumberOfFunctions ||
            directory.NumberOfFunctions > bytes.size() / sizeof(DWORD) ||
            directory.NumberOfNames > directory.NumberOfFunctions) return false;
        std::size_t functions = 0, names = 0, ordinals = 0;
        if (!Map(directory.AddressOfFunctions, static_cast<std::size_t>(directory.NumberOfFunctions) * sizeof(DWORD), functions)) return false;
        if (directory.NumberOfNames &&
            (!Map(directory.AddressOfNames, static_cast<std::size_t>(directory.NumberOfNames) * sizeof(DWORD), names) ||
             !Map(directory.AddressOfNameOrdinals, static_cast<std::size_t>(directory.NumberOfNames) * sizeof(WORD), ordinals))) return false;
        std::string text;
        if (!String(directory.Name, text)) return false;
        bool any = false;
        for (DWORD i = 0; i < directory.NumberOfFunctions; ++i) {
            DWORD address = 0;
            if (!At(bytes, functions + static_cast<std::size_t>(i) * sizeof(DWORD), address)) return false;
            if (!address) continue; // An empty ordinal slot is not an export.
            any = true;
            if (address < exports.VirtualAddress || address >= exportEnd || !String(address, text, exportEnd)) return false;
            const std::string prefix = "quazip_orig.";
            if (text.compare(0, prefix.size(), prefix) != 0 || text.size() == prefix.size()) return false;
            if (text[prefix.size()] == '#') {
                if (text.size() == prefix.size() + 1 ||
                    !std::all_of(text.begin() + prefix.size() + 1, text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
            }
        }
        for (DWORD i = 0; i < directory.NumberOfNames; ++i) {
            DWORD name = 0, address = 0;
            WORD ordinal = 0;
            if (!At(bytes, names + static_cast<std::size_t>(i) * sizeof(DWORD), name) ||
                !At(bytes, ordinals + static_cast<std::size_t>(i) * sizeof(WORD), ordinal) ||
                ordinal >= directory.NumberOfFunctions || !String(name, text) ||
                !At(bytes, functions + static_cast<std::size_t>(ordinal) * sizeof(DWORD), address) || !address) return false;
        }
        return any;
    }
};

struct InspectionData {
    PatchInspection result;
    Snapshot host, current, backup;
};

InspectionData InspectFiles(const fs::path& dir, const std::wstring& payloadHash,
                            const DeploymentPolicy& policy, bool transaction) {
    InspectionData data;
    auto& result = data.result;
    DWORD error = ERROR_SUCCESS;
    if (!ValidPolicy(policy)) { result.error = ERROR_INVALID_PARAMETER; return data; }
    if (!IsDigest(payloadHash) || payloadHash == policy.originalDllSha256) {
        result.error = ERROR_INVALID_DATA; return data;
    }
    if (!CheckDirectory(dir, error) || !OpenSnapshot(dir / L"010Editor.exe", data.host, error)) {
        result.error = error; return data;
    }
    if (!data.host.present) { result.error = ERROR_FILE_NOT_FOUND; return data; }
    if (data.host.hash != policy.hostSha256) { result.error = ERROR_REVISION_MISMATCH; return data; }
    if (!OpenSnapshot(dir / L"quazip_orig.dll", data.backup, error)) {
        result.error = error;
        result.backupPresent = GetFileAttributesW((dir / L"quazip_orig.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
        return data;
    }
    result.backupPresent = data.backup.present;
    result.backupSha256 = data.backup.hash;
    if (data.backup.present && data.backup.hash != policy.originalDllSha256) {
        result.error = ERROR_CRC; return data;
    }
    if (!OpenSnapshot(dir / L"quazip.dll", data.current, error,
                      transaction ? GENERIC_READ | DELETE : GENERIC_READ,
                      transaction ? FILE_SHARE_READ | FILE_SHARE_DELETE : FILE_SHARE_READ)) {
        result.error = error; return data;
    }
    result.currentSha256 = data.current.hash;
    if (!data.current.present) {
        result.state = data.backup.present ? PatchState::Recoverable : PatchState::MissingBackup;
        result.error = data.backup.present ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
        return data;
    }
    if (data.current.hash == policy.originalDllSha256) {
        result.state = PatchState::Original;
        return data;
    }
    PeImage image{data.current.bytes};
    const bool validPe = image.Parse();
    const bool pinnedOld = std::find(policy.olderProxySha256.begin(), policy.olderProxySha256.end(),
                                     data.current.hash) != policy.olderProxySha256.end();
    const bool oldProxy = pinnedOld && validPe && image.AllOriginalForwarders();
    if (data.current.hash == payloadHash || oldProxy) {
        if (!data.backup.present) {
            result.state = PatchState::MissingBackup;
            result.error = ERROR_FILE_NOT_FOUND;
        } else result.state = data.current.hash == payloadHash ? PatchState::Installed : PatchState::OlderProxy;
    } else if (pinnedOld) {
        result.error = ERROR_BAD_EXE_FORMAT; // Digest alone does not authorize a malformed legacy PE.
    } else if (data.backup.present && !validPe && !HasPeHeader(data.current.bytes)) {
        result.state = PatchState::Recoverable;
    } else {
        result.error = (validPe || HasPeHeader(data.current.bytes)) ? ERROR_REVISION_MISMATCH : ERROR_CRC;
    }
    return data;
}

class TemporaryFile {
public:
    fs::path path;
    ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
    void committed() { path.clear(); }
};

bool StageFile(const fs::path& dir, const Bytes& bytes, const std::wstring& expected,
               TemporaryFile& temporary, DWORD& error) {
    static std::atomic<unsigned long long> serial{0};
    Handle output;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto name = L".quazip.forge-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial) + L".tmp";
        const auto candidate = dir / name;
        output.reset(CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (output.valid()) { temporary.path = candidate; break; }
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
    }
    if (!output.valid()) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(output.get(), bytes.data() + offset, request, &written, nullptr)) {
            error = GetLastError(); return false;
        }
        if (!written) { error = ERROR_WRITE_FAULT; return false; }
        offset += written;
    }
    if (!FlushFileBuffers(output.get())) { error = GetLastError(); return false; }
    const HANDLE raw = output.release();
    if (!CloseHandle(raw)) { error = GetLastError(); return false; }
    Snapshot verified;
    if (!OpenSnapshot(temporary.path, verified, error)) return false;
    if (!verified.present || verified.hash != expected) { error = ERROR_CRC; return false; }
    error = ERROR_SUCCESS;
    return true;
}

PatchResult ReplaceCurrent(const fs::path& dir, const Bytes& bytes, const std::wstring& expected,
                           InspectionData& files) {
    DWORD error = ERROR_SUCCESS;
    TemporaryFile temporary;
    if (!StageFile(dir, bytes, expected, temporary, error)) return {false, false, error};
    const auto currentPath = dir / L"quazip.dll";
    if (!StillCurrent(currentPath, files.current, error)) return {false, false, error};
    // Same-directory rename on one volume. No DeleteFile(current), no rename-away
    // interval, and no fallback which might overwrite a changed/unknown image.
    const BOOL replaced = files.current.present
        ? ReplaceFileW(currentPath.c_str(), temporary.path.c_str(), nullptr, 0, nullptr, nullptr)
        : MoveFileExW(temporary.path.c_str(), currentPath.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!replaced) {
        const DWORD replacementError = GetLastError();
        // ReplaceFileW documents rare partial failures (1176/1177). Do not
        // claim unchanged when its target name moved; the original backup is
        // still pinned and retained, and the primary Win32 error is preserved.
        DWORD verificationError = ERROR_SUCCESS;
        const bool changed = !StillCurrent(currentPath, files.current, verificationError);
        return {false, changed, replacementError};
    }
    temporary.committed();
    Snapshot committed;
    if (!OpenSnapshot(currentPath, committed, error)) return {false, true, error};
    if (!committed.present || committed.hash != expected) return {false, true, ERROR_CRC};
    return {true, true, ERROR_SUCCESS};
}

} // namespace

bool ReadBytes(const fs::path& path, Bytes& bytes, DWORD& error) {
    bytes.clear();
    Snapshot file;
    if (!OpenSnapshot(path, file, error)) return false;
    if (!file.present) { error = ERROR_FILE_NOT_FOUND; return false; }
    bytes = std::move(file.bytes);
    error = ERROR_SUCCESS;
    return true;
}

std::wstring Sha256(const Bytes& bytes) {
    // The caller-owned hash object storage must outlive BCryptDestroyHash.
    Bytes object;
    struct CryptoHandles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~CryptoHandles() {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    auto fail = [](NTSTATUS status) -> std::wstring { SetLastError(CryptoError(status)); return {}; };
    NTSTATUS status = BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) return fail(status);
    DWORD objectSize = 0, length = 0;
    status = BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &length, 0);
    if (status < 0) return fail(status);
    try { object.resize(objectSize); }
    catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return {}; }
    status = BCryptCreateHash(handles.algorithm, &handles.hash, object.data(), objectSize, nullptr, 0, 0);
    if (status < 0) return fail(status);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ULONG count = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        status = BCryptHashData(handles.hash, const_cast<PUCHAR>(bytes.data() + offset), count, 0);
        if (status < 0) return fail(status);
        offset += count;
    }
    UCHAR digest[32]{};
    status = BCryptFinishHash(handles.hash, digest, sizeof(digest), 0);
    if (status < 0) return fail(status);
    const wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(64);
    for (const auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    SetLastError(ERROR_SUCCESS);
    return result;
}

DeploymentPolicy SupportedPolicy() {
    return {
        L"865f43ab9b0cb7fa3ce385ed82b8b11ab4ea36fb7054cf70cb136a5f4f5b0a7d",
        L"a6d829e0457ebc374e57c05cef02192b5031b1564361313e7ae86970fb8d3b20",
        {L"a30e97ec3d5d5b03b264480f2cd7894c2dc706b9b1ee1e04245b33da6fee39ab"}
    };
}

Deployment::Deployment(Bytes payload, DeploymentPolicy policy)
    : payload_(std::move(payload)), payloadSha256_(Sha256(payload_)), policy_(std::move(policy)) {
    policy_.hostSha256 = LowerDigest(std::move(policy_.hostSha256));
    policy_.originalDllSha256 = LowerDigest(std::move(policy_.originalDllSha256));
    for (auto& hash : policy_.olderProxySha256) hash = LowerDigest(std::move(hash));
    if (payload_.empty()) payloadSha256_.clear();
}

bool Deployment::HostSupported(const fs::path& dir) const {
    DWORD error = ERROR_SUCCESS;
    Snapshot host;
    if (!ValidPolicy(policy_)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (!CheckDirectory(dir, error) || !OpenSnapshot(dir / L"010Editor.exe", host, error)) {
        SetLastError(error); return false;
    }
    if (!host.present) { SetLastError(ERROR_FILE_NOT_FOUND); return false; }
    if (host.hash != policy_.hostSha256) { SetLastError(ERROR_REVISION_MISMATCH); return false; }
    SetLastError(ERROR_SUCCESS);
    return true;
}

PatchInspection Deployment::Inspect(const fs::path& dir) const {
    return InspectFiles(dir, payloadSha256_, policy_, false).result;
}

PatchResult Deployment::Install(const fs::path& dir) const {
    auto files = InspectFiles(dir, payloadSha256_, policy_, true);
    const auto& inspection = files.result;
    if (inspection.error != ERROR_SUCCESS) return {false, false, inspection.error};
    if (inspection.state == PatchState::Installed) return {true, false, ERROR_SUCCESS};
    if (inspection.state != PatchState::Original && inspection.state != PatchState::OlderProxy)
        return {false, false, ERROR_INVALID_DATA};
    if (files.current.attributes & FILE_ATTRIBUTE_READONLY) return {false, false, ERROR_ACCESS_DENIED};
    DWORD error = ERROR_SUCCESS;
    if (!files.backup.present) {
        // Only the exact supported original reaches here. Publish a verified
        // copy without ever moving away or deleting that current original.
        TemporaryFile backup;
        if (!StageFile(dir, files.current.bytes, policy_.originalDllSha256, backup, error)) return {false, false, error};
        if (!StillCurrent(dir / L"quazip.dll", files.current, error)) return {false, false, error};
        if (!MoveFileExW(backup.path.c_str(), (dir / L"quazip_orig.dll").c_str(), MOVEFILE_WRITE_THROUGH))
            return {false, false, GetLastError()};
        backup.committed();
        if (!OpenSnapshot(dir / L"quazip_orig.dll", files.backup, error)) return {false, false, error};
        if (!files.backup.present || files.backup.hash != policy_.originalDllSha256) return {false, false, ERROR_CRC};
    }
    return ReplaceCurrent(dir, payload_, payloadSha256_, files);
}

PatchResult Deployment::Restore(const fs::path& dir) const {
    auto files = InspectFiles(dir, payloadSha256_, policy_, true);
    const auto& inspection = files.result;
    if (inspection.error != ERROR_SUCCESS) return {false, false, inspection.error};
    if (inspection.state == PatchState::Original) return {true, false, ERROR_SUCCESS};
    if (!files.backup.present) return {false, false, ERROR_FILE_NOT_FOUND};
    if (inspection.state != PatchState::Installed && inspection.state != PatchState::OlderProxy &&
        inspection.state != PatchState::Recoverable) return {false, false, ERROR_INVALID_DATA};
    if (files.current.present && (files.current.attributes & FILE_ATTRIBUTE_READONLY))
        return {false, false, ERROR_ACCESS_DENIED};
    // Copy rather than rename: the independently verified backup survives every
    // failure and also remains available after a successful restore.
    return ReplaceCurrent(dir, files.backup.bytes, policy_.originalDllSha256, files);
}

} // namespace forge
