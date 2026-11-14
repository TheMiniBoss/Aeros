// Copyright Aeros. All Rights Reserved.
//
// Preview of the demo terrain (AerosWeatherDemo/Public/AerosDemoTerrainModel.h) without Unreal:
// writes a shaded map (PNG) and prints heights at the demo's key places.
//   terrain_preview [out.png] [half-size km] [pixels]
// Same regions as AAerosDemoGameMode (ocean 70 km east, desert 120 km west, volcano 25 km north-west).

#include "../../Source/AerosWeatherDemo/Public/AerosDemoTerrainModel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
	uint32_t Crc(const unsigned char* Data, size_t Len, uint32_t C = 0xFFFFFFFFu)
	{
		for (size_t I = 0; I < Len; ++I)
		{
			C ^= Data[I];
			for (int K = 0; K < 8; ++K) C = (C >> 1) ^ (0xEDB88320u & (0u - (C & 1u)));
		}
		return C;
	}

	void Chunk(std::vector<unsigned char>& Out, const char* Tag, const std::vector<unsigned char>& Data)
	{
		const uint32_t N = uint32_t(Data.size());
		unsigned char Len[4] = { (unsigned char)(N >> 24), (unsigned char)(N >> 16), (unsigned char)(N >> 8), (unsigned char)N };
		Out.insert(Out.end(), Len, Len + 4);
		std::vector<unsigned char> Body(Tag, Tag + 4);
		Body.insert(Body.end(), Data.begin(), Data.end());
		Out.insert(Out.end(), Body.begin(), Body.end());
		const uint32_t C = Crc(Body.data(), Body.size()) ^ 0xFFFFFFFFu;
		unsigned char CB[4] = { (unsigned char)(C >> 24), (unsigned char)(C >> 16), (unsigned char)(C >> 8), (unsigned char)C };
		Out.insert(Out.end(), CB, CB + 4);
	}

	/** RGB PNG with stored (uncompressed) deflate blocks. */
	bool WritePng(const char* Path, int W, int H, const std::vector<unsigned char>& Rgb)
	{
		std::vector<unsigned char> Raw;
		for (int Y = 0; Y < H; ++Y)
		{
			Raw.push_back(0);
			Raw.insert(Raw.end(), Rgb.begin() + size_t(Y) * W * 3, Rgb.begin() + size_t(Y + 1) * W * 3);
		}
		std::vector<unsigned char> Z = { 0x78, 0x01 };
		uint32_t A = 1, B = 0;
		for (unsigned char C : Raw) { A = (A + C) % 65521u; B = (B + A) % 65521u; }
		for (size_t P = 0; P < Raw.size(); P += 65535)
		{
			const size_t N = std::min<size_t>(65535, Raw.size() - P);
			Z.push_back(P + N >= Raw.size() ? 1 : 0);
			Z.push_back((unsigned char)(N & 0xFF)); Z.push_back((unsigned char)(N >> 8));
			Z.push_back((unsigned char)(~N & 0xFF)); Z.push_back((unsigned char)((~N >> 8) & 0xFF));
			Z.insert(Z.end(), Raw.begin() + P, Raw.begin() + P + N);
		}
		const uint32_t Ad = (B << 16) | A;
		Z.push_back((unsigned char)(Ad >> 24)); Z.push_back((unsigned char)(Ad >> 16)); Z.push_back((unsigned char)(Ad >> 8)); Z.push_back((unsigned char)Ad);

		std::vector<unsigned char> Out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		std::vector<unsigned char> Hdr = { (unsigned char)(W >> 24), (unsigned char)(W >> 16), (unsigned char)(W >> 8), (unsigned char)W,
			(unsigned char)(H >> 24), (unsigned char)(H >> 16), (unsigned char)(H >> 8), (unsigned char)H, 8, 2, 0, 0, 0 };
		Chunk(Out, "IHDR", Hdr);
		Chunk(Out, "IDAT", Z);
		Chunk(Out, "IEND", {});
		FILE* F = std::fopen(Path, "wb");
		if (!F) return false;
		std::fwrite(Out.data(), 1, Out.size(), F);
		std::fclose(F);
		return true;
	}
}

