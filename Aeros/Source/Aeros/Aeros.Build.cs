// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;

// Host game module. The weather system lives in Plugins/AerosWeather.
public class Aeros : ModuleRules
{
	public Aeros(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });
		PrivateDependencyModuleNames.AddRange(new string[] { "AerosWeather" });
	}
}
