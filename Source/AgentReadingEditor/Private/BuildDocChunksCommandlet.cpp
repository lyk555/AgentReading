#include "BuildDocChunksCommandlet.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Misc/Parse.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Containers/UnrealString.h"

UBuildDocChunksCommandlet::UBuildDocChunksCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

static void CollectMarkdownFilesRecursive(const FString& RootDir, TArray<FString>& OutFiles)
{
	IFileManager& FM = IFileManager::Get();
	FM.FindFilesRecursive(OutFiles, *RootDir, TEXT("*.md"), true, false, false);
	FM.FindFilesRecursive(OutFiles, *RootDir, TEXT("*.markdown"), true, false, false);
}

static void SplitPatterns(const FString& Spec, TArray<FString>& OutPatterns)
{
	OutPatterns.Reset();
	if (Spec.IsEmpty()) return;

	Spec.ParseIntoArray(OutPatterns, TEXT(";"), /*bCullEmpty*/true);
	for (FString& P : OutPatterns)
	{
		P = P.TrimStartAndEnd();
	}
}

static bool MatchAnyWildcard(const FString& Text, const TArray<FString>& Patterns)
{
	if (Patterns.Num() == 0) return false;
	for (const FString& Pat : Patterns)
	{
		// UE 的通配符：* ? 支持
		if (Text.MatchesWildcard(Pat, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

// Strategy A：只 ingest “API/Lua 文档”，其它一律跳过
static bool ShouldIngestDocFile(const FString& AbsPath, const FString& DocsRootAbs, const TArray<FString>& Includes, const TArray<FString>& Excludes)
{
	// RelPath 用于匹配 Doc/xxx.md 这种
	FString Rel = AbsPath;
	FPaths::MakePathRelativeTo(Rel, *DocsRootAbs);
	Rel.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString FileName = FPaths::GetCleanFilename(AbsPath);

	// 先 Exclude（优先级最高）
	if (MatchAnyWildcard(Rel, Excludes) || MatchAnyWildcard(FileName, Excludes))
	{
		return false;
	}

	// Include 为空：给一个“默认白名单规则”
	if (Includes.Num() == 0)
	{
		// 默认只保留“明显是 API/Lua 手册”的文件（纯 ASCII，避免编码坑）
		// 你也可以完全依赖 -Include 参数，不想默认规则就 return false;
		const FString Lower = FileName.ToLower();
		if (Lower.Contains(TEXT("api"))) return true;
		if (Lower.Contains(TEXT("lua"))) return true;
		if (Lower.Contains(TEXT("binding"))) return true;
		if (Lower.Contains(TEXT("reference"))) return true;
		return false;
	}

	// Include 非空：只要命中 include 任意一个即可
	return MatchAnyWildcard(Rel, Includes) || MatchAnyWildcard(FileName, Includes);
}

static void LoadApiKeysIfProvided(const FString& ApiPath, TSet<FString>& OutKeys)
{
	OutKeys.Reset();
	if (ApiPath.IsEmpty()) return;

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ApiPath)) return;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
	if (!Root->TryGetArrayField(TEXT("records"), Records) || !Records) return;

	for (const TSharedPtr<FJsonValue>& V : *Records)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid()) continue;
		const FString Key = Obj->GetStringField(TEXT("key"));
		if (!Key.IsEmpty()) OutKeys.Add(Key);
	}
}

static void ExtractCandidateKeys(const FString& Text, const TSet<FString>& ApiKeysOrEmpty, TArray<FString>& OutKeys)
{
	OutKeys.Reset();

	// 支持 Env.Xxx / Math.Yyy / Time.Xxx / World.Xxx / UI.Xxx
	const FRegexPattern Pat(TEXT(R"(([A-Za-z_]\w*)\.([A-Za-z_]\w*))"));
	FRegexMatcher M(Pat, Text);

	TSet<FString> Seen;
	while (M.FindNext())
	{
		const FString Table = M.GetCaptureGroup(1);
		const FString Func = M.GetCaptureGroup(2);
		const FString Key = Table + TEXT(".") + Func;

		if (!ApiKeysOrEmpty.IsEmpty() && !ApiKeysOrEmpty.Contains(Key))
		{
			continue;
		}
		Seen.Add(Key);
	}

	OutKeys = Seen.Array();
	OutKeys.Sort();
}

