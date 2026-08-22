// ================================================================
// disk_manager.h — 磁盘管理器
// 功能：分区列表、格式化、挂载/卸载、磁盘状态
// ================================================================
#pragma once
#include "widgets.h"
#include "desktop.h"
#include "vfs.h"

class DiskManagerApp {
public:
    DiskManagerApp(Desktop* desktop, VirtualFileSystem* vfs);
    void show();
private:
    Desktop* desktop;
    VirtualFileSystem* vfs;
    int window_id;
    ListBox* partition_list;
    Button* mount_btn;
    Button* unmount_btn;
    Button* format_btn;
    Label* status_label;
    ProgressBar* usage_bar;
    void refresh();
    void mount();
    void unmount();
    void format();
};