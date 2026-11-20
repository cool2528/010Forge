#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace forge {
using Bytes = std::vector<std::uint8_t>;

// Reads regular non-reparse disk files only; returns the original Win32 error.
bool ReadBytes(const std::filesystem::path& path, Bytes& data, DWORD& error);
// BCrypt SHA-256, lowercase hex. Empty result indicates an error in GetLastError.
std::wstring Sha256(const Bytes& data);

struct DeploymentPolicy {
    std::wstring hostSha256;
    std::wstring originalDllSha256;
    // A legacy proxy needs BOTH a pinned digest and a fully parsed export
    // table whose every actual export forwards to quazip_orig.<symbol>.
    // A string/marker, file size or a partial forwarder table is not identity.
    std::vector<std::wstring> olderProxySha256;
};

// Exact supported artifacts, not a claim of cross-version compatibility.
DeploymentPolicy SupportedPolicy();

enum class PatchState {
    Original,
    Installed,
    OlderProxy,
    Recoverable,
    MissingBackup,
    Conflict
};

struct PatchInspection {
    PatchState state = PatchState::Conflict;
    DWORD error = ERROR_SUCCESS;
    bool backupPresent = false;
    std::wstring currentSha256;
    std::wstring backupSha256;
};

struct PatchResult {
    bool ok = false;
    bool changed = false;
    DWORD error = ERROR_SUCCESS;
};

// No process termination, registry writes or target execution. All writes are
// confined to dir/quazip.dll, its verified backup and temporary sibling files.
// Inspect is read-only. Unsupported hosts and unknown PE images fail closed,
// including unsupported PE layouts. Recoverable denotes a missing current file
// or damaged bytes with no recognizable PE header and a verified backup; only
// Restore repairs this state. Restore retains the
// verified quazip_orig.dll even after success. PatchResult::changed describes
// quazip.dll only (a failed Install may have safely published its backup).
// Hash mismatch: ERROR_CRC; unsupported/unknown valid image: ERROR_REVISION_MISMATCH;
// malformed input/policy: ERROR_INVALID_DATA/ERROR_INVALID_PARAMETER. Win32 I/O
// failures, including ERROR_SHARING_VIOLATION, retain their exact error code.
class Deployment {
public:
    Deployment(Bytes payload, DeploymentPolicy policy);
    PatchInspection Inspect(const std::filesystem::path& dir) const;
    PatchResult Install(const std::filesystem::path& dir) const;
    PatchResult Restore(const std::filesystem::path& dir) const;
    bool HostSupported(const std::filesystem::path& dir) const;
    const std::wstring& PayloadSha256() const { return payloadSha256_; }
private:
    Bytes payload_;
    std::wstring payloadSha256_;
    DeploymentPolicy policy_;
};
} // namespace forge
