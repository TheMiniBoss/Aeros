// Copyright Aeros. All Rights Reserved.

#include "AerosCloudNoise.h"

#include <algorithm>
#include <cmath>

namespace AerosCloudNoiseInternal
{
	inline uint32_t Hash(int32_t X, int32_t Y, int32_t Z, uint32_t Seed)
	{
		uint32_t H = Seed * 0x9E3779B9u;
		H ^= uint32_t(X) * 0x85EBCA6Bu; H = (H << 13) | (H >> 19);
		H ^= uint32_t(Y) * 0xC2B2AE35u; H = (H << 11) | (H >> 21);
		H ^= uint32_t(Z) * 0x27D4EB2Fu;
		H ^= H >> 16; H *= 0x7FEB352Du; H ^= H >> 15; H *= 0x846CA68Bu; H ^= H >> 16;
		return H;
	}

	inline float Rand01(uint32_t H) { return float(H & 0xFFFFFFu) / float(0x1000000); }

	inline int32_t Wrap(int32_t I, int32_t N) { const int32_t M = I % N; return M < 0 ? M + N : M; }

	// Inverted F1 Worley (1 at a feature point), tileable: Cells cells per tile. P in tile units [0, 1).
	float Worley(float Px, float Py, float Pz, int32_t Cells, uint32_t Seed)
	{
		const float X = Px * Cells, Y = Py * Cells, Z = Pz * Cells;
		const int32_t Cx = int32_t(std::floor(X)), Cy = int32_t(std::floor(Y)), Cz = int32_t(std::floor(Z));
		float Best = 9.f;
		for (int32_t K = -1; K <= 1; ++K)
		for (int32_t J = -1; J <= 1; ++J)
		for (int32_t I = -1; I <= 1; ++I)
		{
			const int32_t Nx = Cx + I, Ny = Cy + J, Nz = Cz + K;
			const uint32_t H = Hash(Wrap(Nx, Cells), Wrap(Ny, Cells), Wrap(Nz, Cells), Seed);
			const float Fx = Nx + Rand01(H) - X;
			const float Fy = Ny + Rand01(H * 747796405u + 1u) - Y;
			const float Fz = Nz + Rand01(H * 2891336453u + 7u) - Z;
			Best = std::min(Best, Fx * Fx + Fy * Fy + Fz * Fz);
		}
		return 1.f - std::min(std::sqrt(Best), 1.f);
	}

	inline float Fade(float T) { return T * T * T * (T * (T * 6.f - 15.f) + 10.f); }

	inline float Grad(uint32_t H, float X, float Y, float Z)
	{
		switch (H % 12u)
		{
		case 0: return X + Y;   case 1: return -X + Y;  case 2: return X - Y;   case 3: return -X - Y;
		case 4: return X + Z;   case 5: return -X + Z;  case 6: return X - Z;   case 7: return -X - Z;
		case 8: return Y + Z;   case 9: return -Y + Z;  case 10: return Y - Z;  default: return -Y - Z;
		}
	}

	// Gradient noise with a wrapped lattice (Period cells per tile), about [-1, 1].
	float Perlin(float Px, float Py, float Pz, int32_t Period, uint32_t Seed)
	{
		const float X = Px * Period, Y = Py * Period, Z = Pz * Period;
		const int32_t Ix = int32_t(std::floor(X)), Iy = int32_t(std::floor(Y)), Iz = int32_t(std::floor(Z));
		const float Fx = X - Ix, Fy = Y - Iy, Fz = Z - Iz;
		auto G = [&](int32_t Dx, int32_t Dy, int32_t Dz)
		{
			return Grad(Hash(Wrap(Ix + Dx, Period), Wrap(Iy + Dy, Period), Wrap(Iz + Dz, Period), Seed), Fx - Dx, Fy - Dy, Fz - Dz);
		};
		const float U = Fade(Fx), V = Fade(Fy), W = Fade(Fz);
		auto L = [](float A, float B, float T) { return A + (B - A) * T; };
		return L(L(L(G(0, 0, 0), G(1, 0, 0), U), L(G(0, 1, 0), G(1, 1, 0), U), V),
		         L(L(G(0, 0, 1), G(1, 0, 1), U), L(G(0, 1, 1), G(1, 1, 1), U), V), W);
	}

