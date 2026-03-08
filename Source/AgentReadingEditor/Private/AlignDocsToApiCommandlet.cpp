#include "AlignDocsToApiCommandlet.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/Parse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Containers/StringConv.h" // FUTF8ToTCHAR

UAlignDocsToApiCommandlet::UAlignDocsToApiCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

static bool LoadJsonRoot(const FString& Path, TSharedPtr<FJsonObject>& OutRoot)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(R, OutRoot) && OutRoot.IsValid();
}

static bool SaveJsonRoot(const FString& Path, const TSharedPtr<FJsonObject>& Root)
{
	FString Out;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), W)) return false;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveStringToFile(Out, *Path);
}

static void ExtractTopLevelJsonObjects(const FString& Text, TArray<FString>& OutObjects)
{
	OutObjects.Reset();

	int32 Depth = 0;
	bool bInString = false;
	bool bEscape = false;
	int32 ObjStart = INDEX_NONE;

	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const TCHAR C = Text[i];

		if (bInString)
		{
			if (bEscape) { bEscape = false; continue; }
			if (C == TEXT('\\')) { bEscape = true; continue; }
			if (C == TEXT('"')) { bInString = false; continue; }
			continue;
		}
		else
		{
			if (C == TEXT('"')) { bInString = true; continue; }

			if (C == TEXT('{'))
			{
				if (Depth == 0) ObjStart = i;
				Depth++;
			}
			else if (C == TEXT('}'))
			{
				Depth--;
				if (Depth == 0 && ObjStart != INDEX_NONE)
				{
					const int32 Len = i - ObjStart + 1;
					OutObjects.Add(Text.Mid(ObjStart, Len));
					ObjStart = INDEX_NONE;
				}
			}
		}
	}
}

