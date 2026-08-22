// ================================================================
// browser.cpp — APinux OS 内置极简浏览器
// 依赖：netstack.h, gui.h, vfs.h, kernel.h
// 功能：HTTP/1.1 请求, HTML 解析, 页面渲染, 超链接导航
// ================================================================
#include "netstack.h"
#include "gui.h"
#include "vfs.h"
#include "config.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

// ---------- 浏览器状态 ----------
class WebBrowser {
public:
    WebBrowser(NetStack* net, Desktop* desk) : net_stack(net), desktop(desk) {}

    // 导航到 URL
    bool navigate(const char* url);

    // 渲染当前页面
    void render();

    // 处理键盘事件（方向键滚动）
    void handle_key(int key);

    // 处理鼠标点击（超链接选择）
    void handle_mouse(int x, int y);

private:
    NetStack* net_stack;
    Desktop* desktop;
    Window* browser_window = nullptr;
    char current_url[256] = "";
    char page_title[128] = "";
    char* html_content = nullptr;
    size_t html_size = 0;
    int scroll_y = 0;
    int max_scroll = 0;
    static constexpr int MAX_URL = 256;

    // 解析的 DOM 节点
    struct DOMNode {
        enum Type { TEXT, HEADING, PARAGRAPH, LINK, LINE_BREAK, LIST_ITEM, UNKNOWN };
        Type type;
        char content[256];
        char href[256];         // 链接地址
        int x, y, width, height; // 渲染位置
        DOMNode* next = nullptr;
    };
    DOMNode* dom_root = nullptr;

    // HTTP 请求
    bool fetch_url(const char* url, char** out_html, size_t* out_len);

    // HTML 解析
    void parse_html(const char* html, size_t len);

    // 渲染 DOM 到窗口
    void render_dom();

    // 释放 DOM
    void free_dom();
};

// ---------- HTTP 请求实现 ----------
bool WebBrowser::fetch_url(const char* url, char** out_html, size_t* out_len) {
    // 解析 URL
    const char* http_prefix = "http://";
    if (strncmp(url, http_prefix, 7) != 0) return false;
    const char* host_start = url + 7;
    const char* path_start = strchr(host_start, '/');
    char host[64];
    if (path_start) {
        size_t host_len = path_start - host_start;
        if (host_len >= 64) return false;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
    } else {
        strncpy(host, host_start, 63);
        host[63] = '\0';
        path_start = "/";
    }

    // DNS 解析 (简化：使用 hosts 文件或内置映射)
    uint32_t ip = 0;
    // 尝试从配置文件获取 IP
    FILE* hosts_file = fopen("/etc/hosts", "r");
    if (hosts_file) {
        char line[256];
        while (fgets(line, sizeof(line), hosts_file)) {
            char h[64]; uint32_t ip_addr;
            if (sscanf(line, "%x %s", &ip_addr, h) == 2) {
                if (strcmp(h, host) == 0) { ip = ip_addr; break; }
            }
        }
        fclose(hosts_file);
    }
    if (ip == 0) return false; // DNS 失败

    // 建立 TCP 连接 (端口 80)
    int conn_id = net_stack->tcp_connect(ip, 80);
    if (conn_id < 0) return false;

    // 构造 HTTP GET 请求
    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path_start ? path_start : "/", host);
    net_stack->tcp_send(conn_id, request, strlen(request));

    // 接收响应
    char response[16384]; // 16KB
    size_t total = 0;
    while (total < sizeof(response) - 1) {
        size_t len = sizeof(response) - 1 - total;
        if (!net_stack->tcp_recv(conn_id, response + total, &len)) break;
        total += len;
    }
    response[total] = '\0';
    net_stack->tcp_close(conn_id);

    // 分离 HTTP 头部和主体
    const char* body = strstr(response, "\r\n\r\n");
    if (!body) return false;
    body += 4;
    size_t body_len = total - (body - response);

    // 复制 HTML 内容
    *out_html = (char*)malloc(body_len + 1);
    memcpy(*out_html, body, body_len);
    (*out_html)[body_len] = '\0';
    *out_len = body_len;
    return true;
}

