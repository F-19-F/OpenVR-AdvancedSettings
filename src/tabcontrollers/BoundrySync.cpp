#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <vector>
#include <cmath>

// OpenVR
#include <openvr.h>
#pragma comment(lib, "openvr_api.lib")

// Winsock
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <string>
#include <filesystem>
#include <thread>
#include <chrono>
#include <windows.h> // 必须包含
#include "ChaperoneTabController.h"
#include "DiscoveryProtocol.h"
#include <easylogging++.h>



class DiscoveryBroadcaster {
public:
    // tcpPort: 告诉 Android 连接此 TCP 端口
    // udpPort: UDP 广播的目标端口 (Android 监听端口)
    DiscoveryBroadcaster(int tcpPort, int udpPort);
    ~DiscoveryBroadcaster();

    bool start();
    void stop();

    // 设置单播目标 IP。传入 "" (空字符串) 则只发广播。
    void setTargetIP(const std::string& ip);

private:
    void workerLoop();

    int m_tcpPort;
    int m_udpPort;
    
    std::atomic<bool> m_isRunning;
    std::thread m_workerThread;
    SOCKET m_socket;

    std::mutex m_ipMutex;
    std::string m_targetIP;
};
DiscoveryBroadcaster::DiscoveryBroadcaster(int tcpPort, int udpPort)
    : m_tcpPort(tcpPort), m_udpPort(udpPort), m_isRunning(false), m_socket(INVALID_SOCKET) {
}

DiscoveryBroadcaster::~DiscoveryBroadcaster() {
    stop();
}

bool DiscoveryBroadcaster::start() {
    if (m_isRunning) return true;

    // 创建 UDP Socket
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        std::cerr << "[PC] Create socket failed: " << WSAGetLastError() << std::endl;
        return false;
    }

    // 启用广播
    BOOL broadcast = TRUE;
    if (setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast)) < 0) {
        std::cerr << "[PC] Setsockopt broadcast failed." << std::endl;
        closesocket(m_socket);
        return false;
    }

    m_isRunning = true;
    m_workerThread = std::thread(&DiscoveryBroadcaster::workerLoop, this);
    std::cout << "[PC] Discovery Service Started." << std::endl;
    return true;
}

void DiscoveryBroadcaster::stop() {
    if (!m_isRunning) return;
    m_isRunning = false;

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    std::cout << "[PC] Discovery Service Stopped." << std::endl;
}

void DiscoveryBroadcaster::setTargetIP(const std::string& ip) {
    std::lock_guard<std::mutex> lock(m_ipMutex);
    m_targetIP = ip;
    if (!ip.empty()) {
        std::cout << "[PC] Added Unicast Target: " << ip << std::endl;
    } else {
        std::cout << "[PC] Unicast Target Cleared (Broadcast Only)." << std::endl;
    }
}

