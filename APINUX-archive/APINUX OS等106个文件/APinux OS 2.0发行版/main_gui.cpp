// ================================================================
// main_gui.cpp — APinux OS 新界面启动入口
// 功能：初始化桌面、启动系统应用、进入事件循环
// ================================================================
#include "desktop.h"
#include "wifi_app.h"
#include "bluetooth_app.h"
#include "settings_app.h"
#include "disk_manager.h"
#include "file_manager.h"
#include "mouse.h"
#include "drivers.h"

Desktop* g_desktop = nullptr;
WiFiApp* g_wifi_app = nullptr;
BluetoothApp* g_bluetooth_app = nullptr;
SettingsApp* g_settings_app = nullptr;
DiskManagerApp* g_disk_app = nullptr;
FileManagerApp* g_file_app = nullptr;

void init_gui_apps() {
    g_desktop = new Desktop();
    
    // 创建开始菜单中的应用快捷方式（可通过任务栏或桌面图标启动）
    // 这里直接预创建窗口，实际产品中可改为按需创建
    g_wifi_app = new WiFiApp(g_desktop, &g_wlan_driver);
    g_bluetooth_app = new BluetoothApp(g_desktop, &g_blu_driver);
    g_settings_app = new SettingsApp(g_desktop, &g_config_mgr);
    g_disk_app = new DiskManagerApp(g_desktop, &g_vfs);
    g_file_app = new FileManagerApp(g_desktop, &g_vfs);
}

void gui_event_loop() {
    int cursor_x = 512, cursor_y = 300;
    bool last_click = false;
    
    while (true) {
        // 轮询鼠标
        if (g_mouse) {
            MouseEvent ev;
            if (g_mouse->poll(&ev)) {
                cursor_x = ev.dx; // 绝对坐标
                cursor_y = ev.dy;
                bool current_click = (ev.buttons & 0x01);
                
                // 仅当鼠标移动或点击状态变化时才重绘
                if (cursor_x != last_cursor_x || cursor_y != last_cursor_y || current_click != last_click) {
                    g_desktop->handle_mouse(cursor_x, cursor_y, current_click && !last_click);
                    last_cursor_x = cursor_x;
                    last_cursor_y = cursor_y;
                    last_click = current_click;
                }
            }
        }
        
        // 绘制桌面
        g_desktop->draw();
        
        // 绘制光标（叠加在桌面之上）
        if (g_mouse) {
            int mx, my;
            g_mouse->get_cursor(&mx, &my);
            draw_cursor(mx, my); // 实现一个简单的箭头光标
        }
        
        // 调度其他进程
        g_proc_mgr.schedule();
    }
}

int main_gui() {
    init_gui_apps();
    gui_event_loop();
    return 0;
}