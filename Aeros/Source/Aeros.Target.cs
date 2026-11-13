// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class AerosTarget : TargetRules
{
	public AerosTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new string[] { "Aeros" });
	}
}
