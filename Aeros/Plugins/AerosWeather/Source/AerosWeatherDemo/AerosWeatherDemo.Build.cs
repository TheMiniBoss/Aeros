// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;

public class AerosWeatherDemo : ModuleRules
{
	public AerosWeatherDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AerosWeather",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"SlateCore",
			"RenderCore", // ShaderCompiler.h: the tour waits for shader compilation
			"ProceduralMeshComponent", // procedural landscape (AAerosDemoTerrain)
		});
	}
}
