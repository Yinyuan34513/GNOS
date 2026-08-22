// ================================================================
// disk_manager.cpp — 磁盘管理器实现
// ================================================================
#include "disk_manager.h"

DiskManagerApp::DiskManagerApp(Desktop* d, VirtualFileSystem* v) : desktop(d), vfs(v) {
    window_id = d->create_window("磁盘管理", 450, 350);
    const char* dummy[] = {"disk0", "disk1"};
    partition_list = new ListBox(20, 40, 200, 200, dummy, 2, nullptr);
    d->add_widget(window_id, partition_list);
    mount_btn = new Button(240, 40, 100, 30, "挂载", [this]() { mount(); });
    d->add_widget(window_id, mount_btn);
    unmount_btn = new Button(240, 80, 100, 30, "卸载", [this]() { unmount(); });
    d->add_widget(window_id, unmount_btn);
    format_btn = new Button(240, 120, 100, 30, "格式化", [this]() { format(); });
    d->add_widget(window_id, format_btn);
    usage_bar = new ProgressBar(20, 260, 400, 0.3f);
    d->add_widget(window_id, usage_bar);
    status_label = new Label(20, 290, "磁盘就绪", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, status_label);
}

void DiskManagerApp::refresh() {
    // 更新分区列表和容量条
}

void DiskManagerApp::mount() {
    vfs->mount("/dev/sda", FS_APFS);
    status_label->text = "已挂载";
}

void DiskManagerApp::unmount() {
    vfs->unmount();
    status_label->text = "已卸载";
}

void DiskManagerApp::format() {
    status_label->text = "格式化完成";
}