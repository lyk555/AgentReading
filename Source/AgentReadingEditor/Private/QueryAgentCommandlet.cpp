#include "QueryAgentCommandlet.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "RAG/DocEmbeddingStore.h"

static FString TrimQuotes(const FString& In)
{
	FString S = In.TrimStartAndEnd();
	if (S.Len() >= 2 && ((S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\""))) || (S.StartsWith(TEXT("'")) && S.EndsWith(TEXT("'")))))
	{
		return S.Mid(1, S.Len() - 2);
	}
	return S;
}

// Escape a string for use inside a quoted command-line arg.
// We pass: --text "<escaped>"
static FString EscapeForQuotedArg(const FString& In)
{
	FString S = In;
	S.ReplaceInline(TEXT("\r"), TEXT(" "));
	S.ReplaceInline(TEXT("\n"), TEXT(" "));
	// Escape quotes
	S.ReplaceInline(TEXT("\""), TEXT("\\\""));
	return S;
}

static bool ParseSingleJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObj)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

static bool RunPythonEmbedQuery(
	const FString& PythonExe,
	const FString& ScriptPath,
	const FString& ModelName,
	const FString& QueryText, 
	double TimeoutSec, 
	int32& OutDim,
	float& OutNorm,
	TArray<uint8>& OutPackedF16,
	FString& OutStdOut,
	FString& OutError)
{
	OutDim = 0;
	OutNorm = 0.0f;
	OutPackedF16.Reset();
	OutStdOut.Reset();
	OutError.Reset();

	// Build args:
	// "<script>" --text "<query>" --model "<model>"
	const FString EscText = EscapeForQuotedArg(QueryText);
	const FString EscModel = EscapeForQuotedArg(ModelName);

	const FString Args = FString::Printf(
		TEXT("-u \"%s\" --text \"%s\" --model \"%s\""),
		*ScriptPath,
		*EscText,
		*EscModel);

	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	// Launch
	FProcHandle Proc = FPlatformProcess::CreateProc(
		*PythonExe,
		*Args,
		/*bLaunchDetached*/ false,  // 关键：不要 detached，避免 Windows 单独弹出控制台窗口
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		nullptr,
		0,
		nullptr,
		WritePipe,
		nullptr);

	if (!Proc.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to launch python. PythonExe=%s Args=%s"), *PythonExe, *Args);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		return false;
	}

	// Wait
	const double StartT = FPlatformTime::Seconds();
	double LastLogT = StartT;

	while (FPlatformProcess::IsProcRunning(Proc))
	{
		OutStdOut += FPlatformProcess::ReadPipe(ReadPipe);

		const double Now = FPlatformTime::Seconds();
		if (TimeoutSec > 0.0 && (Now - StartT) > TimeoutSec)
		{
			OutError = FString::Printf(TEXT("Python timed out after %.1fs. PythonExe=%s Args=%s"),
				TimeoutSec, *PythonExe, *Args);
			FPlatformProcess::TerminateProc(Proc, /*KillTree*/true);
			break;
		}

		if ((Now - LastLogT) > 5.0)
		{
			UE_LOG(LogTemp, Warning, TEXT("QueryAgent: waiting python... %.1fs elapsed"), (float)(Now - StartT));
			LastLogT = Now;
		}

		FPlatformProcess::Sleep(0.05f);
	}

	FPlatformProcess::WaitForProc(Proc);
	OutStdOut += FPlatformProcess::ReadPipe(ReadPipe);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
	if (ReturnCode != 0)
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Python exited with code %d. PythonExe=%s Args=%s"),
				ReturnCode, *PythonExe, *Args);
		}
		return false;
	}

	if (!OutError.IsEmpty())
	{
		return false;
	}

	if (ReturnCode != 0)
	{
		OutError = FString::Printf(TEXT("Python exited with code %d. Stdout=%s"), ReturnCode, *OutStdOut);
		return false;
	}

	// Some python env prints warnings; try to find the first '{' and last '}'.
	FString JsonLine = OutStdOut;
	{
		const int32 L = JsonLine.Find(TEXT("{"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
		const int32 R = JsonLine.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (L == INDEX_NONE || R == INDEX_NONE || R <= L)
		{
			OutError = FString::Printf(TEXT("Python stdout does not contain a JSON object: %s"), *OutStdOut);
			return false;
		}
		JsonLine = JsonLine.Mid(L, R - L + 1);
	}

	TSharedPtr<FJsonObject> Obj;
	if (!ParseSingleJsonObject(JsonLine, Obj))
	{
		OutError = FString::Printf(TEXT("Failed to parse JSON from python stdout: %s"), *JsonLine);
		return false;
	}

	if (!Obj->HasField(TEXT("dim")) || !Obj->HasField(TEXT("norm")) || !Obj->HasField(TEXT("embedding_b64_f16")))
	{
		OutError = FString::Printf(TEXT("JSON missing required fields: %s"), *JsonLine);
		return false;
	}

	OutDim = Obj->GetIntegerField(TEXT("dim"));
	OutNorm = (float)Obj->GetNumberField(TEXT("norm"));
	const FString B64 = Obj->GetStringField(TEXT("embedding_b64_f16"));

	if (OutDim <= 0)
	{
		OutError = FString::Printf(TEXT("Bad dim from python: %d"), OutDim);
		return false;
	}

	if (!FBase64::Decode(B64, OutPackedF16))
	{
		OutError = TEXT("Base64 decode failed for query embedding.");
		return false;
	}

	if (OutPackedF16.Num() != OutDim * 2)
	{
		OutError = FString::Printf(TEXT("Query embedding bytes mismatch: got=%d expected=%d"), OutPackedF16.Num(), OutDim * 2);
		return false;
	}

	if (OutNorm <= 1e-8f)
	{
		// If model normalizes, norm ~ 1; if not, still should be > 0
		OutError = FString::Printf(TEXT("Query norm too small: %f"), OutNorm);
		return false;
	}

	return true;
}

static FString DefaultSavedAgentReadingPath(const TCHAR* Filename)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading"), Filename);
}

