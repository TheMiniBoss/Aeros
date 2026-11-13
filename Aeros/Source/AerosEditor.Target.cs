// Copyright Aeros. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class AerosEditorTarget : TargetRules
{
	public AerosEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new string[] { "Aeros" });
	}
}
