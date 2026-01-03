// Fill out your copyright notice in the Description page of Project Settings.


#include "CommandBuilder.h"
#include "CommandResolver.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Math/UnrealMathUtility.h"

static void CleanJson(FString& s)
{
	s.ReplaceInline(TEXT("\r"), TEXT(""));
	s.ReplaceInline(TEXT("\n"), TEXT(""));
    s.ReplaceInline(TEXT("\t"), TEXT(""));
    s.ReplaceInline(TEXT(" "), TEXT(""));

    if (!s.EndsWith(TEXT("\r\n")))
		s.Append(TEXT("\r\n"));
}

FString UCommandBuilder::GlobalConfigCommand(const FString& clipperSn, const FString& dummySn)
{
    FString outJson;

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());
	root->SetStringField(TEXT("cmd"), TEXT("rescueAppConfig"));
	root->SetNumberField(TEXT("fps"), 60);
	root->SetNumberField(TEXT("engine"), 0);	// Unity是0   UE是1
	root->SetStringField(TEXT("asepticClipper"), clipperSn);
	root->SetStringField(TEXT("dummy"), dummySn);

    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), Writer);
    CleanJson(outJson);

	UCommandResolver::GetResolver()->globalConfig = EConfigResult::Configing;
    return outJson;
}

FString UCommandBuilder::StartCommand(EMotionType motionType)
{
	FString outJson;

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());

	// cmd
	switch (motionType)
	{
	case EMotionType::Trajectory:
		root->SetStringField(TEXT("cmd"), TEXT("trajectoryAnalysis"));
		break;
	case EMotionType::Cpr:
		root->SetStringField(TEXT("cmd"), TEXT("cprAnalysis"));
		break;
	case EMotionType::ZShape:
		root->SetStringField(TEXT("cmd"), TEXT("zshapeTrajectoryAnalysis"));
		break;
	default:
		UE_LOG(LogTemp, Error, TEXT("StartCommand: unknown motion type %d"), (int32)motionType);
		root->SetStringField(TEXT("cmd"), TEXT("unknown"));
		return outJson;
	}

	// action
	root->SetStringField(TEXT("action"), TEXT("begin"));

	// stamp: 毫秒时间戳
	const int64 timestampMs = static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970,1,1)).GetTotalMilliseconds());
	root->SetNumberField(TEXT("stamp"), (double)timestampMs);

    TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), writer);
    CleanJson(outJson);
    return outJson;
}

FString UCommandBuilder::EndCommand(EMotionType motionType)
{
	FString outJson;

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());
	UCommandResolver* resolver = UCommandResolver::GetResolver();
	FString bizId;
	if (resolver == nullptr)
	{
#if WITH_EDITOR
		bizId = FString::Printf(TEXT("EditorTest"));
#else
		return outJson;
#endif
	}

	bizId = resolver->GetBizId();
	if (bizId.IsEmpty())
	{
		resolver->onMessageUpdate.Broadcast(FString::Printf(TEXT("未开始分析(bizId为空)\n因此无法发送结束指令")), EMessageType::Message);
#if WITH_EDITOR
		bizId = FString::Printf(TEXT("EditorTest"));
#else
		return outJson;
#endif
	}
	root->SetStringField(TEXT("bizId"), bizId);

	// cmd, bizId
	switch (motionType)
	{
		case EMotionType::Trajectory:
			root->SetStringField(TEXT("cmd"), TEXT("trajectoryAnalysis"));
			resolver->SetAnalyzing(false);
			break;
		case EMotionType::Cpr:
			root->SetStringField(TEXT("cmd"), TEXT("cprAnalysis"));
			break;
		case EMotionType::ZShape:
			root->SetStringField(TEXT("cmd"), TEXT("zshapeTrajectoryAnalysis"));
			resolver->SetAnalyzing(false);
			break;
		default:
			UE_LOG(LogTemp, Error, TEXT("EndCommand: unknown motion type %d"), (int32)motionType);
			root->SetStringField(TEXT("cmd"), TEXT("unknown"));
			break;
	}

	// action
	root->SetStringField(TEXT("action"), TEXT("stop"));

	// stamp
	const int64 TimestampMs = static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970,1,1)).GetTotalMilliseconds());
	root->SetNumberField(TEXT("stamp"), (double)TimestampMs);

    TSharedRef<TJsonWriter<>> writer2 = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), writer2);
    CleanJson(outJson);
    return outJson;
}