UQueryAgentCommandlet::UQueryAgentCommandlet()
{
	IsClient = false;
	IsServer = false;

	// 关键：这是 Editor Commandlet（在 *Editor 模块里），需要让引擎走 Editor 路径创建 GEditor，
	// 否则像 EditorDataStorage/TedsCore 这类模块初始化会 check(GEditor) 直接崩。
	IsEditor = true;

	LogToConsole = true;
}

int32 UQueryAgentCommandlet::Main(const FString& Params)
{

	UE_LOG(LogTemp, Warning, TEXT(">>> ENTER QueryAgent Main. Params=%s"), *Params);
	GLog->FlushThreadedLogs();
	// Parse args
	FString Query;
	int32 TopKDocs = 8;
	double PyTimeoutSec = 300.0;
	FString PythonExe = TEXT("python");
	FString EmbedScript; // default computed from ProjectDir
	FString ModelName = TEXT("sentence-transformers/all-MiniLM-L6-v2");

	FString DocEmbPath; // default saved path
	FString OutPath;    // default saved path

	FParse::Value(*Params, TEXT("Query="), Query);
	FParse::Value(*Params, TEXT("TopKDocs="), TopKDocs);

	FParse::Value(*Params, TEXT("PythonExe="), PythonExe);
	FParse::Value(*Params, TEXT("EmbedScript="), EmbedScript);
	FParse::Value(*Params, TEXT("Model="), ModelName);

	FParse::Value(*Params, TEXT("DocEmb="), DocEmbPath);
	FParse::Value(*Params, TEXT("Out="), OutPath);
	FParse::Value(*Params, TEXT("PyTimeoutSec="), PyTimeoutSec);

	Query = TrimQuotes(Query);
	PythonExe = TrimQuotes(PythonExe);
	EmbedScript = TrimQuotes(EmbedScript);
	ModelName = TrimQuotes(ModelName);
	DocEmbPath = TrimQuotes(DocEmbPath);
	OutPath = TrimQuotes(OutPath);

	if (Query.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("QueryAgent: missing -Query=\"...\""));
		return 1;
	}

	TopKDocs = FMath::Clamp(TopKDocs, 1, 100);

	// If user didn't specify -PythonExe=..., prefer project-local venv python.
	if (PythonExe.IsEmpty() || PythonExe.Equals(TEXT("python"), ESearchCase::IgnoreCase))
	{
		const FString VenvPy = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT(".venv/Scripts/python.exe"))
		);

		if (FPaths::FileExists(VenvPy))
		{
			PythonExe = VenvPy;
		}
	}

	if (EmbedScript.IsEmpty())
	{
		EmbedScript = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/embeddings/embed_query.py")));
	}

	if (DocEmbPath.IsEmpty())
	{
		DocEmbPath = DefaultSavedAgentReadingPath(TEXT("DocEmbeddings.jsonl"));
	}

	if (OutPath.IsEmpty())
	{
		OutPath = DefaultSavedAgentReadingPath(TEXT("SearchResult.json"));
	}

	// Load embedding store
	FDocEmbeddingStore Store;
	FString Err;
	if (!Store.LoadFromJsonl(DocEmbPath, Err))
	{
		UE_LOG(LogTemp, Error, TEXT("QueryAgent: Load doc embeddings failed: %s"), *Err);
		UE_LOG(LogTemp, Error, TEXT("DocEmb=%s"), *DocEmbPath);
		return 2;
	}

	// Embed query via python
	int32 QDim = 0;
	float QNorm = 0.0f;
	TArray<uint8> QPackedF16;
	FString PyOut;
	if (!RunPythonEmbedQuery(PythonExe, EmbedScript, ModelName, Query, PyTimeoutSec, QDim, QNorm, QPackedF16, PyOut, Err))
	{
		UE_LOG(LogTemp, Error, TEXT("QueryAgent: Embed query failed: %s"), *Err);
		UE_LOG(LogTemp, Error, TEXT("PythonExe=%s"), *PythonExe);
		UE_LOG(LogTemp, Error, TEXT("EmbedScript=%s"), *EmbedScript);
		UE_LOG(LogTemp, Error, TEXT("Model=%s"), *ModelName);
		UE_LOG(LogTemp, Error, TEXT("PythonStdout=%s"), *PyOut);
		return 3;
	}

	if (QDim != Store.GetDim())
	{
		UE_LOG(LogTemp, Error, TEXT("QueryAgent: dim mismatch. QueryDim=%d StoreDim=%d"), QDim, Store.GetDim());
		return 4;
	}

	// Search TopK docs
	TArray<FScoredDocHit> DocHits;
	Store.SearchTopK(QPackedF16, QNorm, TopKDocs, DocHits);

	// Write SearchResult.json
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("query"), Query);

	TArray<TSharedPtr<FJsonValue>> DocsJson;
	for (const FScoredDocHit& H : DocHits)
	{
		const FDocEmbeddingRecord& R = Store.Get(H.Index);

		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("doc_file"), R.DocFile);
		O->SetStringField(TEXT("chunk_id"), R.ChunkId);
		O->SetStringField(TEXT("heading"), R.Heading);
		O->SetStringField(TEXT("type"), R.Type);
		O->SetNumberField(TEXT("score"), (double)H.Score);
		DocsJson.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("topk_docs"), DocsJson);

	// Placeholder for Milestone 4.1 (Doc->API backref) and Milestone 4.2 (API embeddings)
	Root->SetArrayField(TEXT("topk_apis"), TArray<TSharedPtr<FJsonValue>>());

	FString OutStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	const FString OutDir = FPaths::GetPath(OutPath);
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/true);

	if (!FFileHelper::SaveStringToFile(OutStr, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("QueryAgent: Failed to write output: %s"), *OutPath);
		return 5;
	}

	UE_LOG(LogTemp, Display, TEXT("QueryAgent OK."));
	UE_LOG(LogTemp, Display, TEXT("  DocEmb: %s  (records=%d dim=%d)"), *DocEmbPath, Store.Num(), Store.GetDim());
	UE_LOG(LogTemp, Display, TEXT("  Out   : %s"), *OutPath);

	return 0;
}