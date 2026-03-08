using UnrealBuildTool;

public class AgentReadingEditor : ModuleRules
{
    public AgentReadingEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "AgentReading"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "UnrealEd",
            "Projects",
            "Json",
            "JsonUtilities"
        });
    }
}