#pragma once

#include "Commandlets/Commandlet.h"
#include "AlignDocsToApiCommandlet.generated.h"

UCLASS()
class UAlignDocsToApiCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UAlignDocsToApiCommandlet();
	virtual int32 Main(const FString& Params) override;
};