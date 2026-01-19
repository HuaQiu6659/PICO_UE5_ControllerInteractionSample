#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "TrackerData.generated.h"

// 追踪器数据结构（与截图中的键一一对应）
USTRUCT(BlueprintType)
struct MOTIONPOSTBACKER_API FTrackerData
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="sn"))
        FString sn;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="lt"))
        FVector lt = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="lr"))
        FQuat lr = FQuat::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="gt"))
        FVector gt = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="gr"))
        FQuat gr = FQuat::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tracker", meta=(DisplayName="isConfidence"))
        bool bIsConfidence = false;
};