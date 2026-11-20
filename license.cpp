#include "license.h"
#include "lic_table.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <vector>

// ---------- 基础工具 ----------
static unsigned int HASH[256];
static bool g_tableLoaded = false;

static void EnsureTable() {
    if (!g_tableLoaded) { memcpy(HASH, HASH_TABLE, sizeof(HASH)); g_tableLoaded = true; }
}

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'l' || c == 'L') return 1;
    if (c == 'o' || c == 'O') return 0;
    if (c >= 'a' && c <= 'f') return c - '87';
    if (c >= 'A' && c <= 'F') return c - '55';
    return 0;
}

static bool ParseSerial(const std::string& s, unsigned char* b, int* nbytes) {
    std::string t;
    for (char c : s) if (c != '-' && c != ' ' && c != '\t') t += c;
    if (t.size() != 16 && t.size() != 20) return false;
    for (size_t i = 0; i < t.size(); i += 2)
        b[i/2] = (unsigned char)((HexVal(t[i]) << 4) | HexVal(t[i+1]));
    *nbytes = (int)(t.size()/2);
    return true;
}

static std::string FmtSerial(const unsigned char* b, int n) {
    char buf[64]; int p = 0;
    for (int i = 0; i < n; i += 2) {
        if (i) buf[p++] = '-';
        p += sprintf(buf + p, "%02X%02X", b[i], b[i+1]);
    }
    buf[p] = 0;
    return std::string(buf);
}

static unsigned char DecUsers(unsigned char b) {
    return (unsigned char)((((b ^ 0x18) + 61) & 0xFF) ^ 0xA7);
}
static unsigned char EncUsers(int u) {
    return (unsigned char)((((u ^ 0xA7) - 61) & 0xFF) ^ 0x18);
}
static int DecNum16(unsigned short w) {
    unsigned int v = (((w ^ 0x7892) + 0x4D30) & 0xFFFF) ^ 0x3421;
    return (v % 11 == 0) ? (int)(v / 11) : 0;
}
static unsigned short EncNum16(int n) {
    unsigned int v = (n * 11) & 0xFFFF;
    return (unsigned short)((((v ^ 0x3421) - 0x4D30) & 0xFFFF) ^ 0x7892);
}
static int DecDate24(unsigned int v24, unsigned int k) {
    unsigned int v = ((((k ^ v24 ^ 0x22C078) - 180597) & 0xFFFFFFFFu) ^ 0xFFE53167u) & 0xFFFFFF;
    return (v % 17 == 0) ? (int)(v / 17) : 0;
}
static unsigned int EncDate24(int days, unsigned int k) {
    unsigned int v = (days * 17) & 0xFFFFFF;
    return ((((v ^ 0xFFE53167u) + 180597) & 0xFFFFFFFFu) ^ 0x22C078u) ^ k;
}

static unsigned int NameHash(const std::string& name, int flag, int users, int licnum) {
    EnsureTable();
    unsigned int acc = 0;
    int i17 = (17 * users) & 0xFF;   // 反汇编 imul edi, r8d, 11h (17*users)，勿写成 15
    int i15 = (15 * (licnum & 0xFF)) & 0xFF;
    int c19 = 0, c7 = 0;
    for (unsigned char ch : name) {
        int c = (ch >= 'a' && ch <= 'z') ? (ch - 32) : ch;
        unsigned int t = (acc + HASH[c]) & 0xFFFFFFFFu;
        int x, y;
        if (flag) {
            t ^= HASH[(c + 13) & 0xFF];
            x = (c + 47) & 0xFF; y = c19;
        } else {
            t ^= HASH[(c + 63) & 0xFF];
            x = (c + 23) & 0xFF; y = c7;
        }
        acc = (HASH[x] * t + HASH[y] + HASH[i17] + HASH[i15]) & 0xFFFFFFFFu;
        i17 = (i17 + 9) & 0xFF;
        i15 = (i15 + 13) & 0xFF;
        c19 = (c19 + 19) & 0xFF;
        c7  = (c7 + 7) & 0xFF;
    }
    return acc;
}

// ---------- 对外接口 ----------
std::string License::GenSite(const std::string& name, int licnum, int supportDays) {
    unsigned int crc = NameHash(name, 1, supportDays, licnum);
    unsigned char b[10];
    b[4] = (unsigned char)(crc & 0xFF);
    b[5] = (unsigned char)((crc >> 8) & 0xFF);
    b[6] = (unsigned char)((crc >> 16) & 0xFF);
    b[7] = (unsigned char)((crc >> 24) & 0xFF);
    unsigned short w = EncNum16(licnum);
    b[1] = (unsigned char)((w >> 8) ^ b[7]);
    b[2] = (unsigned char)((w & 0xFF) ^ b[5]);
    unsigned int v24 = EncDate24(supportDays, 5999655);
    b[3] = 0xAC;
    b[0] = (unsigned char)((v24 & 0xFF) ^ b[6]);
    b[8] = (unsigned char)(((v24 >> 8) & 0xFF) ^ b[4]);
    b[9] = (unsigned char)(((v24 >> 16) & 0xFF) ^ b[5]);
    return FmtSerial(b, 10);
}

