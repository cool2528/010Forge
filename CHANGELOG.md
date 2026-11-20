# Changelog / 更新记录

## 4.1.0 / Tracker Edition

- Keep the original **B / Terminal Pulse** score and voices; accelerate 148 → **168 BPM** (11.43-second stereo loop).
- Dark Tracker-style GDI theme with monospaced type, green event console, cyan borders and amber Restore action.
- Retain the music toggle, all five action buttons and input-preserving EN/ZH switching.
- Replace historical GUI documentation images with current EN/ZH preview window captures.
- Private-desktop capture checks control bounds, equal button sizes and DPI-correct bitmap coordinates; no user desktop or audio interaction.

## 4.0.0

### Added / 新增
- GUI deployment of the embedded quazip localization proxy, plus a Restore original button.
- SHA-256 based installation/backup identity and staged atomic file replacement.
- Recovery tests for missing/damaged DLLs, conflicting backups and sharing locks.
- Original dark cyber-electronic loop **Grid After Midnight** (100 BPM, 19.2 s), with playback toggle.
- Separate Chinese and English READMEs, consistent buttons and stable label IDs.
- CTest coverage for resources, music, languages and existing codecs.

### Fixed / 修复
- Deploy now installs the automatic-load proxy instead of only extracting the session-injection DLL.
- Restore retains the verified original backup; no delete-first window.
- No forced termination of 010 Editor instances.
- Typed directory paths are used by every file operation.
- Executable paths containing spaces are quoted correctly on launch.
- Language switching preserves inputs; labels do not depend on child-window order.
- Removed the scan thread's stack-context lifetime error.
- Existing hex parser used multi-character constants instead of numeric offsets.
- Updated DLL/icon/WAV changes regenerate the embedded resources; build products stay out of source.
- CI packages README images, tests before publishing, and guards Release publishing to pushed tags.

### Compatibility / 兼容性
- Verified against the pinned 010 Editor 17.0.1 x64 and original quazip.dll identities only.
- zhcn.dll is optional; normal GUI use requires only 010Forge.exe.
- Translation is partial; future application releases are not implicitly supported.
