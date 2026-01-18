// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DataHelper.generated.h"

/**
 * 数据转换
 */
UCLASS()
class LINFRAMEWORK_API UDataHelper : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintPure, Category = "LinFramework|Data")
	static FVector VectorFromUnity(FVector vector) 
	{
		return FVector(vector.Z, vector.X, vector.Y) * 100;
	}

	UFUNCTION(BlueprintPure, Category = "LinFramework|Data")
	static FQuat QuatFromUnity(FQuat quat) 
	{
		return FQuat(quat.Z, quat.X, quat.Y, quat.W);
	}

	UFUNCTION(BlueprintPure, Category = "LinFramework|Data")
	static FVector VectorToUnity(FVector vector) 
	{
		return FVector(vector.Y, vector.Z, vector.X) / 100;
	}

	UFUNCTION(BlueprintPure, Category = "LinFramework|Data")
	static FQuat QuatToUnity(FQuat quat)
	{
		return FQuat(quat.Y, quat.Z, quat.X, quat.W);
	}
};
