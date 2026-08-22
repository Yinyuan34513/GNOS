// ================================================================
// config.h — APinux OS 配置管理
// 功能：系统参数、网络设置、启动项、环境变量
// ================================================================
#pragma once
#include "kernel.h"
#include "userlib.h"   // 构建修复：FILE 类型与 fopen/fread/fclose 声明
#include <cstdint>

struct SysConfig {
    // 显示设置
    int screen_width = 1024;
    int screen_height = 600;
    uint32_t wallpaper_color = 0xFF1A1A2E;
    
    // 网络设置
    char hostname[64] = "apinux";
    uint8_t mac_addr[6] = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    char wlan_ssid[32] = "";
    char wlan_password[32] = "";
    uint32_t ip_addr = 0xC0A8000A;  // 192.168.0.10
    
    // AI 引擎设置
    enum AIEngine { AI_ACPNN, AI_ACPU, AI_PCP, AI_OPENPUFF, AI_A2 } ai_engine = AI_ACPNN;
    char ai_model_path[128] = "/models/default.bin";
    
    // 启动项
    char autostart_app[128] = "";  // 开机自启动应用
    bool show_boot_anim = true;
    
    // 权限
    bool allow_multiuser = false;
    char admin_password[32] = "apinux";
};

class ConfigManager {
    SysConfig cfg;
    const char* config_file = "/etc/apinux.conf";
public:
    bool load();
    bool save();
    SysConfig& get() { return cfg; }
    void set(const SysConfig& c) { cfg = c; }
    
    // 便捷访问
    const char* get_hostname() const { return cfg.hostname; }
    uint8_t* get_mac() { return cfg.mac_addr; }
    const char* get_model_path() const { return cfg.ai_model_path; }
};

// 实现
inline bool ConfigManager::load() {
    // 从配置文件读取 (假设存在)
    FILE* f = fopen(config_file, "r");
    if (!f) return false;
    fread(&cfg, sizeof(SysConfig), 1, f);
    fclose(f);
    return true;
}

inline bool ConfigManager::save() {
    FILE* f = fopen(config_file, "w");
    if (!f) return false;
    fwrite(&cfg, sizeof(SysConfig), 1, f);
    fclose(f);
    return true;
}