int main(int Argc, char** Argv)
{
	using namespace AerosDemoTerrainModel;
	const char* OutPath = Argc > 1 ? Argv[1] : "terrain_preview.png";
	const double HalfKm = Argc > 2 ? std::atof(Argv[2]) : 160.0;
	const int N = Argc > 3 ? std::atoi(Argv[3]) : 640;

	FModel M;
	M.Seed = 1337;
	M.SeaLevelM = -1.5;
	FCircle Ocean; Ocean.X = 270000.0; Ocean.RadiusM = 200000.0; M.Oceans.push_back(Ocean);
	FCircle Desert; Desert.X = -120000.0; Desert.Y = 20000.0; Desert.RadiusM = 55000.0; Desert.EdgeM = 15000.0; M.Deserts.push_back(Desert);
	FCircle Pad; Pad.X = -18000.0; Pad.Y = -18000.0; Pad.RadiusM = 7000.0; Pad.EdgeM = 4000.0; M.Pads.push_back(Pad);
	M.Init();

	const double Step = 2.0 * HalfKm * 1000.0 / N;
	std::vector<float> H(size_t(N) * N);
	float Lo = 1e9f, Hi = -1e9f;
	int Water = 0;
	for (int J = 0; J < N; ++J)
		for (int I = 0; I < N; ++I)
		{
			const double X = -HalfKm * 1000.0 + (I + 0.5) * Step, Y = -HalfKm * 1000.0 + (J + 0.5) * Step;
			const float V = M.Sample(X, Y).HeightM;
			H[size_t(J) * N + I] = V;
			Lo = std::fmin(Lo, V); Hi = std::fmax(Hi, V);
			Water += V < M.SeaLevelM ? 1 : 0;
		}

	std::vector<unsigned char> Rgb(size_t(N) * N * 3);
	for (int J = 0; J < N; ++J)
		for (int I = 0; I < N; ++I)
		{
			const double X = -HalfKm * 1000.0 + (I + 0.5) * Step, Y = -HalfKm * 1000.0 + (J + 0.5) * Step;
			const FSurface S = M.Sample(X, Y);
			const float V = H[size_t(J) * N + I];
			const float VX = H[size_t(J) * N + std::min(I + 1, N - 1)] - H[size_t(J) * N + std::max(I - 1, 0)];
			const float VY = H[size_t(std::min(J + 1, N - 1)) * N + I] - H[size_t(std::max(J - 1, 0)) * N + I];
			const double Slope = std::sqrt(VX * VX + VY * VY) / (2.0 * Step);
			const double Shade = Clamp01(0.75 + (-VX + VY) / (2.0 * Step) * 1.5);
			double R, G, B;
			if (V < M.SeaLevelM)
			{
				const double D = Clamp01((M.SeaLevelM - V) / 300.0);
				R = Lerp(0.15, 0.02, D); G = Lerp(0.45, 0.1, D); B = Lerp(0.6, 0.3, D);
			}
			else
			{
				const double Above = V - M.SeaLevelM;
				R = Lerp(0.13, 0.26, S.Dryness); G = Lerp(0.24, 0.24, S.Dryness); B = Lerp(0.07, 0.1, S.Dryness);
				const double Sand = std::fmax(1.0 - Smooth(1.5, 3.5, Above), S.Desert);
				R = Lerp(R, 0.62, Sand); G = Lerp(G, 0.54, Sand); B = Lerp(B, 0.38, Sand);
				const double Rock = std::fmax(Smooth(0.45, 0.75, Slope), Smooth(1800.0, 2600.0, V) * 0.7);
				R = Lerp(R, 0.3, Rock); G = Lerp(G, 0.28, Rock); B = Lerp(B, 0.26, Rock);
				const double Snow = Smooth(2300.0, 2700.0, V) * (1.0 - Smooth(0.6, 0.9, Slope));
				R = Lerp(R, 0.9, Snow); G = Lerp(G, 0.92, Snow); B = Lerp(B, 0.95, Snow);
				R *= Shade; G *= Shade; B *= Shade;
			}
			// Simulation domain (320 km) and the start
			if ((std::abs(std::abs(X) - 160000.0) < Step && std::abs(Y) < 160000.0) || (std::abs(std::abs(Y) - 160000.0) < Step && std::abs(X) < 160000.0)) { R = 1; G = 0; B = 0; }
			if (X * X + Y * Y < 4.0 * Step * Step) { R = 1; G = 1; B = 0; }
			unsigned char* P = &Rgb[(size_t(J) * N + I) * 3];
			P[0] = (unsigned char)(Clamp01(R) * 255); P[1] = (unsigned char)(Clamp01(G) * 255); P[2] = (unsigned char)(Clamp01(B) * 255);
		}
	WritePng(OutPath, N, N, Rgb);

	std::printf("Map %s: %.0f km wide, heights %.0f .. %.0f m, water %.0f %%\n", OutPath, 2 * HalfKm, Lo, Hi, 100.0 * Water / (double(N) * N));
	const double Places[][2] = { { 0, 0 }, { 3000, 0 }, { -18000, -18000 }, { -4000, 4000 }, { 65000, 0 }, { 72000, 0 }, { -120000, 20000 } };
	const char* Names[] = { "start", "crates 3 km", "volcano", "flood test", "coast -5 km", "sea +2 km", "desert" };
	for (int K = 0; K < 7; ++K)
	{
		const FSurface S = M.Sample(Places[K][0], Places[K][1]);
		std::printf("  %-12s (%6.0f, %6.0f) km: %7.1f m  desert %.2f mountain %.2f\n", Names[K], Places[K][0] / 1000, Places[K][1] / 1000, S.HeightM, S.Desert, S.Mountain);
	}
	return 0;
}
