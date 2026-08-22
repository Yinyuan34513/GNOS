// ================================================================
// usb_mouse_final.cpp — USB HID 鼠标驱动最终版
// 功能：解析标准鼠标报告，支持滚轮和多键
// ================================================================
#include "drivers.h"
#include "mouse.h"
#include <cstring>

class USBHIDMouseFinal : public MouseDevice {
    USBHostController* usb;
    int endpoint_addr;
    uint8_t report[8];   // 最多8字节报告
    int report_len = 4;
    
    int cursor_x = 512, cursor_y = 300;
    
public:
    USBHIDMouseFinal(USBHostController* u, int ep) : usb(u), endpoint_addr(ep) {}
    
    bool init() override {
        // 发送 SET_IDLE 请求（HID 标准）
        uint8_t idle_req[8] = {0x21, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        usb->control_transfer(0x21, idle_req, 8);
        return true;
    }
    
    bool poll(MouseEvent* out) override {
        size_t len = report_len;
        if (!usb->bulk_transfer(endpoint_addr, report, len))
            return false;
        
        // 解析标准鼠标报告（3字节基础 + 可选滚轮）
        out->buttons = report[0] & 0x07;           // 左/右/中键
        out->dx = (int)(int8_t)report[1];          // X 相对移动
        out->dy = (int)(int8_t)report[2];          // Y 相对移动
        out->wheel = 0;
        
        // 如果报告长度 >= 4，解析滚轮（第4字节）
        if (report_len >= 4) {
            out->wheel = (int)(int8_t)report[3];
        }
        
        // 更新光标位置
        cursor_x = std::clamp(cursor_x + out->dx, 0, screen_w - 1);
        cursor_y = std::clamp(cursor_y + out->dy, 0, screen_h - 1);
        
        // 将绝对坐标回填到事件中（上层可直接使用）
        out->dx = cursor_x;
        out->dy = cursor_y;
        
        return true;
    }
    
    void get_cursor(int* x, int* y) { *x = cursor_x; *y = cursor_y; }
};

// 全局鼠标实例
USBHIDMouseFinal* g_mouse = nullptr;

// 初始化鼠标（在 USB 枚举完成后调用）
void init_usb_mouse(USBHostController* usb, int endpoint) {
    g_mouse = new USBHIDMouseFinal(usb, endpoint);
    g_mouse->init();
}