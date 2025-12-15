// Fill out your copyright notice in the Description page of Project Settings.
#include "CommandResolver.h"
#include "UObject/Package.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Misc/ScopeLock.h"
#include "Engine/Engine.h"

UCommandResolver* UCommandResolver::Instance = nullptr;

UCommandResolver* UCommandResolver::GetResolver()
{
    if (!Instance || !IsValid(Instance))
    {
        Instance = NewObject<UCommandResolver>(GetTransientPackage(), UCommandResolver::StaticClass());
        Instance->AddToRoot();
    }
    return Instance;
}

bool UCommandResolver::ShouldSendTrackerData()
{
    return !currentBizId.IsEmpty() && isAnalyzing;
}

// 从缓冲区中提取下一个完整的 JSON 对象（按花括号配平，忽略字符串中的括号与转义）
static int32 FindJsonObjectEnd(const FString& s, int32 startIndex)
{
    int32 depth = 0;
    bool inString = false;
    bool escapeNext = false;
    for (int32 i = startIndex; i < s.Len(); i++)
    {
        const TCHAR ch = s[i];
        if (escapeNext)
        {
            escapeNext = false;
            continue;
        }

        if (ch == '\\')
        {
            escapeNext = true;
            continue;
        }

        if (ch == '"')
        {
            inString = !inString;
            continue;
        }

        if (inString)
            continue;

        if (ch == '{') depth++;
        else if (ch == '}')
        {
            depth--;
            if (depth == 0) return i;
        }
    }
    return INDEX_NONE;
}

// 移除缓冲区起始处的噪声（如非打印字符、协议前缀），聚焦到第一个 '{'
static void StripNonJsonPrefix(FString& buffer)
{
    buffer.TrimStartInline();
    const int32 startBrace = buffer.Find(TEXT("{"));

    if (startBrace == INDEX_NONE)
        return;

    if (startBrace > 0) 
        buffer.RemoveAt(0, startBrace);
}

static bool ExtractNextJsonObject(FString& buffer, FString& outObject)
{
    // 统一前置清理：去除前导空白与 BOM，避免误判
    buffer.TrimStartInline();
    while (buffer.Len() > 0 && buffer[0] == 0xFEFF) 
        buffer.RemoveAt(0);

    // 1) 优先按换行符 '\n' 拆包（兼容 CRLF）
    const int32 newlineIndex = buffer.Find(TEXT("\n"));
    if (newlineIndex != INDEX_NONE)
    {
        FString line = buffer.Left(newlineIndex);
        buffer.RemoveAt(0, newlineIndex + 1);

        // 去除 Windows 风格结尾的 '\r'
        if (line.Len() > 0 && line[line.Len() - 1] == '\r')
            line.RemoveAt(line.Len() - 1);

        outObject = line;

        // 兼容可能存在的前置噪声/BOM
        StripNonJsonPrefix(outObject);

        outObject.TrimStartAndEndInline();
        buffer.TrimStartInline();

        // 若该行包含 JSON 以及后续噪声，按花括号配平截取纯 JSON 片段，避免误判为非法
        const int32 startBrace = outObject.Find(TEXT("{"));
        if (startBrace != INDEX_NONE)
        {
            const int32 endBrace = FindJsonObjectEnd(outObject, startBrace);
            if (endBrace != INDEX_NONE)
                outObject = outObject.Mid(startBrace, endBrace - startBrace + 1);
        }

        // 已消费一行；让上层继续处理后续行；若本行为空包将被上层跳过
        return true;
    }

    // 2) 无换行时，尝试直接按花括号配平从缓冲区提取一个完整 JSON
    const int32 startBraceInBuffer = buffer.Find(TEXT("{"));
    if (startBraceInBuffer == INDEX_NONE)
        return false;

    const int32 endBraceInBuffer = FindJsonObjectEnd(buffer, startBraceInBuffer);
    if (endBraceInBuffer == INDEX_NONE) 
        return false; // 尚未收到完整对象，继续等待后续数据

    outObject = buffer.Mid(startBraceInBuffer, endBraceInBuffer - startBraceInBuffer + 1);

    // 删除本 JSON 后的同一行噪声：若有换行符，直接去到下一行开始；否则仅去到 JSON 末尾
    int32 postNewlineIndex = INDEX_NONE;
    {
        const FString tail = buffer.Mid(endBraceInBuffer + 1);
        const int32 localNewline = tail.Find(TEXT("\n"));
        if (localNewline != INDEX_NONE) postNewlineIndex = endBraceInBuffer + 1 + localNewline;
    }
    if (postNewlineIndex != INDEX_NONE)
        buffer.RemoveAt(0, postNewlineIndex + 1);
    else
        buffer.RemoveAt(0, endBraceInBuffer + 1);

    buffer.TrimStartInline();
    return true;
}

