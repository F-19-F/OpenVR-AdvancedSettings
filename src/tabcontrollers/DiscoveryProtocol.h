// =============================================================
// 文件名: DiscoveryProtocol.h
// 作用: 定义通信协议结构体、常量和校验算法
// 兼容性: 适用于 Windows (PC) 和 Linux/Android
// =============================================================
#pragma once

#include <cstdint>
#include <cstring>

// 协议魔数 "_DIS" (用于识别是否为本应用的包)
const uint32_t DISCOVERY_MAGIC = 0x5F444953;
const uint16_t DISCOVERY_VERSION = 1;
const uint16_t DISCOVERY_PORT = 19191;
// 强制 1 字节对齐，确保跨平台内存布局一致
#pragma pack(push, 1)
struct DiscoveryPacket {
    uint32_t magic;      // 协议头
    uint16_t version;    // 版本号
    uint16_t tcpPort;    // PC端实际监听的 TCP 端口
    uint32_t checksum;   // 校验和 (放在末尾)
};
#pragma pack(pop)

// 简单的 FNV-1a 哈希算法，计算数据的校验和
// 也就是计算除了最后 checksum 字段本身之外的所有字节
inline uint32_t calculateChecksum(const DiscoveryPacket& pkg) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&pkg);
    // 计算长度 = 结构体总大小 - 校验和字段本身的大小
    size_t len = sizeof(DiscoveryPacket) - sizeof(uint32_t);
    
    uint32_t hash = 2166136261u; // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u; // FNV prime
    }
    return hash;
}