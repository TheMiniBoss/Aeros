# Aeros Weather — plugin Unreal Engine 5.8

Système de météo et de catastrophes naturelles **réaliste et optimisé**. Ce n'est pas un système d'effets scriptés : un **moteur thermodynamique** tourne en arrière-plan (thread dédié). L'état de l'atmosphère (température, humidité, pression, vent) produit des instabilités (CAPE), qui déclenchent nuages, pluie, neige, grêle, orages, supercellules, tornades et ouragans. Les visuels et les dégâts ne font que lire ces données.

> Statut : version 0.4 (catastrophes naturelles + météo réelle). Le cœur de simulation est compilé et testé hors moteur (21 tests : scénarios, déterminisme, crue, tsunami, sable, cendres, blizzard). La partie Unreal (acteurs, rendu, réseau, HTTP) n'est **pas compilée de mon côté** : prévoir quelques corrections au premier build.

---

## Démarrage rapide

1. Ouvrir `Aeros.uproject` (UE 5.8). Accepter la compilation des modules.
2. **Démo immédiate** : n'importe quel niveau (même vide) → *World Settings* → *GameMode Override* = `AerosDemoGameMode` → **Play**. Le mode démo construit le ciel, le soleil, le brouillard, un sol de 200 km, des caisses physiques, un volcan (cône de secours), le contrôleur météo et le radar.
3. **Démo complète** : *Tools → Execute Python Script…* → `Plugins/AerosWeather/Content/Python/aeros_setup.py`.

### Que fait le script Python ?

Les matériaux et les cartes sont des fichiers binaires (`.uasset`, `.umap`) que seul l'éditeur sait écrire. Le script les crée une fois dans `/AerosWeather` :

| Asset | Rôle |
|---|---|
| `Materials/MPC_AerosWeather` | Material Parameter Collection remplie par le plugin à chaque image (mouillé, neige, cendres, crue, secousse…) |
| `Materials/T_AerosDefaultWeather` | petite texture linéaire par défaut (nécessaire au matériau nuages) |
| `Materials/M_AerosVolumetricCloud` | nuages volumétriques pilotés par la simulation (couverture, base, sommet, orages) |
| `Materials/M_AerosGround` | sol qui se mouille, s'enneige, blanchit sous la grêle, grisit sous la cendre |
| `Materials/M_AerosWater` | eau translucide des crues, ondes de tempête et tsunamis |
| `Maps/L_AerosDemo` | carte démo avec *GameMode Override* = `AerosDemoGameMode` |

Chaque étape est indépendante : à la fin, l'Output Log affiche `[Aeros] SUMMARY` avec OK / FAILED pour chaque asset. Sans le script, la démo marche quand même (nuages du moteur, sol uni, eau opaque).

### Touches de la démo (clavier AZERTY)

| Touche | Action |
|---|---|
| Souris, **Z Q S D** (ou flèches) | se déplacer ; **Espace / E** monter, **A / C** descendre ; Shift ×10, Ctrl ×100 |
| **& é " ' ( - è _ ç à** | scénarios : Climat, Beau temps, Orages d'été, Supercellules, Tempête de neige, Saison tropicale, Pluie frontale, **Blizzard**, **Désert / tempêtes de sable**, **Mousson / inondations** |
| B / T / H | bulle chaude (orage) / tornade / ouragan (avec onde de tempête sur la côte) |
| **G** | séisme M6.6 à 15 km |
| **Y** | séisme sous-marin M9 au large → **tsunami** |
| **U** | éruption du volcan (VEI 3, 4, 5, 6, 2…) |
| **O** | déluge de 250 mm → **crue éclair** |
| **L** | rafale descendante → **haboob** sur sol sec |
| **W** | **météo réelle** : Paris, Oklahoma City, Miami, Tokyo, Reykjavik, Phoenix, Mumbai, Montréal, Dakar, Sydney |
| F / V / I | aller à l'orage le plus proche / au volcan / sur la côte face à la mer |
| = / ) (ou pavé + / −, PgUp / PgDn) | vitesse du temps ×2 / ÷2 ; P pause |
| N / M, J / K | −/+ 1 heure, −/+ 1 mois |
| R | radar : réflectivité / vitesse / type de précipitation / **risques** (crue, sable, cendre, tsunami) |
| X | debug ; F1 / Tab : HUD complet → radar → caché → aide |

Les chiffres QWERTY (1-0) marchent aussi pour les scénarios.

Console : `aeros.Scenario 0-10`, `aeros.TimeScale X`, `aeros.Time jour heure`, `aeros.Quake M km`, `aeros.Tsunami M`, `aeros.Flood mm`, `aeros.Haboob`, `aeros.Eruption VEI`, `aeros.Live lat lon nom`, `aeros.Bubble`, `aeros.Hurricane`, `aeros.Tornado`, `aeros.Debug 0-2`, `stat Aeros`.

---

## Catastrophes naturelles