void UCommandResolver::Resolve(const FString& json)
{
    // 粘包与半包处理：
    recvBuffer.Append(json);

    FString packet;
    while (ExtractNextJsonObject(recvBuffer, packet))
    {
        //FString jsonText = FString::Printf(TEXT("originalLength=%d, packetLength=%d"), json.Len(), packet.Len());
        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, jsonText);

        packet.TrimStartAndEndInline();

        if (packet.IsEmpty()) 
            continue;

        ResolveOne(packet);
    }

    // 无意义字符串, 不是粘包
    if (recvBuffer.Find(TEXT("{")) == INDEX_NONE)
        recvBuffer.Reset();
}

// 处理单条 JSON 指令
void UCommandResolver::ResolveOne(const FString& json)
{
    onMessageUpdate.Broadcast(json, EMessageType::Json);

    TSharedPtr<FJsonObject> jsonObject;
    TSharedRef<TJsonReader<TCHAR>> reader = TJsonReaderFactory<TCHAR>::Create(json);
    if (!FJsonSerializer::Deserialize(reader, jsonObject) || !jsonObject.IsValid())
    {
        FString err = FString::Printf(TEXT("Resolve: 无法解析为合法的 JSON: %s"), *json);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *err);
        return;
    }

    const FString cmd = jsonObject->GetStringField(TEXT("cmd"));

    if (cmd.Equals(TEXT("onTrajectoryAnalysis"), ESearchCase::IgnoreCase))
    {
        OnTrajectoryAnalysis(jsonObject);
        return;
    }

    if (cmd.Equals(TEXT("onCprAnalysis"), ESearchCase::IgnoreCase))
    {
        OnCprAnalysis(jsonObject);
        return;
    }

    // Z形轨迹记录回传：兼容服务端不同大小写/前缀
    if (cmd.Equals(TEXT("onZshapeTrajectoryAnalysis"), ESearchCase::IgnoreCase)
        || cmd.Equals(TEXT("onZShapeTrajectoryAnalysis"), ESearchCase::IgnoreCase)
        || cmd.Equals(TEXT("zshapeTrajectoryAnalysis"), ESearchCase::IgnoreCase))
    {
        OnZShapeTrajectoryAnalysis(jsonObject);
        return;
    }

    if (cmd.Equals(TEXT("onRescueAppConfig"), ESearchCase::IgnoreCase))
        OnRescueAppConfig(jsonObject);
}

void UCommandResolver::OnRescueAppConfig(const TSharedPtr<FJsonObject>& json)
{
    const int32 code = json->HasField(TEXT("code")) ? (int32)json->GetNumberField(TEXT("code")) : 0;
    const FString msg = json->HasField(TEXT("msg")) ? json->GetStringField(TEXT("msg")) : TEXT("");

    FString uiText;
    if (code == SUCCESS_CODE)
        uiText = TEXT("配置成功");
    else
        uiText = FString::Printf(TEXT("配置失败, %s"), *msg);
    UE_LOG(LogTemp, Log, TEXT("%s"), *uiText);
    onMessageUpdate.Broadcast(uiText, EMessageType::Message);
}

