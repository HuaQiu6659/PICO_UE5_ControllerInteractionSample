// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Logger.generated.h"

UCLASS()
class LINFRAMEWORK_API ULogger : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category = "LinFramework|Log")
        static ULogger* GetLogger();

    UFUNCTION(BlueprintCallable, Category = "LinFramework|Log")
        void Log(const FString& message);

    UFUNCTION(BlueprintCallable, Category = "LinFramework|Log")
        void Warning(const FString& message);

    UFUNCTION(BlueprintCallable, Category = "LinFramework|Log")
        void Error(const FString& message);

    // 设置企业微信机器人 Webhook（需是完整 URL，例如：https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=XXXXXX）
    // 用法：在调用发送接口前先设置 Webhook；若未设置则发送会告警并退出
    UFUNCTION(BlueprintCallable, Category = "LinFramework|Log")
        void SetWeComWebhook(const FString& webhookUrl);

    // 向企业微信机器人发送当天的日志文件（异步）：
    // 1) 解析 webhook 中的 key；2) 调用 upload_media 上传文件获取 media_id；3) 使用 webhook 发送 file 消息
    UFUNCTION(BlueprintCallable, Category = "LinFramework|Log")
        void SendLogFileToWeCom();

private:
    // 所有日志统一写入该文件（按日期生成），不区分 Log/Warning/Error
    FString combinedFilePath;

    // 企业微信机器人 webhook
    FString wecomWebhook;
    FCriticalSection writeLock;

    void Initialize();
    void WriteLine(const FString& content, const FString& filePath);
    void EnsureFilePathInitialized(FString& filePath);
    FString GetTodayLogFilePath() const;

    // 从 webhook URL 中解析 key 参数，用于上传文件接口
    bool ParseWeComKeyFromWebhook(const FString& webhook, FString& outKey) const;
};
