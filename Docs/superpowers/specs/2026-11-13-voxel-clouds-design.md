# Nuages voxel « à la Nubis 3 » pour Aeros — conception

Branche : `claude/voxel-clouds`.

## Pourquoi

Les nuages actuels (carte 2D vue en volume + bruit) donnent des « boules » sans anatomie d'orage.
L'utilisateur veut un rendu aussi crédible que les simulateurs (CM1) et que les mods Minecraft
(ProtoManly's Weather, Opus), **en restant fluide**. Approche choisie par l'utilisateur : **B, grille 3D
nourrie par la simulation** (comme Nubis 3 de Guerrilla, qui a quitté le 2,5D pour des voxels).

## Objectifs / non-objectifs / anti-objectifs

- **Objectifs** : chaque orage a sa vraie forme 3D (tour, sommet qui dépasse, enclume, mammatus,
  nuage-mur, nuage en plateau, ligne de tours de flanc, cœur de précipitations) ; cumulus crédibles
  autour ; éclairage nuageux réaliste (liseré argenté, cœurs sombres) ; tout dérive de la simulation.
- **Non-objectifs** : solveur de fluides 3D (type CM1) ; ouragans en spirale (plus tard) ; nouvelle
  physique dans `Core/` (on lit les champs existants).
- **Anti-objectifs** : chute de fluidité (cible : coût nuages proche d'aujourd'hui) ; « popping »
  visible quand la grille se met à jour ; nuages qui ne correspondent plus au radar / à la simulation.

## Contraintes

- La simulation est 2,5D (3 niveaux par colonne, cases de 2 km) : la 3D est **synthétisée** à partir de
  champs physiques (LCL, EL, courant ascendant, cisaillement, vent d'altitude, front de rafales…).
- Déterminisme de `Core/` intact (la grille voxel est cosmétique, côté jeu).
- Sources ASCII, unity build (namespaces uniques), matériaux générés par `aeros_setup.py`.

## Approches considérées

| | Approche | Pour | Contre |
|---|---|---|---|
| A | Formes analytiques par orage dans le shader | rapide, fluide | orages identiques, reste du ciel inchangé |
| **B (choisie)** | **Grille 3D de densité remplie en C++ près de la caméra** | anatomie par orage, données = simulation, shader léger | travail moyen, mémoire (≈ 6-12 Mo) |
| C | Solveur fluide 3D GPU type CM1 | réalisme maximal | semaines de travail, lourd |

## Conception

### 1. Grille voxel (côté jeu, `UAerosCloudManager` + nouveau `FAerosCloudVolumeBuilder`)

- 256 × 256 × 48 voxels, **80 × 80 km × 16 km** autour de la caméra (≈ 312 m × 312 m × 333 m).
  Recentrée par pas de 10 km quand la caméra bouge (pas de glissement).
- Format RG8 : R = densité grande échelle, G = « noirceur » (précipitations / épaisseur).
- Reconstruite ~1 fois par seconde sur un **thread de travail** (ParallelFor par tranche), puis envoyée
  au GPU (`RHIUpdateTexture3D`). **Double tampon** : deux textures, le matériau fond de l'ancienne à la
  nouvelle → aucun saut.
- Champs lus : snapshot (cellules interpolées bilinéairement), liste des orages, carte « forme »
  (enclume, front de rafales), vent synoptique 850/500 hPa.

### 2. Anatomie synthétisée (règles physiques)

Pour chaque orage proche (`FAerosStormData`) :
- **Tour** : centrée sur le courant ascendant, base au LCL, sommet à l'EL (`CloudTop`), rayon 2-6 km,
  **penchée** dans le sens du cisaillement (décalage croissant avec l'altitude).
- **Sommet qui dépasse** : dôme au-dessus de l'EL si le courant ascendant est fort (> 30 m/s).
- **Enclume** : nappe de 1,5-2,5 km d'épaisseur sous l'EL, étalée sous le vent (carte forme R), bords
  fins et effilochés ; **mammatus** (lobes pendants) sous l'enclume près du cœur.
- **Cœur de précipitations** (flanc avant, autour de `HailCoreLocation`) : masse épaisse et sombre.
- **Nuage-mur** : disque abaissé sous le courant ascendant si mésocyclone > 0,4 (la tornade en sort).
- **Nuage en plateau** : coin en strates sur le front de rafales (carte forme B).
- **Ligne de flanc** : 3-5 tours de plus en plus basses, en arrière à droite de la tour principale.
- **Couches stratiformes** : nappe base-sommet là où la couverture est forte sans convection.

Les **cumulus** de beau temps restent procéduraux dans le shader (cellules de 1,3 km, plus fines que
les voxels), bornés par base/sommet de la carte 2D.

### 3. Matériau (`M_AerosVolumetricCloud`, réécrit)

- Densité = max(voxels érodés par le bruit 3D du moteur, cumulus cellulaires).
- Au-delà de la grille (40 km) : repli sur le rendu 2D actuel, fondu sur 5 km.
- **Éclairage** : sortie « Volumetric Advanced » (phase double lobe avant/arrière, diffusion multiple)
  pour le liseré argenté et la douceur des bords ; cœurs plus absorbants.
- Coût cible : 1 lecture 3D voxel + 2 bruits + carte 2D par échantillon (léger).

### 4. Test et réglage

- Nouvelle étape de visite « supercellule » (vue de côté à ~40 km, et de dessous).
- `-AerosDumpMaps` étendu : coupes de la grille (horizontales à 2 et 10 km, verticale à travers
  l'orage le plus proche) en PNG pour vérifier l'anatomie sans deviner.
- La visite attend les shaders (déjà en place).

## Décisions laissées au constructeur (mes choix, à corriger si besoin)

- Taille exacte de la grille (256²×48) et fréquence (1 Hz) : à ajuster selon le coût mesuré.
- Constantes de forme (rayons, épaisseurs, inclinaison) : réglées à l'œil avec les coupes et captures.
- Ouragans et feux pyrocumulus : hors de cette itération.
