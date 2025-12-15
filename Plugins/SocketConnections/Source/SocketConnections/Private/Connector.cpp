// Connector.cpp: 多线程 Socket 连接实现，提供蓝图接口与委托广播

#include "Connector.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Async/Async.h"
#include "Logging/LogMacros.h"
#include "Misc/CoreDelegates.h"

// 本文件内使用的日志分类
DEFINE_LOG_CATEGORY_STATIC(LogSocketConnections, Log, All);

// 工作线程：负责创建 Socket、维护连接与接收循环
class FSocketWorker : public FRunnable
{
public:
    FSocketWorker(AConnector* inOwner, const FString& inAddress, int32 inPort, bool inUseUdp)
        : owner(inOwner)
        , address(inAddress)
        , port(inPort)
        , useUdp(inUseUdp)
        , socket(nullptr)
        , shouldStop(false)
        , connected(false)
    {
    }

    virtual ~FSocketWorker()
    {
        CloseSocket();
    }

    virtual bool Init() override
    {
        shouldStop = false;
        connected = false;
        return true;
    }

    virtual uint32 Run() override
    {
        // 根据协议初始化并进入接收循环
        if (useUdp)
            UdpRecvLoop();
        else
            TcpRecvLoop();

        return 0;
    }

    virtual void Stop() override
    {
        shouldStop = true;
    }

    bool IsConnected() const
    {
        return connected;
    }

    bool SendString(const FString& message)
    {
        if (message.IsEmpty())
            return false;

        FScopeLock scopeLock(&socketMutex);
        if (!socket || !connected) 
        {
            UE_LOG(LogSocketConnections, Warning, TEXT("TCP Send 失败: Socket未连接"));
            BroadcastError(TEXT("发送失败：Socket 未连接"));
            return false;
        }

        FTCHARToUTF8 converter(*message);
        int32 bytesSent = 0;
        if (useUdp)
        {
            if (!remoteAddr.IsValid())
            {
                BroadcastError(TEXT("发送失败：UDP 远端地址无效"));
                return false;
            }
            const bool ok = socket->SendTo((uint8*)converter.Get(), converter.Length(), bytesSent, *remoteAddr) && bytesSent > 0;
            if (!ok) 
                BroadcastError(TEXT("UDP 发送失败"));

            return ok;
        }
        else
        {
            const bool ok = socket->Send((uint8*)converter.Get(), converter.Length(), bytesSent) && bytesSent > 0;
            if (!ok) BroadcastError(TEXT("TCP 发送失败"));
            return ok;
        }
    }

    void CloseSocket()
    {
        FScopeLock scopeLock(&socketMutex);
        if (socket)
        {
            socket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(socket);
            socket = nullptr;
        }
        connected = false;
    }

private:
    // TCP 接收循环：非阻塞连接 + 轮询状态，随后非阻塞接收
    void TcpRecvLoop()
    {
        ISocketSubsystem* s = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!s)
        {
            BroadcastError(TEXT("当前平台的 SocketSubsystem 不可用"));
            BroadcastState(ESocketState::Unconnect);
            return;
        }

        TSharedRef<FInternetAddr> addr = s->CreateInternetAddr();
        bool ipOk = false;
        addr->SetIp(*address, ipOk);
        addr->SetPort(port);
        if (!ipOk)
        {
            BroadcastError(FString::Printf(TEXT("非法 IP 地址: %s:%d"), *address, port));
            BroadcastState(ESocketState::Unconnect);
            return;
        }

        // 创建 Socket：连接阶段使用非阻塞，接收阶段切换为阻塞
        socket = s->CreateSocket(NAME_Stream, TEXT("SocketConnectionsTCP"), false);
        if (!socket)
        {
            BroadcastError(TEXT("CreateSocket 失败"));
            BroadcastState(ESocketState::Unconnect);
            return;
        }
        socket->SetNonBlocking(true);
        socket->SetReuseAddr(true);

        // 发起连接并在更长的时间窗口内轮询状态（避免误判慢网络为失败）
        // 注意：非阻塞 connect 返回 false 并不一定是错误，常见为“正在握手”(EWOULDBLOCK/EINPROGRESS)
        const bool connectInitiated = socket->Connect(*addr);
        if (!connectInitiated)
        {
            const ESocketErrors lastError = s->GetLastErrorCode();
            UE_LOG(LogSocketConnections, Verbose, TEXT("TCP 非阻塞 connect 返回 false，lastError=%d (可能为握手进行中) -> %s:%d"), (int32)lastError, *address, port);
        }
        const double start = FPlatformTime::Seconds();
        const double connectTimeoutSec = 5.0; // TCP 握手可能较慢，适当延长超时
        while (!shouldStop)
        {
            // 在非阻塞握手阶段，使用写就绪等待更可靠地判断握手完成（Windows/Linux/Mac）
            bool writeReady = true;
#if PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MAC || PLATFORM_ANDROID
            writeReady = socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(100));
#endif

