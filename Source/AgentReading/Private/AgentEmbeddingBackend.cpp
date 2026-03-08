#include "AgentEmbeddingBackend.h"

#include "AgentLocalOnnxEmbeddingBackend.h"
#include "AgentPythonEmbeddingBackend.h"
#include "AgentReadingSettings.h"
#include "Misc/Paths.h"

TUniquePtr<IAgentEmbeddingBackend> FAgentEmbeddingBackendFactory::Create(FString& OutError)
{
	OutError.Reset();

	const UAgentReadingSettings* Settings = UAgentReadingSettings::Get();
	switch (Settings->EmbeddingBackend)
	{
	case EAgentEmbeddingBackendType::Python:
		return MakeUnique<FAgentPythonEmbeddingBackend>();

	case EAgentEmbeddingBackendType::LocalOnnx:
		return MakeUnique<FAgentLocalOnnxEmbeddingBackend>();

	case EAgentEmbeddingBackendType::Auto:
	default:
	{
		const FString ModelPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Settings->LocalOnnxModelPath));
		const FString TokenizerPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Settings->LocalTokenizerPath));
		if (FPaths::FileExists(ModelPath) && FPaths::FileExists(TokenizerPath))
		{
			return MakeUnique<FAgentLocalOnnxEmbeddingBackend>();
		}

		return MakeUnique<FAgentPythonEmbeddingBackend>();
	}
	}
}
