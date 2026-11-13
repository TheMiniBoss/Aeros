// Long runs of the disasters (flood, tsunami, dust, volcano, blizzard, surge) with a summary.
// Usage: hazards [name]
#include "Core/AerosSimulation.h"
#include "Core/AerosSnapshot.h"
#include <cstdio>
#include <cstring>
#include <map>
static const char* PN[]={"Clear","PartCld","Overcast","Fog","Drizzle","Rain","HvyRain","Snow","Blizzard","FrzRain","Tstorm","SevTstorm","Hail","Supercell","Tornado","TropStorm","Hurricane","Sandstorm","Ashfall","Flood","FlashFlood","Surge","Tsunami","Quake"};
struct St{float meanPR=0,flooded=0,maxFlow=0,maxFlood=0,maxEta=0,minEta=0,maxDust=0,maxAsh=0,maxAshD=0,maxBlow=0,minFeels=99,maxPR=0,meanSnow=0,maxWind=0; int ph[24]={0}; int coastFlooded=0;};
St Stats(const FAerosSimulation& S){St s; auto&C=S.GetCells(); for(auto&c:C){s.meanPR+=c.PrecipRate/C.Num(); if(c.Flood>0.3f) s.flooded++; s.maxFlow=std::max(s.maxFlow,(float)std::hypot(c.FlowU,c.FlowV)); s.maxFlood=std::max(s.maxFlood,c.Flood); if(c.Water>=0.5f){s.maxEta=std::max(s.maxEta,c.Eta); s.minEta=std::min(s.minEta,c.Eta);} else if(c.Flood>0.3f && c.TsunamiZone) s.coastFlooded++;
 s.maxDust=std::max(s.maxDust,c.Dust); s.maxAsh=std::max(s.maxAsh,c.AshFall); s.maxAshD=std::max(s.maxAshD,c.AshDepth); s.maxBlow=std::max(s.maxBlow,c.Blow); s.minFeels=std::min(s.minFeels,c.FeelsLike); s.maxPR=std::max(s.maxPR,c.PrecipRate); s.meanSnow+=c.Snow/C.Num(); s.maxWind=std::max(s.maxWind,(float)std::hypot(c.Us+c.Uo,c.Vs+c.Vo)); s.ph[c.Phen]++;} return s;}
void Print(const char* tag,double h,const St&s,const FAerosSimulation& S){printf("%s t=%4.1fh meanPR %.2f flooded %.0f flow %.1f flood %.2fm eta[%.2f,%.2f] coastFl %d dust %.1f ashfall %.2fmm/h ashD %.1fmm blow %.2f feels %.0f PR %.0f snow %.1f V %.0f ocean %d |",tag,h,s.meanPR,s.flooded,s.maxFlow,s.maxFlood,s.minEta,s.maxEta,s.coastFlooded,s.maxDust,s.maxAsh,s.maxAshD,s.maxBlow,s.minFeels,s.maxPR,s.meanSnow,s.maxWind,S.IsOceanActive()); for(int p=0;p<24;++p) if(s.ph[p]&&p>=3) printf(" %s:%d",PN[p],s.ph[p]); printf("\n");}
FAerosSimulationSettings Base(EAerosScenario sc,int day,float hour){FAerosSimulationSettings S; S.Scenario=sc; S.StartDayOfYear=day; S.StartHour=hour; S.GridSizeX=S.GridSizeY=128; S.CellSizeKm=2; return S;}
void Run(const char* tag, FAerosSimulation& Sim, float hours, int every, std::function<void(int)> hook=nullptr){ int steps=hours*120; double t0=FPlatformTime::Seconds(); for(int i=0;i<=steps;++i){ if(hook) hook(i); if(i) Sim.Step(30); if(i%every==0) Print(tag,i/120.0,Stats(Sim),Sim);} printf("%s avg step %.1f ms\n",tag,(FPlatformTime::Seconds()-t0)/steps*1000);}
int main(int argc,char**argv){
 const char* only=argc>1?argv[1]:""; auto want=[&](const char*n){return !*only||strstr(n,only);};
 if(want("flood")){ auto S=Base(EAerosScenario::Monsoon,200,6); FAerosSimulation Sim; Sim.Initialize(S);
   TArray<float> H; H.SetNum(128*128); for(int j=0;j<128;++j)for(int i=0;i<128;++i){ float x=(i-64)*2.f, y=(j-64)*2.f; float hill=900*std::exp(-((x+40)*(x+40)+(y-20)*(y-20))/(30*30.f)) + 600*std::exp(-((x-30)*(x-30)+(y+30)*(y+30))/(25*25.f)); float valley = 20 + 0.6f*std::abs(y-0.3f*x)/2; H[i+j*128]=hill+valley; }
   Sim.SetElevation(H, FVector(0,0,0)); Run("flood",Sim,8,120,[&](int i){ if(i==120) Sim.InjectRain(FVector(-8000000.0,-4000000.0,0),15,250); }); }
 if(want("tsunami")){ auto S=Base(EAerosScenario::FairWeather,200,10); FAerosWaterRegion O; O.Center=FVector(28000000.0,0,0); O.RadiusKm=200; S.WaterRegions.Add(O); FAerosSimulation Sim; Sim.Initialize(S);
   TArray<float> H; H.SetNum(128*128); for(auto&h:H) h=2.f; Sim.SetElevation(H,FVector(0,0,0));
   Run("tsunami",Sim,2,10,[&](int i){ if(i==1) printf("trigger %d\n", Sim.TriggerTsunami(FVector(16000000.0,0,0), 9.0f)); }); }
 if(want("dust")){ auto S=Base(EAerosScenario::DesertDust,190,12); FAerosSimulation Sim; Sim.Initialize(S); Run("dust",Sim,6,60,[&](int i){ if(i==60) Sim.InjectOutflow(FVector(0,0,0),15,-10); }); }
 if(want("volcano")){ auto S=Base(EAerosScenario::FairWeather,120,8); FAerosSimulation Sim; Sim.Initialize(S); Sim.AddVolcanicEmitter(FVector(-6000000.0,0,0),1e6,4*3600); int ltg=0; Run("volcano",Sim,6,120); FAerosSnapshot Sn; Sim.FillSnapshot(Sn); printf("lightning strikes (last 2 min): %d\n",Sn.Lightning.Num()); }
 if(want("blizzard")){ auto S=Base(EAerosScenario::Blizzard,20,6); FAerosSimulation Sim; Sim.Initialize(S); Run("blizzard",Sim,10,120); }
 if(want("surge")){ auto S=Base(EAerosScenario::TropicalSeason,250,0); S.CellSizeKm=4; FAerosWaterRegion O; O.Center=FVector(40000000.0,0,0); O.RadiusKm=300; S.WaterRegions.Add(O); FAerosSimulation Sim; Sim.Initialize(S);
   TArray<float> H; H.SetNum(128*128); for(auto&h:H) h=2.f; Sim.SetElevation(H,FVector(0,0,0)); Sim.SpawnCyclone(FVector(18000000.0,0,0),945,30); Run("surge",Sim,12,120); }
}
