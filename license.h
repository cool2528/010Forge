// 010 Editor 序列号算法（跨版本通用核心）
#pragma once
#include <string>

struct LicenseInfo {
    int     users;       // 用户数（站点码=2）
    int     licnum;      // 许可证编号
    int     supportDays; // 支持天数（0=单用户码无日期）
    int     type;        // 0x9C=单用户, 0xAC=站点
    std::string serial;
    std::string name;
};

class License {
public:
    // 生成站点授权码（带支持到期日）
    // name: 用户名; licnum: 许可证号(1..5000); supportDays: 自1970-01-02起的天数
    static std::string GenSite(const std::string& name, int licnum, int supportDays);

    // 生成单用户码（无日期字段，UI 会显示 1970）
    static std::string GenSingle(const std::string& name, int users, int licnum);

    // 校验码是否有效，返回状态码（45/78/147/231 等，45=有效）
    static int Validate(const std::string& name, const std::string& serial, LicenseInfo* out = nullptr);

    // 日期工具：某年1月1日对应的 supportDays
    static int DaysForYear(int year);

    // 从 exe 动态提取哈希表（返回是否成功）；失败则用内置表
    static bool LoadTableFromExe(const std::wstring& exePath);
};
