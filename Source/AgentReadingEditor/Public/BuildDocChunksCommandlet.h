#pragma once

#include "Commandlets/Commandlet.h"
#include "BuildDocChunksCommandlet.generated.h"

UCLASS()
class UBuildDocChunksCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UBuildDocChunksCommandlet();
	virtual int32 Main(const FString& Params) override;
};