int32 UAlignDocsToApiCommandlet::Main(const FString& Params)
{
	// UEEditor-Cmd.exe <Project>.uproject -run=AlignDocsToApi -Api=".../ApiRegistry.json" -Chunks=".../DocChunks.jsonl" -Out=".../EnrichedRegistry.json" -Report=".../AlignmentReport.json"
	FString ApiPath, ChunksPath, OutPath, ReportPath;
	FParse::Value(*Params, TEXT("-Api="), ApiPath);
	FParse::Value(*Params, TEXT("-Chunks="), ChunksPath);
	FParse::Value(*Params, TEXT("-Out="), OutPath);
	FParse::Value(*Params, TEXT("-Report="), ReportPath);

	if (ApiPath.IsEmpty() || ChunksPath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Missing -Api=... or -Chunks=..."));
		return 1;
	}
	if (OutPath.IsEmpty())
	{
		OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading/EnrichedRegistry.json"));
	}
	if (ReportPath.IsEmpty())
	{
		ReportPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading/AlignmentReport.json"));
	}

	TSharedPtr<FJsonObject> ApiRoot;
	if (!LoadJsonRoot(ApiPath, ApiRoot))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load Api %s"), *ApiPath);
		return 2;
	}

	const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
	if (!ApiRoot->TryGetArrayField(TEXT("records"), Records) || !Records)
	{
		UE_LOG(LogTemp, Error, TEXT("Api missing records[]"));
		return 3;
	}

	// key -> attachments[]
	TMap<FString, TArray<TSharedPtr<FJsonObject>>> KeyToDocs;

	// 读 jsonl
	// chunks (兼容单行 JSONL 和多行 pretty JSON)
	TArray<FString> ChunkObjects;
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *ChunksPath))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load Chunks %s"), *ChunksPath);
			return 4;
		}

		// 强制按 UTF-8 解码（不依赖 BOM，避免中文导致 JSON 反序列化失败）
		FUTF8ToTCHAR Utf8((const ANSICHAR*)Bytes.GetData(), Bytes.Num());
		FString Text(Utf8.Length(), Utf8.Get());

		ExtractTopLevelJsonObjects(Text, ChunkObjects);
	}

	int32 ChunksTotal = 0;
	int32 ChunksAligned = 0;
	TArray<TSharedPtr<FJsonObject>> Unaligned;

	for (const FString& ObjText : ChunkObjects)
	{
		const FString Trim = ObjText.TrimStartAndEnd();
		if (Trim.IsEmpty()) continue;

		TSharedPtr<FJsonObject> Chunk;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Trim);
		if (!FJsonSerializer::Deserialize(R, Chunk) || !Chunk.IsValid())
		{
			continue;
		}

		ChunksTotal++;

		const TArray<TSharedPtr<FJsonValue>>* Cand = nullptr;
		if (!Chunk->TryGetArrayField(TEXT("candidate_keys"), Cand) || !Cand || Cand->Num() == 0)
		{
			Unaligned.Add(Chunk);
			continue;
		}

		bool bAny = false;
		for (const TSharedPtr<FJsonValue>& V : *Cand)
		{
			const FString Key = V.IsValid() ? V->AsString() : TEXT("");
			if (Key.IsEmpty()) continue;

			TSharedPtr<FJsonObject> Att = MakeShared<FJsonObject>();
			Att->SetStringField(TEXT("doc_file"), Chunk->GetStringField(TEXT("doc_file")));
			Att->SetNumberField(TEXT("chunk_id"), Chunk->GetNumberField(TEXT("chunk_id")));
			Att->SetStringField(TEXT("heading"), Chunk->GetStringField(TEXT("heading")));
			Att->SetStringField(TEXT("type"), Chunk->GetStringField(TEXT("type")));
			if (Chunk->HasField(TEXT("fence_lang")))
			{
				Att->SetStringField(TEXT("fence_lang"), Chunk->GetStringField(TEXT("fence_lang")));
			}
			Att->SetNumberField(TEXT("score"), 1.0);

			KeyToDocs.FindOrAdd(Key).Add(Att);
			bAny = true;
		}

		if (bAny) ChunksAligned++;
		else Unaligned.Add(Chunk);
	}

	// enrich records：每条 record 增加 docs:[]
	int32 ApiTotal = Records->Num();
	int32 ApiWithDocs = 0;
	TArray<FString> ApiNoDocs;

	TArray<TSharedPtr<FJsonValue>> NewRecords;
	NewRecords.Reserve(ApiTotal);

	for (const TSharedPtr<FJsonValue>& V : *Records)
	{
		const TSharedPtr<FJsonObject> Rec = V.IsValid() ? V->AsObject() : nullptr;
		if (!Rec.IsValid()) continue;

		const FString Key = Rec->GetStringField(TEXT("key"));

		TArray<TSharedPtr<FJsonValue>> DocsArr;
		if (const TArray<TSharedPtr<FJsonObject>>* AttList = KeyToDocs.Find(Key))
		{
			if (AttList->Num() > 0) ApiWithDocs++;
			for (const TSharedPtr<FJsonObject>& Att : *AttList)
			{
				DocsArr.Add(MakeShared<FJsonValueObject>(Att));
			}
		}
		else
		{
			ApiNoDocs.Add(Key);
		}

		TSharedPtr<FJsonObject> NewRec = MakeShared<FJsonObject>(*Rec);
		NewRec->SetArrayField(TEXT("docs"), DocsArr);
		NewRecords.Add(MakeShared<FJsonValueObject>(NewRec));
	}

	TSharedPtr<FJsonObject> OutRoot = MakeShared<FJsonObject>(*ApiRoot);
	OutRoot->SetArrayField(TEXT("records"), NewRecords);

	if (!SaveJsonRoot(OutPath, OutRoot))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save %s"), *OutPath);
		return 5;
	}

	// report
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("api_total"), ApiTotal);
	Report->SetNumberField(TEXT("api_with_docs"), ApiWithDocs);
	Report->SetNumberField(TEXT("api_no_docs"), ApiTotal - ApiWithDocs);
	Report->SetNumberField(TEXT("chunks_total"), ChunksTotal);
	Report->SetNumberField(TEXT("chunks_aligned"), ChunksAligned);
	Report->SetNumberField(TEXT("chunks_unaligned"), ChunksTotal - ChunksAligned);

	ApiNoDocs.Sort();
	TArray<TSharedPtr<FJsonValue>> NoDocsArr;
	for (const FString& K : ApiNoDocs) NoDocsArr.Add(MakeShared<FJsonValueString>(K));
	Report->SetArrayField(TEXT("api_no_docs_keys"), NoDocsArr);

	TArray<TSharedPtr<FJsonValue>> UnalignedArr;
	for (const TSharedPtr<FJsonObject>& C : Unaligned)
	{
		TSharedPtr<FJsonObject> Lite = MakeShared<FJsonObject>();
		Lite->SetStringField(TEXT("doc_file"), C->GetStringField(TEXT("doc_file")));
		Lite->SetNumberField(TEXT("chunk_id"), C->GetNumberField(TEXT("chunk_id")));
		Lite->SetStringField(TEXT("heading"), C->GetStringField(TEXT("heading")));
		Lite->SetStringField(TEXT("type"), C->GetStringField(TEXT("type")));
		UnalignedArr.Add(MakeShared<FJsonValueObject>(Lite));
	}
	Report->SetArrayField(TEXT("unaligned_chunks"), UnalignedArr);

	if (!SaveJsonRoot(ReportPath, Report))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save report %s"), *ReportPath);
		return 6;
	}

	UE_LOG(LogTemp, Display, TEXT("AlignDocsToApi done. Api=%d WithDocs=%d Chunks=%d Aligned=%d"), ApiTotal, ApiWithDocs, ChunksTotal, ChunksAligned);
	UE_LOG(LogTemp, Display, TEXT("Out=%s Report=%s"), *OutPath, *ReportPath);
	return 0;
}