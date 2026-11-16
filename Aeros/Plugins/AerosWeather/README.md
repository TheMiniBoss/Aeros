# Aeros Weather — plugin Unreal Engine 5.8

Système de météo et de catastrophes naturelles **réaliste et optimisé**. Ce n'est pas un système d'effets scriptés : un **moteur thermodynamique** tourne en arrière-plan (thread dédié). L'état de l'atmosphère (température, humidité, pression, vent) produit des instabilités (CAPE), qui déclenchent nuages, pluie, neige, grêle, orages, supercellules, tornades et ouragans. Les visuels et les dégâts ne font que lire ces données.

> Statut : version 0.4 (catastrophes naturelles + météo réelle). Le cœur de simulation est compilé et testé hors moteur (21 tests : scénarios, déterminisme, crue, tsunami, sable, cendres, blizzard). La partie Unreal (acteurs, rendu, réseau, HTTP) n'est **pas compilée de mon côté** : prévoir quelques corrections au premier build.

---

## Démarrage rapide

1. Ouvrir `Aeros.uproject` (UE 5.8). Accepter la compilation des modules.
2. **Démo immédiate** : n'importe quel niveau (même vide) → *World Settings* → *GameMode Override* = `AerosDemoGameMode` → **Play** (menu : démo jouable ou démo technique, voir plus bas). Le mode démo construit le ciel, le soleil, le brouillard, un **paysage procédural** (collines, montagnes enneigées, lacs, plages, mer, désert), des caisses physiques, un volcan (cône de secours), le contrôleur météo et le radar.
3. **Démo complète** : *Tools → Execute Python Script…* → `Plugins/AerosWeather/Content/Python/aeros_setup.py`.

### Que fait le script Python ?

Les matériaux et les cartes sont des fichiers binaires (`.uasset`, `.umap`) que seul l'éditeur sait écrire. Le script les crée une fois dans `/AerosWeather` :

| Asset | Rôle |
|---|---|
| `Materials/MPC_AerosWeather` | Material Parameter Collection remplie par le plugin à chaque image (mouillé, neige, cendres, crue, secousse…) |
| `Materials/T_AerosDefaultWeather` | petite texture linéaire par défaut (nécessaire au matériau nuages) |
| `Materials/M_AerosVolumetricCloud` | nuages volumétriques pilotés par la simulation : champ de cumulus cellulaire (bases plates, sommets en tourelles creusés façon Nubis), tours d'orage, enclumes, nuages en plateau, bruits 3D Perlin-Worley du moteur (un seul canal, étiré sur 0-1) |
| `Materials/M_AerosCloudShadow` | fonction de lumière du soleil : ombre des nuages du rendu maison (lit la carte de transmittance `AerosCloudShadowMap`, fondu au bord des 100 km) |
| `Materials/M_AerosGround` | sol du paysage : herbe / sable / roche / neige (masques en couleur de sommet), qui se mouille, s'enneige, blanchit sous la grêle, grisit sous la cendre |
| `Materials/M_AerosSea` | mer et lacs opaques (un plan au niveau de la mer), petites vagues qui bougent |
| `Materials/M_AerosWater` | eau translucide des crues, ondes de tempête et tsunamis |
| `Materials/M_AerosEmissive` | lueur sans éclairage (secours) |
| `Materials/M_AerosTornadoFunnel` | entonnoir de tornade : stries en rotation, bords doux, fondu par couleur de sommet |
| `Materials/M_AerosLightning` | canal d'éclair : lueur × luminosité par segment (custom data d'instance) |
| `Materials/M_AerosShaft` | volume translucide doux : rideau de pluie / grêle sous les orages |
| `Maps/L_AerosDemo` | carte démo avec *GameMode Override* = `AerosDemoGameMode` |

Chaque étape est indépendante : à la fin, l'Output Log affiche `[Aeros] SUMMARY` avec OK / FAILED pour chaque asset. Sans le script, la démo marche quand même (nuages du moteur, sol sans couleurs ni détail, mer unie, pas de rideau de pluie).

