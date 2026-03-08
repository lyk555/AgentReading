#pragma once

#include "CoreMinimal.h"

class IAgentEmbeddingBackend
{
public:
	virtual ~IAgentEmbeddingBackend() = default;

	virtual FString GetBackendName() const = 0;
	virtual bool EmbedQuery(const FString& QueryText, int32& OutDim, float& OutNorm, TArray<uint8>& OutPackedF16, FString& OutError) = 0;
};

class AGENTREADING_API FAgentEmbeddingBackendFactory
{
public:
	static TUniquePtr<IAgentEmbeddingBackend> Create(FString& OutError);
};