            if (writeReady)
            {
                const ESocketConnectionState state = socket->GetConnectionState();
                if (state == SCS_Connected)
                {
                    connected = true;
                    BroadcastState(ESocketState::Connected);
                    // 切换到阻塞式接收：让 Recv 在无数据时阻塞，以可靠检测远端关闭
                    socket->SetNonBlocking(false);
                    UE_LOG(LogSocketConnections, Log, TEXT("TCP 握手成功：%s:%d"), *address, port);
                    break;
                }
                else if (state == SCS_ConnectionError)
                {
                    const ESocketErrors lastError = s->GetLastErrorCode();
                    UE_LOG(LogSocketConnections, Warning, TEXT("TCP 握手错误(lastError=%d)，断开：%s:%d"), (int32)lastError, *address, port);
                    BroadcastError(FString::Printf(TEXT("TCP 握手错误，错误码=%d"), (int32)lastError));
                    BroadcastState(ESocketState::Unconnect);
                    CloseSocket();
                    return;
                }
                // 其它状态（如 NotConnected）继续等待，避免误判慢网络
            }

            if (FPlatformTime::Seconds() - start >= connectTimeoutSec)
            {
                // 超时未连接（通常为远端不可达或被防火墙拒绝）
                const ESocketErrors lastError = s->GetLastErrorCode();
                UE_LOG(LogSocketConnections, Warning, TEXT("TCP 连接超时(%.2fs)，lastError=%d，断开：%s:%d"), connectTimeoutSec, (int32)lastError, *address, port);
                BroadcastError(FString::Printf(TEXT("TCP 连接超时(%.2fs)"), connectTimeoutSec, (int32)lastError));
                BroadcastState(ESocketState::Unconnect);
                CloseSocket();
                return;
            }

            FPlatformProcess::Sleep(0.01f);
        }

        // 已连接，进入接收循环（阻塞式 Recv，配合有限等待保证可中断）
        while (!shouldStop && socket && connected)
        {
            // 检查连接状态，若服务器关闭或异常，立刻断开并广播
            ESocketConnectionState current = socket->GetConnectionState();
            if (current == SCS_ConnectionError)
            {
                UE_LOG(LogSocketConnections, Warning, TEXT("TCP 接收循环检测到连接错误，断开：%s:%d"), *address, port);
                CloseSocket();
                BroadcastError(TEXT("接收循环检测到连接错误"));
                BroadcastState(ESocketState::Unconnect);
                return;
            }
            else if (current == SCS_NotConnected)
            {
                // 已建立连接后出现 NotConnected，通常表示远端优雅断开或连接丢失
                UE_LOG(LogSocketConnections, Warning, TEXT("TCP 连接丢失(SCS_NotConnected)，断开：%s:%d"), *address, port);
                CloseSocket();
                BroadcastError(TEXT("Tcp服务器断开"));
                BroadcastState(ESocketState::Unconnect);
                return;
            }
            // 使用有限时长等待：兼顾阻塞接收与线程可中断退出
            // 注意：许多平台在远端关闭时，读取事件也会变为就绪，随后 Recv 返回 0
            bool readyForRead = true;
#if PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MAC || PLATFORM_ANDROID
            readyForRead = socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(100));