// -------------------------- Trajectory --------------------------
void UCommandResolver::OnTrajectoryAnalysis(const TSharedPtr<FJsonObject>& json)
{
    const int32 code = json->HasField(TEXT("code")) ? (int32)json->GetNumberField(TEXT("code")) : 0;
    const FString msg = json->HasField(TEXT("msg")) ? json->GetStringField(TEXT("msg")) : TEXT("");

    if (code != SUCCESS_CODE)
    {
        FString result = FString::Printf(TEXT("无菌钳, %s"), *msg);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *result);
        onMessageUpdate.Broadcast(result, EMessageType::Message);
        return;
    }

    // 安全读取 data，避免字段缺失导致崩溃
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("无菌钳: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    const TSharedPtr<FJsonObject> data = *dataPtr;
    const FString action = data->HasField(TEXT("action")) ? data->GetStringField(TEXT("action")) : TEXT("");

    if (action.Equals(TEXT("begin"), ESearchCase::IgnoreCase))
        OnTrajectoryAnalysis_Begin(json);
    else if (action.Equals(TEXT("stop"), ESearchCase::IgnoreCase))
        OnTrajectoryAnalysis_Stop(json);
    else if (action.Equals(TEXT("trReport"), ESearchCase::IgnoreCase))
        OnTrajectoryAnalysis_TrReport(json);
    else if (action.Equals(TEXT("result"), ESearchCase::IgnoreCase))
        OnTrajectoryAnalysis_Result(json);
    else
    {
        const FString warn = FString::Printf(TEXT("无菌钳, 未知子指令: %s"), *action);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
    }
}

void UCommandResolver::OnTrajectoryAnalysis_Begin(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("无菌钳: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    isAnalyzing = true;
    (*dataPtr)->TryGetStringField(TEXT("bizId"), currentBizId);
    currentMode = EMotionType::Trajectory;
    UE_LOG(LogTemp, Log, TEXT("无菌钳轨迹分析: 已开始"));
    onMessageUpdate.Broadcast(TEXT("无菌钳轨迹分析: 已开始"), EMessageType::Message);
    onAnalysisStateChanged.Broadcast(true);
}

void UCommandResolver::OnTrajectoryAnalysis_Stop(const TSharedPtr<FJsonObject>& json)
{
    UE_LOG(LogTemp, Log, TEXT("无菌钳轨迹分析: 已停止"));
    onMessageUpdate.Broadcast(TEXT("无菌钳轨迹分析: 已停止"), EMessageType::Message);
    onAnalysisStateChanged.Broadcast(false);
}

void UCommandResolver::OnTrajectoryAnalysis_TrReport(const TSharedPtr<FJsonObject>& json)
{
    //啥也不用干
}

void UCommandResolver::OnTrajectoryAnalysis_Result(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("无菌钳: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    const TSharedPtr<FJsonObject> data = *dataPtr;
    const bool isFinish = data->HasField(TEXT("isFinish")) ? data->GetBoolField(TEXT("isFinish")) : false;
    if (isFinish)
    {
        const TSharedPtr<FJsonObject>* summaryPtr = nullptr;
        if (data->TryGetObjectField(TEXT("summary"), summaryPtr) && summaryPtr && summaryPtr->IsValid())
        {
            const bool is10cmFromInjurySite = (*summaryPtr)->HasField(TEXT("is10cmFromInjurySite")) ? (*summaryPtr)->GetBoolField(TEXT("is10cmFromInjurySite")) : false;
            const bool isSpiral = (*summaryPtr)->HasField(TEXT("isSpiral")) ? (*summaryPtr)->GetBoolField(TEXT("isSpiral")) : false;
            const bool isInOrder = (*summaryPtr)->HasField(TEXT("isInOrder")) ? (*summaryPtr)->GetBoolField(TEXT("isInOrder")) : false;
            const double sphereDiameter = (*summaryPtr)->HasField(TEXT("sphereDiameter")) ? (*summaryPtr)->GetNumberField(TEXT("sphereDiameter")) : 0.0;
            const double score = (*summaryPtr)->HasField(TEXT("score")) ? (*summaryPtr)->GetNumberField(TEXT("score")) : 0.0;
            
            FString result = FString::Printf(TEXT("避开穿刺点1cm: %s\n螺旋式消毒: %s\n方向和顺序: %s\n消毒直径: %.2f米\n得分: %.2f"),
                is10cmFromInjurySite ? TEXT("是") : TEXT("否"),
                isSpiral ? TEXT("是") : TEXT("否"),
                isInOrder ? TEXT("是") : TEXT("否"),
                sphereDiameter, score);

            UE_LOG(LogTemp, Log, TEXT("%s"), *result);
            //if (GEngine)
            //{
            //    // 在屏幕上显示分析结果 5 秒，便于用户查看
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, result);
            //    // 同时显示对应的 JSON 字符串 5 秒
            //    FString jsonText;
            //    {
            //        TSharedRef<TJsonWriter<TCHAR>> writer = TJsonWriterFactory<TCHAR>::Create(&jsonText);
            //        FJsonSerializer::Serialize(json.ToSharedRef(), writer);
            //    }
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, jsonText);
            //}
            onMessageUpdate.Broadcast(result, EMessageType::AnalysisResult);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("无菌钳\n轨迹分析: 未完成"));
        onMessageUpdate.Broadcast(TEXT("无菌钳\n轨迹分析: 未完成"), EMessageType::AnalysisResult);
    }
}

