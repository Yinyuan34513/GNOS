// ================================================================
// drivers.h — APinux OS 设备驱动框架
// 支持：V4L2 摄像头, WLAN, 蓝牙, USB, 以太网
// ================================================================
#pragma once
#include "kernel.h"
#include <cstdint>

// ---------- 通用设备接口 ----------
struct Device {
    uint32_t id;
    char name[32];
    int type;               // 0=V4L2, 1=WLAN, 2=BLU, 3=USB, 4=ETH
    void* mmio_base;
    uint32_t irq;
    bool (*init)(Device*);
    bool (*read)(Device*, void* buf, size_t len);
    bool (*write)(Device*, const void* buf, size_t len);
    Device* next;
};

class DeviceManager {
    Device* dev_list = nullptr;
public:
    void register_device(Device* dev);
    Device* find_by_type(int type);
    Device* find_by_name(const char* name);
};

// ---------- V4L2 摄像头驱动 ----------
class V4L2Driver {
    Device dev;
    volatile uint32_t* regs;
public:
    bool init(uint32_t mmio_addr);
    bool capture_frame(void* buf, size_t* size);
};

// ---------- WLAN 驱动 (简化，直接访问MMIO) ----------
class WLANDriver {
    Device dev;
public:
    bool init(uint32_t mmio_addr);
    bool scan(char ssid_list[][32], int* count);
    bool connect(const char* ssid, const char* password);
    bool send(const void* data, size_t len);
    bool recv(void* buf, size_t* len);
};

// ---------- 蓝牙驱动 ----------
class BLUDriver {
    Device dev;
public:
    bool init(uint32_t mmio_addr);
    bool discover(uint8_t addr_list[][6], int* count);
    bool pair(const uint8_t addr[6]);
    bool send_data(const uint8_t addr[6], const void* data, size_t len);
    bool recv_data(uint8_t* addr, void* buf, size_t* len);
};

// ---------- USB 主机控制器 (OHCI/EHCI 简化) ----------
class USBHostController {
    Device dev;
    volatile uint32_t* op_regs;
public:
    bool init(uint32_t mmio_addr);
    bool enumerate();       // 扫描端口，分配地址
    bool bulk_transfer(int endpoint, void* buf, size_t len);
    bool control_transfer(uint8_t request, uint8_t* buf, size_t len);
};

// ---------- 以太网驱动 ----------
class EthernetDriver {
    Device dev;
    volatile uint32_t* mac_regs;
    uint8_t mac_addr[6];
public:
    bool init(uint32_t mmio_addr);
    bool send_packet(const void* data, size_t len);
    bool recv_packet(void* buf, size_t* len);
    void set_mac(const uint8_t addr[6]);
};

// ---------- 多机互联调度（以太网） ----------
class ClusterScheduler {
    EthernetDriver* eth;
public:
    void attach(EthernetDriver* driver) { eth = driver; }
    bool broadcast_task(uint32_t task_id, const void* data, size_t len);
    bool recv_task(uint32_t* task_id, void* buf, size_t* len);
};