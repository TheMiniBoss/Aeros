// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;

// Custom volumetric cloud renderer (spike): own ray march + wind-aware temporal
// accumulation, hooked into the post-processing chain by a scene view extension.
// Loaded at PostConfigInit so its shader directory is mapped before shaders compile.
public class AerosCloudRender : ModuleRules
{
	public AerosCloudRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"Renderer",
			"RHI",
			"Projects",
		});
	}
}