// ---------- HTML 解析 ----------
void WebBrowser::parse_html(const char* html, size_t len) {
    free_dom(); // 清除旧 DOM
    dom_root = nullptr;
    DOMNode* tail = nullptr;

    const char* p = html;
    const char* end = html + len;
    DOMNode::Type current_type = DOMNode::UNKNOWN;

    while (p < end) {
        if (*p == '<') {
            const char* tag_start = p + 1;
            const char* tag_end = strchr(tag_start, '>');
            if (!tag_end) break;
            size_t tag_len = tag_end - tag_start;
            p = tag_end + 1;

            // 解析标签名
            char tag[32] = {0};
            if (tag_len < 32) strncpy(tag, tag_start, tag_len);

            // 闭合标签：回到默认类型
            if (tag[0] == '/') {
                current_type = DOMNode::UNKNOWN;
                continue;
            }

            // 打开标签
            if (strncmp(tag, "h1", 2) == 0 || strncmp(tag, "h2", 2) == 0 ||
                strncmp(tag, "h3", 2) == 0 || strncmp(tag, "h4", 2) == 0 ||
                strncmp(tag, "h5", 2) == 0 || strncmp(tag, "h6", 2) == 0) {
                current_type = DOMNode::HEADING;
            } else if (strncmp(tag, "p", 1) == 0) {
                current_type = DOMNode::PARAGRAPH;
            } else if (strncmp(tag, "a", 1) == 0) {
                current_type = DOMNode::LINK;
                // 提取 href
                const char* href_start = strstr(tag, "href=\"");
                if (href_start) {
                    href_start += 6;
                    const char* href_end = strchr(href_start, '"');
                    if (href_end) {
                        size_t hlen = href_end - href_start;
                        // 暂时存储到第一个新创建的链接节点，稍后在添加文本时设置
                    }
                }
            } else if (strncmp(tag, "br", 2) == 0) {
                // 换行
                DOMNode* node = new DOMNode;
                node->type = DOMNode::LINE_BREAK;
                node->next = nullptr;
                if (!dom_root) dom_root = tail = node;
                else { tail->next = node; tail = node; }
                current_type = DOMNode::UNKNOWN;
            } else if (strncmp(tag, "title", 5) == 0) {
                // 标题文本
                const char* title_end = strstr(p, "</title>");
                if (title_end) {
                    size_t tlen = title_end - p;
                    if (tlen < 128) { strncpy(page_title, p, tlen); page_title[tlen] = '\0'; }
                }
            } else if (strncmp(tag, "li", 2) == 0) {
                current_type = DOMNode::LIST_ITEM;
            } else if (strncmp(tag, "ul", 2) == 0 || strncmp(tag, "ol", 2) == 0) {
                current_type = DOMNode::UNKNOWN; // 列表容器不创建节点
            } else {
                current_type = DOMNode::UNKNOWN;
            }
        } else {
            // 提取文本直到下一个 '<'
            const char* text_start = p;
            const char* next_tag = strchr(p, '<');
            if (!next_tag) next_tag = end;
            size_t text_len = next_tag - text_start;
            p = next_tag;

            // 跳过空白文本
            bool only_white = true;
            for (size_t i = 0; i < text_len; ++i) if (!isspace(text_start[i])) { only_white = false; break; }
            if (only_white) continue;

            // 创建 DOM 文本节点
            DOMNode* node = new DOMNode;
            node->type = current_type == DOMNode::UNKNOWN ? DOMNode::TEXT : current_type;
            size_t copy_len = text_len < 255 ? text_len : 255;
            strncpy(node->content, text_start, copy_len);
            node->content[copy_len] = '\0';
            node->href[0] = '\0';
            if (current_type == DOMNode::LINK) {
                // 已解析 href，此处简化，假设 href 存储在全局变量中
                // 实际实现需更复杂的上下文处理
                strncpy(node->href, "http://example.com", 255); // 示例
            }
            node->next = nullptr;
            if (!dom_root) dom_root = tail = node;
            else { tail->next = node; tail = node; }
            current_type = DOMNode::UNKNOWN; // 重置类型
        }
    }
}

