// Fill out your copyright notice in the Description page of Project Settings.


#include "Logger.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "UObject/Package.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Engine/Engine.h"
#include "Logging/LogMacros.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

static ULogger* gInstance = nullptr;

ULogger* ULogger::GetLogger()
{
    if (gInstance) return gInstance;
    gInstance = NewObject<ULogger>(GetTransientPackage(), ULogger::StaticClass());
    gInstance->AddToRoot();
    gInstance->Initialize();
    return gInstance;
}

void ULogger::Log(const FString& message)
{
    // 将所有日志写入统一文件
    EnsureFilePathInitialized(combinedFilePath);
    WriteLine(message, combinedFilePath);
}

void ULogger::Warning(const FString& message)
{
    EnsureFilePathInitialized(combinedFilePath);
    WriteLine(message, combinedFilePath);
}

void ULogger::Error(const FString& message)
{
    EnsureFilePathInitialized(combinedFilePath);
    WriteLine(message, combinedFilePath);
}

void ULogger::Initialize()
{
    FScopeLock lock(&writeLock);
    const FString dir = FPaths::ProjectLogDir();
    IFileManager::Get().MakeDirectory(*dir, true);

    // 用法：初始化时将日志目录路径打印到屏幕，持续5秒，便于确认目标文件夹位置
    const float DISPLAY_SECONDS = 5.0f;
    if (GEngine)
        GEngine->AddOnScreenDebugMessage(-1, DISPLAY_SECONDS, FColor::Green, FString::Printf(TEXT("日志目录: %s"), *dir));
}

// 修改方法：使用数值组装日期字符串，避免格式串不生效
FString ULogger::GetTodayLogFilePath() const
{
    const FDateTime now = FDateTime::Now();
    const FString date = FString::Printf(TEXT("%04d%02d%02d"), now.GetYear(), now.GetMonth(), now.GetDay());
    // 统一使用 All-YYYYMMDD.log，不区分类别
    return FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("Logs-%s.log"), *date));
}