| Type | Comment c'est simulé | Gameplay |
|---|---|---|
| **Crues, inondations** | relief tracé sous la grille (line traces), infiltration selon l'humidité du sol (sol gelé ou désert = ruissellement), ruissellement de saturation, routage de Manning vers l'aval, fonte des neiges | plan d'eau autour du joueur (`UAerosWaterSurfaceComponent`), courant qui pousse objets et personnages, flottabilité, dégâts si eau profonde et rapide |
| **Tsunamis** | soulèvement du fond selon Mw, propagation en eau peu profonde (vitesse √(g·h), bathymétrie plateau → abysses), absorption aux bords, déferlement sur la côte | séisme sous-marin M ≥ 7 peu profond → tsunami automatique ; montée de la mer puis retrait ; alerte HUD |
| **Onde de tempête** | même solveur : baromètre inverse (1 cm/hPa) + tension du vent des ouragans | inondation côtière quand un ouragan arrive |
| **Séismes** | Mw, profondeur, loi d'atténuation (PGA), ondes P (6 km/s) puis S (3,5 km/s), durée selon la magnitude, répliques (Bath / Omori), sismicité spontanée (Gutenberg-Richter) | secousse caméra, objets physiques secoués, fractures Chaos, chutes ; intensité de Mercalli au HUD |
| **Volcans** | `AAerosVolcano` : repos → agitation (essaim de séismes) → éruption → déclin ; VEI 1-7, débit ~10^(VEI+2,5) kg/s, colonne de Mastin ; cendres dans la simulation (transport par le vent d'altitude, retombées, ciel assombri), éclairs volcaniques, pyro-convection | coulées de lave qui suivent le vrai relief et brûlent, bombes volcaniques (dégâts + Chaos), nuées ardentes (VEI ≥ 4), cendre au sol |
| **Blizzard** | manteau neigeux initial dans les climats froids, neige soufflée (vent > 9 m/s), voile blanc, refroidissement éolien | visibilité < 400 m, particules de neige au ras du sol, brouillard blanc |
| **Tempêtes de sable** | soulèvement si vent > seuil (dépend de l'humidité), flux ~ (u − u*)²·u, régions désertiques, dépôt, lessivage par la pluie ; les fronts de rafales d'orages créent des haboobs | visibilité réduite, ciel ocre, soleil voilé, particules de sable |

Réglages : `FAerosSimulationSettings` (régions désertiques, zones sismiques, niveau de la mer `SeaLevelZ`, échantillonnage du relief) et *Project Settings → Plugins → Aeros Weather* (Niagara, sons, sismicité spontanée, force de la secousse caméra).

## Météo réelle (bonus)

`UAerosWeatherSubsystem::RequestRealWeather(Latitude, Longitude, Nom)` (ou touche **W**, ou `aeros.Live 48.85 2.35 Paris`) :

1. interroge **Open-Meteo** (gratuit, sans clé) : surface (température, point de rosée, pression, vent, nuages, précipitations, code météo OMM, CAPE, humidité du sol), niveaux 850 et 500 hPa, et la température de la mer (API marine) ;
2. convertit tout en état synoptique de la simulation, règle la latitude et l'heure locale du lieu, passe le temps en ×1 ;
3. amorce ce qui se passe déjà (pluie en cours, orages signalés) ;
4. se met à jour toutes les 15 minutes (réglable).

Ensuite la physique prend le relais : fronts, orages et nuages évoluent à partir de l'état réel. Les coordonnées envoyées à Open-Meteo sont seulement celles du lieu choisi. *Non testé ici : le réseau de mon environnement bloque l'accès à Open-Meteo.*

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
 ├─ UAerosHazardManager                            <-- séismes, physique des crues, volcans
 │    ├─ AAerosEarthquake    (répliqué, ondes P/S, secousse caméra, Chaos, répliques)
 │    └─ AAerosVolcano       (placé dans le niveau : cendres, lave, bombes, nuées ardentes)
 ├─ UAerosLiveWeather                              <-- météo réelle (Open-Meteo, HTTP + JSON)
 └─ UAerosCloudManager                             <-- textures GPU + Material Parameter Collection
AAerosWeatherController   (dans le niveau : réglages, soleil, brouillard, matériau nuages)
AAerosWeatherRadar        (radar Doppler rotatif -> texture UI)
UAerosPrecipitationComponent (pluie/neige/grêle/sable/cendre locales : Niagara ou rendu instancié de secours)
UAerosWaterSurfaceComponent  (plan d'eau des crues, tsunamis, ondes de tempête autour du joueur)
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
* MPC `MPC_AerosWeather` : `AerosWetness`, `AerosSnowDepth`, `AerosHailCover`, `AerosRainIntensity`, `AerosSnowIntensity`, `AerosHailIntensity`, `AerosCloudCover`, `AerosTemperature`, `AerosWindSpeed`, `AerosLightningFlash`, `AerosFogDensity`, `AerosFloodDepth`, `AerosDust`, `AerosAshDepth`, `AerosAshFall`, `AerosBlowingSnow`, `AerosShake`, `AerosSeaLevel`, vecteurs `AerosWind`, `AerosDomain`.

## Réseau

La grille n'est **jamais** répliquée (risque n°1 de la roadmap). Serveur et clients exécutent la même simulation déterministe : même seed, mêmes réglages, mêmes commandes datées en temps simulé (`AAerosWeatherNetState`). Un client qui rejoint rattrape le temps en accéléré et rejoue les commandes. Les acteurs de tempête, tornade et ouragan répliquent seulement leur macro-état.

## Coordonnées

Simulation : x = est, y = nord. Unreal : X = est, **−Y = nord**. Toutes les API exposées renvoient des vecteurs monde Unreal (m/s).

## Tests

* `Session Frontend → Automation → Aeros` : thermodynamique, scénario supercellule, déterminisme, catastrophes (crue, tsunami, haboob, cendres, blizzard).
* Sans Unreal : `Tests/Standalone` (`./build.sh` ou `build.bat`) compile le cœur et lance les mêmes tests en quelques minutes. Voir `Tests/Standalone/README.md`.

## Suite (roadmap)

* Compute shaders (RDG) si besoin de grilles plus fines (phase 6).
* Rivières à l'échelle sous-maille, glissements de terrain, incendies de forêt, assets Niagara/MetaSounds dédiés.
