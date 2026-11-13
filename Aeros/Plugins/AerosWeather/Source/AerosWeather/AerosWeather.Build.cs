// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;

public class AerosWeather : ModuleRules
{
	public AerosWeather(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"HTTP",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Niagara",
			"FieldSystemEngine",
			"Chaos",
			"PhysicsCore",
			"NetCore",
			"Json",
			"ProceduralMeshComponent", // deformable tornado funnel
		});
	}
}