std::string License::GenSingle(const std::string& name, int users, int licnum) {
    int u = users < 2 ? users : 0;
    unsigned int crc = NameHash(name, 1, u, licnum);
    unsigned char b[8];
    b[4] = (unsigned char)(crc & 0xFF);
    b[5] = (unsigned char)((crc >> 8) & 0xFF);
    b[6] = (unsigned char)((crc >> 16) & 0xFF);
    b[7] = (unsigned char)((crc >> 24) & 0xFF);
    unsigned short w = EncNum16(licnum);
    b[1] = (unsigned char)((w >> 8) ^ b[7]);
    b[2] = (unsigned char)((w & 0xFF) ^ b[5]);
    b[0] = (unsigned char)(EncUsers(users) ^ b[6]);
    b[3] = 0x9C;
    return FmtSerial(b, 8);
}

int License::Validate(const std::string& name, const std::string& serial, LicenseInfo* out) {
    unsigned char b[10]; int n = 0;
    if (out) *out = LicenseInfo{};   // 初始化，避免校验失败时显示垃圾值
    if (!ParseSerial(serial, b, &n)) return 0;
    unsigned int crc = 0;
    int type = b[3];
    if (type == 0x9C) {
        int users = DecUsers(b[0] ^ b[6]);
        int num = DecNum16((unsigned short)((b[2] ^ b[5]) | ((b[1] ^ b[7]) << 8)));
        if (!users || num < 1 || num > 1000) return 0;
        int u = users < 2 ? users : 0;
        crc = NameHash(name, 1, u, num);
        if (out) { out->users = users; out->licnum = num; out->supportDays = 0; out->type = type; }
        if ((crc & 0xFF) != b[4] || ((crc>>8)&0xFF) != b[5] ||
            ((crc>>16)&0xFF) != b[6] || ((crc>>24)&0xFF) != b[7]) return 0;
        return (18 > users) ? 78 : 45;
    } else if (type == 0xFC) {
        crc = NameHash(name, 0, 255, 1);
        if (out) { out->users = 255; out->licnum = 1; out->type = type; out->supportDays = DecDate24(b[0]|(b[1]<<8)|(b[2]<<16), crc); }
        if ((crc & 0xFF) != b[4] || ((crc>>8)&0xFF) != b[5] ||
            ((crc>>16)&0xFF) != b[6] || ((crc>>24)&0xFF) != b[7]) return 0;
        return 147;
    } else if (type == 0xAC) {
        int num = DecNum16((unsigned short)((b[2]^b[5]) | ((b[1]^b[7])<<8)));
        if (num < 1 || num > 5000) return 0;
        unsigned int v24 = (b[0]^b[6]) | ((b[4]^b[8])<<8) | ((b[9]^b[5])<<16);
        int days = DecDate24(v24, 5999655);
        if (!days) return 0;
        crc = NameHash(name, 1, days, num);
        if (out) { out->users = 2; out->licnum = num; out->supportDays = days; out->type = type; }
        if ((crc & 0xFF) != b[4] || ((crc>>8)&0xFF) != b[5] ||
            ((crc>>16)&0xFF) != b[6] || ((crc>>24)&0xFF) != b[7]) return 0;
        return (20755 > days) ? 78 : 45;
    }
    return 0;
}

int License::DaysForYear(int year) {
    // 自 1970-01-02 起的天数
    std::tm t = {}; t.tm_year = year - 1900; t.tm_mon = 0; t.tm_mday = 1;
    std::tm e = {}; e.tm_year = 70; e.tm_mon = 0; e.tm_mday = 2;
    std::time_t tt = std::mktime(&t);
    std::time_t te = std::mktime(&e);
    return (int)((tt - te) / 86400);
}

bool License::LoadTableFromExe(const std::wstring& exePath) {
    // 特征搜索：找到 256 DWORD 表。这里用已知偏移 + 校验首值。
    FILE* f = _wfopen(exePath.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0xE8B2F0 + 1024) { fclose(f); return false; }
    // v17.0.1 固定偏移；后续版本可在此扩展特征扫描
    fseek(f, 0xE8B2F0, SEEK_SET);
    unsigned int tmp[256];
    if (fread(tmp, 4, 256, f) != 256) { fclose(f); return false; }
    fclose(f);
    if (tmp[0] != 0x39CB44B8u) return false; // 版本校验
    memcpy(HASH, tmp, sizeof(HASH));
    g_tableLoaded = true;
    return true;
}