void DiscoveryBroadcaster::workerLoop() {
    // 准备数据包
    DiscoveryPacket packet;
    // 使用 htonl/htons 转为网络字节序 (Big Endian)
    packet.magic = htonl(DISCOVERY_MAGIC);
    packet.version = htons(DISCOVERY_VERSION);
    packet.tcpPort = htons((uint16_t)m_tcpPort);
    // 计算校验和 (注意：对已经转为网络序的数据进行计算)
    packet.checksum = htonl(calculateChecksum(packet));

    // 广播地址
    sockaddr_in broadcastAddr;
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(m_udpPort);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    while (m_isRunning) {
        // 1. 发送广播
        sendto(m_socket, (const char*)&packet, sizeof(packet), 0, 
               (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

        // 2. 发送单播 (如果设置了 IP)
        std::string currentIP;
        {
            std::lock_guard<std::mutex> lock(m_ipMutex);
            currentIP = m_targetIP;
        }

        if (!currentIP.empty()) {
            sockaddr_in unicastAddr;
            unicastAddr.sin_family = AF_INET;
            unicastAddr.sin_port = htons(m_udpPort);
            // 将字符串 IP 转为地址结构
            if (inet_pton(AF_INET, currentIP.c_str(), &unicastAddr.sin_addr) == 1) {
                sendto(m_socket, (const char*)&packet, sizeof(packet), 0, 
                       (sockaddr*)&unicastAddr, sizeof(unicastAddr));
            }
        }

        // 间隔 1 秒
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ==========================================
// 1. 数据结构定义 (与 Android 端对齐)
// ==========================================
#pragma pack(push, 1)
struct SteamVRChaperoneData {
    float playAreaX;
    float playAreaZ;
    struct Point { float x, y, z; };
    Point collisionBounds[4]; // 相对于头显的 4 个角点
    float hmdMatrix34[12];    // (接收但不使用，因为我们用本地的 SteamVR Pose)
};
#pragma pack(pop)

// 辅助数学工具
struct Vector3 { float x, y, z; };

// 变换点: P_world = Matrix * P_local
Vector3 TransformPoint(const vr::HmdMatrix34_t& m, float x, float y, float z) {
    Vector3 out;
    out.x = m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3];
    out.y = m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3];
    out.z = m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3];
    return out;
}

// 计算两点距离 (用于防抖动)
float Dist(const Vector3& a, const Vector3& b) {
    return sqrt(pow(a.x - b.x, 2) + pow(a.z - b.z, 2));
}

// ==========================================
// 2. 核心类
// ==========================================
class ChaperoneSyncClient {
public:
    ChaperoneSyncClient() {}

    ~ChaperoneSyncClient() {
        if (clientSock != INVALID_SOCKET) closesocket(clientSock);
        WSACleanup();
        vr::VR_Shutdown();
    }

    bool Init(vr::IVRSystem* vr,advsettings::MoveCenterTabController* moveCenterTabController) {
        this->m_pHMD = vr;
        m_pSetup = vr::VRChaperoneSetup();
        m_moveCenterTabController = moveCenterTabController;
        if (!m_pSetup) {
            std::cerr << "Failed to get VRChaperoneSetup" << std::endl;
            return false;
        }
        // --- Init Winsock ---
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        listenSock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(1191);

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
        if (listen(listenSock, 1) == SOCKET_ERROR) return false;
        m_discoveryBroadcaster.start();
        std::cout << ">>> Waiting for Quest Connection on Port 1191..." << std::endl;
        return true;
    }

    void Run() {
        clientSock = accept(listenSock, NULL, NULL);
        if (clientSock == INVALID_SOCKET) return;

        std::cout << ">>> Quest Connected!" << std::endl;

        // 缓存上一次的中心点，防止微小抖动导致每一帧都 Commit
        Vector3 lastCenter = { 0,0,0 };
        bool hasSyncedOnce = false;

        while (true) {
            SteamVRChaperoneData data;
            int ret = recv(clientSock, (char*)&data, sizeof(data), 0);
            if (ret <= 0) {
                std::cout << "Disconnected." << std::endl;
                break;
            }

            if (ret == sizeof(data)) {
                ProcessData(data, lastCenter, hasSyncedOnce);
            }
        }

    }
    void Stop() {
        if (listenSock) {
            closesocket(listenSock);
            listenSock = INVALID_SOCKET;
        }
        m_discoveryBroadcaster.stop();
    }

    bool PollEvent(vr::VREvent_t *event,uint32_t size) {
        return m_pHMD->PollNextEvent(event, size);
    }

private:
    vr::IVRSystem* m_pHMD = nullptr;
    vr::IVRChaperoneSetup* m_pSetup = nullptr;
    advsettings::MoveCenterTabController* m_moveCenterTabController = nullptr;
    DiscoveryBroadcaster m_discoveryBroadcaster{ 1191, DISCOVERY_PORT };
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;
    void ProcessData(const SteamVRChaperoneData& data, Vector3& lastCenter, bool& hasSyncedOnce) {
        if(data.playAreaX <= 0 || data.playAreaZ <= 0) {
            LOG(INFO) << "[ChaperoneSync] updateChaperoneResetData";
            vr::VRChaperoneSetup()->RevertWorkingCopy();
            m_moveCenterTabController->updateChaperoneResetData(false);
            return;
        }
        // 1. 获取 SteamVR 头显位姿
        vr::TrackedDevicePose_t trackedPose[vr::k_unMaxTrackedDeviceCount];
        m_pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, trackedPose, vr::k_unMaxTrackedDeviceCount);

        if (!trackedPose[0].bPoseIsValid) return;
        vr::HmdMatrix34_t hmdPose = trackedPose[0].mDeviceToAbsoluteTracking;

        // 2. 还原世界坐标 (Head -> World)
        std::vector<Vector3> worldCorners;
        for (int i = 0; i < 4; i++) {
            // 调用之前的 TransformPoint (HMD_Matrix * Local_Point)
            // 这一步不需要改，标准的矩阵乘向量
            Vector3 p = TransformPoint(hmdPose, data.collisionBounds[i].x, data.collisionBounds[i].y, data.collisionBounds[i].z);

            // [关键] 强制把 Y 压平到 0
            // 因为低头/抬头会导致计算出的墙壁有一点点翘起，我们只要 X/Z
            p.y = 0.0f;
            worldCorners.push_back(p);
        }

        // ... (防抖动代码保持不变) ...

        // 3. 生成墙壁
        std::vector<vr::HmdQuad_t> geometryQuads;
        float wallHeight = 2.4f;

        for (int i = 0; i < 4; i++) {
            int next = (i + 1) % 4;
            vr::HmdQuad_t wall;

            Vector3 p1 = worldCorners[i];
            Vector3 p2 = worldCorners[next];

            // 构建墙壁 (注意顺序，SteamVR 默认逆时针为正面，或者双面渲染)
            wall.vCorners[0].v[0] = p1.x; wall.vCorners[0].v[1] = 0;          wall.vCorners[0].v[2] = p1.z;
            wall.vCorners[1].v[0] = p2.x; wall.vCorners[1].v[1] = 0;          wall.vCorners[1].v[2] = p2.z;
            wall.vCorners[2].v[0] = p2.x; wall.vCorners[2].v[1] = wallHeight; wall.vCorners[2].v[2] = p2.z;
            wall.vCorners[3].v[0] = p1.x; wall.vCorners[3].v[1] = wallHeight; wall.vCorners[3].v[2] = p1.z;

            geometryQuads.push_back(wall);
        }

        // 4. [最重要] 强制重置 SteamVR 的中心点
        // 如果这里不重置，SteamVR 可能会叠加它自己的 "Room Setup" 偏移，导致整体位置偏差
        vr::HmdMatrix34_t identityMat;
        memset(&identityMat, 0, sizeof(identityMat));
        identityMat.m[0][0] = 1; identityMat.m[1][1] = 1; identityMat.m[2][2] = 1;

        m_pSetup->RevertWorkingCopy(); // 先清空
        m_pSetup->SetWorkingPlayAreaSize(data.playAreaX, data.playAreaZ);
        m_pSetup->SetWorkingCollisionBoundsInfo(geometryQuads.data(), (uint32_t)geometryQuads.size());

        // 告诉 SteamVR：不要做任何额外的校准，我的坐标已经是绝对准确的了
        m_pSetup->SetWorkingStandingZeroPoseToRawTrackingPose(&identityMat);
        m_pSetup->SetWorkingSeatedZeroPoseToRawTrackingPose(&identityMat);

        m_pSetup->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
        m_moveCenterTabController->updateChaperoneResetData(false);
    }
};
ChaperoneSyncClient app;
std::atomic<bool> g_bRunning(true);
std::thread *worker = nullptr;
void updateThread() {
  while (g_bRunning) {
    app.Run();
  }
}
void BoundrySyncStart(vr::IVRSystem* vr,advsettings::MoveCenterTabController* moveCenterTabController) {
    if (app.Init(vr,moveCenterTabController)) {
        worker = new std::thread(updateThread);
    }
    else {
        g_bRunning = false;
    }
}
void BoundrySyncStop(){
    g_bRunning = false;
    app.Stop();
    if (worker && worker->joinable()) {
        worker->join();
    }
}