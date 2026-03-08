#include "Modules/ModuleManager.h"

class FAgentReadingEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Keep empty for now. Do NOT read files / register menus here yet.
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FAgentReadingEditorModule, AgentReadingEditor)