FString UCommandBuilder::AnalysisCommand(EMotionType motionType)
{
	FString outJson;

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());
	UCommandResolver* resolver = UCommandResolver::GetResolver();
	const FString bizId = resolver ? resolver->GetBizId() : FString();
	if (bizId.IsEmpty())
		return outJson;

	root->SetStringField(TEXT("bizId"), bizId);

	// cmd, bizId
	switch (motionType)
	{
		case EMotionType::Trajectory:
			root->SetStringField(TEXT("cmd"), TEXT("trajectoryAnalysis"));
			break;
		case EMotionType::Cpr:
			root->SetStringField(TEXT("cmd"), TEXT("cprAnalysis"));
			break;
		case EMotionType::ZShape:
			// 纠正拼写错误，保持与样例协议一致
			root->SetStringField(TEXT("cmd"), TEXT("zshapeTrajectoryAnalysis"));
			break;
		default:
			return outJson;
	}

	// action
	root->SetStringField(TEXT("action"), TEXT("result"));

    TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), writer);
    CleanJson(outJson);
    return outJson;
}

FString UCommandBuilder::TrackerDatas(const TArray<FTrackerData>& trackers)
{
	FString outJson;

	auto resolver = UCommandResolver::GetResolver();

	const FString bizId = resolver ? resolver->GetBizId() : FString();
	if (bizId.IsEmpty())
		return outJson;
#if !WITH_EDITOR
    if (!resolver->IsAnalyzing())
		return outJson;
#endif

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> trackersArray;
	trackersArray.Reserve(trackers.Num());

	for (const FTrackerData& t : trackers)
	{
		TSharedPtr<FJsonObject> tObj = MakeShareable(new FJsonObject());
		tObj->SetStringField(TEXT("sn"), t.sn);
		// lt: [x, y, z]
		{
			TArray<TSharedPtr<FJsonValue>> ltArr;
			ltArr.Add(MakeShareable(new FJsonValueNumber((t.lt.X))));
			ltArr.Add(MakeShareable(new FJsonValueNumber((t.lt.Y))));
			ltArr.Add(MakeShareable(new FJsonValueNumber((t.lt.Z))));
			tObj->SetArrayField(TEXT("lt"), ltArr);
		}
		// lr: [x, y, z, w]
		{
			TArray<TSharedPtr<FJsonValue>> lrArr;
			lrArr.Add(MakeShareable(new FJsonValueNumber((t.lr.X))));
			lrArr.Add(MakeShareable(new FJsonValueNumber((t.lr.Y))));
			lrArr.Add(MakeShareable(new FJsonValueNumber((t.lr.Z))));
			lrArr.Add(MakeShareable(new FJsonValueNumber((t.lr.W))));
			tObj->SetArrayField(TEXT("lr"), lrArr);
		}
		// gt: [x, y, z]
		{
			TArray<TSharedPtr<FJsonValue>> gtArr;
			gtArr.Add(MakeShareable(new FJsonValueNumber((t.gt.X))));
			gtArr.Add(MakeShareable(new FJsonValueNumber((t.gt.Y))));
			gtArr.Add(MakeShareable(new FJsonValueNumber((t.gt.Z))));
			tObj->SetArrayField(TEXT("gt"), gtArr);
		}
		// gr: [x, y, z, w]
		{
			TArray<TSharedPtr<FJsonValue>> grArr;
			grArr.Add(MakeShareable(new FJsonValueNumber((t.gr.X))));
			grArr.Add(MakeShareable(new FJsonValueNumber((t.gr.Y))));
			grArr.Add(MakeShareable(new FJsonValueNumber((t.gr.Z))));
			grArr.Add(MakeShareable(new FJsonValueNumber((t.gr.W))));
			tObj->SetArrayField(TEXT("gr"), grArr);
		}
		tObj->SetBoolField(TEXT("isConfidence"), t.bIsConfidence);
		trackersArray.Add(MakeShareable(new FJsonValueObject(tObj)));
	}
	root->SetArrayField(TEXT("trackerList"), trackersArray);

	auto mode = resolver->GetCurrentMode();
	switch (mode)
	{
		case EMotionType::Trajectory:
			root->SetStringField(TEXT("cmd"), TEXT("trajectoryAnalysis"));
			break;

		case EMotionType::ZShape:
			root->SetStringField(TEXT("cmd"), TEXT("zshapeTrajectoryAnalysis"));
			break;

		default:
			return outJson;
	}
	root->SetStringField(TEXT("bizId"), bizId);
	root->SetStringField(TEXT("action"), TEXT("trReport"));
	const int64 TimestampMs = static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970,1,1)).GetTotalMilliseconds());
	root->SetNumberField(TEXT("stamp"), (double)TimestampMs);
    TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), writer);
    CleanJson(outJson);
    return outJson;
}

