// 010 Editor 17.0.1 (x64) 汉化补丁
// 原理：IAT Hook QCoreApplication::translate，把 sourceText 查表替换为 UTF-8 中文，
//       再调用 Qt 的 QString::fromUtf8 构造返回值。不修改原 exe，可随时移除。
#include <windows.h>
#include <unordered_map>
#include <string>
#include "mem_patch.h"

// Qt6 的 QString 在 x64 下是 24 字节（一个 QArrayDataPointer<QChar>）。
// 我们不需要知道其内部结构：fromUtf8 和 translate 都按“返回 24 字节对象”
// 的 ABI 工作，用一个 24 字节的不透明占位表示即可。
struct QStringOpaque { unsigned char raw[24]; };

typedef QStringOpaque(__cdecl* PFN_TRANSLATE)(const char*, const char*, const char*, int);
// QMetaObject::tr: this, [ret QString], sourceText, disambiguation, n
typedef void(__cdecl* PFN_MTR)(void*, void*, const char*, const char*, int);
typedef QStringOpaque(__cdecl* PFN_FROMUTF8)(const char*, long long);

static PFN_TRANSLATE g_origTranslate = nullptr;
static PFN_MTR       g_origMetaTr = nullptr;
static PFN_FROMUTF8  g_fromUtf8 = nullptr;

// context 无关，按 sourceText 精确匹配。英文原文 -> 简体中文 UTF-8。
static const std::unordered_map<std::string, std::string> DICT = {
#include "dict_gen.inc"
};

static QStringOpaque __cdecl HookedTranslate(const char* context, const char* sourceText,
                                             const char* disambiguation, int n)
{
    if (sourceText) {
        auto it = DICT.find(sourceText);
        if (it != DICT.end() && g_fromUtf8) {
            return g_fromUtf8(it->second.c_str(), -1);
        }
    }
    return g_origTranslate(context, sourceText, disambiguation, n);
}

// QMetaObject::tr(this, ret, sourceText, disambiguation, n)
static void __cdecl HookedMetaTr(void* self, void* ret, const char* sourceText,
                                 const char* disambiguation, int n)
{
    if (sourceText) {
        auto it = DICT.find(sourceText);
        if (it != DICT.end() && g_fromUtf8) {
            QStringOpaque q = g_fromUtf8(it->second.c_str(), -1);
            memcpy(ret, &q, sizeof(q));
            return;
        }
    }
    g_origMetaTr(self, ret, sourceText, disambiguation, n);
}

// 把 IAT 槽里的函数指针换成我们的 hook
static void ApplyIatHook()
{
    HMODULE self = GetModuleHandleW(nullptr); // 010Editor.exe
    if (!self) return;
    DWORD old = 0;
    // QCoreApplication::translate 导入槽 RVA = 0x10B69A0
    BYTE* s1 = (BYTE*)self + 0x10B69A0;
    if (VirtualProtect(s1, sizeof(void*), PAGE_READWRITE, &old)) {
        g_origTranslate = *(PFN_TRANSLATE*)s1;
        *(PFN_TRANSLATE*)s1 = &HookedTranslate;
        VirtualProtect(s1, sizeof(void*), old, &old);
    }
    // QMetaObject::tr 导入槽 RVA = 0x10B6790
    BYTE* s2 = (BYTE*)self + 0x10B6790;
    if (VirtualProtect(s2, sizeof(void*), PAGE_READWRITE, &old)) {
        g_origMetaTr = *(PFN_MTR*)s2;
        *(PFN_MTR*)s2 = &HookedMetaTr;
        VirtualProtect(s2, sizeof(void*), old, &old);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HMODULE qt = GetModuleHandleW(L"Qt6Core.dll");
        if (qt) {
            g_fromUtf8 = (PFN_FROMUTF8)GetProcAddress(qt, "?fromUtf8@QString@@SA?AV1@PEBD_J@Z");
        }
        if (g_fromUtf8) ApplyIatHook();
        // patch 走 QString::fromUtf8 的硬编码 UI 字符串（资源库安装对话框等）
        ApplyStringPatches();
    }
    return TRUE;
}