// -------------------------- CPR --------------------------
void UCommandResolver::OnCprAnalysis(const TSharedPtr<FJsonObject>& json)
{
    int32 code = 0; FString msg;
    json->TryGetNumberField(TEXT("code"), code);
    json->TryGetStringField(TEXT("msg"), msg);
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    FString action; 
    (*dataPtr)->TryGetStringField(TEXT("action"), action);

    if (code == SUCCESS_CODE)
    {
        if (action.Equals(TEXT("begin"), ESearchCase::IgnoreCase)) 
            OnCprAnalysis_Begin(json);
        else if (action.Equals(TEXT("stop"), ESearchCase::IgnoreCase)) 
            OnCprAnalysis_End(json);
        else if (action.Equals(TEXT("result"), ESearchCase::IgnoreCase))
            OnCprAnalysis_Result(json);
        else if (action.Equals(TEXT("frameComparison"), ESearchCase::IgnoreCase))
            OnCprAnalysis_FrameComparsion(json);
        else if (action.Equals(TEXT("motions"), ESearchCase::IgnoreCase))
            OnCprAnalysis_Motions(json);
        else if (action.Equals(TEXT("motionsConfirm"), ESearchCase::IgnoreCase))
            OnCprAnalysis_MotionsConfirm(json);
        else
        {
            const FString warn = FString::Printf(TEXT("CPR, 未知子指令: %s"), *action);
            UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
            onMessageUpdate.Broadcast(warn, EMessageType::Message);
        }
    }
    else
    {
        const FString warn = FString::Printf(TEXT("CPR: %s"), *msg);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
    }
}

void UCommandResolver::OnCprAnalysis_Begin(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    (*dataPtr)->TryGetStringField(TEXT("bizId"), currentBizId);
    currentMode = EMotionType::Cpr;
    UE_LOG(LogTemp, Log, TEXT("CPR 分析: 已开始"));
    onMessageUpdate.Broadcast(TEXT("CPR 分析: 已开始"), EMessageType::Message);
    onAnalysisStateChanged.Broadcast(true);
}

void UCommandResolver::OnCprAnalysis_End(const TSharedPtr<FJsonObject>& json)
{
    UE_LOG(LogTemp, Log, TEXT("CPR 分析: 已停止"));
    onMessageUpdate.Broadcast(TEXT("CPR 分析: 已停止"), EMessageType::Message);
    onAnalysisStateChanged.Broadcast(false);
}

