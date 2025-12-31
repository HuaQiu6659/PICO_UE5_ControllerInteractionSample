// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Enums.h"
#include "TrackerData.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CommandBuilder.generated.h"

UCLASS()
class MOTIONPOSTBACKER_API UCommandBuilder : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
	/// <summary>
	/// 配置指令
	/// </summary>
	/// <param name="clipperSn"> 无菌钳传感器 </param>
	/// <param name="dummySn"> 假人传感器 </param>
	/// <returns> command Json </returns>
    UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString GlobalConfigCommand(const FString& clipperSn, const FString& dummySn);

	/// <summary>
	/// 开始动作分析指令
	/// </summary>
	/// <param name="motionType"> Trajectory 或 Cpr </param>
	/// <returns> command Json </returns>
    UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString StartCommand(EMotionType motionType);

	/// <summary>
	/// 结束动作分析指令
	/// </summary>
	/// <param name="motionType"> Trajectory 或 Cpr </param>
	/// <returns> command Json </returns>
    UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString EndCommand(EMotionType motionType);

	/// <summary>
	/// 分析结果查询
	/// </summary>
	/// <param name="motionType"> Trajectory 或 Cpr </param>
	/// <returns> command Json </returns>
    UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString AnalysisCommand(EMotionType motionType);

	/// <summary>
	/// 追踪器数据上报（数组）
	/// </summary>
	/// <param name="trackers"> FTrackerData 数组 </param>
	/// <returns> command Json </returns>
    UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString TrackerDatas(const TArray<FTrackerData>& trackers);

	/// <summary>
	/// CPR动作数据回放请求
	/// </summary>
	/// <returns></returns>
	UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString CprMotionsCommand();

		/// <summary>
		/// CPR动作数据接收确认请求
		/// </summary>
		/// <returns></returns>
	UFUNCTION(BlueprintPure, Category = "CommandBuilder")
		static FString CprMotionsConfirmCommand();
};
