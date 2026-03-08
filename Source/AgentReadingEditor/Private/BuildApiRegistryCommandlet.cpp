#include "BuildApiRegistryCommandlet.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/Parse.h"                    
#include "Internationalization/Regex.h"    
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "AgentLuaApiTypes.h"

UBuildApiRegistryCommandlet::UBuildApiRegistryCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

static void CollectFilesRecursive(const FString& RootDir, TArray<FString>& OutFiles)
{
	IFileManager& FM = IFileManager::Get();
	FM.FindFilesRecursive(OutFiles, *RootDir, TEXT("*.cpp"), true, false, false);
	FM.FindFilesRecursive(OutFiles, *RootDir, TEXT("*.h"), true, false, false);
}

int32 UBuildApiRegistryCommandlet::Main(const FString& Params)
{
	// 用法示例：
	// UEEditor-Cmd.exe <Project>.uproject -run=BuildApiRegistry -Src="D:/Your/Source" -Out="D:/Saved/ApiRegistry.json"

	FString SrcDir, OutPath;
	FParse::Value(*Params, TEXT("-Src="), SrcDir);
	FParse::Value(*Params, TEXT("-Out="), OutPath);

	if (SrcDir.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Missing -Src=..."));
		return 1;
	}
	if (OutPath.IsEmpty())
	{
		OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentReading/ApiRegistry.json"));
	}

	TArray<FString> Files;
	CollectFilesRecursive(SrcDir, Files);

	// 关键：按你的注册格式抽取
	// MVP 先按两类正则：
	// 1) LUA_ENSURE_GLOBAL_TABLE(L, "Env")
	// 2) REGISTER_LUA_STATIC(L, "CreateMap", UEnvWorldBlueprintLibrary, CreateMapData)
	const FRegexPattern TablePat(TEXT(R"(LUA_ENSURE_GLOBAL_TABLE\s*\(\s*L\s*,\s*\"([^\"]+)\"\s*\))"));
	const FRegexPattern RegPat(TEXT(R"(REGISTER_LUA_STATIC\s*\(\s*L\s*,\s*\"([^\"]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_:]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))"));

	FString CurrentTable;
	TArray<FAgentLuaApiRecord> Records;

	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) continue;

		// 行号统计：预先把每个字符位置对应行号
		TArray<int32> PosToLine;
		PosToLine.SetNum(Text.Len() + 1);
		int32 Line = 1;
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			PosToLine[i] = Line;
			if (Text[i] == '\n') Line++;
		}
		PosToLine[Text.Len()] = Line;

		// Table 扫描
		{
			FRegexMatcher M(TablePat, Text);
			while (M.FindNext())
			{
				CurrentTable = M.GetCaptureGroup(1);
				// 不 return；继续扫 register
			}
		}

		// Register 扫描
		{
			FRegexMatcher M(RegPat, Text);
			while (M.FindNext())
			{
				const FString LuaName = M.GetCaptureGroup(1);
				const FString Owner = M.GetCaptureGroup(2);
				const FString CppFunc = M.GetCaptureGroup(3);

				const int32 MatchBegin = M.GetMatchBeginning();
				const int32 SrcLine = (MatchBegin >= 0 && MatchBegin < PosToLine.Num()) ? PosToLine[MatchBegin] : 0;

				FAgentLuaApiRecord R;
				R.Table = CurrentTable.IsEmpty() ? TEXT("Global") : CurrentTable;
				R.LuaName = LuaName;
				R.Key = R.Table + TEXT(".") + R.LuaName;
				R.Owner = Owner;
				R.CppFunc = CppFunc;
				R.SourceFile = File;
				R.SourceLine = SrcLine;
				Records.Add(MoveTemp(R));
			}
		}
	}

	// 写 JSON
	FString Json;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);

	W->WriteObjectStart();
	W->WriteValue(TEXT("src"), SrcDir);
	W->WriteArrayStart(TEXT("records"));
	for (const auto& R : Records)
	{
		W->WriteObjectStart();
		W->WriteValue(TEXT("table"), R.Table);
		W->WriteValue(TEXT("lua_name"), R.LuaName);
		W->WriteValue(TEXT("key"), R.Key);
		W->WriteValue(TEXT("owner"), R.Owner);
		W->WriteValue(TEXT("cpp_func"), R.CppFunc);
		W->WriteValue(TEXT("source_file"), R.SourceFile);
		W->WriteValue(TEXT("source_line"), R.SourceLine);
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();
	W->WriteObjectEnd();
	W->Close();

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
	if (!FFileHelper::SaveStringToFile(Json, *OutPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to write %s"), *OutPath);
		return 2;
	}

	UE_LOG(LogTemp, Display, TEXT("Wrote %d API records to %s"), Records.Num(), *OutPath);
	return 0;
}