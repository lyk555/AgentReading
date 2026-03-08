#pragma once

#include "CoreMinimal.h"
#include "AgentEmbeddingTypes.h"

/**
 * Loads DocEmbeddings.jsonl and does brute-force cosine TopK search.
 * Designed for MVP scale (hundreds~low thousands of chunks).
 */
class FDocEmbeddingStore
{
public:
	bool LoadFromJsonl(const FString& JsonlPath, FString& OutError);

	int32 Num() const { return Records.Num(); }
	int32 GetDim() const { return Dim; }
	const FDocEmbeddingRecord& Get(int32 Index) const { return Records[Index]; }

	void SearchTopK(const TArray<uint8>& QueryVecPackedF16, float QueryNorm, int32 TopK, TArray<FScoredDocHit>& OutHits) const;

private:
	int32 Dim = 0;
	TArray<FDocEmbeddingRecord> Records;
};