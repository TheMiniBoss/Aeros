// Copyright Aeros. All Rights Reserved.

#include "Core/AerosSnapshot.h"

bool FAerosSnapshot::WorldToGrid(const FVector& World, float& OutGX, float& OutGY) const
{
	const float Xm = float(World.X - DomainCenter.X) * 0.01f;
	const float Ym = -float(World.Y - DomainCenter.Y) * 0.01f; // world -Y = north
	OutGX = Xm / CellM + NX * 0.5f;
	OutGY = Ym / CellM + NY * 0.5f;
	return OutGX >= 0.f && OutGY >= 0.f && OutGX < NX && OutGY < NY;
}

FVector FAerosSnapshot::GridToWorld(float GX, float GY) const
{
	const float Xm = (GX - NX * 0.5f) * CellM;
	const float Ym = (GY - NY * 0.5f) * CellM;
	return FVector(DomainCenter.X + Xm * 100.0, DomainCenter.Y - Ym * 100.0, 0.0);
}

const FAerosSnapCell* FAerosSnapshot::CellAtWorld(const FVector& World) const
{
	if (!IsValid()) return nullptr;
	float GX, GY;
	WorldToGrid(World, GX, GY);
	return &At(FMath::FloorToInt(GX), FMath::FloorToInt(GY));
}

FAerosWeatherSample FAerosSnapshot::Sample(const FVector& World) const
{
	FAerosWeatherSample S;
	if (!IsValid()) return S;

	float GX, GY;
	WorldToGrid(World, GX, GY);
	const float FX = FMath::Clamp(GX - 0.5f, 0.f, float(NX - 1));
	const float FY = FMath::Clamp(GY - 0.5f, 0.f, float(NY - 1));
	const int32 I0 = FMath::Min(FMath::FloorToInt(FX), NX - 1);
	const int32 J0 = FMath::Min(FMath::FloorToInt(FY), NY - 1);
	const float TX = FX - I0, TY = FY - J0;

	const FAerosSnapCell& A = At(I0, J0);
	const FAerosSnapCell& B = At(I0 + 1, J0);
	const FAerosSnapCell& C = At(I0, J0 + 1);
	const FAerosSnapCell& D = At(I0 + 1, J0 + 1);
	const float WA = (1 - TX) * (1 - TY), WB = TX * (1 - TY), WC = (1 - TX) * TY, WD = TX * TY;

#define AEROS_BL(F) (A.F * WA + B.F * WB + C.F * WC + D.F * WD)

	// Discrete fields come from the dominant corner.
	const FAerosSnapCell* Dom = &A; float WMax = WA;
	if (WB > WMax) { Dom = &B; WMax = WB; }
	if (WC > WMax) { Dom = &C; WMax = WC; }
	if (WD > WMax) { Dom = &D; }

	S.bValid = true;
	S.Phenomenon = static_cast<EAerosPhenomenon>(Dom->Phen);
	S.PrecipitationType = static_cast<EAerosPrecipType>(Dom->PType);

	S.Temperature = AEROS_BL(T);
	S.DewPoint = AEROS_BL(Td);
	S.RelativeHumidity = AEROS_BL(RH);
	S.Pressure = AEROS_BL(P);
	S.GroundTemperature = AEROS_BL(Tg);
	S.Wind = SimToWorldDir(AEROS_BL(Us), AEROS_BL(Vs));
	S.WindSpeed = float(S.Wind.Size());
	S.Wind850 = SimToWorldDir(AEROS_BL(U850), AEROS_BL(V850));
	S.Visibility = AEROS_BL(Visibility);

	S.CloudCover = AEROS_BL(CloudCover);
	S.CloudBase = AEROS_BL(CloudBase);
	S.CloudTop = AEROS_BL(CloudTop);
	S.CloudWater = AEROS_BL(CloudWater);
	S.PrecipitationRate = AEROS_BL(PrecipRate);
	S.HailDiameter = Dom->Hail;
	S.Reflectivity = AEROS_BL(DBZ);
	S.LightningRate = AEROS_BL(Lightning);

	S.ConvectiveActivity = AEROS_BL(Conv);
	S.Updraft = AEROS_BL(W);
	S.Mesocyclone = AEROS_BL(Meso);
	S.CAPE = AEROS_BL(CAPE);
	S.CIN = AEROS_BL(CIN);
	S.LCL = AEROS_BL(LCL);
	S.LFC = AEROS_BL(LFC);
	S.EquilibriumLevel = AEROS_BL(EL);
	S.FreezingLevel = AEROS_BL(FZL);
	S.LiftedIndex = AEROS_BL(LI);
	S.SRH01 = AEROS_BL(SRH01);
	S.SRH03 = AEROS_BL(SRH03);
	S.BulkShear06 = AEROS_BL(Shear06);
	S.STP = AEROS_BL(STP);
	S.SCP = AEROS_BL(SCP);
	S.SHIP = AEROS_BL(SHIP);

	S.Wetness = AEROS_BL(Wet);
	S.SnowDepth = AEROS_BL(Snow);
	S.HailCover = AEROS_BL(HailCover);
#undef AEROS_BL
	return S;
}

FAerosWeatherSample FAerosSnapshot::LerpSample(const FAerosWeatherSample& A, const FAerosWeatherSample& B, float T)
{
	if (!A.bValid) return B;
	if (!B.bValid) return A;
	FAerosWeatherSample S = T < 0.5f ? A : B; // discrete fields
#define AEROS_L(F) S.F = A.F + (B.F - A.F) * T
	AEROS_L(Temperature); AEROS_L(DewPoint); AEROS_L(RelativeHumidity); AEROS_L(Pressure); AEROS_L(GroundTemperature);
	AEROS_L(Wind); AEROS_L(Wind850); AEROS_L(Visibility);
	AEROS_L(CloudCover); AEROS_L(CloudBase); AEROS_L(CloudTop); AEROS_L(CloudWater);
	AEROS_L(PrecipitationRate); AEROS_L(Reflectivity); AEROS_L(LightningRate);
	AEROS_L(ConvectiveActivity); AEROS_L(Updraft); AEROS_L(Mesocyclone);
	AEROS_L(CAPE); AEROS_L(CIN); AEROS_L(LCL); AEROS_L(LFC); AEROS_L(EquilibriumLevel); AEROS_L(FreezingLevel); AEROS_L(LiftedIndex);
	AEROS_L(SRH01); AEROS_L(SRH03); AEROS_L(BulkShear06); AEROS_L(STP); AEROS_L(SCP); AEROS_L(SHIP);
	AEROS_L(Wetness); AEROS_L(SnowDepth); AEROS_L(HailCover);
#undef AEROS_L
	S.WindSpeed = float(S.Wind.Size());
	return S;
}
