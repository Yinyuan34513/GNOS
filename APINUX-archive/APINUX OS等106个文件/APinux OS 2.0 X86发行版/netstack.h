// ================================================================
// netstack.h — APinux OS 轻量网络协议栈
// 支持：ARP, IPv4, TCP (简化), UDP
// ================================================================
#pragma once
#include "kernel.h"
#include "drivers.h"
#include <cstdint>

// ---------- 以太网帧 ----------
struct EthFrame {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
    uint8_t payload[1500];
} __attribute__((packed));

// ---------- IP 数据包 ----------
struct IPPacket {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t payload[1480];
} __attribute__((packed));

// ---------- TCP 段 ----------
struct TCPSegment {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
    uint8_t options[40];
    uint8_t data[1400];
} __attribute__((packed));

// ---------- UDP 数据报 ----------
struct UDPDatagram {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
    uint8_t data[1472];
} __attribute__((packed));

// ---------- 网络栈 ----------
class NetStack {
    EthernetDriver* eth;
    uint8_t mac[6];
    uint32_t ip;
    uint16_t next_port = 1024;
    
    struct Connection {
        uint32_t remote_ip;
        uint16_t remote_port;
        uint16_t local_port;
        uint32_t seq;
        uint32_t ack;
        bool connected;
        uint8_t recv_buf[4096];
        size_t recv_len;
    };
    Connection conns[4];  // 最多4个连接

public:
    bool init(EthernetDriver* driver, const uint8_t* mac_addr, uint32_t ip_addr);
    
    // ARP
    bool arp_request(uint32_t ip, uint8_t* out_mac);
    bool arp_reply(const EthFrame* frame);
    
    // IP
    bool ip_send(uint32_t dst_ip, uint8_t protocol, const void* data, size_t len);
    bool ip_recv(const IPPacket* packet, void* out, size_t* len);
    
    // TCP
    int tcp_connect(uint32_t ip, uint16_t port);
    bool tcp_send(int conn_id, const void* data, size_t len);
    bool tcp_recv(int conn_id, void* buf, size_t* len);
    void tcp_close(int conn_id);
    
    // UDP
    bool udp_send(uint32_t ip, uint16_t port, const void* data, size_t len);
    bool udp_recv(UDPDatagram* out);
    
    // 主循环处理
    void poll();
};

// 实现 (简化)
inline bool NetStack::init(EthernetDriver* driver, const uint8_t* mac_addr, uint32_t ip_addr) {
    eth = driver;
    memcpy(mac, mac_addr, 6);
    ip = ip_addr;
    for (auto& c : conns) c.connected = false;
    return true;
}

inline void NetStack::poll() {
    uint8_t buf[2048];
    size_t len;
    if (eth->recv_packet(buf, &len)) {
        EthFrame* frame = (EthFrame*)buf;
        if (frame->ethertype == 0x0800) {  // IPv4
            IPPacket* ip_pkt = (IPPacket*)frame->payload;
            if (ip_pkt->protocol == 6) {   // TCP
                // 处理 TCP 段
            } else if (ip_pkt->protocol == 17) { // UDP
                // 处理 UDP 数据报
            }
        } else if (frame->ethertype == 0x0806) { // ARP
            arp_reply(frame);
        }
    }
}