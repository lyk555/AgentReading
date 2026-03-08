#pragma once

#include "CoreMinimal.h"
#include "AgentLuaApiTypes.generated.h"

USTRUCT(BlueprintType)
struct AGENTREADING_API FAgentLuaApiRecord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString Table;       // Env/Math
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString LuaName;     // CreateMap
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString Key;         // Env.CreateMap

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString Owner;       // UEnvWorldBlueprintLibrary
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString CppFunc;     // CreateMapData

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString LuaSig;      // ¿É¿Õ
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString CppSig;      // ¿É¿Õ
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString Desc;        // ¿É¿Õ

    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FString> Tags;

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FString SourceFile;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 SourceLine = 0;
};

USTRUCT(BlueprintType)
struct AGENTREADING_API FAgentLuaApiRegistry
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FAgentLuaApiRecord> Records;
};