#endif
            if (!readyForRead)
            {
                // 超时，无数据到达；继续下一轮以便响应 shouldStop
                continue;
            }

            // 执行阻塞接收（在阻塞模式下，若无数据将阻塞，若远端关闭将返回 0）
            TArray<uint8> buffer;
            buffer.SetNumUninitialized(64 * 1024);
            int32 bytesRead = 0;
            if (socket->Recv(buffer.GetData(), buffer.Num(), bytesRead))
            {
                // 如果 bytesRead == 0，表示远端优雅断开
                if (bytesRead == 0)
                {
                    UE_LOG(LogSocketConnections, Warning, TEXT("TCP 接收到长度为 0 的数据(优雅断开)，断开：%s:%d"), *address, port);
                    CloseSocket();
                    BroadcastError(TEXT("Tcp服务器断开"));
                    BroadcastState(ESocketState::Unconnect);
                    return;
                }
                FString msg = UTF8ToFString(buffer.GetData(), bytesRead);
                BroadcastMessage(msg);
            }
            else
            {
                // Recv 失败：优先判断 bytesRead==0（常见于远端优雅断开，Windows/Android 均会出现）
                if (bytesRead == 0)
                {
                    UE_LOG(LogSocketConnections, Warning, TEXT("TCP Recv 返回 false 且 bytesRead==0，判定为远端优雅断开：%s:%d"), *address, port);
                    CloseSocket();
                    BroadcastError(TEXT("Tcp服务器断开"));
                    BroadcastState(ESocketState::Unconnect);
                    return;
                }

                // 若非优雅断开，则检查连接状态与错误码，处理网络错误/异常断开
                const ESocketConnectionState afterRecv = socket->GetConnectionState();
                if (afterRecv == SCS_ConnectionError)
                {
                    const ESocketErrors lastError = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
                    UE_LOG(LogSocketConnections, Warning, TEXT("TCP Recv 失败后检测到 ConnectionError(lastError=%d)，断开：%s:%d"), (int32)lastError, *address, port);
                    CloseSocket();
                    BroadcastError(FString::Printf(TEXT("TCP 接收失败，错误码=%d"), (int32)lastError));
                    BroadcastState(ESocketState::Unconnect);
                    return;
                }
                else if (afterRecv == SCS_NotConnected)
                {
                    UE_LOG(LogSocketConnections, Warning, TEXT("TCP Recv 失败后检测到 NotConnected，断开：%s:%d"), *address, port);
                    CloseSocket();
                    BroadcastError(TEXT("接收失败后检测到 NotConnected"));
                    BroadcastState(ESocketState::Unconnect);
                    return;
                }
            }
        }

        CloseSocket();
        BroadcastState(ESocketState::Unconnect);
    }

    // UDP 接收循环：绑定到本地端口，使用 RecvFrom 读取并广播
    void UdpRecvLoop()
    {
        ISocketSubsystem* s = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!s)
        {
            BroadcastError(TEXT("当前平台Socket 不可用(UDP)"));
            BroadcastState(ESocketState::Unconnect);
            return;
        }

        // 远端地址用于发送
        remoteAddr = s->CreateInternetAddr();
        bool ipOk = false;
        remoteAddr->SetIp(*address, ipOk);
        remoteAddr->SetPort(port);
        if (!ipOk)
        {
            BroadcastError(FString::Printf(TEXT("非法 IP 地址(UDP): %s:%d"), *address, port));
            BroadcastState(ESocketState::Unconnect);
            return;
        }

        // 创建 UDP socket 并绑定本地端口
        socket = s->CreateSocket(NAME_DGram, TEXT("SocketConnectionsUDP"), false);
        if (!socket)
        {
            BroadcastError(TEXT("CreateSocket 失败(UDP)"));
            BroadcastState(ESocketState::Unconnect);
            return;
        }
        socket->SetNonBlocking(true);
        socket->SetReuseAddr(true);

        // 绑定到任意本地地址:port（若端口被占用，绑定失败）
        TSharedRef<FInternetAddr> local = s->CreateInternetAddr();
        local->SetAnyAddress();
        local->SetPort(port);
        if (!socket->Bind(*local))
        {
            BroadcastError(TEXT("UDP 绑定端口失败"));
            BroadcastState(ESocketState::Unconnect);
            CloseSocket();
            return;
        }

        connected = true; // UDP 无连接态，绑定成功即视为可通信
        BroadcastState(ESocketState::Connected);

        TSharedRef<FInternetAddr> sender = s->CreateInternetAddr();
        while (!shouldStop && socket)
        {
            uint32 pendingSize = 0;
            if (socket->HasPendingData(pendingSize) && pendingSize > 0)
            {
                TArray<uint8> buffer;
                buffer.SetNumUninitialized(FMath::Min<int32>(pendingSize, 64 * 1024));
                int32 bytesRead = 0;
                if (socket->RecvFrom(buffer.GetData(), buffer.Num(), bytesRead, *sender))
                {
                    FString msg = UTF8ToFString(buffer.GetData(), bytesRead);
                    BroadcastMessage(msg);
                }
            }
            else
            {
                FPlatformProcess::Sleep(0.01f);
            }
        }

        CloseSocket();
        BroadcastState(ESocketState::Unconnect);
    }

    // 将字节数组按 UTF-8 转为 FString
    static FString UTF8ToFString(const uint8* data, int32 length)
    {
        if (!data || length <= 0)
            return FString();

        const ANSICHAR* ansiPtr = reinterpret_cast<const ANSICHAR*>(data);
        FUTF8ToTCHAR conv(ansiPtr, length);
        return FString(conv.Get(), conv.Length());
    }

    // 在游戏线程广播消息
    void BroadcastMessage(const FString& msg)
    {
        if (!owner) return;
        TWeakObjectPtr<AConnector> weakOwner(owner);
        AsyncTask(ENamedThreads::GameThread, [weakOwner, msg]
        {
            AConnector* o = weakOwner.Get();
            if (!o || !IsValid(o)) return;
            o->onMessageReceived.Broadcast(msg);
        });
    }

    // 在游戏线程广播状态变化
    void BroadcastState(ESocketState state)
    {
        if (!owner) return;
        TWeakObjectPtr<AConnector> weakOwner(owner);
        AsyncTask(ENamedThreads::GameThread, [weakOwner, state]
        {
            AConnector* o = weakOwner.Get();
            if (!o || !IsValid(o)) return;
            o->onConnectorStateChanged.Broadcast(state);
        });
    }

    // 在游戏线程广播错误原因
    void BroadcastError(const FString& reason)
    {
        if (!owner) return;
        TWeakObjectPtr<AConnector> weakOwner(owner);
        AsyncTask(ENamedThreads::GameThread, [weakOwner, reason]
        {
            AConnector* o = weakOwner.Get();
            if (!o || !IsValid(o)) return;
            o->onConnectorError.Broadcast(reason);
        });
    }