	// Maps each channel so its 5th-95th percentile spans 13-242.
	void NormaliseToBytes(const std::vector<float>& Values, int Channels, std::vector<uint8_t>& Out)
	{
		const size_t N = Values.size() / Channels;
		Out.resize(Values.size());
		std::vector<float> Tmp(N);
		for (int C = 0; C < Channels; ++C)
		{
			for (size_t I = 0; I < N; ++I) Tmp[I] = Values[I * Channels + C];
			std::nth_element(Tmp.begin(), Tmp.begin() + N * 5 / 100, Tmp.end());
			const float Lo = Tmp[N * 5 / 100];
			std::nth_element(Tmp.begin(), Tmp.begin() + N * 95 / 100, Tmp.end());
			const float Hi = Tmp[N * 95 / 100];
			const float Scale = (242.f - 13.f) / std::max(Hi - Lo, 1e-6f);
			for (size_t I = 0; I < N; ++I)
			{
				const float V = 13.f + (Values[I * Channels + C] - Lo) * Scale;
				Out[I * Channels + C] = uint8_t(std::clamp(V + 0.5f, 0.f, 255.f));
			}
		}
	}
}

namespace AerosCloudNoise
{
	std::vector<uint8_t> MakeShape(int Size, uint32_t Seed)
	{
		using namespace AerosCloudNoiseInternal;
		std::vector<float> V(size_t(Size) * Size * Size * 4);
		for (int Z = 0; Z < Size; ++Z)
		for (int Y = 0; Y < Size; ++Y)
		for (int X = 0; X < Size; ++X)
		{
			const float Px = (X + 0.5f) / Size, Py = (Y + 0.5f) / Size, Pz = (Z + 0.5f) / Size;
			// Perlin fbm (4 / 8 / 16 cells) in 0-1
			const float P = 0.5f + 0.5f * (Perlin(Px, Py, Pz, 4, Seed) * 0.6f + Perlin(Px, Py, Pz, 8, Seed + 1) * 0.3f + Perlin(Px, Py, Pz, 16, Seed + 2) * 0.1f);
			const float W4 = Worley(Px, Py, Pz, 4, Seed + 3);
			const float W8 = Worley(Px, Py, Pz, 8, Seed + 4);
			const float W16 = Worley(Px, Py, Pz, 16, Seed + 5);
			const float W32 = Worley(Px, Py, Pz, 32, Seed + 6);
			const float WF = W4 * 0.625f + W8 * 0.25f + W16 * 0.125f;
			// Perlin-Worley (Schneider): remap(perlin, worley - 1, 1, 0, 1)
			const float PW = (P - (WF - 1.f)) / (1.f - (WF - 1.f));
			float* Out = &V[((size_t(Z) * Size + Y) * Size + X) * 4];
			Out[0] = PW;
			Out[1] = W8;
			Out[2] = W16;
			Out[3] = W32;
		}
		std::vector<uint8_t> Bytes;
		NormaliseToBytes(V, 4, Bytes);
		return Bytes;
	}

	std::vector<uint8_t> MakeDetail(int Size, uint32_t Seed)
	{
		using namespace AerosCloudNoiseInternal;
		std::vector<float> V(size_t(Size) * Size * Size * 4);
		for (int Z = 0; Z < Size; ++Z)
		for (int Y = 0; Y < Size; ++Y)
		for (int X = 0; X < Size; ++X)
		{
			const float Px = (X + 0.5f) / Size, Py = (Y + 0.5f) / Size, Pz = (Z + 0.5f) / Size;
			const float A = Worley(Px, Py, Pz, 4, Seed);
			const float B = Worley(Px, Py, Pz, 8, Seed + 1);
			const float C = Worley(Px, Py, Pz, 16, Seed + 2);
			float* Out = &V[((size_t(Z) * Size + Y) * Size + X) * 4];
			Out[0] = A;
			Out[1] = B;
			Out[2] = C;
			Out[3] = A * 0.5f + B * 0.3f + C * 0.2f;
		}
		std::vector<uint8_t> Bytes;
		NormaliseToBytes(V, 4, Bytes);
		return Bytes;
	}
}