FString UCommandBuilder::CprMotionsCommand()
{
    FString outJson;
    UCommandResolver* resolver = UCommandResolver::GetResolver();
    FString bizId;
    if (resolver == nullptr)
    {
#if WITH_EDITOR
        bizId = FString::Printf(TEXT("EditorTest"));
#else
        return outJson;
#endif
    }
    else
    {
        bizId = resolver->GetBizId();
    }

    if (bizId.IsEmpty())
    {
        if (resolver)
            resolver->onMessageUpdate.Broadcast(TEXT("未开始分析(bizId为空)\n因此无法请求CPR动作数据回放"), EMessageType::Message);
#if WITH_EDITOR
        bizId = FString::Printf(TEXT("EditorTest"));
#else
        return outJson;
#endif
    }

    TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());
    root->SetStringField(TEXT("cmd"), TEXT("cprAnalysis"));
    root->SetStringField(TEXT("action"), TEXT("motions"));
    root->SetStringField(TEXT("bizId"), bizId);

    TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
    FJsonSerializer::Serialize(root.ToSharedRef(), writer);
    CleanJson(outJson);
    return outJson;
}

FString UCommandBuilder::CprMotionsConfirmCommand()
{
	FString outJson;
	UCommandResolver* resolver = UCommandResolver::GetResolver();
	FString bizId;
	if (resolver == nullptr)
	{
#if WITH_EDITOR
		bizId = FString::Printf(TEXT("EditorTest"));
#else
		return outJson;
#endif
	}
	else
	{
		bizId = resolver->GetBizId();
	}

	if (bizId.IsEmpty())
	{
		if (resolver)
			resolver->onMessageUpdate.Broadcast(TEXT("未开始分析(bizId为空)\n因此无法请求CPR动作数据回放"), EMessageType::Message);
#if WITH_EDITOR
		bizId = FString::Printf(TEXT("EditorTest"));
#else
		return outJson;
#endif
	}

	TSharedPtr<FJsonObject> root = MakeShareable(new FJsonObject());
	root->SetStringField(TEXT("cmd"), TEXT("cprAnalysis"));
	root->SetStringField(TEXT("action"), TEXT("motionsConfirm"));
	root->SetStringField(TEXT("bizId"), bizId);

	TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
	FJsonSerializer::Serialize(root.ToSharedRef(), writer);
	CleanJson(outJson);
	return outJson;
}

FString UCommandBuilder::RefreshTimeStamp(const FString& json)
{
	TSharedPtr<FJsonObject> root;
	TSharedRef<TJsonReader<>> reader = TJsonReaderFactory<>::Create(json);
	if (FJsonSerializer::Deserialize(reader, root) && root.IsValid())
	{
		const int64 timestampMs = static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
		root->SetNumberField(TEXT("stamp"), (double)timestampMs);
		root->SetStringField(TEXT("bizId"), UCommandResolver::GetResolver()->GetBizId());

		FString outJson;
		TSharedRef<TJsonWriter<>> writer = TJsonWriterFactory<>::Create(&outJson);
		FJsonSerializer::Serialize(root.ToSharedRef(), writer);
		CleanJson(outJson);
		return outJson;
	}

	return json;
}