Sans ouvrir l'éditeur : `UnrealEditor-Cmd.exe Aeros.uproject -run=pythonscript -script=<chemin>/aeros_setup.py -unattended -nullrhi` (l'étape carte échoue dans ce mode, les matériaux sont bien créés).

### Démo jouable : Chasseur d'orages

**Play** ouvre un menu : *Chasseur d'orages*, *Démo technique (vol libre)*, *Quitter* (Z/S ou flèches, Entrée). `-AerosTech` (ou toute option `-AerosTour…`) démarre directement dans la démo technique. Retour au menu depuis la démo technique : **Retour arrière**.

Une fin d'après-midi de supercellules (16 h → 20 h 30, temps ×2 : les orages filent à ≈ 30 m/s à l'écran, le véhicule monte à 300 km/h ; la journée peut durer 2 h 15, la pause permet de la finir). On conduit un véhicule au sol, on rejoint **l'unique supercellule** (radar zoomé à 30 km), on l'approche par le côté et on photographie ce que le ciel donne. **La journée finit quand l'orage meurt** (15 s sans orage), au coucher du soleil, ou depuis la pause.

* **Briefing** : le scénario *Supercellules* tourne en accéléré depuis 15 h 15 pendant qu'on lit les règles (≈ 1 min : à ×600 la simulation ne fait que ×40-100). **Un seul orage** : au début du briefing la naissance spontanée des orages est coupée et ceux en cours effacés (`SetConvectionControl(0, true)`), puis une bulle chaude, 8-16 km du joueur, sur la plaine basse (pas les montagnes : la base de l'orage passait sous le relief), de préférence à l'ouest, fait naître la supercellule (relancée si rien ne pousse). Pas de tornade pendant le briefing : on voit la première descendre. Le briefing part de 14 h 30 et dure ≈ 10 s (avec un seul orage la simulation suit vraiment ×600) ; il attend une supercellule **mûre** (mésocyclone, enclume au-dessus de 10 km, une heure d'âge) : Entrée part alors tout de suite, sinon départ à 16 h (au plus tard 2 min). Départ à ≈ 20 km de l'orage, sur son flanc droit (sud-est), au sec dans un rayon de 5 km. Les tornades ne peuvent naître qu'après 45 s de conduite. Retour au menu ou à la démo technique : tout est remis (naissance des orages, tornades, portée du radar).
* **Tornades** : pendant la journée, le plugin tente une tornade toutes les 60 s simulées par supercellule critique au lieu de 600 s (`TornadoCheckInterval`, remis à la fin) : à ×2, une tentative toutes les 30 s, sinon il fallait attendre 5 min réelles par essai.
* **Points** (`AerosChaseRules.h`, testés hors moteur) : tornade (100 × (EF+1), bien plus près de 2 km, plus rien au-delà de 20 km), nuage-mur, éclair pris au bon moment (dans les 0,6 s), supercellule entière vue de 10 à 80 km, traversée d'un cœur de grêle (> 2 cm). Le sujet doit être dans le quart central du cadre, **vu** (pas caché par une colline ou une dune : tracé contre le relief) et **pas noyé dans la pluie** (la visibilité est la pire rencontrée sur la ligne de visée). Une seule photo compte par sujet : une meilleure ajoute la différence.
* **Dangers** : vents de tornade (> 35 m/s, ils poussent aussi le véhicule), grêle > 2 cm, foudre à moins de 300 m, routes inondées (> 0,4 m). Hors danger le véhicule se répare lentement. Détruit : pénalité d'un quart du score, nouveau véhicule à 8 km.
* **Rangs** : Débutant, Observateur (400), Chasseur (1200), Chasseur confirmé (3000), Chasseur légendaire (6000).
* **Interface** : heure et coucher du soleil, score et rang, boussole avec orages et tornades, radar 160 km, vitesse, état du véhicule, marqueurs à l'écran, alertes, viseur avec estimation des points en direct. L'écran de fin liste les meilleures photos. Les photos (sans interface) sont dans `Saved/Screenshots/<plateforme>/AerosChase/`.

