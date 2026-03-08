// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AgentReading : ModuleRules
{
	public AgentReading(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
				"NNE"
			}
			);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Projects",
				"NNERuntimeORT"
			}
			);
	}
}
