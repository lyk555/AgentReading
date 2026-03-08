#pragma once

#include "Commandlets/Commandlet.h"
#include "QueryAgentCommandlet.generated.h"

/**
 * QueryAgent Commandlet
 * 
 * Usage:
 *   -run=QueryAgent -Query="..." [-TopKDocs=8]
 *   [-PythonExe="python"] [-EmbedScript="(ProjectDir)/Tools/embeddings/embed_query.py"] [-Model="sentence-transformers/all-MiniLM-L6-v2"]
 *   [-DocEmb="(Saved)/AgentReading/DocEmbeddings.jsonl"] [-Out="(Saved)/AgentReading/SearchResult.json"]
 */
UCLASS()
class AGENTREADINGEDITOR_API UQueryAgentCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQueryAgentCommandlet();
	virtual int32 Main(const FString& Params) override;
};