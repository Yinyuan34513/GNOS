// ================================================================
// bluetooth_app.h — 蓝牙图形化配置应用
// 功能：扫描设备、配对、连接、断开
// ================================================================
#pragma once
#include "widgets.h"
#include "desktop.h"
#include "drivers.h"

class BluetoothApp {
public:
    BluetoothApp(Desktop* desktop, BLUDriver* blu);
    void show();
private:
    Desktop* desktop;
    BLUDriver* blu;
    int window_id;
    ListBox* device_list;
    Button* scan_btn;
    Button* pair_btn;
    Button* connect_btn;
    Label* status_label;
    uint8_t device_addrs[32][6];
    int device_count;
    void scan_devices();
    void pair();
    void connect();
};