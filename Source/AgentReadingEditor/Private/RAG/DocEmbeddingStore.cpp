#include "DocEmbeddingStore.h"

#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Robust UTF-8 loader for jsonl (handles BOM)
static bool LoadUtf8FileToString(const FString& Path, FString& OutText)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		return false;

	if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Bytes.RemoveAt(0, 3, /*bAllowShrinking*/false);
	}

	Bytes.Add(0); // null-terminate for UTF8 converter

	const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
	OutText = FString(Conv.Length(), Conv.Get());
	return true;
}

static void SplitLines(const FString& Text, TArray<FString>& OutLines)
{
	OutLines.Reset();

	// Normalize to \n
	FString Normalized = Text;
	Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"));

	Normalized.ParseIntoArrayLines(OutLines, /*CullEmpty*/false);
}

static bool ParseJsonLine(const FString& Line, TSharedPtr<FJsonObject>& OutObj)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

bool FDocEmbeddingStore::LoadFromJsonl(const FString& JsonlPath, FString& OutError)
{
	Records.Reset();
	Dim = 0;
	OutError.Reset();

	FString Text;
	if (!LoadUtf8FileToString(JsonlPath, Text))
	{
		OutError = FString::Printf(TEXT("Failed to read file: %s"), *JsonlPath);
		return false;
	}

	TArray<FString> Lines;
	SplitLines(Text, Lines);

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty())
			continue;

		TSharedPtr<FJsonObject> Obj;
		if (!ParseJsonLine(Line, Obj))
		{
			OutError = FString::Printf(TEXT("Bad JSON at line %d"), i + 1);
			return false;
		}

		FDocEmbeddingRecord Rec;
		if (!Obj->TryGetStringField(TEXT("doc_file"), Rec.DocFile) ||
			!Obj->TryGetStringField(TEXT("chunk_id"), Rec.ChunkId) ||
			!Obj->TryGetStringField(TEXT("heading"), Rec.Heading) ||
			!Obj->TryGetStringField(TEXT("type"), Rec.Type))
		{
			OutError = FString::Printf(TEXT("Missing required string fields at line %d"), i + 1);
			return false;
		}

		Rec.Dim = Obj->GetIntegerField(TEXT("dim"));
		Rec.Norm = (float)Obj->GetNumberField(TEXT("norm"));

		FString B64;
		if (!Obj->TryGetStringField(TEXT("embedding_b64_f16"), B64))
		{
			OutError = FString::Printf(TEXT("Missing embedding_b64_f16 at line %d"), i + 1);
			return false;
		}

		if (!FBase64::Decode(B64, Rec.PackedF16))
		{
			OutError = FString::Printf(TEXT("Base64 decode failed at line %d"), i + 1);
			return false;
		}

		if (Rec.Dim <= 0)
		{
			OutError = FString::Printf(TEXT("Bad dim at line %d: %d"), i + 1, Rec.Dim);
			return false;
		}

		if (Dim == 0)
			Dim = Rec.Dim;

		if (Rec.Dim != Dim)
		{
			OutError = FString::Printf(TEXT("Dim mismatch at line %d: %d vs %d"), i + 1, Rec.Dim, Dim);
			return false;
		}

		if (Rec.PackedF16.Num() != Dim * 2)
		{
			OutError = FString::Printf(TEXT("Packed bytes mismatch at line %d: got=%d expected=%d"),
				i + 1, Rec.PackedF16.Num(), Dim * 2);
			return false;
		}

		if (Rec.Norm <= 1e-8f)
		{
			// Allowed but unusual; keep it and it will be skipped at scoring
		}

		Records.Add(MoveTemp(Rec));
	}

	if (Records.Num() == 0)
	{
		OutError = TEXT("No records loaded (empty embeddings file?)");
		return false;
	}

	return true;
}

void FDocEmbeddingStore::SearchTopK(const TArray<uint8>& QueryVecPackedF16, float QueryNorm, int32 TopK, TArray<FScoredDocHit>& OutHits) const
{
	OutHits.Reset();

	if (Records.Num() == 0 || Dim <= 0)
		return;

	if (QueryVecPackedF16.Num() != Dim * 2)
		return;

	if (QueryNorm <= 1e-8f)
		return;

	const uint8* Q = QueryVecPackedF16.GetData();

	TArray<FScoredDocHit> Tmp;
	Tmp.Reserve(Records.Num());

	for (int32 idx = 0; idx < Records.Num(); ++idx)
	{
		const FDocEmbeddingRecord& R = Records[idx];
		if (R.Norm <= 1e-8f)
			continue;

		const float Dot = Dot_F16F16(Q, R.PackedF16.GetData(), Dim);
		const float Cos = Dot / (QueryNorm * R.Norm);

		FScoredDocHit H;
		H.Index = idx;
		H.Score = Cos;
		Tmp.Add(H);
	}

	Tmp.Sort([](const FScoredDocHit& A, const FScoredDocHit& B) { return A.Score > B.Score; });

	const int32 Take = FMath::Clamp(TopK, 1, Tmp.Num());
	OutHits.Append(Tmp.GetData(), Take);
}