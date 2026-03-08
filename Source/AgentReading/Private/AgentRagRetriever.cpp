#include "AgentRagRetriever.h"

#include "AgentEmbeddingBackend.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Math/Float16.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	struct FChunkRecord
	{
		FString DocFile;
		int32 ChunkId = -1;
		FString Heading;
		FString Type;
		FString Text;
		TArray<FString> CandidateKeys;
		int32 HitKeyCount = 0;
		float Score = 0.0f;
	};

	struct FApiRecord
	{
		FString Key;
		FString Owner;
		FString CppFunc;
		FString SourceFile;
		int32 SourceLine = 0;
		TArray<FString> AttachedChunkRefs;
		float Score = 0.0f;
	};

	struct FEmbeddingRecord
	{
		FString ChunkRef;
		int32 Dim = 0;
		float Norm = 0.0f;
		TArray<uint8> PackedF16;
	};

	static bool LoadUtf8FileToString(const FString& Path, FString& OutText)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return false;
		}

		if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
		{
			Bytes.RemoveAt(0, 3, EAllowShrinking::No);
		}

		Bytes.Add(0);
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
		OutText = FString(Conv.Length(), Conv.Get());
		return true;
	}

	static bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	static void ParseJsonLines(const FString& Text, TArray<FString>& OutLines)
	{
		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"));
		Normalized.ParseIntoArrayLines(OutLines, false);
	}

	static FString MakeChunkRef(const FString& DocFile, int32 ChunkId)
	{
		return FString::Printf(TEXT("%s::%d"), *DocFile, ChunkId);
	}

	static bool IsAsciiTerm(const FString& Term)
	{
		for (TCHAR Ch : Term)
		{
			if (Ch > 127)
			{
				return false;
			}
		}
		return true;
	}

	static void CollectAsciiTerms(const FString& Query, TSet<FString>& OutTerms)
	{
		const FRegexPattern Pattern(TEXT(R"([A-Za-z_][A-Za-z0-9_\.]+)"));
		FRegexMatcher Matcher(Pattern, Query);
		while (Matcher.FindNext())
		{
			FString Term = Matcher.GetCaptureGroup(0).ToLower();
			if (Term.Len() >= 2)
			{
				OutTerms.Add(MoveTemp(Term));
			}
		}
	}

	static void CollectCjkBigrams(const FString& Query, TSet<FString>& OutTerms)
	{
		FString Sequence;
		auto Flush = [&OutTerms, &Sequence]()
		{
			if (Sequence.Len() >= 2)
			{
				for (int32 Index = 0; Index < Sequence.Len() - 1; ++Index)
				{
					OutTerms.Add(Sequence.Mid(Index, 2));
				}
			}
			Sequence.Reset();
		};

		for (TCHAR Ch : Query)
		{
			if (Ch >= 0x4E00 && Ch <= 0x9FFF)
			{
				Sequence.AppendChar(Ch);
			}
			else
			{
				Flush();
			}
		}
		Flush();
	}

	static void ExtractExplicitKeys(const FString& Query, TSet<FString>& OutKeys)
	{
		const FRegexPattern Pattern(TEXT(R"(([A-Za-z_]\w*)\.([A-Za-z_]\w*))"));
		FRegexMatcher Matcher(Pattern, Query);
		while (Matcher.FindNext())
		{
			OutKeys.Add(Matcher.GetCaptureGroup(1) + TEXT(".") + Matcher.GetCaptureGroup(2));
		}
	}

	static float CountMatches(const FString& HaystackRaw, const TSet<FString>& Terms)
	{
		float Score = 0.0f;
		const FString HaystackAscii = HaystackRaw.ToLower();
		for (const FString& Term : Terms)
		{
			if (Term.IsEmpty())
			{
				continue;
			}

			if (IsAsciiTerm(Term))
			{
				if (HaystackAscii.Contains(Term))
				{
					Score += 1.0f;
				}
			}
			else if (HaystackRaw.Contains(Term))
			{
				Score += 0.6f;
			}
		}
		return Score;
	}

	static bool LoadChunks(const FString& Path, TArray<FChunkRecord>& OutChunks, FString& OutError)
	{
		OutChunks.Reset();
		FString Text;
		if (!LoadUtf8FileToString(Path, Text))
		{
			OutError = FString::Printf(TEXT("Failed to read chunk file: %s"), *Path);
			return false;
		}

		TArray<FString> Lines;
		ParseJsonLines(Text, Lines);
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> Obj;
			if (!ParseJsonObject(Line, Obj))
			{
				continue;
			}

			FChunkRecord Record;
			if (!Obj->TryGetStringField(TEXT("doc_file"), Record.DocFile) ||
				!Obj->TryGetStringField(TEXT("heading"), Record.Heading) ||
				!Obj->TryGetStringField(TEXT("type"), Record.Type) ||
				!Obj->TryGetStringField(TEXT("text"), Record.Text))
			{
				continue;
			}

			Record.ChunkId = Obj->GetIntegerField(TEXT("chunk_id"));

			const TArray<TSharedPtr<FJsonValue>>* CandidateKeys = nullptr;
			if (Obj->TryGetArrayField(TEXT("candidate_keys"), CandidateKeys) && CandidateKeys)
			{
				for (const TSharedPtr<FJsonValue>& Value : *CandidateKeys)
				{
					const FString Key = Value.IsValid() ? Value->AsString() : TEXT("");
					if (!Key.IsEmpty())
					{
						Record.CandidateKeys.Add(Key);
					}
				}
			}

			OutChunks.Add(MoveTemp(Record));
		}

		if (OutChunks.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No chunks loaded from: %s"), *Path);
			return false;
		}

		return true;
	}

	static bool LoadApis(const FString& Path, TArray<FApiRecord>& OutApis, FString& OutError)
	{
		OutApis.Reset();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to read api file: %s"), *Path);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!ParseJsonObject(JsonText, Root))
		{
			OutError = FString::Printf(TEXT("Failed to parse api file: %s"), *Path);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		if (!Root->TryGetArrayField(TEXT("records"), Records) || !Records)
		{
			OutError = FString::Printf(TEXT("Missing records[] in api file: %s"), *Path);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Records)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}

			FApiRecord Record;
			if (!Obj->TryGetStringField(TEXT("key"), Record.Key))
			{
				continue;
			}

			Obj->TryGetStringField(TEXT("owner"), Record.Owner);
			Obj->TryGetStringField(TEXT("cpp_func"), Record.CppFunc);
			Obj->TryGetStringField(TEXT("source_file"), Record.SourceFile);
			double SourceLineValue = 0.0;
			Obj->TryGetNumberField(TEXT("source_line"), SourceLineValue);
			Record.SourceLine = static_cast<int32>(SourceLineValue);

			const TArray<TSharedPtr<FJsonValue>>* Docs = nullptr;
			if (Obj->TryGetArrayField(TEXT("docs"), Docs) && Docs)
			{
				for (const TSharedPtr<FJsonValue>& DocValue : *Docs)
				{
					const TSharedPtr<FJsonObject> DocObj = DocValue.IsValid() ? DocValue->AsObject() : nullptr;
					if (!DocObj.IsValid())
					{
						continue;
					}

					FString DocFile;
					double ChunkIdValue = -1.0;
					if (DocObj->TryGetStringField(TEXT("doc_file"), DocFile) && DocObj->TryGetNumberField(TEXT("chunk_id"), ChunkIdValue))
					{
						const int32 ChunkId = static_cast<int32>(ChunkIdValue);
						Record.AttachedChunkRefs.Add(MakeChunkRef(DocFile, ChunkId));
					}
				}
			}

			OutApis.Add(MoveTemp(Record));
		}

		if (OutApis.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No api records loaded from: %s"), *Path);
			return false;
		}

		return true;
	}

	static bool LoadEmbeddings(const FString& Path, TMap<FString, FEmbeddingRecord>& OutEmbeddings, int32& OutDim, FString& OutError)
	{
		OutEmbeddings.Reset();
		OutDim = 0;

		FString Text;
		if (!LoadUtf8FileToString(Path, Text))
		{
			OutError = FString::Printf(TEXT("Failed to read embeddings file: %s"), *Path);
			return false;
		}

		TArray<FString> Lines;
		ParseJsonLines(Text, Lines);
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> Obj;
			if (!ParseJsonObject(Line, Obj))
			{
				continue;
			}

			FString DocFile;
			double ChunkIdValue = -1.0;
			if (!Obj->TryGetStringField(TEXT("doc_file"), DocFile) || !Obj->TryGetNumberField(TEXT("chunk_id"), ChunkIdValue))
			{
				continue;
			}

			FEmbeddingRecord Record;
			Record.ChunkRef = MakeChunkRef(DocFile, static_cast<int32>(ChunkIdValue));
			Record.Dim = Obj->GetIntegerField(TEXT("dim"));
			Record.Norm = static_cast<float>(Obj->GetNumberField(TEXT("norm")));

			FString B64;
			if (!Obj->TryGetStringField(TEXT("embedding_b64_f16"), B64))
			{
				continue;
			}

			if (!FBase64::Decode(B64, Record.PackedF16))
			{
				continue;
			}

			if (Record.Dim <= 0 || Record.PackedF16.Num() != Record.Dim * 2)
			{
				continue;
			}

			if (OutDim == 0)
			{
				OutDim = Record.Dim;
			}
			if (Record.Dim != OutDim)
			{
				continue;
			}

			OutEmbeddings.Add(Record.ChunkRef, MoveTemp(Record));
		}

		if (OutEmbeddings.Num() == 0)
		{
			OutError = FString::Printf(TEXT("No embeddings loaded from: %s"), *Path);
			return false;
		}

		return true;
	}

	static FORCEINLINE float DotF16F16(const uint8* A, const uint8* B, int32 Dim)
	{
		const FFloat16* FA = reinterpret_cast<const FFloat16*>(A);
		const FFloat16* FB = reinterpret_cast<const FFloat16*>(B);
		float Sum = 0.0f;
		for (int32 Index = 0; Index < Dim; ++Index)
		{
			Sum += float(FA[Index]) * float(FB[Index]);
		}
		return Sum;
	}

	static FString TrimForPrompt(const FString& Text, int32 MaxChars)
	{
		FString Out = Text.TrimStartAndEnd();
		if (Out.Len() > MaxChars)
		{
			Out = Out.Left(MaxChars) + TEXT("...");
		}
		return Out;
	}
}

