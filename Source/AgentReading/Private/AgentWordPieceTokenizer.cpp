#include "AgentWordPieceTokenizer.h"

#include "Containers/StringConv.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	static bool ParseJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	static bool TryResolveLowercaseFlag(const TSharedPtr<FJsonObject>& Obj, bool& bOutLowercase)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		bool bValue = false;
		if (Obj->TryGetBoolField(TEXT("lowercase"), bValue))
		{
			bOutLowercase = bValue;
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Normalizers = nullptr;
		if (Obj->TryGetArrayField(TEXT("normalizers"), Normalizers) && Normalizers)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Normalizers)
			{
				if (TryResolveLowercaseFlag(Value.IsValid() ? Value->AsObject() : nullptr, bOutLowercase))
				{
					return true;
				}
			}
		}

		return false;
	}
}

bool FAgentWordPieceTokenizer::LoadFromFile(const FString& TokenizerPath, FString& OutError)
{
	Vocab.Reset();
	UnkId = INDEX_NONE;
	ClsId = INDEX_NONE;
	SepId = INDEX_NONE;
	PadId = 0;
	bDoLowerCase = true;

	const FString LowerPath = TokenizerPath.ToLower();
	if (LowerPath.EndsWith(TEXT(".txt")))
	{
		return LoadVocabTxt(TokenizerPath, OutError);
	}

	return LoadTokenizerJson(TokenizerPath, OutError);
}

bool FAgentWordPieceTokenizer::Tokenize(const FString& Text, int32 MaxTokens, FAgentTokenizedText& OutTokens, FString& OutError) const
{
	OutTokens = FAgentTokenizedText();
	OutError.Reset();

	if (!IsLoaded())
	{
		OutError = TEXT("Tokenizer is not loaded.");
		return false;
	}

	const int32 SafeMaxTokens = FMath::Max(8, MaxTokens);
	TArray<FString> BasicTokens;
	BasicTokenize(Text, BasicTokens);

	TArray<int32> PieceIds;
	PieceIds.Reserve(SafeMaxTokens);
	for (const FString& Token : BasicTokens)
	{
		TArray<FString> Pieces;
		SplitWordPiece(Token, Pieces);
		for (const FString& Piece : Pieces)
		{
			const int32* TokenId = Vocab.Find(Piece);
			PieceIds.Add(TokenId ? *TokenId : UnkId);
			if (PieceIds.Num() >= SafeMaxTokens - 2)
			{
				break;
			}
		}
		if (PieceIds.Num() >= SafeMaxTokens - 2)
		{
			break;
		}
	}

	OutTokens.InputIds.Reserve(PieceIds.Num() + 2);
	OutTokens.AttentionMask.Reserve(PieceIds.Num() + 2);
	OutTokens.TokenTypeIds.Reserve(PieceIds.Num() + 2);

	OutTokens.InputIds.Add(ClsId);
	OutTokens.AttentionMask.Add(1);
	OutTokens.TokenTypeIds.Add(0);

	for (int32 Id : PieceIds)
	{
		OutTokens.InputIds.Add(Id);
		OutTokens.AttentionMask.Add(1);
		OutTokens.TokenTypeIds.Add(0);
	}

	OutTokens.InputIds.Add(SepId);
	OutTokens.AttentionMask.Add(1);
	OutTokens.TokenTypeIds.Add(0);
	return true;
}

bool FAgentWordPieceTokenizer::IsLoaded() const
{
	return Vocab.Num() > 0 && UnkId != INDEX_NONE && ClsId != INDEX_NONE && SepId != INDEX_NONE;
}

bool FAgentWordPieceTokenizer::LoadTokenizerJson(const FString& TokenizerPath, FString& OutError)
{
	FString JsonText;
	if (!LoadFileUtf8(TokenizerPath, JsonText))
	{
		OutError = FString::Printf(TEXT("Failed to read tokenizer json: %s"), *TokenizerPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!ParseJsonObject(JsonText, Root))
	{
		OutError = FString::Printf(TEXT("Failed to parse tokenizer json: %s"), *TokenizerPath);
		return false;
	}

	if (const TSharedPtr<FJsonObject>* NormalizerObj = nullptr; Root->TryGetObjectField(TEXT("normalizer"), NormalizerObj) && NormalizerObj)
	{
		bool bLowercase = true;
		if (TryResolveLowercaseFlag(*NormalizerObj, bLowercase))
		{
			bDoLowerCase = bLowercase;
		}
	}

	const TSharedPtr<FJsonObject>* ModelObj = nullptr;
	if (!Root->TryGetObjectField(TEXT("model"), ModelObj) || !ModelObj || !ModelObj->IsValid())
	{
		OutError = TEXT("Tokenizer json missing model object.");
		return false;
	}

	const TSharedPtr<FJsonObject>* VocabObj = nullptr;
	if (!(*ModelObj)->TryGetObjectField(TEXT("vocab"), VocabObj) || !VocabObj || !VocabObj->IsValid())
	{
		OutError = TEXT("Tokenizer json missing model.vocab.");
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*VocabObj)->Values)
	{
		const int32 TokenId = Pair.Value.IsValid() ? static_cast<int32>(Pair.Value->AsNumber()) : INDEX_NONE;
		if (TokenId != INDEX_NONE)
		{
			Vocab.Add(Pair.Key, TokenId);
		}
	}

	FString UnkToken;
	if ((*ModelObj)->TryGetStringField(TEXT("unk_token"), UnkToken))
	{
		UnkId = Vocab.FindRef(UnkToken);
	}

	const TArray<TSharedPtr<FJsonValue>>* AddedTokens = nullptr;
	if (Root->TryGetArrayField(TEXT("added_tokens"), AddedTokens) && AddedTokens)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AddedTokens)
		{
			const TSharedPtr<FJsonObject> Added = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Added.IsValid())
			{
				continue;
			}

			FString Content;
			double IdValue = -1.0;
			if (!Added->TryGetStringField(TEXT("content"), Content) || !Added->TryGetNumberField(TEXT("id"), IdValue))
			{
				continue;
			}

			const int32 Id = static_cast<int32>(IdValue);
			Vocab.FindOrAdd(Content) = Id;
			if (Content == TEXT("[UNK]"))
			{
				UnkId = Id;
			}
			else if (Content == TEXT("[CLS]"))
			{
				ClsId = Id;
			}
			else if (Content == TEXT("[SEP]"))
			{
				SepId = Id;
			}
			else if (Content == TEXT("[PAD]"))
			{
				PadId = Id;
			}
		}
	}

	if (UnkId == INDEX_NONE)
	{
		UnkId = Vocab.FindRef(TEXT("[UNK]"));
	}
	ClsId = Vocab.FindRef(TEXT("[CLS]"));
	SepId = Vocab.FindRef(TEXT("[SEP]"));
	PadId = Vocab.Contains(TEXT("[PAD]")) ? Vocab.FindRef(TEXT("[PAD]")) : 0;

	if (!IsLoaded())
	{
		OutError = TEXT("Tokenizer json did not contain required special tokens or vocab.");
		return false;
	}

	return true;
}

