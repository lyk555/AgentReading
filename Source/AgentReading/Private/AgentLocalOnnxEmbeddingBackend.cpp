#include "AgentLocalOnnxEmbeddingBackend.h"

#include "AgentReadingSettings.h"
#include "AgentWordPieceTokenizer.h"
#include "Math/Float16.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "UObject/Package.h"

namespace
{
	struct FLocalEmbeddingState
	{
		FCriticalSection Mutex;
		FString LoadedModelPath;
		FString LoadedTokenizerPath;
		FString LoadedRuntimeName;
		FAgentWordPieceTokenizer Tokenizer;
		UNNEModelData* ModelData = nullptr;
		TSharedPtr<UE::NNE::IModelCPU> Model;
		TSharedPtr<UE::NNE::IModelInstanceCPU> ModelInstance;
		TArray<UE::NNE::FTensorDesc> InputDescs;
		TArray<UE::NNE::FTensorDesc> OutputDescs;

		void Reset()
		{
			LoadedModelPath.Reset();
			LoadedTokenizerPath.Reset();
			LoadedRuntimeName.Reset();
			Tokenizer = FAgentWordPieceTokenizer();
			Model.Reset();
			ModelInstance.Reset();
			InputDescs.Reset();
			OutputDescs.Reset();
			if (ModelData)
			{
				ModelData->RemoveFromRoot();
				ModelData = nullptr;
			}
		}
	};

	static FLocalEmbeddingState GLocalEmbeddingState;

	enum class EAgentInputRole : uint8
	{
		Unknown,
		InputIds,
		AttentionMask,
		TokenTypeIds
	};