FString FAgentRagContext::BuildPromptSection(int32 MaxDocChars) const
{
	FString Out;
	Out += TEXT("[RAG Context]\n");
	Out += TEXT("Use the retrieved project knowledge below as ground truth when it is relevant. Prefer cited APIs and document snippets over guesswork.\n");

	if (ApiHits.Num() > 0)
	{
		Out += TEXT("\n[Relevant APIs]\n");
		for (const FAgentRagApiHit& Hit : ApiHits)
		{
			Out += FString::Printf(TEXT("- %s | %s::%s | line %d\n"),
				*Hit.Key,
				*Hit.Owner,
				*Hit.CppFunc,
				Hit.SourceLine);
		}
	}

	if (DocHits.Num() > 0)
	{
		Out += TEXT("\n[Relevant Docs]\n");
		for (const FAgentRagDocHit& Hit : DocHits)
		{
			Out += FString::Printf(TEXT("- %s | %s | chunk %d | %s | score %.3f\n%s\n"),
				*Hit.DocFile,
				*Hit.Heading,
				Hit.ChunkId,
				*Hit.Type,
				Hit.Score,
				*TrimForPrompt(Hit.Text, MaxDocChars));
			if (Hit.CandidateKeys.Num() > 0)
			{
				Out += FString::Printf(TEXT("  candidate_keys: %s\n"), *FString::Join(Hit.CandidateKeys, TEXT(", ")));
			}
		}
	}

	Out += TEXT("\nIf the retrieved context is insufficient, say so explicitly instead of inventing project-specific APIs.\n");
	return Out;
}

