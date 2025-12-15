// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "HAL/CriticalSection.h"
#include "Runtime/Json/Public/Dom/JsonObject.h"
#include "Runtime/Core/Public/Templates/SharedPointer.h"
#include "Enums.h"
#include "CommandResolver.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAnalysisStateDelegate, bool, isAnalyzing);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMessageDelegate, const FString&, message, EMessageType, messageType);

UCLASS()
class MOTIONPOSTBACKER_API UCommandResolver : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category = "Motion")
		static UCommandResolver* GetResolver();

    UFUNCTION(BlueprintCallable, Category = "Motion")
		FString GetBizId() { return currentBizId; }

	UFUNCTION(BlueprintCallable, Category = "Motion")
		bool ShouldSendTrackerData();

	UPROPERTY(BlueprintAssignable, Category="Motion")
		FMessageDelegate onMessageUpdate;

	UPROPERTY(BlueprintAssignable, Category = "Motion")
		FAnalysisStateDelegate onAnalysisStateChanged;

	UFUNCTION(BlueprintCallable, Category = "Motion")
		void Resolve(const FString& json);

	UFUNCTION(BlueprintCallable, Category = "Motion")
		void SetAnalyzing(bool analyzing) { isAnalyzing = analyzing; }

	EMotionType GetCurrentMode() { return currentMode; }
	bool IsAnalyzing() { return isAnalyzing; }

private:
    // 处理单条 JSON 指令（已按粘包拆分后的一个包）
    void ResolveOne(const FString& json);
    // 粘包处理缓冲区：累积未完整的包体，待下次补齐后再解析
    FString recvBuffer;

    static const int32 SUCCESS_CODE = 1000;

    static UCommandResolver* Instance;

	FCriticalSection idMapMutex;
	EMotionType currentMode;
	bool isAnalyzing;
	FString currentBizId;

	void OnRescueAppConfig(const TSharedPtr<FJsonObject>& json);

	// trajectory
	void OnTrajectoryAnalysis(const TSharedPtr<FJsonObject>& json);
	void OnTrajectoryAnalysis_Begin(const TSharedPtr<FJsonObject>& json);
	void OnTrajectoryAnalysis_Stop(const TSharedPtr<FJsonObject>& json);
	void OnTrajectoryAnalysis_TrReport(const TSharedPtr<FJsonObject>& json);
	void OnTrajectoryAnalysis_Result(const TSharedPtr<FJsonObject>& json);

	// cpr
	void OnCprAnalysis(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_Begin(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_End(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_Result(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_FrameComparsion(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_Motions(const TSharedPtr<FJsonObject>& json);
	void OnCprAnalysis_MotionsConfirm(const TSharedPtr<FJsonObject>& json);

	// zshape
	void OnZShapeTrajectoryAnalysis(const TSharedPtr<FJsonObject>& json);
	void OnZShapeTrajectoryAnalysis_Begin(const TSharedPtr<FJsonObject>& json);
	void OnZShapeTrajectoryAnalysis_Stop(const TSharedPtr<FJsonObject>& json);
	void OnZShapeTrajectoryAnalysis_TrReport(const TSharedPtr<FJsonObject>& json);
	void OnZShapeTrajectoryAnalysis_Result(const TSharedPtr<FJsonObject>& json);
};