	static FString ToAbsoluteProjectPath(const FString& RelativeOrAbsolutePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), RelativeOrAbsolutePath));
	}

	static bool LoadModelBytes(const FString& ModelPath, TArray64<uint8>& OutBytes, FString& OutError)
	{
		if (!FFileHelper::LoadFileToArray(OutBytes, *ModelPath))
		{
			OutError = FString::Printf(TEXT("Failed to read ONNX model: %s"), *ModelPath);
			return false;
		}
		if (OutBytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("ONNX model is empty: %s"), *ModelPath);
			return false;
		}
		return true;
	}

	static EAgentInputRole ResolveInputRole(const FString& InputName)
	{
		const FString Lower = InputName.ToLower();
		if (Lower.Contains(TEXT("input_ids")) || Lower.EndsWith(TEXT("ids")))
		{
			return EAgentInputRole::InputIds;
		}
		if (Lower.Contains(TEXT("attention_mask")) || Lower.Contains(TEXT("mask")))
		{
			return EAgentInputRole::AttentionMask;
		}
		if (Lower.Contains(TEXT("token_type_ids")) || Lower.Contains(TEXT("segment")))
		{
			return EAgentInputRole::TokenTypeIds;
		}
		return EAgentInputRole::Unknown;
	}

	static bool FillIntTensorBytes(const TArray<int32>& SourceValues, const UE::NNE::FTensorDesc& Desc, TArray<uint8>& OutBytes, FString& OutError)
	{
		const uint32 ElementBytes = Desc.GetElementByteSize();
		OutBytes.SetNumUninitialized(SourceValues.Num() * static_cast<int32>(ElementBytes));

		switch (Desc.GetDataType())
		{
		case ENNETensorDataType::Int64:
		{
			TArray<int64> Temp;
			Temp.Reserve(SourceValues.Num());
			for (int32 Value : SourceValues)
			{
				Temp.Add(static_cast<int64>(Value));
			}
			FMemory::Memcpy(OutBytes.GetData(), Temp.GetData(), OutBytes.Num());
			return true;
		}
		case ENNETensorDataType::Int32:
			FMemory::Memcpy(OutBytes.GetData(), SourceValues.GetData(), OutBytes.Num());
			return true;
		case ENNETensorDataType::UInt32:
		{
			TArray<uint32> Temp;
			Temp.Reserve(SourceValues.Num());
			for (int32 Value : SourceValues)
			{
				Temp.Add(static_cast<uint32>(FMath::Max(Value, 0)));
			}
			FMemory::Memcpy(OutBytes.GetData(), Temp.GetData(), OutBytes.Num());
			return true;
		}
		case ENNETensorDataType::UInt64:
		{
			TArray<uint64> Temp;
			Temp.Reserve(SourceValues.Num());
			for (int32 Value : SourceValues)
			{
				Temp.Add(static_cast<uint64>(FMath::Max(Value, 0)));
			}
			FMemory::Memcpy(OutBytes.GetData(), Temp.GetData(), OutBytes.Num());
			return true;
		}
		default:
			OutError = FString::Printf(TEXT("Unsupported input tensor type for %s: %d"), *Desc.GetName(), static_cast<int32>(Desc.GetDataType()));
			OutBytes.Reset();
			return false;
		}
	}

	static bool EnsureLocalModelLoaded(const FString& ModelPath, const FString& TokenizerPath, const FString& RuntimeName, FString& OutError)
	{
		FLocalEmbeddingState& State = GLocalEmbeddingState;
		if (State.ModelInstance.IsValid() && State.LoadedModelPath == ModelPath && State.LoadedTokenizerPath == TokenizerPath && State.LoadedRuntimeName == RuntimeName)
		{
			return true;
		}

		State.Reset();
		if (!State.Tokenizer.LoadFromFile(TokenizerPath, OutError))
		{
			return false;
		}

		TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(RuntimeName);
		if (!Runtime.IsValid())
		{
			OutError = FString::Printf(TEXT("NNE runtime is not available: %s. Enable the NNERuntimeORT plugin for this target."), *RuntimeName);
			return false;
		}

		TArray64<uint8> ModelBytes;
		if (!LoadModelBytes(ModelPath, ModelBytes, OutError))
		{
			return false;
		}

		UNNEModelData* ModelData = NewObject<UNNEModelData>(GetTransientPackage());
		ModelData->AddToRoot();
		ModelData->Init(TEXT("onnx"), TConstArrayView64<uint8>(ModelBytes.GetData(), ModelBytes.Num()));

		if (Runtime->CanCreateModelCPU(ModelData) != INNERuntimeCPU::ECanCreateModelCPUStatus::Ok)
		{
			ModelData->RemoveFromRoot();
			OutError = FString::Printf(TEXT("NNE runtime %s cannot create a CPU model from %s"), *RuntimeName, *ModelPath);
			return false;
		}

		TSharedPtr<UE::NNE::IModelCPU> Model = Runtime->CreateModelCPU(ModelData);
		if (!Model.IsValid())
		{
			ModelData->RemoveFromRoot();
			OutError = FString::Printf(TEXT("Failed to create CPU model from ONNX file: %s"), *ModelPath);
			return false;
		}

		TSharedPtr<UE::NNE::IModelInstanceCPU> Instance = Model->CreateModelInstanceCPU();
		if (!Instance.IsValid())
		{
			ModelData->RemoveFromRoot();
			OutError = TEXT("Failed to create CPU model instance for local embedding model.");
			return false;
		}

		State.ModelData = ModelData;
		State.Model = MoveTemp(Model);
		State.ModelInstance = MoveTemp(Instance);
		State.LoadedModelPath = ModelPath;
		State.LoadedTokenizerPath = TokenizerPath;
		State.LoadedRuntimeName = RuntimeName;
		State.InputDescs.Append(State.ModelInstance->GetInputTensorDescs().GetData(), State.ModelInstance->GetInputTensorDescs().Num());
		State.OutputDescs.Append(State.ModelInstance->GetOutputTensorDescs().GetData(), State.ModelInstance->GetOutputTensorDescs().Num());

		if (State.InputDescs.Num() == 0 || State.OutputDescs.Num() == 0)
		{
			State.Reset();
			OutError = TEXT("Local embedding model did not expose required input/output tensors.");
			return false;
		}

		return true;
	}

	static bool BuildEmbeddingFromOutput(const TArray<uint8>& OutputBytes, const UE::NNE::FTensorDesc& OutputDesc, const UE::NNE::FTensorShape& OutputShape, const TArray<int32>& AttentionMask, int32& OutDim, float& OutNorm, TArray<uint8>& OutPackedF16, FString& OutError)
	{
		OutDim = 0;
		OutNorm = 0.0f;
		OutPackedF16.Reset();

		const TConstArrayView<uint32> Dims = OutputShape.GetData();
		if (Dims.Num() < 2)
		{
			OutError = TEXT("Local embedding output tensor rank is too small.");
			return false;
		}

		const int32 Rank = Dims.Num();
		const int32 SequenceLength = Rank >= 3 ? static_cast<int32>(Dims[Rank - 2]) : 1;
		const int32 HiddenSize = static_cast<int32>(Dims[Rank - 1]);
		if (HiddenSize <= 0)
		{
			OutError = TEXT("Local embedding output hidden size is invalid.");
			return false;
		}

		auto ReadValue = [&](int32 Index) -> float
		{
			switch (OutputDesc.GetDataType())
			{
			case ENNETensorDataType::Float:
				return reinterpret_cast<const float*>(OutputBytes.GetData())[Index];
			case ENNETensorDataType::Half:
				return static_cast<float>(reinterpret_cast<const FFloat16*>(OutputBytes.GetData())[Index]);
			default:
				return 0.0f;
			}
		};

		if (OutputDesc.GetDataType() != ENNETensorDataType::Float && OutputDesc.GetDataType() != ENNETensorDataType::Half)
		{
			OutError = FString::Printf(TEXT("Unsupported output tensor type: %d"), static_cast<int32>(OutputDesc.GetDataType()));
			return false;
		}

		TArray<float> Embedding;
		Embedding.Init(0.0f, HiddenSize);

		if (Rank == 2)
		{
			for (int32 HiddenIndex = 0; HiddenIndex < HiddenSize; ++HiddenIndex)
			{
				Embedding[HiddenIndex] = ReadValue(HiddenIndex);
			}
		}
		else
		{
			const int32 ValidTokens = FMath::Min(AttentionMask.Num(), SequenceLength);
			float TokenWeightSum = 0.0f;
			for (int32 TokenIndex = 0; TokenIndex < ValidTokens; ++TokenIndex)
			{
				if (AttentionMask[TokenIndex] <= 0)
				{
					continue;
				}
				TokenWeightSum += 1.0f;
				const int32 BaseIndex = TokenIndex * HiddenSize;
				for (int32 HiddenIndex = 0; HiddenIndex < HiddenSize; ++HiddenIndex)
				{
					Embedding[HiddenIndex] += ReadValue(BaseIndex + HiddenIndex);
				}
			}

			if (TokenWeightSum <= 0.0f)
			{
				OutError = TEXT("Attention mask produced zero valid tokens for mean pooling.");
				return false;
			}

			for (float& Value : Embedding)
			{
				Value /= TokenWeightSum;
			}
		}

		float SquaredNorm = 0.0f;
		for (float Value : Embedding)
		{
			SquaredNorm += Value * Value;
		}
		OutNorm = FMath::Sqrt(SquaredNorm);
		if (OutNorm <= 1e-6f)
		{
			OutError = TEXT("Local embedding norm was zero.");
			return false;
		}

		OutDim = HiddenSize;
		OutPackedF16.SetNumUninitialized(HiddenSize * sizeof(FFloat16));
		FFloat16* Packed = reinterpret_cast<FFloat16*>(OutPackedF16.GetData());
		for (int32 Index = 0; Index < HiddenSize; ++Index)
		{
			Packed[Index] = FFloat16(Embedding[Index]);
		}
		return true;
	}
}

