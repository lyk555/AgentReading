#pragma once

#include "CoreMinimal.h"

struct AGENTREADING_API FAgentRagDocHit
{
	FString DocFile;
	int32 ChunkId = -1;
	FString Heading;
	FString Type;
	FString Text;
	float Score = 0.0f;
	TArray<FString> CandidateKeys;
};

struct AGENTREADING_API FAgentRagApiHit
{
	FString Key;
	FString Owner;
	FString CppFunc;
	FString SourceFile;
	int32 SourceLine = 0;
	float Score = 0.0f;
};

struct AGENTREADING_API FAgentRagContext
{
	TArray<FAgentRagDocHit> DocHits;
	TArray<FAgentRagApiHit> ApiHits;

	bool HasAny() const
	{
		return DocHits.Num() > 0 || ApiHits.Num() > 0;
	}

	FString BuildPromptSection(int32 MaxDocChars = 1800) const;
};

class AGENTREADING_API FAgentRagRetriever
{
public:
	static bool BuildContext(const FString& Query, int32 MaxDocs, int32 MaxApis, FAgentRagContext& OutContext, FString& OutError);
};