void ULogger::WriteLine(const FString& content, const FString& filePath)
{
    FScopeLock lock(&writeLock);
    const FDateTime now = FDateTime::Now();
    const FString time = FString::Printf(TEXT("%02d:%02d:%02d.%03d"), now.GetHour(), now.GetMinute(), now.GetSecond(), now.GetMillisecond());
    const FString line = FString::Printf(TEXT("[%s] %s\n"), *time, *content);

    UE_LOG(LogTemp, Log, TEXT("%s"), *line);
    FFileHelper::SaveStringToFile(line, *filePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
}

void ULogger::EnsureFilePathInitialized(FString& filePath)
{
    if (!filePath.IsEmpty())
        return;

    FScopeLock lock(&writeLock);
    if (filePath.IsEmpty()) 
        filePath = GetTodayLogFilePath();
}

// 设置企业微信机器人 webhook（完整 URL）
void ULogger::SetWeComWebhook(const FString& webhookUrl)
{
    FScopeLock lock(&writeLock);
    wecomWebhook = webhookUrl;
}

// 解析 webhook 中的 key 参数
bool ULogger::ParseWeComKeyFromWebhook(const FString& webhook, FString& outKey) const
{
    outKey.Empty();
    int32 idx;
    if (!webhook.FindChar('?', idx))
        return false;
    const FString query = webhook.Mid(idx + 1);
    // 查找 key=XXXX 片段
    int32 keyPos = query.Find(TEXT("key="));
    if (keyPos == INDEX_NONE) 
        return false;

    FString rest = query.Mid(keyPos + 4);
    int32 ampPos = rest.Find(TEXT("&"));
    outKey = ampPos == INDEX_NONE ? rest : rest.Left(ampPos);
    return !outKey.IsEmpty();
}

// 异步将当天日志文件发送到企业微信机器人：upload_media -> send file
void ULogger::SendLogFileToWeCom()
{
    EnsureFilePathInitialized(combinedFilePath);

    if (wecomWebhook.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("未设置企业微信机器人 webhook"));
        return;
    }

    FString key;
    if (!ParseWeComKeyFromWebhook(wecomWebhook, key))
    {
        UE_LOG(LogTemp, Warning, TEXT("webhook 中未解析到 key"));
        return;
    }

    TArray<uint8> fileBytes;
    if (!FFileHelper::LoadFileToArray(fileBytes, *combinedFilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("读取日志文件失败: %s"), *combinedFilePath);
        return;
    }

    const FString uploadUrl = FString::Printf(TEXT("https://qyapi.weixin.qq.com/cgi-bin/webhook/upload_media?key=%s&type=file"), *key);

    // 构造 multipart/form-data
    const FString boundary = FString::Printf(TEXT("----------------UEBoundary-%d"), FDateTime::Now().GetTicks() & 0xFFFF);
    const FString fileName = FPaths::GetCleanFilename(combinedFilePath);
    FString headerPart = FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"media\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n"), *boundary, *fileName);
    FString tailPart = FString::Printf(TEXT("\r\n--%s--\r\n"), *boundary);

    TArray<uint8> body;
    {
        FTCHARToUTF8 h(*headerPart);
        body.Append((uint8*)h.Get(), h.Length());
    }
    body.Append(fileBytes);
    {
        FTCHARToUTF8 t(*tailPart);
        body.Append((uint8*)t.Get(), t.Length());
    }

    // 发送上传请求
    FHttpModule* http = &FHttpModule::Get();
    if (!http)
    {
        UE_LOG(LogTemp, Warning, TEXT("HTTP 模块不可用"));
        return;
    }

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> uploadReq = http->CreateRequest();
    uploadReq->SetURL(uploadUrl);
    uploadReq->SetVerb(TEXT("POST"));
    uploadReq->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=%s"), *boundary));
    uploadReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
    uploadReq->SetContent(body);
    uploadReq->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr req, FHttpResponsePtr resp, bool ok)
        {
            if (!ok || !resp.IsValid())
            {
                UE_LOG(LogTemp, Warning, TEXT("上传日志到企业微信失败：网络或响应无效"));
                return;
            }
            const FString json = resp->GetContentAsString();

            // 解析 media_id
            FString mediaId;
            {
                TSharedPtr<FJsonObject> obj;
                const TSharedRef<TJsonReader<>> reader = TJsonReaderFactory<>::Create(json);
                if (FJsonSerializer::Deserialize(reader, obj) && obj.IsValid())
                {
                    obj->TryGetStringField(TEXT("media_id"), mediaId);
                }
            }
            if (mediaId.IsEmpty())
            {
                UE_LOG(LogTemp, Warning, TEXT("上传未返回 media_id，响应: %s"), *json);
                return;
            }

            // 使用 webhook 发送文件消息
            TSharedRef<IHttpRequest, ESPMode::ThreadSafe> sendReq = FHttpModule::Get().CreateRequest();
            sendReq->SetURL(wecomWebhook);
            sendReq->SetVerb(TEXT("POST"));
            sendReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

            // {"msgtype":"file","file":{"media_id":"..."}}
            TSharedPtr<FJsonObject> root = MakeShared<FJsonObject>();
            root->SetStringField(TEXT("msgtype"), TEXT("file"));
            TSharedPtr<FJsonObject> fileObj = MakeShared<FJsonObject>();
            fileObj->SetStringField(TEXT("media_id"), mediaId);
            root->SetObjectField(TEXT("file"), fileObj);
            FString payload;
            {
                TSharedRef<TJsonWriter<>> w = TJsonWriterFactory<>::Create(&payload);
                FJsonSerializer::Serialize(root.ToSharedRef(), w);
            }
            sendReq->SetContentAsString(payload);
            sendReq->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr r, FHttpResponsePtr s, bool o)
                {
                    if (!o || !s.IsValid())
                    {
                        UE_LOG(LogTemp, Warning, TEXT("发送文件消息到企业微信失败"));
                        return;
                    }
                    UE_LOG(LogTemp, Log, TEXT("发送企业微信文件消息成功，响应: %s"), *s->GetContentAsString());
                });
            sendReq->ProcessRequest();
        });
    uploadReq->ProcessRequest();
}
