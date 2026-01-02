// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "IOHelper.generated.h"

/**
 * 
 */
UCLASS()
class LINFRAMEWORK_API UIOHelper : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	
public:
	/**
	 * 读取文件返回字符串
	 * @param FilePath 文件路径
	 * @return 文件内容字符串
	 */
	UFUNCTION(BlueprintCallable, Category = "LinFramework|IO")
	static FString LoadFileToString(const FString& FilePath);

	/**
	 * 读取文件返回byte[]
	 * @param FilePath 文件路径
	 * @return 文件内容字节数组
	 */
	UFUNCTION(BlueprintCallable, Category = "LinFramework|IO")
	static TArray<uint8> LoadFileToBytes(const FString& FilePath);

	/**
	 * 读取文件返回分割后的string[]
	 * @param FilePath 文件路径
	 * @param Delimiter 分隔符
	 * @return 分割后的字符串数组
	 */
	UFUNCTION(BlueprintCallable, Category = "LinFramework|IO")
	static TArray<FString> LoadFileToStringArray(const FString& FilePath, const FString& Delimiter = TEXT("\n"));
};