static void WriteJsonlLine(IFileHandle* Handle, const TSharedPtr<FJsonObject>& Obj)
{
	if (!Handle || !Obj.IsValid()) return;

	FString Line;

	// 关键：用 CondensedJsonPrintPolicy，保证不产生换行/缩进
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);

	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);

	Line += TEXT("\n");

	FTCHARToUTF8 Conv(*Line);
	Handle->Write((const uint8*)Conv.Get(), Conv.Length());
}

static void FlushChunk(
	IFileHandle* Handle,
	const FString& DocRelPath,
	const FString& Heading,
	const FString& Type,
	const FString& FenceLang,
	const FString& Text,
	int32& InOutChunkId,
	const TSet<FString>& ApiKeysOrEmpty)
{
	FString Clean = Text;
	Clean.TrimStartAndEndInline();
	if (Clean.IsEmpty()) return;

	TArray<FString> Candidates;
	ExtractCandidateKeys(Clean, ApiKeysOrEmpty, Candidates);

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("doc_file"), DocRelPath);
	Obj->SetNumberField(TEXT("chunk_id"), InOutChunkId++);
	Obj->SetStringField(TEXT("heading"), Heading);
	Obj->SetStringField(TEXT("type"), Type);
	if (!FenceLang.IsEmpty()) Obj->SetStringField(TEXT("fence_lang"), FenceLang);
	Obj->SetStringField(TEXT("text"), Clean);

	TArray<TSharedPtr<FJsonValue>> CandVals;
	for (const FString& K : Candidates) CandVals.Add(MakeShared<FJsonValueString>(K));
	Obj->SetArrayField(TEXT("candidate_keys"), CandVals);

	WriteJsonlLine(Handle, Obj);
}