bool FAgentRagRetriever::BuildContext(const FString& Query, int32 MaxDocs, int32 MaxApis, FAgentRagContext& OutContext, FString& OutError)
{
	OutContext = FAgentRagContext();
	OutError.Reset();

	if (Query.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Empty query.");
		return false;
	}

	const FString BaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading"));
	const FString ChunkPath = FPaths::Combine(BaseDir, TEXT("DocChunks.jsonl"));
	const FString EmbPath = FPaths::Combine(BaseDir, TEXT("DocEmbeddings.jsonl"));
	const FString EnrichedApiPath = FPaths::Combine(BaseDir, TEXT("EnrichedRegistry.json"));
	const FString ApiPath = IFileManager::Get().FileExists(*EnrichedApiPath)
		? EnrichedApiPath
		: FPaths::Combine(BaseDir, TEXT("ApiRegistry.json"));

	TArray<FChunkRecord> Chunks;
	if (!LoadChunks(ChunkPath, Chunks, OutError))
	{
		return false;
	}

	TArray<FApiRecord> Apis;
	if (!LoadApis(ApiPath, Apis, OutError))
	{
		return false;
	}

	TSet<FString> Terms;
	CollectAsciiTerms(Query, Terms);
	CollectCjkBigrams(Query, Terms);

	TSet<FString> ExplicitKeys;
	ExtractExplicitKeys(Query, ExplicitKeys);

	TMap<FString, float> VectorScores;
	{
		TMap<FString, FEmbeddingRecord> Embeddings;
		int32 EmbDim = 0;
		FString EmbError;
		if (LoadEmbeddings(EmbPath, Embeddings, EmbDim, EmbError))
		{
			FString BackendError;
			TUniquePtr<IAgentEmbeddingBackend> Backend = FAgentEmbeddingBackendFactory::Create(BackendError);
			if (Backend)
			{
				int32 QueryDim = 0;
				float QueryNorm = 0.0f;
				TArray<uint8> QueryPacked;
				if (Backend->EmbedQuery(Query, QueryDim, QueryNorm, QueryPacked, BackendError) && QueryDim == EmbDim && QueryNorm > 1e-6f)
				{
					for (const TPair<FString, FEmbeddingRecord>& Pair : Embeddings)
					{
						const FEmbeddingRecord& Record = Pair.Value;
						if (Record.Norm <= 1e-6f)
						{
							continue;
						}

						const float Dot = DotF16F16(QueryPacked.GetData(), Record.PackedF16.GetData(), EmbDim);
						const float Cos = Dot / (QueryNorm * Record.Norm);
						if (Cos > 0.05f)
						{
							VectorScores.Add(Pair.Key, Cos);
						}
					}
				}
			}
		}
	}

	TSet<FString> SelectedChunkRefs;
	TSet<FString> SelectedApiKeys;

	for (FChunkRecord& Chunk : Chunks)
	{
		Chunk.Score = 0.0f;
		Chunk.HitKeyCount = 0;

		const FString ChunkRef = MakeChunkRef(Chunk.DocFile, Chunk.ChunkId);
		if (const float* VecScore = VectorScores.Find(ChunkRef))
		{
			Chunk.Score += (*VecScore) * 8.0f;
		}

		for (const FString& Key : Chunk.CandidateKeys)
		{
			if (ExplicitKeys.Contains(Key))
			{
				Chunk.Score += 6.0f;
				Chunk.HitKeyCount++;
			}
		}

		Chunk.Score += CountMatches(Chunk.DocFile, Terms) * 0.4f;
		Chunk.Score += CountMatches(Chunk.Heading, Terms) * 1.5f;
		Chunk.Score += CountMatches(Chunk.Text, Terms) * 1.0f;
		Chunk.Score += CountMatches(FString::Join(Chunk.CandidateKeys, TEXT(" ")), Terms) * 2.0f;
	}

	Chunks.Sort([](const FChunkRecord& A, const FChunkRecord& B)
	{
		if (!FMath::IsNearlyEqual(A.Score, B.Score))
		{
			return A.Score > B.Score;
		}
		return A.HitKeyCount > B.HitKeyCount;
	});

	for (const FChunkRecord& Chunk : Chunks)
	{
		if (Chunk.Score <= 0.0f || OutContext.DocHits.Num() >= MaxDocs)
		{
			continue;
		}

		FAgentRagDocHit Hit;
		Hit.DocFile = Chunk.DocFile;
		Hit.ChunkId = Chunk.ChunkId;
		Hit.Heading = Chunk.Heading;
		Hit.Type = Chunk.Type;
		Hit.Text = Chunk.Text;
		Hit.Score = Chunk.Score;
		Hit.CandidateKeys = Chunk.CandidateKeys;
		OutContext.DocHits.Add(MoveTemp(Hit));

		SelectedChunkRefs.Add(MakeChunkRef(Chunk.DocFile, Chunk.ChunkId));
		for (const FString& Key : Chunk.CandidateKeys)
		{
			SelectedApiKeys.Add(Key);
		}
	}

	for (FApiRecord& Api : Apis)
	{
		Api.Score = 0.0f;
		if (ExplicitKeys.Contains(Api.Key))
		{
			Api.Score += 8.0f;
		}
		if (SelectedApiKeys.Contains(Api.Key))
		{
			Api.Score += 4.0f;
		}
		Api.Score += CountMatches(Api.Key, Terms) * 2.5f;
		Api.Score += CountMatches(Api.Owner, Terms) * 0.6f;
		Api.Score += CountMatches(Api.CppFunc, Terms) * 1.2f;

		for (const FString& ChunkRef : Api.AttachedChunkRefs)
		{
			if (SelectedChunkRefs.Contains(ChunkRef))
			{
				Api.Score += 1.5f;
			}
		}
	}

	Apis.Sort([](const FApiRecord& A, const FApiRecord& B)
	{
		return A.Score > B.Score;
	});

	for (const FApiRecord& Api : Apis)
	{
		if (Api.Score <= 0.0f || OutContext.ApiHits.Num() >= MaxApis)
		{
			continue;
		}

		FAgentRagApiHit Hit;
		Hit.Key = Api.Key;
		Hit.Owner = Api.Owner;
		Hit.CppFunc = Api.CppFunc;
		Hit.SourceFile = Api.SourceFile;
		Hit.SourceLine = Api.SourceLine;
		Hit.Score = Api.Score;
		OutContext.ApiHits.Add(MoveTemp(Hit));
	}

	if (!OutContext.HasAny())
	{
		OutError = TEXT("No relevant RAG context found.");
		return false;
	}

	return true;
}
