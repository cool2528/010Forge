// 运行时 patch 硬编码字符串（QString::fromUtf8 直接引用，不走 translate）
// 约束：中文 UTF-8 字节数 <= 原 len（len 是编译期立即数），不足补 0
#pragma once
#include <windows.h>
#include <cstring>

struct StrPatch { DWORD rva; int len; const char* zh; };

// VA = imagebase(0x140000000) + rva；这里记 rva（VA-0x140000000）
// len 取反编译里 fromUtf8 的立即数 = 原英文长度
static const StrPatch STR_PATCHES[] = {
    {0x0823508, 14, "<li>类别："},
    {0x0823520, 17, "<ul><li>作者："},
    {0x0823568, 15, "<li>掩码："},
    {0x0823580, 14, "<li>字节："},
    {0x08235a0, 13, "<li>更新："},
    {0x0823f98, 13, "<li>版本："},
    {0x0823d10, 42, "为文件提供语法高亮：'"},
    {0x0823d48, 39, "解析文件的二进制数据：'"},
    {0x0823d78, 56, "'。是否安装并运行其中一个文件？"},
    {0x0823dc0, 86, "010 Editor 资源库中有以下模板，可用于"},
    {0x0823e28, 47, "'。是否安装并运行此文件？"},
    {0x0823e70, 84, "010 Editor 资源库中有以下模板，可用于"},
    {0x0823ed8, 43, "'。是否安装此文件？"},
};

static inline void ApplyStringPatches()
{
    HMODULE self = GetModuleHandleW(nullptr);
    if (!self) return;
    BYTE* base = (BYTE*)self;
    for (const auto& p : STR_PATCHES) {
        BYTE* addr = base + p.rva;
        DWORD old = 0;
        if (!VirtualProtect(addr, p.len, PAGE_READWRITE, &old)) continue;
        size_t n = strlen(p.zh);
        if ((int)n > p.len) n = p.len;
        memcpy(addr, p.zh, n);
        if ((int)n < p.len) memset(addr + n, 0, p.len - n);
        VirtualProtect(addr, p.len, old, &old);
    }
}
