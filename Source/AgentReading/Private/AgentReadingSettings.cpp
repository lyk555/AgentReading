#include "AgentReadingSettings.h"

UAgentReadingSettings::UAgentReadingSettings()
	: EmbeddingBackend(EAgentEmbeddingBackendType::Auto)
	, PythonExeOverride(TEXT(""))
	, PythonEmbedScript(TEXT("Tools/embeddings/embed_query.py"))
	, PythonModelName(TEXT("sentence-transformers/all-MiniLM-L6-v2"))
	, LocalOnnxModelPath(TEXT("Content/AI/RAG/Models/embedding_model.onnx"))
	, LocalTokenizerPath(TEXT("Content/AI/RAG/Models/tokenizer.json"))
	, LocalRuntimeName(TEXT("NNERuntimeORTCpu"))
	, LocalMaxTokens(256)
{
}

const UAgentReadingSettings* UAgentReadingSettings::Get()
{
	return GetDefault<UAgentReadingSettings>();
}

UAgentReadingSettings* UAgentReadingSettings::GetMutable()
{
	return GetMutableDefault<UAgentReadingSettings>();
}

FName UAgentReadingSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

#if WITH_EDITOR
FText UAgentReadingSettings::GetSectionText() const
{
	return NSLOCTEXT("AgentReadingSettings", "SectionText", "Agent Reading");
}

FText UAgentReadingSettings::GetSectionDescription() const
{
	return NSLOCTEXT("AgentReadingSettings", "SectionDescription", "Configure embedding backend selection for AgentReading runtime RAG.");
}
#endif