| Touche | Action |
|---|---|
| **Z / S** (ou flèches) | accélérer / freiner, marche arrière |
| **Q / D** | tourner |
| Souris | regarder autour |
| Clic droit (maintenu) | zoom ×4 (viseur) |
| Clic gauche ou **Espace** | photo |
| **T** | trajet rapide : à ≈ 4 km de la tornade la plus proche (sinon de la supercellule), hors de la pluie ; l'horloge avance de 10 min |
| **Échap** ou **P** | pause (la météo s'arrête) ; en pause, **Entrée** finit la journée |
| **Retour arrière** | menu |

Pas de touches de fonction (F1-F12) dans la démo jouable : elles servent souvent au débogage du moteur.

Options de test : `-AerosChase` (saute le menu), `-AerosChaseAuto` (pilote automatique : va vers les tornades, vise, photographie toutes les 6 s), `-AerosChaseShots=N` (capture avec interface toutes les N s, `AerosChase/auto_###.png`), `-AerosChaseEndAfter=S` (fin de journée après S s de conduite), `-AerosChaseQuitAfter=S`, `-AerosChaseFromMenu=S` (passe par le menu : Entrée après S s), `-AerosChaseNear=km` (distance gardée aux tornades par le pilote, 3 km). Le log écrit aussi la bulle, le briefing et le départ (`Aeros chase: …`). Le log écrit `Aeros chase: day over, score …`.

### Touches de la démo technique (clavier AZERTY)

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
| X | debug 3D (zones d'orage, flèches de vent) |
| **Échap** | menu pause : la météo s'arrête ; Reprendre, Menu principal (chasseur d'orages), Quitter |
| F1 / Tab | affichage : **simple** → expert → radar seul → caché → aide |
| **F2** | **visite guidée** : 11 étapes (orage, éclairs de nuit, tornade, volcan, séisme, tsunami, crue, haboob, blizzard), chacune expliquée à l'écran |

Les chiffres QWERTY (1-0) marchent aussi pour les scénarios.

### Comprendre ce qui se passe

* **HUD simple** (par défaut) : la météo ici en mots simples, une rose des vents relative à la caméra, un paragraphe « ce qui se passe » qui explique la physique en cours, et des jauges de risque (orage, grêle, tornade, vent, inondation, visibilité, neige, secousse).
* **Marqueurs** : un losange au-dessus de chaque orage, tornade, ouragan, volcan et épicentre, avec type et distance. Hors de l'écran, une flèche au bord donne la direction.
* **Journal d'événements** (en bas à droite) : formation d'orage, tornade, séisme, tsunami, éruption, changement de temps ici.
* **HUD expert** : tous les chiffres (sondage, CAPE, SRH, STP…).
* **Visite guidée** (F2) : chaque étape place la caméra, déclenche le phénomène, l'explique et enregistre une capture dans `Saved/Screenshots/<plateforme>/AerosTour/`. En ligne de commande : `-AerosTour` (ou `-AerosTourQuit` pour quitter à la fin, utile pour des captures automatiques) ; `-AerosTourOnly=1,2,3` ne joue que ces étapes ; `-AerosTornadoShape=0..7` impose la forme des tornades (corde, aiguille, cône, cylindre, concave, sablier, wedge, multi-vortex) et `-AerosTornadoAge=0.4` les fait démarrer à 40 % de leur vie ; `-AerosDumpMaps` enregistre aussi les cartes nuages en PNG (`Saved/AerosDebug`). La visite (et la météo) se met en pause tant que des shaders compilent. L'étape « éclairs » prend sa capture au moment où un éclair brille.

Un seul orage (gameplay) : `UAerosWeatherSubsystem::SetConvectionControl(NaturalInitiation, bClearExisting)` (0 = les orages ne naissent plus que des bulles chaudes `InjectThermalBubble` ; commande répliquée, test `Aeros.Simulation.SingleStorm`).

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
FAerosCloudViewExtension (module AerosCloudRender) <-- rendu maison des nuages (passe avant DOF, r.AerosClouds 1)
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
| `AAerosTornado` | vent, aspiration des objets physiques, dégâts, fractures Chaos ; entonnoir déformable (tube procédural) : formes corde, aiguille, cône, cylindre, concave, sablier, wedge, multi-vortex choisies selon l'EF ; cycle de vie (descente, maturité, « rope out ») ; inclinaison et ondulation ; stries qui tournent à la vitesse du vortex |
| `AAerosWeatherRadar` | dBZ → couleurs NWS, magenta = grêle / tornade |

## Physique simulée

* **Dynamique** : advection semi-lagrangienne (cubique monotone pour les cellules orageuses), ajustement de pression type eau peu profonde, Coriolis, friction, courants de densité séparés (fronts de rafales, brises).
* **Thermodynamique** : bilan d'énergie au sol (soleil astronomique selon latitude/jour/heure, albédo neige/eau, rayonnement IR, flux sensible et latent), couche limite, montée de parcelle réelle (adiabatique sèche puis pseudo-adiabatique) → **CAPE, CIN, LCL, LFC, EL, isotherme 0 °C, Lifted Index**.
* **Cisaillement** : hodographe 3 niveaux → **SRH 0-1/0-3 km**, cisaillement 0-6 km, mouvement de Bunkers, **STP, SCP, SHIP**.
* **Convection** : déclenchement par convergence, thermiques et soulèvement méso-échelle (bruit déterministe), croissance/dissipation, poches froides, mésocyclones, subsidence compensatoire.
* **Microphysique** : condensation, autoconversion (Kessler), chute, évaporation sous la base, type de précipitation (pluie, neige, grésil, pluie verglaçante, grêle).
* **Grêle** (roadmap) : vitesse terminale `Vt = 9·D^0.8` → plus gros grêlon soutenu par l'updraft `W ≈ 0.5·√(2·CAPE)`, fonte sous l'isotherme 0 °C.
* **Foudre** : taux de Price & Rind selon le sommet du nuage, impacts tirés de façon déterministe. Chaque impact part à son heure simulée exacte (pas en rafale par pas de calcul). Rendu procédural (`AAerosLightningFlash`) : canal fractal (déplacement du point milieu) avec branches récursives ; **négatif nuage-sol** (~90 % : traceur par bonds, 1 à 8 arcs en retour espacés de 40-120 ms, parfois courant continu), **positif** (~10 % : part de l'enclume à 6-15 km du cœur, un arc très puissant, long courant continu), **intra-nuage** (lueur dans le nuage, parfois « rampant » horizontal sous l'enclume). Luminosité, épaisseur et lumière suivent le courant de crête ; le nuage ne s'illumine qu'autour de l'éclair.
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

### Rendu des nuages (module `AerosCloudRender`, `r.AerosClouds 1` par défaut)

Le `VolumetricCloud` du moteur reprojette son historique avec le seul mouvement de la caméra : le vent, l'évolution des nuages et les éclairs le faisaient scintiller (« dither »). Le plugin trace donc ses nuages lui-même, dans une extension de vue (`FAerosCloudViewExtension`, passe `BeforeDOF`, shaders dans `Shaders/Private/AerosClouds.usf` et `AerosCloudDensity.ush`). Le `VolumetricCloud` est caché tant que `r.AerosClouds` vaut 1 ; à 0 on retrouve l'ancien chemin et son matériau.

* **Densité** : le même champ que l'ancien matériau (cumulus cellulaires, tours, enclumes, nuages en plateau, grille voxel, voile de pluie), sur des bruits 3D générés au démarrage (`AerosCloudNoise`, C++ pur, tuilables, 4 canaux normalisés : Perlin-Worley + Worley 8/16/32 ; détail Worley 4/8/16 + fbm). LOD continu : chaque octave de détail rejoint sa moyenne avec la distance (`AerosCloudOctaves` : 4 près, 1 au-delà de ~11 km), pas de saut de mip.
* **Tracé** : un pixel sur 4 (1 sur 16 à `Quality 0`), décalé à chaque image dans son bloc ; l'accumulation se fait en pleine résolution, donc chaque pixel reçoit son propre rayon toutes les 4 images (net, pas un agrandissement bilinéaire). Départ à la caméra quand elle est dans les nuages, pas de 60 m près → 500 m loin, pas doublés dans l'air vide, sortie à T < 0,01.
* **Lumière** : soleil (phase double lobe g 0,6 / -0,25, 3 ordres de diffusion 0,25ⁿ / 0,8ⁿ), ombre vers le soleil sur 2,6 km (6 segments près, 3 loin), ciel = irradiance SH × couleur de la sky light, assombri en bas du nuage et sous une colonne épaisse ; albédo plus gris sous la pluie ; perspective aérienne du moteur devant le nuage (lointain bleuté).
* **Temps** : accumulation par vue en pleine résolution (historique remis à zéro sur coupure de caméra, saut > 2 km, changement de taille, trou de temps), relue en Catmull-Rom (une lecture bilinéaire floutait à chaque image en mouvement), reprojetée avec la caméra et une profondeur lissée dans le temps (plus d'ondulation), en essayant deux mouvements, celui des orages et la dérive du vent (cumulus, bruit), et en gardant le plus cohérent ; découpage par variance en YCoCg. Détail fin (Worley 62-125 m) sous ~1,4 km de la caméra seulement.
* **Éclairs** : jusqu'à 8 (`Aeros::TFlashList`, les plus brillants), ajoutés à chaque image sans passer par l'historique ; échelle physique plafonnée (discrets de jour, lumineux la nuit, pas de ciel blanchi).
* **Ombres au sol** : carte 512² de transmittance sur 100 km autour de la caméra (recalée par pas de 1 km, un quart des lignes par image), lue par la fonction de lumière `M_AerosCloudShadow` du soleil ; la sky light baisse sous un ciel couvert.
* **Coût** (RTX 4060, 1336×751, `Quality 2`) : 3,1-3,7 ms (`profilegpu`), GPU total +0,5 ms par rapport à l'ancien chemin.

| Variable | Rôle |
|---|---|
| `r.AerosClouds` | 1 rendu maison (défaut), 0 ancien `VolumetricCloud` |
| `r.AerosClouds.Quality` | 0-3 : pas max 96 / 128 / 192 / 256, échantillons de soleil 3 / 4 / 6 / 8, tracé au quart de résolution à 0 (défaut 2) |
| `r.AerosClouds.HistoryBlend` | poids de l'image neuve dans l'accumulation (0,1) |
| `r.AerosClouds.Ambient`, `r.AerosClouds.Haze` | échelle du ciel et de la perspective aérienne (1 = physique) |
| `r.AerosClouds.Flash` | > 0 : éclairs de test près de la caméra ; < 0 : aucun éclair (mesures) |
| `r.AerosClouds.Wind` | test : mouvement imposé pour la reprojection (m/s vers +X) |
| `r.AerosClouds.LogResets` | 1 : journalise chaque remise à zéro de l'historique |

Mesures dans la visite : `-AerosFlickerShots` (paire à 0,2 s), `-AerosFlickerOnSwap` (paire au moment d'un échange de grille voxel), `-AerosTourStepSeconds=N` (étapes après la première), `-AerosTourExec=profilegpu` (temps GPU dans le log à chaque capture), `-AerosTourMove=V` / `-AerosTourYaw=D` (caméra qui avance de côté en m/s et tourne en °/s pendant les étapes après la première : flou et ondulation en mouvement). Script de mesure : `Tests/Visual/flicker.py`.

### Tornades dans les nuages (`r.AerosClouds 1`)

Avec le rendu maison, l'entonnoir, le nuage-mur et la poussière font partie du nuage (même lumière, même pluie, rien dessiné par-dessus) ; le tube en maillage ne sert plus qu'avec `r.AerosClouds 0`. La physique ne change pas (vent, objets aspirés, dégâts, EF, trajectoire, son).

* **Modèle** (`Core/AerosFunnel.h`, C++ pur, testé hors moteur ; le shader reprend les mêmes formules) : vent du tourbillon `v(r) = Vmax · 2(r/rc) / (1 + (r/rc)²)`, baisse de pression `Δp = ρ Vmax² / (1 + (r/rc)²)`. L'air condense là où `K Δp ≥ ρ g (base − h)` : l'entonnoir s'affine vers le bas, descend plus bas avec un tourbillon plus fort ou une base plus basse, et ne touche le sol que si `K Vmax² ≥ g · base`. Son vent suit la circulation (racine de la force de vie, et plus rapide quand le cœur s'amincit) : l'entonnoir descend de la base à la naissance, reste au sol pendant la phase corde, puis remonte et disparaît.
* **Formes** : elles viennent des paramètres (vent, cœur, base, âge) : cône, corde penchée et courbée en fin de vie, wedge plus large que haut (cœur large, base basse), multi-vortex (EF4+, 2 tirages sur 3 : 3 à 5 petits tourbillons qui tournent dans un voile ; ils se fondent en un seul entonnoir pendant la phase corde).
* **Autour** : nuage-mur en cuvette sous la base (bord déchiqueté, stries en spirale qui tournent lentement, descente limitée à 30 % de la hauteur de la base), poussière au sol là où le vent dépasse ~30 m/s (embruns au-dessus de la mer).
* **Rendu** : au plus 4 tornades (les plus proches, avec un fondu d'1 s quand l'ensemble change) ; la base est le LCL de l'orage, borné à 0,3-3 km comme la grille voxel ; le tracé trouve le volume de chaque tornade sur le rayon et y fait des pas fins (30 m près de l'entonnoir), sans jamais l'enjamber ; l'accumulation répond plus vite sur l'entonnoir (masque du tracé). Coût mesuré : +0,1 ms pour un cône d'un tiers d'écran à 2 km, ≈ 6 ms au total caméra à 300 m de deux entonnoirs.

| Variable / option | Rôle |
|---|---|
| `r.AerosClouds.TornadoK` | calibrage de la condensation, valeur minimale (2 par défaut) : chaque tornade prend au moins le K qui fait toucher le sol à son entonnoir mûr (`Aeros::Funnel::TouchdownK`, 1,3 x le contact). Il pend sous le nuage pendant la naissance et remonte à la fin de la corde. La tornade suit le mésocyclone de son orage (même à x60) |
| `r.AerosClouds.TornadoDebug` | 1 : dessine le volume de chaque tornade en nuage léger |
| `-AerosTornadoCam=Dist,Alt,LookAlt` | cadrage de l'étape tornade (km, m, m ; distance négative = l'autre côté) ; défaut 2 km, 60 m, 250 m |
| `-AerosTornadoVmax=`, `-AerosTornadoCore=`, `-AerosTornadoSubs=`, `-AerosTornadoAge=` | galerie : vent max (m/s), cœur (m), sous-tourbillons, âge (0-1) ; l'étape de la visite démarre à 0,3 |
| `-AerosTornadoLCL=` | force la base de l'entonnoir (m) ; attention, elle ne bouge pas la base du nuage visible |
| `-AerosTourShotEvery=N` | une capture toutes les N secondes de l'étape (`<étape>_tNNN.png`) |