bool FAgentWordPieceTokenizer::LoadVocabTxt(const FString& TokenizerPath, FString& OutError)
{
	FString Text;
	if (!LoadFileUtf8(TokenizerPath, Text))
	{
		OutError = FString::Printf(TEXT("Failed to read vocab txt: %s"), *TokenizerPath);
		return false;
	}

	TArray<FString> Lines;
	Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
	Text.ParseIntoArrayLines(Lines, false);
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FString Token = Lines[Index].TrimStartAndEnd();
		if (!Token.IsEmpty())
		{
			Vocab.Add(Token, Index);
		}
	}

	UnkId = Vocab.FindRef(TEXT("[UNK]"));
	ClsId = Vocab.FindRef(TEXT("[CLS]"));
	SepId = Vocab.FindRef(TEXT("[SEP]"));
	PadId = Vocab.Contains(TEXT("[PAD]")) ? Vocab.FindRef(TEXT("[PAD]")) : 0;
	bDoLowerCase = true;

	if (!IsLoaded())
	{
		OutError = TEXT("Vocab txt did not contain required special tokens.");
		return false;
	}
	return true;
}

void FAgentWordPieceTokenizer::BasicTokenize(const FString& Text, TArray<FString>& OutTokens) const
{
	OutTokens.Reset();
	FString Working = bDoLowerCase ? Text.ToLower() : Text;
	FString Current;

	auto FlushCurrent = [&]()
	{
		if (!Current.IsEmpty())
		{
			OutTokens.Add(Current);
			Current.Reset();
		}
	};

	for (TCHAR Ch : Working)
	{
		if (IsWhitespace(Ch))
		{
			FlushCurrent();
			continue;
		}

		if (IsCjk(Ch) || IsPunctuation(Ch))
		{
			FlushCurrent();
			OutTokens.Add(FString(1, &Ch));
			continue;
		}

		Current.AppendChar(Ch);
	}

	FlushCurrent();
}

void FAgentWordPieceTokenizer::SplitWordPiece(const FString& Token, TArray<FString>& OutPieces) const
{
	OutPieces.Reset();
	if (Token.IsEmpty())
	{
		return;
	}

	if (Vocab.Contains(Token))
	{
		OutPieces.Add(Token);
		return;
	}

	const int32 TokenLen = Token.Len();
	int32 Start = 0;
	while (Start < TokenLen)
	{
		int32 End = TokenLen;
		FString BestPiece;
		while (End > Start)
		{
			FString Piece = Token.Mid(Start, End - Start);
			if (Start > 0)
			{
				Piece = TEXT("##") + Piece;
			}
			if (Vocab.Contains(Piece))
			{
				BestPiece = MoveTemp(Piece);
				break;
			}
			--End;
		}

		if (BestPiece.IsEmpty())
		{
			OutPieces.Reset();
			OutPieces.Add(TEXT("[UNK]"));
			return;
		}

		OutPieces.Add(BestPiece);
		Start = End;
	}
}

bool FAgentWordPieceTokenizer::IsWhitespace(TCHAR Ch)
{
	return FChar::IsWhitespace(Ch);
}

bool FAgentWordPieceTokenizer::IsPunctuation(TCHAR Ch)
{
	return FChar::IsPunct(Ch);
}

bool FAgentWordPieceTokenizer::IsCjk(TCHAR Ch)
{
	return (Ch >= 0x4E00 && Ch <= 0x9FFF) || (Ch >= 0x3400 && Ch <= 0x4DBF);
}

bool FAgentWordPieceTokenizer::LoadFileUtf8(const FString& Path, FString& OutText)
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