void UCommandResolver::OnCprAnalysis_Result(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    const TSharedPtr<FJsonObject> data = *dataPtr;
    const bool isFinish = data->HasField(TEXT("isFinish")) ? data->GetBoolField(TEXT("isFinish")) : false;
    if (isFinish)
    {
        const TSharedPtr<FJsonObject>* summaryPtr = nullptr;

        if (data->TryGetObjectField(TEXT("summary"), summaryPtr) && summaryPtr && summaryPtr->IsValid())
        {
            const bool isArmsStraight = (*summaryPtr)->HasField(TEXT("isArmsStraight")) ? (*summaryPtr)->GetBoolField(TEXT("isArmsStraight")) : false;
            const bool isPerpendicular = (*summaryPtr)->HasField(TEXT("isPerpendicular")) ? (*summaryPtr)->GetBoolField(TEXT("isPerpendicular")) : false;
            const double scoreNum = (*summaryPtr)->HasField(TEXT("score")) ? (*summaryPtr)->GetNumberField(TEXT("score")) : 0.0;

            FString result = FString::Printf(TEXT("CPR 结果: \n手臂是否伸直: %s\n按压是否垂直: %s\n得分: %.2f"),
                isArmsStraight ? TEXT("是") : TEXT("否"),
                isPerpendicular ? TEXT("是") : TEXT("否"),
                scoreNum);

            UE_LOG(LogTemp, Log, TEXT("%s"), *result);
            //if (GEngine)
            //{
            //    // 在屏幕上显示 CPR 结果 5 秒
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, result);
            //    // 同时显示对应的 JSON 字符串 5 秒
            //    FString jsonText;
            //    {
            //        TSharedRef<TJsonWriter<TCHAR>> writer = TJsonWriterFactory<TCHAR>::Create(&jsonText);
            //        FJsonSerializer::Serialize(json.ToSharedRef(), writer);
            //    }
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, jsonText);
            //}
            onMessageUpdate.Broadcast(result, EMessageType::AnalysisResult);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("CPR 分析\n 未完成"));
        onMessageUpdate.Broadcast(TEXT("CPR 分析\n 未完成"), EMessageType::Message);
    }
}

void UCommandResolver::OnCprAnalysis_FrameComparsion(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }

    // 为false时才有对比数据
    const TSharedPtr<FJsonObject> data = *dataPtr;
    const bool isFinish = data->HasField(TEXT("isFinish")) ? data->GetBoolField(TEXT("isFinish")) : false;
    if (isFinish)
    {
        UE_LOG(LogTemp, Log, TEXT("CPR 帧对比: 已完成，无实时对比数据"));
        onMessageUpdate.Broadcast(TEXT("CPR 帧对比: 已完成，无实时对比数据"), EMessageType::Message);
        return;
    }

    const TSharedPtr<FJsonObject>* comparisonPtr = nullptr;
    if (!data->TryGetObjectField(TEXT("comparisonResult"), comparisonPtr) || !(comparisonPtr && comparisonPtr->IsValid()))
    {
        const FString warn = TEXT("CPR 帧对比: comparisonResult 缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }

    const TSharedPtr<FJsonObject> comp = *comparisonPtr;
    int32 tplFrameNo = 0; comp->TryGetNumberField(TEXT("tplFrameNo"), tplFrameNo);
    int32 motionFrameNo = 0; comp->TryGetNumberField(TEXT("motionFrameNo"), motionFrameNo);
    double score = 0.0; comp->TryGetNumberField(TEXT("score"), score);

    FString bonesSummary;
    const TArray<TSharedPtr<FJsonValue>>* bonesPtr = nullptr;
    if (comp->TryGetArrayField(TEXT("bones"), bonesPtr) && bonesPtr)
    {
        for (int32 i = 0; i < bonesPtr->Num(); ++i)
        {
            const TSharedPtr<FJsonValue>& v = (*bonesPtr)[i];
            if (!v.IsValid() || v->Type != EJson::Object) 
                continue;
            const TSharedPtr<FJsonObject> b = v->AsObject();

            FString boneName;
            if (!b->TryGetStringField(TEXT("name"), boneName))
                b->TryGetStringField(TEXT("bone"), boneName);

            double similarity = 0.0; b->TryGetNumberField(TEXT("similarity"), similarity);
            double weight = 0.0; b->TryGetNumberField(TEXT("weight"), weight);

            const FString one = FString::Printf(TEXT("%s: 相似度=%.2f, 权重=%.2f"), *boneName, similarity, weight);
            if (!bonesSummary.IsEmpty()) bonesSummary.Append(TEXT("\n"));
            bonesSummary.Append(one);
        }
    }

    const FString result = FString::Printf(TEXT("CPR 帧对比\n模板帧=%d, 动作帧=%d\n得分=%.0f\n%s"), tplFrameNo, motionFrameNo, score, *bonesSummary);
    UE_LOG(LogTemp, Log, TEXT("%s"), *result);
    onMessageUpdate.Broadcast(result, EMessageType::Message);
}

void UCommandResolver::OnCprAnalysis_Motions(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR 回放: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }

    const TSharedPtr<FJsonObject> data = *dataPtr;
    FString bizId; data->TryGetStringField(TEXT("bizId"), bizId);
    const bool isFinish = data->HasField(TEXT("isFinish")) ? data->GetBoolField(TEXT("isFinish")) : false;
    bool isEnd = false; data->TryGetBoolField(TEXT("isEnd"), isEnd);
    int32 fps = 0; data->TryGetNumberField(TEXT("fps"), fps);
    int32 totalFrames = 0; data->TryGetNumberField(TEXT("totalFrames"), totalFrames);

    const TArray<TSharedPtr<FJsonValue>>* motionListPtr = nullptr;
    if (data->TryGetArrayField(TEXT("motionList"), motionListPtr) && motionListPtr)
    {
        // TODO: 骨骼数据解析??
    }
}

