#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AgentReadingSettings.generated.h"

UENUM(BlueprintType)
enum class EAgentEmbeddingBackendType : uint8
{
	Auto UMETA(DisplayName = "Auto"),
	Python UMETA(DisplayName = "Python (Dev Only)"),
	LocalOnnx UMETA(DisplayName = "Local ONNX")
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Agent Reading Settings"))
class AGENTREADING_API UAgentReadingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAgentReadingSettings();

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	EAgentEmbeddingBackendType EmbeddingBackend = EAgentEmbeddingBackendType::Auto;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString PythonExeOverride;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString PythonEmbedScript;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString PythonModelName;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString LocalOnnxModelPath;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString LocalTokenizerPath;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding")
	FString LocalRuntimeName;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Embedding", meta = (ClampMin = "8", ClampMax = "512"))
	int32 LocalMaxTokens = 256;

	static const UAgentReadingSettings* Get();
	static UAgentReadingSettings* GetMutable();

	virtual FName GetCategoryName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
