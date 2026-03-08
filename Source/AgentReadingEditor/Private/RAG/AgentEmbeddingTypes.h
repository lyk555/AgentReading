#pragma once

#include "CoreMinimal.h"
#include "Math/Float16.h"

// One doc chunk embedding record
struct FDocEmbeddingRecord
{
	FString DocFile;
	FString ChunkId;
	FString Heading;
	FString Type;

	int32 Dim = 0;
	float Norm = 0.0f;

	// Packed float16 bytes: Dim * 2
	TArray<uint8> PackedF16;
};

struct FScoredDocHit
{
	int32 Index = -1;
	float Score = -1.0f;
};

static FORCEINLINE float Dot_F16F16(const uint8* A, const uint8* B, int32 Dim)
{
	const FFloat16* FA = reinterpret_cast<const FFloat16*>(A);
	const FFloat16* FB = reinterpret_cast<const FFloat16*>(B);

	float Sum = 0.0f;
	for (int32 i = 0; i < Dim; ++i)
	{
		Sum += float(FA[i]) * float(FB[i]);
	}
	return Sum;
}