int32 UBuildDocChunksCommandlet::Main(const FString& Params)
{
	// UEEditor-Cmd.exe <Project>.uproject -run=BuildDocChunks -Docs="D:/Docs" -Out="D:/Saved/DocChunks.jsonl" -Api="D:/Saved/ApiRegistry.json"
	FString DocsDir, OutPath, ApiPath;
	FParse::Value(*Params, TEXT("-Docs="), DocsDir);
	FParse::Value(*Params, TEXT("-Out="), OutPath);
	FParse::Value(*Params, TEXT("-Api="), ApiPath);

	FString IncludeSpec, ExcludeSpec;
	FParse::Value(*Params, TEXT("-Include="), IncludeSpec);
	FParse::Value(*Params, TEXT("-Exclude="), ExcludeSpec);

	if (DocsDir.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Missing -Docs=..."));
		return 1;
	}
	if (OutPath.IsEmpty())
	{
		OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading/DocChunks.jsonl"));
	}

	TArray<FString> Files;
	CollectMarkdownFilesRecursive(DocsDir, Files);
	Files.Sort();

	TArray<FString> IncludePatterns, ExcludePatterns;
	SplitPatterns(IncludeSpec, IncludePatterns);
	SplitPatterns(ExcludeSpec, ExcludePatterns);

	const FString DocsDirAbs = FPaths::ConvertRelativePathToFull(DocsDir);

	int32 Skipped = 0;
	for (int32 i = Files.Num() - 1; i >= 0; --i)
	{
		const FString Abs = FPaths::ConvertRelativePathToFull(Files[i]);
		if (!ShouldIngestDocFile(Abs, DocsDirAbs, IncludePatterns, ExcludePatterns))
		{
			Files.RemoveAtSwap(i, 1, EAllowShrinking::No);
			Skipped++;
		}
	}

	Files.Sort();

	UE_LOG(LogTemp, Display, TEXT("Doc filter: kept=%d skipped=%d (Include='%s' Exclude='%s')"),
		Files.Num(), Skipped, *IncludeSpec, *ExcludeSpec);

	TSet<FString> ApiKeys;
	LoadApiKeysIfProvided(ApiPath, ApiKeys);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Handle(PF.OpenWrite(*OutPath, /*bAppend*/false));
	static const uint8 Utf8BOM[] = { 0xEF, 0xBB, 0xBF };
	Handle->Write(Utf8BOM, sizeof(Utf8BOM));

	if (!Handle)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to open %s"), *OutPath);
		return 2;
	}

	int32 TotalChunks = 0;

	for (const FString& File : Files)
	{
		FString Md;
		if (!FFileHelper::LoadFileToString(Md, *File)) continue;

		FString Rel = File;
		FPaths::MakePathRelativeTo(Rel, *DocsDir);

		TArray<FString> Lines;
		Md.ParseIntoArrayLines(Lines, true);

		FString Heading;
		FString AccText;

		bool bInFence = false;
		FString FenceLang;
		FString AccCode;

		int32 ChunkId = 0;

		auto FlushText = [&]()
			{
				FlushChunk(Handle.Get(), Rel, Heading, TEXT("text"), TEXT(""), AccText, ChunkId, ApiKeys);
				AccText.Reset();
			};

		auto FlushCode = [&]()
			{
				FlushChunk(Handle.Get(), Rel, Heading, TEXT("code"), FenceLang, AccCode, ChunkId, ApiKeys);
				AccCode.Reset();
				FenceLang.Reset();
			};

		for (int32 i = 0; i < Lines.Num(); i++)
		{
			const FString L = Lines[i];

			// code fence
			if (L.StartsWith(TEXT("```")))
			{
				if (!bInFence)
				{
					FlushText();
					bInFence = true;
					FenceLang = L.Mid(3).TrimStartAndEnd();
				}
				else
				{
					bInFence = false;
					FlushCode();
				}
				continue;
			}

			if (bInFence)
			{
				AccCode += L + TEXT("\n");
				continue;
			}

			// heading
			// 原来：if (L.StartsWith(TEXT("#")))
			if (L.StartsWith(TEXT("#")))
			{
				// 只把 "# " / "## " / "### " ... 识别为标题
				int32 HashCount = 0;
				while (HashCount < L.Len() && L[HashCount] == TEXT('#')) HashCount++;

				const bool bLooksLikeMarkdownHeading =
					(HashCount >= 1 && HashCount <= 6) &&
					(HashCount < L.Len() && L[HashCount] == TEXT(' '));

				if (bLooksLikeMarkdownHeading)
				{
					FlushText();
					Heading = L.TrimStartAndEnd();
					continue;
				}
			}

			// table block (连续 | 开头合并)
			// table block: 连续 | 行 => 每行一个 chunk
		if (L.StartsWith(TEXT("|")))
		{
			FlushText();

			int32 j = i;
			while (j < Lines.Num() && Lines[j].StartsWith(TEXT("|")))
			{
				const FString Row = Lines[j];

				// 跳过分隔行：| --- | --- |
				const FString TrimRow = Row.Replace(TEXT(" "), TEXT(""));
				const bool bIsSeparator = TrimRow.Contains(TEXT("|---")) || TrimRow.Contains(TEXT("|:---")) || TrimRow.Contains(TEXT("|---:"));

				if (!bIsSeparator)
				{
					FlushChunk(Handle.Get(), Rel, Heading, TEXT("table_row"), TEXT(""), Row + TEXT("\n"), ChunkId, ApiKeys);
				}

				j++;
			}

			i = j - 1;
			continue;
		}

			AccText += L + TEXT("\n");
		}

		FlushText();
		if (bInFence) FlushCode(); // 容错：未闭合 fence

		TotalChunks += ChunkId;
	}

	UE_LOG(LogTemp, Display, TEXT("Wrote DocChunks. Files=%d Chunks=%d Out=%s"), Files.Num(), TotalChunks, *OutPath);
	return 0;
}