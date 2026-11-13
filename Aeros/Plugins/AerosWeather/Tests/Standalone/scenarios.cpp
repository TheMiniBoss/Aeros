// Long runs of the weather scenarios with an hourly summary (diagnostics, tuning).
// Usage: scenarios [name]   names: supercell summer climate-jul climate-jan winter frontal tropical
#include "Core/AerosSimulation.h"
#include "Core/AerosSnapshot.h"
#include "Core/AerosPhysics.h"
#include <cstdio>
#include <cstring>
static const char* PhenName(int p){ static const char* n[]={"Clear","PartlyCloudy","Overcast","Fog","Drizzle","Rain","HeavyRain","Snow","Blizzard","FreezingRain","Tstorm","SevereTstorm","Hail","Supercell","Tornado","TropStorm","Hurricane"}; return n[p]; }
void Run(const char* name, EAerosScenario sc, int day, float hour, float hours, float lat, bool water=false, bool cyclone=false){
  FAerosSimulationSettings S; S.Scenario=sc; S.StartDayOfYear=day; S.StartHour=hour; S.LatitudeDeg=lat; S.GridSizeX=128; S.GridSizeY=128; S.CellSizeKm=2;
  if(water){ FAerosWaterRegion R; R.Center=FVector(0,0,0); R.RadiusKm=2000; S.WaterRegions.Add(R); S.CellSizeKm=6; }
  FAerosSimulation Sim; Sim.Initialize(S);
  if(cyclone) Sim.SpawnCyclone(FVector(-100000.0*100,0,0), 990.f, 40.f);
  printf("\n=== %s ===\n", name);
  int steps=(int)(hours*3600/S.PhysicsStepSeconds); double tsum=0;
  for(int s=0;s<=steps;++s){
    if(s>0){ double t0=FPlatformTime::Seconds(); Sim.Step(S.PhysicsStepSeconds); tsum+=FPlatformTime::Seconds()-t0; }
    if(s%120==0){
      auto& C=Sim.GetCells(); int N=C.Num(); float maxCape=0,maxConv=0,maxHail=0,maxDbz=-32,maxPr=0,meanT=0,minT=99,maxT=-99,maxW=0,maxMeso=0,maxP=-999,minP=999,maxWind=0,maxSnow=0,maxStp=0,cc=0; int phen[17]={0};
      for(int k=0;k<N;++k){auto&c=C[k]; maxCape=std::max(maxCape,c.CAPE); maxConv=std::max(maxConv,c.Conv); maxHail=std::max(maxHail,c.Hail); maxDbz=std::max(maxDbz,c.DBZ); maxPr=std::max(maxPr,c.PrecipRate); meanT+=c.T/N; minT=std::min(minT,c.T); maxT=std::max(maxT,c.T); maxW=std::max(maxW,c.W); maxMeso=std::max(maxMeso,c.Meso); maxP=std::max(maxP,c.P); minP=std::min(minP,c.P); maxWind=std::max(maxWind,(float)std::hypot(c.Us,c.Vs)); maxSnow=std::max(maxSnow,c.Snow); maxStp=std::max(maxStp,c.STP); cc+=c.CloudCover/N; phen[c.Phen]++;}
      auto& m=C[64+64*128]; auto D=Sim.GetDateTime();
      printf("t=%5.1fh d%d %05.2fh sun%5.1f | T %.1f [%.1f..%.1f] Tg %.1f Td %.1f T850 %.1f T500 %.1f | CAPE %4.0f CIN %4.0f LCL %4.0f EL %5.0f FZL %4.0f SRH03 %3.0f Sh %2.0f STP %.1f | dom: CAPEmax %4.0f Conv %.2f W %2.0f Meso %.2f Hail %2.0f dBZ %2.0f PR %5.1f P[%5.1f,%4.1f] V %2.0f snow %.1f cc %.2f STPmax %.1f storms %d cyc %d ltg? |",
        s*S.PhysicsStepSeconds/3600., D.DayOfYear, D.Hour, D.SunElevation, m.T, minT, maxT, m.Tg, m.Td, m.T850, m.T500, m.CAPE, m.CIN, m.LCL, m.EL, m.FZL, m.SRH03, m.Shear06, m.STP,
        maxCape, maxConv, maxW, maxMeso, maxHail, maxDbz, maxPr, minP, maxP, maxWind, maxSnow, cc, maxStp, Sim.GetStorms().Num(), Sim.GetVortices().Num());
      for(int p=0;p<17;++p) if(phen[p]) printf(" %s:%d",PhenName(p),phen[p]);
      for(auto& v: Sim.GetVortices()) printf(" [TC dP %.0f Vmax %.0f]", v.DeltaP, v.MaxWind());
      printf("\n");
    }
  }
  FAerosSnapshot Snap; double t0=FPlatformTime::Seconds(); Sim.FillSnapshot(Snap); double ts=FPlatformTime::Seconds()-t0;
  auto Sm = Snap.Sample(FVector(0,0,0));
  printf("avg step %.2f ms (single thread) snapshot %.2f ms, sample T %.1f valid %d, storms %d ltg %d\n", tsum/steps*1000, ts*1000, Sm.Temperature, Sm.bValid, Snap.Storms.Num(), Snap.Lightning.Num());
  for(int i=0;i<std::min(5,Snap.Storms.Num());++i){auto&d=Snap.Storms[i]; printf("  storm %d type %d r %.1fkm dBZ %.0f W %.0f hail %.0f ltg %.1f/min meso %.2f STP %.1f age %.0f v(%.1f,%.1f)\n", d.Id,(int)d.Type,d.RadiusKm,d.MaxReflectivity,d.MaxUpdraft,d.MaxHail,d.LightningRate,d.Mesocyclone,d.STP,d.Age,d.Velocity.X,d.Velocity.Y);}
}
int main(int argc,char**argv){
  const char* only = argc>1?argv[1]:"";
  auto want=[&](const char*n){return !*only||strstr(n,only);};
  if(want("supercell")) Run("supercell", EAerosScenario::SupercellOutbreak, 140, 10, 10, 37);
  if(want("summer")) Run("summer", EAerosScenario::SummerThunderstorms, 190, 8, 12, 40);
  if(want("climate-jul")) Run("climate-jul", EAerosScenario::Climate, 196, 0, 24, 45);
  if(want("climate-jan")) Run("climate-jan", EAerosScenario::Climate, 15, 0, 24, 45);
  if(want("winter")) Run("winter", EAerosScenario::WinterStorm, 20, 0, 12, 45);
  if(want("frontal")) Run("frontal", EAerosScenario::FrontalRain, 300, 0, 12, 48);
  if(want("tropical")) Run("tropical", EAerosScenario::TropicalSeason, 250, 0, 24, 20, true, true);
}
