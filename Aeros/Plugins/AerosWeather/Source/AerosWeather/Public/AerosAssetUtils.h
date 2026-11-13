// Copyright Aeros. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPtr.h"

namespace Aeros
{
	/** Load an optional asset without "failed to find object" warnings when it was never created. */
	template <typename T>
	T* LoadOptional(const TSoftObjectPtr<T>& Soft)
	{
		const FSoftObjectPath Path = Soft.ToSoftObjectPath();
		if (!Path.IsValid()) return nullptr;
		if (T* Loaded = Soft.Get()) return Loaded;
		return FPackageName::DoesPackageExist(Path.GetLongPackageName()) ? Soft.LoadSynchronous() : nullptr;
	}
}
