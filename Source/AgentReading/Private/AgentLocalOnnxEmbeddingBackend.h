#pragma once

#include "AgentEmbeddingBackend.h"

class FAgentLocalOnnxEmbeddingBackend final : public IAgentEmbeddingBackend
{
public:
	virtual FString GetBackendName() const override;
	virtual bool EmbedQuery(const FString& QueryText, int32& OutDim, float& OutNorm, TArray<uint8>& OutPackedF16, FString& OutError) override;
};
