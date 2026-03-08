#include "AgentPythonEmbeddingBackend.h"

#include "AgentReadingSettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	static FString EscapeForQuotedArg(const FString& In)
	{
		FString S = In;
		S.ReplaceInline(TEXT("\r"), TEXT(" "));
		S.ReplaceInline(TEXT("\n"), TEXT(" "));
		S.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return S;
	}

	static bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}
}

FString FAgentPythonEmbeddingBackend::GetBackendName() const
{
	return TEXT("Python");
}

bool FAgentPythonEmbeddingBackend::EmbedQuery(const FString& QueryText, int32& OutDim, float& OutNorm, TArray<uint8>& OutPackedF16, FString& OutError)
{
	OutDim = 0;
	OutNorm = 0.0f;
	OutPackedF16.Reset();
	OutError.Reset();

	const UAgentReadingSettings* Settings = UAgentReadingSettings::Get();
	FString PythonExe = Settings->PythonExeOverride;
	if (PythonExe.IsEmpty())
	{
		PythonExe = FPaths::Combine(FPaths::ProjectDir(), TEXT(".venv/Scripts/python.exe"));
		if (!FPaths::FileExists(PythonExe))
		{
			PythonExe = TEXT("python");
		}
	}

	const FString ScriptPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Settings->PythonEmbedScript));
	if (!FPaths::FileExists(ScriptPath))
	{
		OutError = FString::Printf(TEXT("Missing embed script: %s"), *ScriptPath);
		return false;
	}

	const FString Args = FString::Printf(
		TEXT("-u \"%s\" --text \"%s\" --model \"%s\""),
		*ScriptPath,
		*EscapeForQuotedArg(QueryText),
		*EscapeForQuotedArg(Settings->PythonModelName));

	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	FProcHandle Proc = FPlatformProcess::CreateProc(
		*PythonExe,
		*Args,
		false,
		true,
		true,
		nullptr,
		0,
		nullptr,
		WritePipe,
		nullptr);

	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutError = FString::Printf(TEXT("Failed to launch python backend: %s"), *PythonExe);
		return false;
	}

	FString StdOut;
	const double Start = FPlatformTime::Seconds();
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		StdOut += FPlatformProcess::ReadPipe(ReadPipe);
		if ((FPlatformTime::Seconds() - Start) > 30.0)
		{
			FPlatformProcess::TerminateProc(Proc, true);
			break;
		}
		FPlatformProcess::Sleep(0.02f);
	}

	FPlatformProcess::WaitForProc(Proc);
	StdOut += FPlatformProcess::ReadPipe(ReadPipe);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
	if (ReturnCode != 0)
	{
		OutError = FString::Printf(TEXT("Python backend exited with code %d"), ReturnCode);
		return false;
	}

	const int32 Begin = StdOut.Find(TEXT("{"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
	const int32 End = StdOut.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (Begin == INDEX_NONE || End == INDEX_NONE || End <= Begin)
	{
		OutError = TEXT("Python backend stdout did not contain JSON.");
		return false;
	}

	TSharedPtr<FJsonObject> Obj;
	if (!ParseJsonObject(StdOut.Mid(Begin, End - Begin + 1), Obj))
	{
		OutError = TEXT("Failed to parse python backend JSON.");
		return false;
	}

	OutDim = Obj->GetIntegerField(TEXT("dim"));
	OutNorm = static_cast<float>(Obj->GetNumberField(TEXT("norm")));
	FString B64;
	if (!Obj->TryGetStringField(TEXT("embedding_b64_f16"), B64) || !FBase64::Decode(B64, OutPackedF16))
	{
		OutError = TEXT("Failed to decode python backend embedding payload.");
		return false;
	}

	if (OutDim <= 0 || OutPackedF16.Num() != OutDim * 2)
	{
		OutError = TEXT("Python backend returned invalid embedding dimensions.");
		return false;
	}

	return true;
}