private:
    AConnector* owner;
    FString address;
    int32 port;
    bool useUdp;

    FSocket* socket;
    TSharedPtr<FInternetAddr> remoteAddr;

    FCriticalSection socketMutex;
    bool shouldStop;
    bool connected;
};

// ===================== AConnector =====================
AConnector::AConnector()
    : worker(nullptr)
    , thread(nullptr)
    , connectPort(0)
    , useUdp(false)
{
    PrimaryActorTick.bCanEverTick = false; // 默认关闭 Tick；采用后台线程接收
}

void AConnector::BeginPlay()
{
    Super::BeginPlay();

    // 注册应用前后台事件，适配安卓熄屏/亮屏导致的网络断开
    // UE5 的 CoreDelegates 成员以 *Delegate 结尾，需使用正确的名称
    bgHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &AConnector::OnAppBackground);
    fgHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &AConnector::OnAppForeground);
}

void AConnector::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
}

void AConnector::TryConnectServer(const FString& address, int32 port, bool inUseUdp)
{
    // 用法：蓝图调用，设置地址/端口/协议并启动接收线程
    Stop();

    connectAddress = address;
    connectPort = port;
    useUdp = inUseUdp;

    // 在主线程通知“连接中”
    onConnectorStateChanged.Broadcast(ESocketState::Connecting);

    worker = new FSocketWorker(this, connectAddress, connectPort, useUdp);
    thread = FRunnableThread::Create(worker, TEXT("SocketConnectionsWorker"));
}

bool AConnector::SendString(const FString& message)
{
    FScopeLock scopeLock(&sendMutex);
    if (!worker)
    {
        UE_LOG(LogSocketConnections, Warning, TEXT("TCP Send 失败: 未启动工作线程"));
        onConnectorError.Broadcast(TEXT("发送失败：未连接服务端"));
        return false;
    }

    return worker->SendString(message);
}

void AConnector::Stop()
{
    // 用法：蓝图调用，停止并释放连接资源
    if (worker)
        worker->Stop();

    if (thread)
    {
        thread->WaitForCompletion();
        delete thread;
        thread = nullptr;
    }
    if (worker)
    {
        delete worker;
        worker = nullptr;
    }

    onConnectorStateChanged.Broadcast(ESocketState::Unconnect);
}

bool AConnector::IsConnected() const
{
    return worker && worker->IsConnected();
}

void AConnector::OnAppBackground()
{
    // 安卓熄屏或进入后台：主动停止连接，释放资源，避免系统挂起导致线程/Socket异常
    UE_LOG(LogSocketConnections, Log, TEXT("[SocketConnections] App goes to background, stopping connection"));
    Stop();
}

void AConnector::OnAppForeground()
{
    // 回到前台：若有历史连接参数，自动尝试重连（TCP/UDP均可）
    UE_LOG(LogSocketConnections, Log, TEXT("[SocketConnections] App returns to foreground, auto reconnect if params exist"));
    if (!connectAddress.IsEmpty() && connectPort > 0)
    {
        TryConnectServer(connectAddress, connectPort, useUdp);
    }
}

