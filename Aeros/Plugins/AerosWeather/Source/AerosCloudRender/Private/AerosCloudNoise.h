// Copyright Aeros. All Rights Reserved.
//
// Tileable 3D cloud noise for the custom cloud renderer. Pure C++ (no Unreal headers) so the
// standalone tests compile it. Generated once at startup, then uploaded as two volume textures.

#pragma once

#include <cstdint>
#include <vector>

namespace AerosCloudNoise
{
	// RGBA8 voxels, x fastest, then y, then z. Tileable on all three axes.
	// Shape (128^3): R = Perlin-Worley (billowy, 4 cells per tile), G / B / A = Worley at 8 / 16 / 32 cells.
	// Detail (32^3): R / G / B = Worley at 4 / 8 / 16 cells, A = their fbm (0.5 / 0.3 / 0.2).
	// Every channel is normalised so its 5th-95th percentile spans 13-242.
	std::vector<uint8_t> MakeShape(int Size = 128, uint32_t Seed = 1);
	std::vector<uint8_t> MakeDetail(int Size = 32, uint32_t Seed = 2);
}