FString FAgentLocalOnnxEmbeddingBackend::GetBackendName() const
{
	return TEXT("LocalOnnx");
}

bool FAgentLocalOnnxEmbeddingBackend::EmbedQuery(const FString& QueryText, int32& OutDim, float& OutNorm, TArray<uint8>& OutPackedF16, FString& OutError)
{
	OutDim = 0;
	OutNorm = 0.0f;
	OutPackedF16.Reset();
	OutError.Reset();

	const UAgentReadingSettings* Settings = UAgentReadingSettings::Get();
	const FString ModelPath = ToAbsoluteProjectPath(Settings->LocalOnnxModelPath);
	const FString TokenizerPath = ToAbsoluteProjectPath(Settings->LocalTokenizerPath);
	const FString RuntimeName = Settings->LocalRuntimeName.IsEmpty() ? TEXT("NNERuntimeORTCpu") : Settings->LocalRuntimeName;

	if (!FPaths::FileExists(ModelPath))
	{
		OutError = FString::Printf(TEXT("Missing local ONNX model: %s"), *ModelPath);
		return false;
	}
	if (!FPaths::FileExists(TokenizerPath))
	{
		OutError = FString::Printf(TEXT("Missing local tokenizer: %s"), *TokenizerPath);
		return false;
	}

	FLocalEmbeddingState& State = GLocalEmbeddingState;
	FScopeLock Lock(&State.Mutex);
	if (!EnsureLocalModelLoaded(ModelPath, TokenizerPath, RuntimeName, OutError))
	{
		return false;
	}

	FAgentTokenizedText Tokens;
	if (!State.Tokenizer.Tokenize(QueryText, Settings->LocalMaxTokens, Tokens, OutError))
	{
		return false;
	}

	TArray<UE::NNE::FTensorShape> InputShapes;
	InputShapes.Reserve(State.InputDescs.Num());
	TArray<uint32> ShapeData;
	ShapeData.Add(1);
	ShapeData.Add(static_cast<uint32>(Tokens.InputIds.Num()));
	for (int32 Index = 0; Index < State.InputDescs.Num(); ++Index)
	{
		InputShapes.Add(UE::NNE::FTensorShape::Make(ShapeData));
	}

	if (State.ModelInstance->SetInputTensorShapes(InputShapes) != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok)
	{
		OutError = TEXT("Failed to set input tensor shapes for local embedding model.");
		return false;
	}

	TArray<TArray<uint8>> InputBuffers;
	InputBuffers.SetNum(State.InputDescs.Num());
	TArray<UE::NNE::FTensorBindingCPU> InputBindings;
	InputBindings.Reserve(State.InputDescs.Num());
	for (int32 Index = 0; Index < State.InputDescs.Num(); ++Index)
	{
		const UE::NNE::FTensorDesc& Desc = State.InputDescs[Index];
		const EAgentInputRole Role = ResolveInputRole(Desc.GetName());
		const TArray<int32>* SourceValues = nullptr;
		switch (Role)
		{
		case EAgentInputRole::InputIds:
			SourceValues = &Tokens.InputIds;
			break;
		case EAgentInputRole::AttentionMask:
			SourceValues = &Tokens.AttentionMask;
			break;
		case EAgentInputRole::TokenTypeIds:
			SourceValues = &Tokens.TokenTypeIds;
			break;
		case EAgentInputRole::Unknown:
		default:
			SourceValues = (Index == 0) ? &Tokens.InputIds : (Index == 1 ? &Tokens.AttentionMask : &Tokens.TokenTypeIds);
			break;
		}

		if (!FillIntTensorBytes(*SourceValues, Desc, InputBuffers[Index], OutError))
		{
			return false;
		}

		InputBindings.Add({ InputBuffers[Index].GetData(), static_cast<uint64>(InputBuffers[Index].Num()) });
	}

	const TConstArrayView<UE::NNE::FTensorShape> OutputShapes = State.ModelInstance->GetOutputTensorShapes();
	if (OutputShapes.Num() == 0)
	{
		OutError = TEXT("Local embedding model could not resolve output tensor shapes.");
		return false;
	}

	int32 SelectedOutputIndex = 0;
	for (int32 Index = 0; Index < State.OutputDescs.Num() && Index < OutputShapes.Num(); ++Index)
	{
		if (State.OutputDescs[Index].GetDataType() == ENNETensorDataType::Float || State.OutputDescs[Index].GetDataType() == ENNETensorDataType::Half)
		{
			SelectedOutputIndex = Index;
			break;
		}
	}

	const UE::NNE::FTensorDesc& OutputDesc = State.OutputDescs[SelectedOutputIndex];
	const UE::NNE::FTensorShape& OutputShape = OutputShapes[SelectedOutputIndex];
	TArray<uint8> OutputBuffer;
	OutputBuffer.SetNumUninitialized(static_cast<int32>(OutputDesc.GetElementByteSize() * OutputShape.Volume()));
	TArray<UE::NNE::FTensorBindingCPU> OutputBindings;
	OutputBindings.Add({ OutputBuffer.GetData(), static_cast<uint64>(OutputBuffer.Num()) });

	if (State.ModelInstance->RunSync(InputBindings, OutputBindings) != UE::NNE::IModelInstanceCPU::ERunSyncStatus::Ok)
	{
		OutError = TEXT("Local embedding ONNX inference failed.");
		return false;
	}

	return BuildEmbeddingFromOutput(OutputBuffer, OutputDesc, OutputShape, Tokens.AttentionMask, OutDim, OutNorm, OutPackedF16, OutError);
}
