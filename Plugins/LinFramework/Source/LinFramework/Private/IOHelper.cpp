// Fill out your copyright notice in the Description page of Project Settings.


#include "IOHelper.h"
#include "Misc/FileHelper.h"

FString UIOHelper::LoadFileToString(const FString& FilePath)
{
	FString result;
	// 如果读取失败，Result保持为空
	FFileHelper::LoadFileToString(result, *FilePath);
	return result;
}

TArray<uint8> UIOHelper::LoadFileToBytes(const FString& FilePath)
{
	TArray<uint8> result;
	// 如果读取失败，Result保持为空
	FFileHelper::LoadFileToArray(result, *FilePath);
	return result;
}

TArray<FString> UIOHelper::LoadFileToStringArray(const FString& FilePath, const FString& Delimiter)
{
	TArray<FString> result;
	FString fileContent;

	if (FFileHelper::LoadFileToString(fileContent, *FilePath))
	{
		// 使用传入的分隔符进行分割，默认剔除空字符串
		fileContent.ParseIntoArray(result, *Delimiter, true);
	}

	return result;
}
