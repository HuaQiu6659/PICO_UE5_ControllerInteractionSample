// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

/*
 * 追踪器类型
 */
UENUM(BlueprintType)
enum class EPicoType : uint8 {
	Forceps,
	Body
};

/*
 * 配置状态
 */
UENUM(BlueprintType)
enum class EConfigResult : uint8 {
	UnConfig,
	Configing,
	Successed,
	Failed
};

/*
 * 动作分析类型
 */
UENUM(BlueprintType)
enum class EMotionType : uint8 {
	Trajectory,
	Cpr,
	ZShape
};

/*
 * 消息类型
 */
UENUM(BlueprintType)
enum class EMessageType : uint8 {
	AnalysisResult,
	Message,
	Json
};