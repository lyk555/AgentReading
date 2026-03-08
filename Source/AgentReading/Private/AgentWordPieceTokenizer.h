#pragma once

#include "CoreMinimal.h"

struct FAgentTokenizedText
{
	TArray<int32> InputIds;
	TArray<int32> AttentionMask;
	TArray<int32> TokenTypeIds;
};

class FAgentWordPieceTokenizer
{
public:
	bool LoadFromFile(const FString& TokenizerPath, FString& OutError);
	bool Tokenize(const FString& Text, int32 MaxTokens, FAgentTokenizedText& OutTokens, FString& OutError) const;
	bool IsLoaded() const;

private:
	bool LoadTokenizerJson(const FString& TokenizerPath, FString& OutError);
	bool LoadVocabTxt(const FString& TokenizerPath, FString& OutError);
	void BasicTokenize(const FString& Text, TArray<FString>& OutTokens) const;
	void SplitWordPiece(const FString& Token, TArray<FString>& OutPieces) const;

	static bool IsWhitespace(TCHAR Ch);
	static bool IsPunctuation(TCHAR Ch);
	static bool IsCjk(TCHAR Ch);
	static bool LoadFileUtf8(const FString& Path, FString& OutText);

private:
	TMap<FString, int32> Vocab;
	int32 UnkId = INDEX_NONE;
	int32 ClsId = INDEX_NONE;
	int32 SepId = INDEX_NONE;
	int32 PadId = 0;
	bool bDoLowerCase = true;
};