### Matériaux

* **Nuages voxel (branche `claude/voxel-clouds`)** : `FAerosCloudVolume` (C++ pur, `Core/`) construit une grille 3D de 320 × 320 × 48 voxels (100 × 100 km × 16 km, voxels de 312 m, recentrée par pas de 10 km) autour de la caméra à partir de la simulation : tour inclinée (LCL → EL), sommet qui dépasse, enclume étalée par le vent de 500 hPa (les enclumes qui se recouvrent mélangent leur hauteur de sommet, s'amincissent vers les bords) avec mammatus irréguliers (bruit de valeurs sur les km monde), cœur de précipitations, nuage-mur, ligne de tours de flanc, nuage en plateau, nappes stratiformes. Reconstruite ≈ 1 fois/s sur un thread de travail, envoyée dans deux textures 3D en alternance (fondu), masquée après un saut > 40 km jusqu'à la nouvelle grille. Le matériau la lit (déformée et érodée par le bruit 3D), garde les cumulus procéduraux, repasse en 2D au-delà de 44-50 km ; au-dessus de 9-11,5 km le nuage est de la glace (enclume, haut des tours) : lisse et fibreux (bruit d'érosion étiré 16 × 16 × 1,5 km) au lieu d'être creusé en bulles ; la lecture verticale de la grille est déformée au-dessus de 6-9 km (±0,5 km de houle, ±0,2 km de bosses) pour donner du relief au dessus des enclumes ; les enclumes divergent et se courbent (`FAerosCloudVolume::AnvilBend` : ±10° au départ, ±25° au bout, bruit de 40 km, même règle en 2D et en 3D) ; les bruits de détail sont lus à un mip choisi selon la distance (LOD) ; ombre des nuages sur 15 km avec 2× les échantillons (25 km / 10 échantillons laissait l'enclume d'un blanc plat), éclairage « Volumetric Advanced » (phase double lobe, 2 octaves de diffusion multiple). Coupes de contrôle : `-AerosDumpMaps` → `Saved/AerosDebug/*_vox_*.png`. Coût GPU (`stat unit`, 1336×751) : ≈ 12 ms vu du sol, 17-20 ms au-dessus d'un champ de cumulus ; réglable par `AAerosWeatherController::CloudSampleScale` (1,25 par défaut) ; `bSharpCloudTracing` (faux par défaut : le tracé demi-résolution, mode 1, scintillait même à x1) et `CloudUpsamplingMode` (-1 = réglage du projet). Mesure du scintillement : `-AerosTourTimeScale=1 -AerosFlickerShots` (deux captures à 0,2 s d'écart, `<étape>_b.png`) ; `-AerosCloudMode=N` essaie un autre `r.VolumetricRenderTarget.Mode`. Le matériau évite tout détail sous ~200 m (bruits au mip 2-3 selon la distance), pilote les déformations par des bruits lisses (dont un second échantillon décorrélé à 9 km par tuile), garde un cœur plein, supprime les miettes hors du contour, et fond le champ de cumulus en couverture douce au-delà de 30-70 km.
* Nuages : `UV = (WorldXY − AerosDomain.xy) / AerosDomain.zw + 0.5` dans `AerosWeatherMap` (R couverture, G base 0-5 km, B sommet 0-16 km, A précipitation) et `AerosCloudShapeMap` (R enclume étalée par le vent de 500 hPa, G courant ascendant, B front de rafales / nuage en plateau, A convection). Le nœud HLSL du matériau place un nuage par cellule de 1,1 km avec une probabilité égale à la couverture (cumulus de 0,6-2,5 km d'épaisseur par beau temps, tours là où la colonne est convective). Ce champ ne donne que la couverture : le bruit `T_VolumeNoiseShape128` à 3 km par tuile (mip 1) creuse les tourelles (remap Nubis : au cœur toutes les bulles restent, au bord seulement les plus grosses), puis `T_VolumeNoiseErosion32` (4 km par tuile) érode ; ces bruits n'ont qu'un canal et sont étirés sur 0-1 ; le bruit dérive avec le vent simulé (`AerosNoiseOffset`), au plus x60 (`UAerosCloudManager::MaxNoiseDriftTimeScale`) : à x600 le champ glissait de 9 km/s et la reconstruction temporelle en faisait du grain. Portée de rendu 180 km.
* **Paysage de la démo** (`AAerosDemoTerrain`, module démo) : hauteur déterministe en C++ pur (`AerosDemoTerrainModel.h`, même seed = même relief) : plaines autour du départ (≈ 0,7 m), collines, lacs (creux sous le niveau de la mer), chaînes de montagnes en bruit « ridged » (jusqu'à ≈ 4 km, aucune à moins de 20 km du départ), côte des `WaterRegions` avec plage, plateau continental et baies (les baies ne font qu'avancer la mer dans les terres : la mer de la simulation reste de la mer), dunes dans les `DesertRegions`, socle plat sous le volcan. Rendu : 9 anneaux imbriqués de 128 × 128 mailles (25 m → 6,4 km, 3,2 → 819 km de large) recentrés sur la caméra (le plus fin tous les 200 m), calculés sur un thread de travail puis échangés ensemble ; bords cousus (pas de fissure). Collisions : une section cachée par anneau (WorldStatic), donc les tracés au sol, le pawn, les caisses, les tornades et l'échantillonnage du relief par la simulation voient le même relief (`TerrainHeight` = relief visible, à la cellule de 2 km près). Couleur de sommet : R sable, G roche (pente, altitude), B neige (> 2,5-2,9 km), A herbe sèche ; le matériau y ajoute un bruit en coordonnées monde (7, 61, 530 m) et la météo. Mer : un plan opaque au niveau de la mer (`SeaLevelZ`), qui suit l'anomalie au large (tsunami, onde de tempête) ; le plan translucide du pawn ne sert plus qu'aux crues. Les lacs sont des cellules d'eau pour la simulation (humidité, eau libre), mais `FindNearestOcean` (tsunami, ouragan, touche I) ignore les étendues d'eau de moins de 150 cellules (600 km²). Aperçu sans Unreal : `Tests/Standalone/terrain_preview.cpp` (carte PNG + hauteurs aux points clés). Dans une carte qui a son propre sol, rien ne change (sauf le plan plat du moteur, remplacé).
* MPC `MPC_AerosWeather` : `AerosWetness`, `AerosSnowDepth`, `AerosHailCover`, `AerosRainIntensity`, `AerosSnowIntensity`, `AerosHailIntensity`, `AerosCloudCover`, `AerosTemperature`, `AerosWindSpeed`, `AerosLightningFlash`, `AerosFogDensity`, `AerosFloodDepth`, `AerosDust`, `AerosAshDepth`, `AerosAshFall`, `AerosBlowingSnow`, `AerosShake`, `AerosSeaLevel`, vecteurs `AerosWind`, `AerosDomain`.

## Réseau

La grille n'est **jamais** répliquée (risque n°1 de la roadmap). Serveur et clients exécutent la même simulation déterministe : même seed, mêmes réglages, mêmes commandes datées en temps simulé (`AAerosWeatherNetState`). Un client qui rejoint rattrape le temps en accéléré et rejoue les commandes. Les acteurs de tempête, tornade et ouragan répliquent seulement leur macro-état.

## Coordonnées

Simulation : x = est, y = nord. Unreal : X = est, **−Y = nord**. Toutes les API exposées renvoient des vecteurs monde Unreal (m/s).

## Tests

* `Session Frontend → Automation → Aeros` : thermodynamique, scénario supercellule, déterminisme, catastrophes (crue, tsunami, haboob, cendres, blizzard).
* Sans Unreal : `Tests/Standalone` (`./build.sh` ou `build.bat`) compile le cœur et lance les mêmes tests en quelques minutes, plus les tests des bruits de nuages (tuilables, étendue, canaux décorrélés, déterminisme) et de la liste d'éclairs. Voir `Tests/Standalone/README.md`.

## Suite (roadmap)

* Compute shaders (RDG) si besoin de grilles plus fines (phase 6).
* Rivières à l'échelle sous-maille, glissements de terrain, incendies de forêt, assets Niagara/MetaSounds dédiés.
