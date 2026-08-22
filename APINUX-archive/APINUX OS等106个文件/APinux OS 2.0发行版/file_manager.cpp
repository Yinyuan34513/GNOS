// ================================================================
// file_manager.cpp — 文件管理器实现
// ================================================================
#include "file_manager.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

FileManagerApp::FileManagerApp(Desktop* d, VirtualFileSystem* v) : desktop(d), vfs(v) {
    window_id = d->create_window("文件管理器", 600, 450);
    strcpy(current_path, "/");
    clipboard_path[0] = '\0';
    clipboard_is_cut = false;
    
    // 路径栏
    path_box = new TextBox(20, 30, 500);
    d->add_widget(window_id, path_box);
    Button* go_btn = new Button(530, 30, 50, 22, "转到", [this]() {
        navigate(path_box->get_text());
    });
    d->add_widget(window_id, go_btn);
    
    // 文件列表
    const char* empty[] = {"(空)"};
    file_list = new ListBox(20, 60, 560, 280, empty, 1, [this](int idx) {
        if (idx >= 0 && idx < file_count) {
            open_file(file_names[idx]);
        }
    });
    d->add_widget(window_id, file_list);
    
    // 操作按钮
    copy_btn = new Button(20, 350, 80, 28, "复制", [this]() { copy(); });
    d->add_widget(window_id, copy_btn);
    paste_btn = new Button(110, 350, 80, 28, "粘贴", [this]() { paste(); });
    d->add_widget(window_id, paste_btn);
    delete_btn = new Button(200, 350, 80, 28, "删除", [this]() { remove(); });
    d->add_widget(window_id, delete_btn);
    rename_btn = new Button(290, 350, 80, 28, "重命名", [this]() { rename_file(); });
    d->add_widget(window_id, rename_btn);
    
    status_label = new Label(20, 390, "就绪", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, status_label);
    
    refresh();
}

void FileManagerApp::navigate(const char* path) {
    if (!path || path[0] == '\0') return;
    strncpy(current_path, path, 255);
    current_path[255] = '\0';
    refresh();
}

void FileManagerApp::refresh() {
    current_dir = vfs->lookup(current_path);
    if (!current_dir) {
        file_count = 0;
        status_label->text = "目录不存在";
        return;
    }
    
    file_count = 0;
    for (VNode* child = current_dir->children; child; child = child->next_sibling) {
        if (file_count < 256) {
            char name_buf[256];
            if (child->type == 1) { // 目录
                snprintf(name_buf, 256, "[%s]", child->name);
            } else {
                snprintf(name_buf, 256, " %s  (%d B)", child->name, (int)child->size);
            }
            strncpy(file_names[file_count], name_buf, 255);
            file_count++;
        }
    }
    
    // 更新列表
    const char* items[256];
    for (int i = 0; i < file_count; ++i) items[i] = file_names[i];
    if (file_count == 0) {
        items[0] = "(空)";
        file_count = 1;
    }
    delete file_list;
    file_list = new ListBox(20, 60, 560, 280, items, file_count, [this](int idx) {
        if (idx >= 0 && idx < file_count) {
            open_file(file_names[idx]);
        }
    });
    desktop->add_widget(window_id, file_list);
    
    // 更新路径框
    // path_box 需要通过其他方式更新文本，此处简化
    char msg[128];
    snprintf(msg, 128, "共 %d 个项目", file_count);
    status_label->text = msg;
}

void FileManagerApp::open_file(const char* name) {
    char real_name[256];
    // 去除目录标记 [ ]
    if (name[0] == '[') {
        size_t len = strlen(name);
        strncpy(real_name, name + 1, len - 2);
        real_name[len - 2] = '\0';
    } else {
        strncpy(real_name, name + 1, strchr(name, '(') ? strchr(name, '(') - name - 2 : strlen(name) - 1);
        real_name[strlen(real_name)] = '\0';
    }
    
    char full_path[512];
    snprintf(full_path, 512, "%s/%s", current_path, real_name);
    
    VNode* node = vfs->lookup(full_path);
    if (!node) return;
    
    if (node->type == 1) { // 目录
        navigate(full_path);
    } else {
        // 文件预览（简化：显示大小）
        char info[256];
        snprintf(info, 256, "文件: %s, 大小: %d B", real_name, (int)node->size);
        status_label->text = info;
    }
}

void FileManagerApp::copy() {
    int idx = file_list->selected;
    if (idx >= 0 && idx < file_count) {
        char name[256];
        // 提取实际文件名
        strncpy(name, file_names[idx], 255);
        if (name[0] == '[') {
            size_t len = strlen(name);
            strncpy(name, name + 1, len - 2);
            name[len - 2] = '\0';
        }
        snprintf(clipboard_path, 256, "%s/%s", current_path, name);
        clipboard_is_cut = false;
        status_label->text = "已复制到剪贴板";
    }
}

void FileManagerApp::paste() {
    if (clipboard_path[0] == '\0') return;
    char dest[512];
    snprintf(dest, 512, "%s/%s", current_path, strrchr(clipboard_path, '/') + 1);
    // 复制文件内容（简化，实际需要 VFS 支持）
    VNode* src = vfs->lookup(clipboard_path);
    VNode* dst = vfs->create_file(dest);
    if (src && dst) {
        uint8_t* buf = (uint8_t*)malloc(src->size);
        vfs->read_file(src, buf, 0, src->size);
        vfs->write_file(dst, buf, 0, src->size);
        free(buf);
        status_label->text = "粘贴成功";
        if (clipboard_is_cut) {
            vfs->delete_file(clipboard_path);
            clipboard_path[0] = '\0';
        }
        refresh();
    }
}

void FileManagerApp::remove() {
    int idx = file_list->selected;
    if (idx >= 0 && idx < file_count) {
        char name[256];
        strncpy(name, file_names[idx], 255);
        if (name[0] == '[') {
            size_t len = strlen(name);
            strncpy(name, name + 1, len - 2);
            name[len - 2] = '\0';
        }
        char full_path[512];
        snprintf(full_path, 512, "%s/%s", current_path, name);
        vfs->delete_file(full_path);
        status_label->text = "已删除";
        refresh();
    }
}

void FileManagerApp::rename_file() {
    // 简化实现：弹出重命名对话框（需要额外控件支持）
    status_label->text = "重命名功能需要额外输入框";
}