void UCommandResolver::OnCprAnalysis_MotionsConfirm(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("CPR 回放确认: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }

    FString bizId; (*dataPtr)->TryGetStringField(TEXT("bizId"), bizId);
    const FString info = FString::Printf(TEXT("CPR 回放确认\nbizId=%s"), *bizId);
    UE_LOG(LogTemp, Log, TEXT("%s"), *info);
    onMessageUpdate.Broadcast(info, EMessageType::Message);
}

// -------------------------- ZShape --------------------------
void UCommandResolver::OnZShapeTrajectoryAnalysis(const TSharedPtr<FJsonObject>& json)
{
    int32 code = 0; FString msg;
    json->TryGetNumberField(TEXT("code"), code);
    json->TryGetStringField(TEXT("msg"), msg);
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("Z形轨迹: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    FString action; (*dataPtr)->TryGetStringField(TEXT("action"), action);

    if (code != SUCCESS_CODE)
    {
        const FString result = FString::Printf(TEXT("Z形轨迹, %s"), *msg);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *result);
        onMessageUpdate.Broadcast(result, EMessageType::Message);
        return;
    }

    if (action.Equals(TEXT("begin"), ESearchCase::IgnoreCase))
        OnZShapeTrajectoryAnalysis_Begin(json);
    else if (action.Equals(TEXT("stop"), ESearchCase::IgnoreCase))
        OnZShapeTrajectoryAnalysis_Stop(json);
    else if (action.Equals(TEXT("trReport"), ESearchCase::IgnoreCase))
        OnZShapeTrajectoryAnalysis_TrReport(json);
    else if (action.Equals(TEXT("result"), ESearchCase::IgnoreCase))
        OnZShapeTrajectoryAnalysis_Result(json);
    else
    {
        const FString warn = FString::Printf(TEXT("Z形轨迹记录\n未知子指令: %s"), *action);
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
    }
}

void UCommandResolver::OnZShapeTrajectoryAnalysis_Begin(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("Z形轨迹: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    isAnalyzing = true;
    (*dataPtr)->TryGetStringField(TEXT("bizId"), currentBizId);
    currentMode = EMotionType::ZShape;
    const FString info = FString::Printf(TEXT("Z形轨迹分析: 已开始"), *currentBizId);
    UE_LOG(LogTemp, Log, TEXT("%s"), *info);
    onMessageUpdate.Broadcast(info, EMessageType::Message);
    onAnalysisStateChanged.Broadcast(true);
}

void UCommandResolver::OnZShapeTrajectoryAnalysis_Stop(const TSharedPtr<FJsonObject>& json)
{
    UE_LOG(LogTemp, Log, TEXT("Z形轨迹记录: 已停止"));
    onMessageUpdate.Broadcast(TEXT("Z形轨迹记录: 已停止"), EMessageType::Message);
    onAnalysisStateChanged.Broadcast(false);
}

void UCommandResolver::OnZShapeTrajectoryAnalysis_TrReport(const TSharedPtr<FJsonObject>& json)
{

}

void UCommandResolver::OnZShapeTrajectoryAnalysis_Result(const TSharedPtr<FJsonObject>& json)
{
    const TSharedPtr<FJsonObject>* dataPtr = nullptr;
    if (!json->TryGetObjectField(TEXT("data"), dataPtr) || !(dataPtr && dataPtr->IsValid()))
    {
        const FString warn = TEXT("Z形轨迹: data 字段缺失或非法");
        UE_LOG(LogTemp, Warning, TEXT("%s"), *warn);
        onMessageUpdate.Broadcast(warn, EMessageType::Message);
        return;
    }
    const TSharedPtr<FJsonObject> data = *dataPtr;
    const bool isFinish = data->HasField(TEXT("isFinish")) ? data->GetBoolField(TEXT("isFinish")) : false;
    if (isFinish)
    {
        const TSharedPtr<FJsonObject>* summaryPtr = nullptr;
        if (data->TryGetObjectField(TEXT("summary"), summaryPtr) && summaryPtr && summaryPtr->IsValid())
        {
            const bool is10cmFromInjurySite = (*summaryPtr)->HasField(TEXT("is10cmFromInjurySite")) ? (*summaryPtr)->GetBoolField(TEXT("is10cmFromInjurySite")) : false;
            const bool isZ = (*summaryPtr)->HasField(TEXT("isZ")) ? (*summaryPtr)->GetBoolField(TEXT("isZ")) : false;
            const bool isClose = (*summaryPtr)->HasField(TEXT("isClose")) ? (*summaryPtr)->GetBoolField(TEXT("isClose")) : false;
            const bool isInOrder = (*summaryPtr)->HasField(TEXT("isInOrder")) ? (*summaryPtr)->GetBoolField(TEXT("isInOrder")) : false;
            const double scoreNum = (*summaryPtr)->HasField(TEXT("score")) ? (*summaryPtr)->GetNumberField(TEXT("score")) : 0.0;

            const FString result = FString::Printf(TEXT("避开穿刺点10cm: %s\nZ形消毒: %s\n外向内: %s\n方向和顺序: %s\n得分: %.0f，满分100"),
                is10cmFromInjurySite ? TEXT("是") : TEXT("否"),
                isZ ? TEXT("是") : TEXT("否"),
                isClose ? TEXT("是") : TEXT("否"),
                isInOrder ? TEXT("是") : TEXT("否"),
                scoreNum);

            UE_LOG(LogTemp, Log, TEXT("%s"), *result);
            //if (GEngine)
            //{
            //    // 在屏幕上显示 Z 形轨迹结果 5 秒
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, result);
            //    // 同时显示对应的 JSON 字符串 5 秒
            //    FString jsonText;
            //    {
            //        TSharedRef<TJsonWriter<TCHAR>> writer = TJsonWriterFactory<TCHAR>::Create(&jsonText);
            //        FJsonSerializer::Serialize(json.ToSharedRef(), writer);
            //    }
            //    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, jsonText);
            //}
            onMessageUpdate.Broadcast(result, EMessageType::AnalysisResult);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("Z形轨迹记录: 未完成"));
        onMessageUpdate.Broadcast(TEXT("Z形轨迹记录: 未完成"), EMessageType::AnalysisResult);
    }
}
