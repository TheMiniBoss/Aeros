# Aeros Weather — plugin Unreal Engine 5.8

Système de météo et de catastrophes naturelles **réaliste et optimisé**. Ce n'est pas un système d'effets scriptés : un **moteur thermodynamique** tourne en arrière-plan (thread dédié). L'état de l'atmosphère (température, humidité, pression, vent) produit des instabilités (CAPE), qui déclenchent nuages, pluie, neige, grêle, orages, supercellules, tornades et ouragans. Les visuels et les dégâts ne font que lire ces données.

> Statut : version 0.3 (conversion du projet en plugin C++). Le cœur de simulation est compilé et testé hors moteur (scénarios, déterminisme). La partie Unreal (acteurs, rendu, réseau) n'a **pas encore été compilée dans l'éditeur 5.8** : prévoir quelques corrections au premier build.

---

## Démarrage rapide

1. Ouvrir `Aeros.uproject` (UE 5.8). Accepter la compilation des modules.
2. **Démo immédiate** : n'importe quel niveau (même vide) → *World Settings* → *GameMode Override* = `AerosDemoGameMode` → **Play**. Le mode démo construit le ciel, le soleil, le brouillard, le sol, des caisses physiques, le contrôleur météo et le radar.
3. **Démo complète** (nuages volumétriques pilotés par la simulation, sol mouillé/enneigé) :
   *Tools → Execute Python Script…* → `Plugins/AerosWeather/Content/Python/aeros_setup.py`.
   Le script crée `MPC_AerosWeather`, `M_AerosVolumetricCloud`, `M_AerosGround` et la carte `/AerosWeather/Maps/L_AerosDemo`. (Script expérimental : non testé dans l'éditeur 5.8.)

### Touches de la démo

| Touche | Action |
|---|---|
| Souris, WASD, E/Espace, Q/C | voler ; Shift ×10, Ctrl ×100 |
| 1 à 7 | scénario : Climat (saisons), Beau temps, Orages d'été, Supercellules, Tempête de neige, Saison tropicale, Pluie frontale |
| B | bulle chaude 15 km devant (déclenche un orage si l'air est instable) |
| T | forcer une tornade sous l'orage le plus fort |
| H | ouragan 250 km à l'est (océan) |
| F | se téléporter près de l'orage le plus proche |
| + / − | vitesse du temps ×2 / ÷2 ; P pause |
| N / M, J / K | −/+ 1 heure, −/+ 1 mois (saisons) |
| R | radar : réflectivité / vitesse Doppler / type de précipitation |
| V | debug (cercles d'orages, flèches de vent) |
| F1 / Tab | HUD : complet → radar seul → caché → aide |

Console : `aeros.Scenario N`, `aeros.TimeScale X`, `aeros.Time jour heure`, `aeros.Bubble`, `aeros.Hurricane`, `aeros.Tornado`, `aeros.Debug 0-2`, `stat Aeros`.

---

## Architecture (fidèle au cahier des charges)

```
UAerosWeatherSubsystem (UTickableWorldSubsystem)   <-- cerveau, point d'accès global
 ├─ FAerosSimulationThread (FRunnable)            <-- thread dédié, file de commandes lock-free
 │    └─ FAerosSimulation                          <-- grille 2.5D, physique (C++ pur)
 │         └─ snapshot immuable publié (TSharedPtr, pool, 0 copie côté jeu)
 ├─ UAerosStormManager                             <-- tempêtes -> acteurs (LOD), tornades, foudre
 │    ├─ AAerosStormSystem   (répliqué 1 Hz, grêle -> ApplyRadialDamage + Chaos)
 │    ├─ AAerosTornado       (vortex de Rankine, forces physiques, Chaos, débris)
 │    └─ AAerosHurricane     (vortex de Holland dans la simulation)
 └─ UAerosCloudManager                             <-- textures GPU + Material Parameter Collection
AAerosWeatherController   (dans le niveau : réglages, soleil, brouillard, matériau nuages)
AAerosWeatherRadar        (radar Doppler rotatif -> texture UI)
UAerosPrecipitationComponent (pluie/neige/grêle locales : Niagara ou rendu instancié de secours)
AAerosWeatherNetState     (réseau : seed + horloge + commandes datées)
```

| Classe | Rôle |
|---|---|
| `FAerosSimulation` | grille 2.5D : 3 niveaux par colonne (surface, 850 hPa, 500 hPa) + sol |
| `UAerosWeatherSubsystem` | démarre/arrête, horloge, requêtes (`GetWeatherAtLocation`), événements |
| `UAerosStormManager` | détection → acteurs, genèse des tornades (tirage déterministe), foudre |
| `UAerosCloudManager` | `WeatherMap` / `SurfaceMap` (une texel par cellule) + MPC |
| `AAerosStormSystem` | macro-état répliqué, dégâts de grêle > 30 mm (Chaos + dégâts radiaux) |
| `AAerosTornado` | vent, aspiration des objets physiques, dégâts, fractures Chaos |
| `AAerosWeatherRadar` | dBZ → couleurs NWS, magenta = grêle / tornade |

## Physique simulée

* **Dynamique** : advection semi-lagrangienne (cubique monotone pour les cellules orageuses), ajustement de pression type eau peu profonde, Coriolis, friction, courants de densité séparés (fronts de rafales, brises).
* **Thermodynamique** : bilan d'énergie au sol (soleil astronomique selon latitude/jour/heure, albédo neige/eau, rayonnement IR, flux sensible et latent), couche limite, montée de parcelle réelle (adiabatique sèche puis pseudo-adiabatique) → **CAPE, CIN, LCL, LFC, EL, isotherme 0 °C, Lifted Index**.
* **Cisaillement** : hodographe 3 niveaux → **SRH 0-1/0-3 km**, cisaillement 0-6 km, mouvement de Bunkers, **STP, SCP, SHIP**.
* **Convection** : déclenchement par convergence, thermiques et soulèvement méso-échelle (bruit déterministe), croissance/dissipation, poches froides, mésocyclones, subsidence compensatoire.
* **Microphysique** : condensation, autoconversion (Kessler), chute, évaporation sous la base, type de précipitation (pluie, neige, grésil, pluie verglaçante, grêle).
* **Grêle** (roadmap) : vitesse terminale `Vt = 9·D^0.8` → plus gros grêlon soutenu par l'updraft `W ≈ 0.5·√(2·CAPE)`, fonte sous l'isotherme 0 °C.
* **Foudre** : taux de Price & Rind selon le sommet du nuage, impacts tirés de façon déterministe.
* **Ouragans** : genèse (SST > 26,5 °C, faible cisaillement, humidité), intensité potentielle selon la SST, déclin sur terre, bandes spirales, œil.
* **Accumulation** : humidité du sol (flaques), neige (cm), grêle au sol, fonte.

Performances mesurées (cœur seul, 1 thread, 128×128 cellules) : ~35 ms par pas de 30 s simulées. Avec `ParallelFor` (UE) le coût est divisé par le nombre de cœurs. À ×60 il faut 2 pas par seconde.

## Utilisation en C++ / Blueprint

```cpp
UAerosWeatherSubsystem* Weather = UAerosWeatherSubsystem::Get(this);
const FAerosWeatherSample W = Weather->GetWeatherAtLocation(GetActorLocation());
if (W.HailDiameter > 25.f) { /* se mettre à l'abri */ }
Weather->OnLightningStrike.AddDynamic(this, &AMyActor::HandleLightning);
Weather->SetScenario(EAerosScenario::WinterStorm);
```

Blueprint : `Get Aeros Weather`, `Get Weather At`, `Describe Weather`, événements `On Storm Formed`, `On Tornado Touchdown`, `On Lightning Strike`, `On Phenomenon Changed`.

### Brancher vos assets (Project Settings → Plugins → Aeros Weather)

* Niagara `RainSystem`, `SnowSystem`, `HailSystem` : paramètres utilisateur `Intensity` (float), `Wind` (vector m/s), `HailSize` (mm).
* `StormSystem` : `SpawnRate` (= intensité × 1000, comme la roadmap), `Intensity`, `HailSize`, `Mesocyclone`.
* `TornadoSystem` : `Strength`, `CoreRadius`, `MaxWind`. `LightningSystem` : `Start`, `End`.
* Sons : pluie, impacts de grêle, tonnerre (retard = distance / 343 m/s), tornade, vent.

### Matériaux

* Nuages : `UV = (WorldXY − AerosDomain.xy) / AerosDomain.zw + 0.5` dans `AerosWeatherMap` (R couverture, G base 0-5 km, B sommet 0-16 km, A précipitation).
* MPC `MPC_AerosWeather` : `AerosWetness`, `AerosSnowDepth`, `AerosHailCover`, `AerosRainIntensity`, `AerosSnowIntensity`, `AerosHailIntensity`, `AerosCloudCover`, `AerosTemperature`, `AerosWindSpeed`, `AerosLightningFlash`, `AerosFogDensity`, vecteurs `AerosWind`, `AerosDomain`.

## Réseau

La grille n'est **jamais** répliquée (risque n°1 de la roadmap). Serveur et clients exécutent la même simulation déterministe : même seed, mêmes réglages, mêmes commandes datées en temps simulé (`AAerosWeatherNetState`). Un client qui rejoint rattrape le temps en accéléré et rejoue les commandes. Les acteurs de tempête, tornade et ouragan répliquent seulement leur macro-état.

## Coordonnées

Simulation : x = est, y = nord. Unreal : X = est, **−Y = nord**. Toutes les API exposées renvoient des vecteurs monde Unreal (m/s).

## Tests

* `Session Frontend → Automation → Aeros` : thermodynamique, scénario supercellule, déterminisme.

## Suite (roadmap)

* Compute shaders (RDG) si besoin de grilles plus fines (phase 6).
* Relief (soulèvement orographique), lignes de grains, inondations, assets Niagara/MetaSounds dédiés.
