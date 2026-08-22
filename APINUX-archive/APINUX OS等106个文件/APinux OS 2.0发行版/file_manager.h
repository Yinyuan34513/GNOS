// ================================================================
// file_manager.h — 图形化文件管理器
// 功能：目录浏览、文件操作（复制、粘贴、删除、重命名）、文件预览
// ================================================================
#pragma once
#include "widgets.h"
#include "desktop.h"
#include "vfs.h"

class FileManagerApp {
public:
    FileManagerApp(Desktop* desktop, VirtualFileSystem* vfs);
    void show();
    void refresh();
    
private:
    Desktop* desktop;
    VirtualFileSystem* vfs;
    int window_id;
    ListBox* file_list;
    Button* copy_btn;
    Button* paste_btn;
    Button* delete_btn;
    Button* rename_btn;
    TextBox* path_box;
    Label* status_label;
    char current_path[256];
    VNode* current_dir;
    char clipboard_path[256];
    bool clipboard_is_cut;
    int file_count;
    char file_names[256][256];
    
    void navigate(const char* path);
    void open_file(const char* name);
    void copy();
    void paste();
    void remove();
    void rename_file();
};