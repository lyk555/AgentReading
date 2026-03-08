#pragma once

#include "Commandlets/Commandlet.h"
#include "BuildApiRegistryCommandlet.generated.h"

UCLASS()
class UBuildApiRegistryCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UBuildApiRegistryCommandlet();

	virtual int32 Main(const FString& Params) override;
};