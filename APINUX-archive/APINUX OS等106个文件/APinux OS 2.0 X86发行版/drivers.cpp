// ================================================================
// drivers.cpp — 跨架构驱动实现
// 构建修复版：原文件 x86 分支引用了不存在的 SerialPort/PIT/PS2Keyboard/
// PS2Mouse 类，无法编译；改为最小桩实现。串口输出由 userlib 的
// serial_write()（x86 走 COM1 0x3F8）直接驱动。
// ================================================================
#include "drivers.h"
#include "config.h"

void drivers_init() {
    // x86_64：无真实设备接入；V4L2/WLAN/BLU/USB/ETH 均为声明桩
}

// ---- USB 主机控制器桩（若后续接入 usb_mouse_final.o 需要） ----
bool USBHostController::init(uint32_t mmio_addr) { (void)mmio_addr; return false; }
bool USBHostController::enumerate() { return false; }
bool USBHostController::bulk_transfer(int endpoint, void* buf, size_t len) {
    (void)endpoint; (void)buf; (void)len; return false;
}
bool USBHostController::control_transfer(uint8_t request, uint8_t* buf, size_t len) {
    (void)request; (void)buf; (void)len; return false;
}

// ---- WLAN / 蓝牙驱动桩（GUI 应用会调用，无真实硬件） ----
bool WLANDriver::init(uint32_t mmio_addr) { (void)mmio_addr; return false; }
bool WLANDriver::scan(char ssid_list[][32], int* count) {
    (void)ssid_list; *count = 0; return true;
}
bool WLANDriver::connect(const char* ssid, const char* password) {
    (void)ssid; (void)password; return false;
}
bool WLANDriver::send(const void* data, size_t len) { (void)data; (void)len; return false; }
bool WLANDriver::recv(void* buf, size_t* len) { (void)buf; (void)len; return false; }

bool BLUDriver::init(uint32_t mmio_addr) { (void)mmio_addr; return false; }
bool BLUDriver::discover(uint8_t addr_list[][6], int* count) {
    (void)addr_list; *count = 0; return true;
}
bool BLUDriver::pair(const uint8_t addr[6]) { (void)addr; return false; }
bool BLUDriver::connect(const uint8_t addr[6]) { (void)addr; return false; }
bool BLUDriver::send_data(const uint8_t addr[6], const void* data, size_t len) {
    (void)addr; (void)data; (void)len; return false;
}
bool BLUDriver::recv_data(uint8_t* addr, void* buf, size_t* len) {
    (void)addr; (void)buf; (void)len; return false;
}
