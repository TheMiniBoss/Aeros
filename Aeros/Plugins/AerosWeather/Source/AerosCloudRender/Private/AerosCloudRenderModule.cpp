// Copyright Aeros. All Rights Reserved.

#include "AerosCloudViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

class FAerosCloudRenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AerosWeather"));
		if (Plugin.IsValid())
		{
			const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/AerosWeather"), ShaderDir);
		}

		// The renderer needs GEngine: create the extension once the engine is up.
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]()
		{
			Extension = FSceneViewExtensions::NewExtension<FAerosCloudViewExtension>();
		});
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		Extension.Reset();
	}

private:
	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FAerosCloudViewExtension, ESPMode::ThreadSafe> Extension;
};

IMPLEMENT_MODULE(FAerosCloudRenderModule, AerosCloudRender)