// ---------- 渲染 DOM 到窗口 ----------
void WebBrowser::render_dom() {
    if (!browser_window) return;
    // 清空窗口
    browser_window->fill(0xFFFFFFFF); // 白底

    int y = 10 + scroll_y;
    int x = 10;
    const int line_height = 20;
    max_scroll = 0;

    DOMNode* node = dom_root;
    while (node) {
        switch (node->type) {
            case DOMNode::HEADING: {
                browser_window->draw_text(x, y, node->content, 0xFF000000, 2); // 大字体，黑
                y += line_height + 10;
                break;
            }
            case DOMNode::PARAGRAPH:
            case DOMNode::TEXT: {
                // 自动换行简单处理
                char buf[256];
                strncpy(buf, node->content, 255);
                int max_width = browser_window->width() - 20;
                int text_w = browser_window->measure_text(buf, 1);
                if (text_w > max_width) {
                    // 简单按空格分割换行
                    char* word = strtok(buf, " ");
                    while (word) {
                        int w = browser_window->measure_text(word, 1);
                        if (x + w > max_width) { x = 10; y += line_height; }
                        browser_window->draw_text(x, y, word, 0xFF000000, 1);
                        x += w + 5;
                        word = strtok(nullptr, " ");
                    }
                    y += line_height;
                    x = 10;
                } else {
                    browser_window->draw_text(x, y, node->content, 0xFF000000, 1);
                    y += line_height;
                }
                break;
            }
            case DOMNode::LINK: {
                browser_window->draw_text(x, y, node->content, 0xFF0000FF, 1); // 蓝色下划线
                y += line_height;
                break;
            }
            case DOMNode::LINE_BREAK: {
                y += line_height / 2;
                break;
            }
            case DOMNode::LIST_ITEM: {
                browser_window->draw_text(x, y, "• ", 0xFF000000, 1);
                browser_window->draw_text(x + 15, y, node->content, 0xFF000000, 1);
                y += line_height;
                break;
            }
            default: break;
        }
        node = node->next;
        if (y > browser_window->height() + scroll_y) {
            max_scroll = y - browser_window->height();
            break;
        }
    }
    max_scroll = y - browser_window->height() + 20;
    if (max_scroll < 0) max_scroll = 0;
    browser_window->update();
}

// ---------- 导航 ----------
bool WebBrowser::navigate(const char* url) {
    strncpy(current_url, url, MAX_URL - 1);
    char* html = nullptr;
    size_t len = 0;
    if (fetch_url(url, &html, &len)) {
        parse_html(html, len);
        free(html);
        render_dom();
        return true;
    }
    return false;
}

void WebBrowser::render() {
    if (browser_window) render_dom();
}

void WebBrowser::handle_key(int key) {
    if (key == KEY_UP) scroll_y -= 20;
    else if (key == KEY_DOWN) scroll_y += 20;
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    render_dom();
}

void WebBrowser::handle_mouse(int x, int y) {
    // 检测链接点击
    DOMNode* node = dom_root;
    int cy = 10 + scroll_y;
    while (node) {
        if (node->type == DOMNode::LINK) {
            if (y >= cy && y < cy + 20 && x >= 10 && x < 10 + 200) { // 简化点击范围
                if (node->href[0]) {
                    navigate(node->href);
                }
                return;
            }
        }
        cy += 20;
        node = node->next;
    }
}

void WebBrowser::free_dom() {
    DOMNode* node = dom_root;
    while (node) {
        DOMNode* next = node->next;
        delete node;
        node = next;
    }
    dom_root = nullptr;
}

// ---------- 全局浏览器实例 ----------
WebBrowser* g_browser = nullptr;