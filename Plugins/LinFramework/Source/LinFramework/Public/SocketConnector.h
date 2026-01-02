// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SocketConnector.generated.h"

// 连接状态枚举：用于通过委托通知上层连接状态改变
UENUM(BlueprintType)
enum class ESocketState : uint8
{
	Unconnect,
	Connecting,
	Connected
};

// 当收到服务端消息时广播（已在游戏线程触发，安全可用于 UI）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMessageReceived, const FString&, message);

// 当连接状态变化时广播（已在游戏线程触发）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectorStateChanged, ESocketState, state);

// 当遇到错误时广播错误原因（已在游戏线程触发）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectorError, const FString&, reason);

UCLASS()
class LINFRAMEWORK_API ASocketConnector : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ASocketConnector();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// 当收到服务端消息时广播
	UPROPERTY(BlueprintAssignable)
	FOnMessageReceived onMessageReceived;

	// 当连接状态变化时广播
	UPROPERTY(BlueprintAssignable)
	FOnConnectorStateChanged onConnectorStateChanged;

	// 当遇到错误时广播
	UPROPERTY(BlueprintAssignable)
	FOnConnectorError onConnectorError;

	// 尝试连接到服务端
	UFUNCTION(BlueprintCallable, Category = "LinFramework|Network")
		void TryConnectServer(const FString& address, int32 port, bool useUdp);

	// 发送字符串到服务端 返回是否发送成功
	UFUNCTION(BlueprintCallable, Category = "LinFramework|Network")
		bool SendString(const FString& message);

	// 停止并释放连接资源
	UFUNCTION(BlueprintCallable, Category = "LinFramework|Network")
		void Stop();

	// 查询当前是否已连接
	UFUNCTION(BlueprintPure, Category = "LinFramework|Network")
		bool IsConnected() const;

private:
	// 线程对象与工作者，用于后台接收数据；注意只在主线程创建与销毁
	class FSocketWorker* worker;
	class FRunnableThread* thread;

	// 连接参数（最新一次调用 TryConnectServer 设置）
	FString connectAddress;
	int32 connectPort;
	bool useUdp;

	// 发送互斥，保护 socket 发送并发
	FCriticalSection sendMutex;

	// 应用前后台事件句柄：用于安卓熄屏/亮屏自动断开与重连
	FDelegateHandle bgHandle;
	FDelegateHandle fgHandle;

	// 应用进入后台时立即停止连接以避免系统回收资源导致悬空
	UFUNCTION()
		void OnAppBackground();

	// 应用回到前台后自动按上次参数重连（若存在参数）
	UFUNCTION()
		void OnAppForeground();
};
