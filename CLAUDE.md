# CLAUDE.md — à lire au début de chaque session

## Style de réponse

- **Toujours répondre en mode caveman** : lance le skill `/caveman` (skill `caveman:caveman`) au début de la session et garde ce style jusqu'à ce que l'utilisateur dise « stop caveman ».
- Répondre en **français** (l'utilisateur écrit en français).
- Le code, les commentaires de code, les noms et les messages de commit restent en anglais.

## Le projet

**Aeros** = plugin Unreal Engine **5.8** de météo et de catastrophes naturelles réalistes et optimisées.

- Cahier des charges / roadmap d'origine : `Docs/Aeros/Cahier des charges - Roadmap.md`
- Documentation complète du plugin (architecture, physique, touches, API) : `Aeros/Plugins/AerosWeather/README.md` → **à lire avant de toucher au code**
- Branche de travail : `claude/fervent-galileo-okll3m` (ne pas pousser ailleurs sans demander, ne pas créer de PR sans demande)

## Arborescence utile

```
Aeros/Aeros.uproject                      projet hôte (EngineAssociation 5.8)
Aeros/Source/Aeros/                       module jeu minimal (le code météo n'est plus ici)
Aeros/Plugins/AerosWeather/
  AerosWeather.uplugin
  Source/AerosWeather/                    module runtime
    Public/Core, Private/Core             cœur de simulation C++ pur (sans UObject) : AerosSimulation, AerosSnapshot, AerosPhysics
    Public, Private                       couche Unreal : subsystem, thread, managers, acteurs, composants
    Private/Tests/AerosSimulationTests.cpp  tests automation (éditeur)
  Source/AerosWeatherDemo/                module démo : game mode, pawn (AZERTY), HUD canvas
  Content/Python/aeros_setup.py           crée MPC, matériaux (nuages, sol, eau, éclair, rideau de pluie) et la carte démo
  Tests/Standalone/                       tests hors moteur (shim des en-têtes UE) : build.sh / build.bat
```

## État actuel (à vérifier / à faire en priorité)

1. **Le plugin compile dans UE 5.8** (Build.bat, éditeur fermé). Commande :
   `"E:\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" AerosEditor Win64 Development "-Project=<repo>\Aeros\Aeros.uproject" -WaitMutex`
   (depuis PowerShell). Si LNK1104 : éditeur encore ouvert, ou supprimer `Plugins/AerosWeather/Intermediate` et `Binaries`.
2. Le cœur de simulation est testé hors moteur : 21 tests (`Tests/Standalone/build.sh`). Les relancer après toute modification de `Private/Core` ou `Public/Core`.
3. **Assets sans éditeur** : `UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<repo>/Aeros/Plugins/AerosWeather/Content/Python/aeros_setup.py -unattended -nosplash -nullrhi`, puis lire `[Aeros] SUMMARY` dans `Aeros/Saved/Logs/Aeros.log` (8 étapes OK attendues).
4. **Boucle de test visuelle automatique** : `UnrealEditor.exe <uproject> -game -windowed -ResX=1600 -ResY=900 -AerosTourQuit -log=AerosTour.log` (lancer avec PowerShell `Start-Process`, pas Git Bash). La visite guidée (12 étapes, dont « supercellule » ; `-AerosTourOnly=1,2,3` pour n'en jouer que certaines ; l'étape 1 fait naître les orages) enregistre une capture par étape dans `Aeros/Saved/Screenshots/WindowsEditor/AerosTour/` puis quitte. Lire les PNG pour juger le rendu ; les lignes `Aeros tour:` / `Aeros demo:` du log disent ce qu'il y a sous la caméra.
5. La météo réelle (Open-Meteo, `UAerosLiveWeather`) n'a jamais été testée.
6. **Unreal MCP** : plugins activés, `.mcp.json` à la racine (http://127.0.0.1:8000/mcp), démarrage auto dans `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`. Le serveur n'a pas encore été vu en marche : si `unreal-mcp` n'apparaît pas, taper `ModelContextProtocol.StartServer` dans la console de l'éditeur puis recharger la session Claude.
7. **Nuages, rendu maison** (branche `claude/clouds-v2`, partie de `claude/voxel-clouds`) : module `AerosCloudRender` (extension de vue, passe avant DOF), `r.AerosClouds 1` par défaut (0 = ancien `VolumetricCloud`). Spec `docs/superpowers/specs/2026-11-15-custom-cloud-renderer-design.md`, plan `docs/superpowers/plans/2026-11-15-custom-cloud-renderer.md`, détails et variables dans le README (section « Rendu des nuages »). Scintillement mesuré à x1 : 0,2-0,5 ; coût 2,8-3,5 ms à `Quality 2`. Corrigé (branche `claude/storm-chaser`) : dessus d'enclume en courbes de niveau vu d'avion (entrée dans le nuage biaisée après un pas double, voir règles), tornade qui ne touchait pas le sol (`Aeros::Funnel::TouchdownK`), tornade laissée sous un ciel clair à x60 (elle suit maintenant son mésocyclone). Encore faible : détail qui glisse en time-lapse x60, lueur d'éclair la nuit à juger, faces à l'ombre bleutées (lumière du ciel, plausible), coût monté à 4,5-7 ms (vue d'une supercellule à 25 km, contre 3,5-4,1 ms avant le correctif). L'historique ci-dessous concerne l'ancien chemin.
   **Tornades volumétriques** (branche `claude/volumetric-tornadoes`, partie de `claude/clouds-sharp`) : entonnoir physique dans le rendu maison (`Core/AerosFunnel.h`, spec `docs/superpowers/specs/2026-11-15-volumetric-tornadoes-design.md`), nuage-mur, poussière ; tube en maillage seulement avec `r.AerosClouds 0`. Pas encore vu : un orage à base haute (entonnoir suspendu), les embruns de près.
   **Ancien chemin** : refonte voxel sur la branche `claude/voxel-clouds` (plan `docs/superpowers/plans/2026-11-13-voxel-clouds.md`, ledger `.superpowers/sdd/2026-11-13-voxel-clouds/progress.md`). Fait : grille 3D + anatomie d'orage + matériau, puis réglage du rendu (cumulus Nubis en tourelles, bruits normalisés, tracé demi-résolution). Coût GPU mesuré (`stat unit`, 1336×751) : ≈ 12 ms vu du sol, 17-20 ms au-dessus d'un champ de cumulus (`CloudSampleScale` = 1,25 ; 2 donne 26 ms). Puis enclumes : plus de marches entre enclumes qui se recouvrent (test `Overlapping anvils`), mammatus irréguliers, glace lisse et fibreuse au-dessus de 9-11,5 km, grille élargie à 100 km (orages en 3D jusqu'à ≈ 45 km), dérive du bruit plafonnée à x60 (grain à x600 nettement réduit). Puis relief des enclumes (houle verticale, ombre plus fine, LOD des bruits, enclumes qui divergent / se courbent) et rideaux de pluie masqués quand la caméra est au-dessus de la base. Cadrage de test : `-AerosSupercellCam=Dist,Alt,LookAlt,LookAlong` (km) sur l'étape 3, par ex. `22,16,10,20` pour voir une enclume d'avion. Puis scintillement : mode 0, bruits lissés (mip ≥ 2-3, warp par bruits lisses), cœur plein, plus de miettes, moins de diffusion multiple (faces à l'ombre plus sombres), champ lointain fondu ; outils de mesure `-AerosTourTimeScale=N`, `-AerosFlickerShots`, `-AerosCloudMode=N`. Encore faible : bords « salés » (pixels du tracé au quart de résolution) sur les images fixes, du sol les cumulus proches cachent encore les tours lointaines (l'étape « supercellule » filme depuis 4 km), pas d'ombre franche côté opposé au soleil. Tornades : tube procédural fait (étape 3), galerie de test : `-AerosTourOnly=1,5 -AerosTornadoShape=k -AerosTornadoAge=0.35`. Visuel encore faible / à faire : nuage-mur en disque (devrait venir des nuages volumétriques), pluie de secours (rectangles sombres), vague de tsunami peu visible, effet « Séisme » qui dure longtemps après un M9 (répliques).
8. **Paysage procédural** (démo) : `AAerosDemoTerrain` (9 anneaux LOD de maillages procéduraux, 25 m → 6,4 km, recentrés sur la caméra, reconstruits sur un thread de travail, collisions WorldStatic) + `AerosDemoTerrainModel.h` (hauteur déterministe, C++ pur : plaines au départ, collines, lacs sous le niveau de la mer, chaînes jusqu'à ≈ 4 km dès 20 km, côte avec baies, dunes) + mer opaque `M_AerosSea`. La simulation lit ce relief par ses tracés (`UpdateTerrainSampling`) : relief et `TerrainHeight` alignés. Aperçu hors moteur : `Tests/Standalone/terrain_preview.cpp` (carte PNG). Le game mode remplace le plan plat de l'ancienne carte. Pas encore vu : lacs et plages de près, mer de près avec ses vaguelettes.

9. **Démo jouable « Chasseur d'orages »** (branche `claude/storm-chaser`, partie de `claude/volumetric-tornadoes`) : `AAerosChasePawn` (véhicule, états menu / briefing / conduite / détruit / pause / résumé), `AAerosChaseHUD`, règles pures `AerosChaseRules.h` (13 tests hors moteur). **Play ouvre maintenant le menu** ; la démo technique (`AAerosDemoPawn` + `AAerosDemoHUD`) s'ouvre par le menu, `-AerosTech` ou toute option `-AerosTour…` (la visite marche comme avant). Test auto : `-AerosChaseAuto -AerosChaseShots=10 -AerosChaseEndAfter=120 -AerosChaseQuitAfter=200`, captures dans `Saved/Screenshots/WindowsEditor/AerosChase/`. Pas encore vu : partie jouée à la main (souris, zoom, T, pause, Retour arrière vers la démo technique et retour), réglage des points sur une vraie partie.

## Règles apprises (erreurs à ne pas refaire)

- Démo jouable : **pas de touches de fonction** (F1-F12, réservées au débogage du moteur), et l'interface de la démo technique reste à la démo technique.
- Photo sans interface : `RequestScreenshot(File, false, ...)` garde quand même le HUD canvas et le flash d'obturateur. Cacher le HUD (`PhotoHideFrames`) 2-3 images, capturer, **puis** lancer le flash.
- À x600 la simulation ne suit pas (≈ x40-100 réel) : ne jamais attendre une heure exacte sans porte de sortie (briefing de ~1 min) ; tester `Hour >= H - 0.02` et proposer de passer.
- `IssueCommand` est **ignoré tant que le thread météo n'existe pas** (`if (!SimThread) return`) : une commande lancée dans un `BeginPlay` (ex. `-AerosChase`) disparaît sans message. L'envoyer depuis le Tick et vérifier l'effet (ex. l'heure) avant de continuer.
- Rendu maison, marche du rayon : dans l'air vide le pas double, mais le jitter de départ ne couvre qu'un pas simple. L'entrée dans un nuage tombait toujours dans la même moitié du pas double : des **bandes fixes** sur les dessus plats vus en rasant (enclumes vues d'avion, « courbes de niveau »). À l'entrée après un pas double, reculer et rentrer au pas normal avec jitter (`LastEmptyStride`). Les rayures ne venaient ni des couches de la grille ni des fibres de glace (testé).
- Tornade : avec K fixe = 2, l'entonnoir d'une EF2-EF3 sous une base de 1-1,5 km s'arrêtait 600-900 m au-dessus du sol (rien vu même à 400 m). K par tornade = `TouchdownK` (1,3 x le contact à maturité).
- Les rampes verticales dans la grille voxel doivent couvrir **plus de 2 couches** (333 m chacune) : une rampe de 300 m mettait le dessus de l'enclume dans une couche ou la suivante (test `Anvil top fades over >= 0.5 km`).
- Chasse, briefing : avec un seul orage la simulation suit **vraiment** x600 (15 h 15 -> 16 h en 4 s) : ne pas démarrer la journée à une heure fixe, attendre une supercellule mûre. Ne jamais accepter l'heure courante comme preuve que `SetDateTime` a pris (depuis le menu l'horloge était déjà vers 16 h) : comparer à l'heure demandée. Placer la bulle sur la plaine basse (les montagnes commencent à ~20 km à l'ouest : la base du nuage passait sous le relief) et le joueur à 20 km au sec (la pluie d'une supercellule s'étend à plus de 20 km).
- Un seul orage voulu : `SetConvectionControl(0, true)` (coupe la naissance spontanée, efface les orages en cours), puis `InjectThermalBubble`. La bulle force `Conv` elle-même, elle ne dépend pas de la naissance spontanée. Les tornades déjà là sont des acteurs : les détruire à part.
- Chasse : un véhicule ne rattrape un orage que si l'échelle de temps reste petite (orage 15 m/s × échelle à l'écran). À x20 ils filaient à 300 m/s. Les essais de tornade sont en temps **simulé** (`TornadoCheckInterval` = 600 s) : à petite échelle il n'y en a presque plus, la chasse le baisse à 60 s pendant la journée.

- **Sources 100 % ASCII** (MSVC) : pas d'accent dans les `.h/.cpp`. Dans les chaînes, écrire \u00E9 (é), \u00E0 (à)... dans `TEXT("...")`. Attention : les outils Write/Edit décodent `\uXXXX` en vrai caractère. Écrire le texte avec accents puis convertir tous les non-ASCII en `\uXXXX` par un petit script Python (`chr(92)+'u%04X'`).
- `FString::ToUpper` ne met pas les accents en majuscule (« DéGAGé ») : ne pas l'utiliser sur du texte français.
- Git Bash convertit les arguments qui commencent par `/` en chemins Windows (`/AerosWeather/Maps/...` devient `C:/Program Files/Git/...`) : lancer Unreal depuis PowerShell.
- Un tracé « sol » avec `LineTraceSingleByChannel(ECC_WorldStatic)` touche aussi les pawns (profil BlockAll), et le pawn existe déjà dans `StartPlay` en 5.8. Utiliser `LineTraceSingleByObjectType(FCollisionObjectQueryParams(ECC_WorldStatic))`.
- Matériau sur un `UInstancedStaticMeshComponent` : cocher `used_with_instanced_static_meshes` dans le script, sinon matériau par défaut en jeu.
- Paramètre vecteur dans un matériau : la sortie par défaut est RGB (3 canaux). Pour lire B/A, prendre la sortie `RGBA`, sinon erreur « Not enough components » et matériau par défaut (c'était la cause du ciel vide).
- Le script Python en mode `-run=pythonscript` doit scanner le registre d'assets (`scan_paths_synchronous`) avant de tester si un asset existe.
- Les effets « unlit » (émissifs) brillent la nuit : tout volume qui doit s'assombrir la nuit (pluie, entonnoir) doit être **lit**. Un flash global sur tout le matériau nuages blanchit le ciel la nuit : la lueur doit être locale (`AerosLightningPos`).
- `FParse::Value` s'arrête aux virgules par défaut : passer `bShouldStopOnSeparator = false` pour lire une liste.
- Pour fermer l'éditeur proprement : `save_assets([])` via MCP (AssetTools), puis `CloseMainWindow()` sur le process. Le MCP n'expose pas Live Coding : compiler éditeur fermé.
- Les bruits 3D du moteur (`T_VolumeNoiseShape128`, `T_VolumeNoiseErosion32`) n'ont **qu'un canal** (R = G = B, A = 1) dans une plage étroite (forme : 80 % entre 0,83 et 0,94 ; érosion : 0,57-0,82). Lire `.r` et l'étirer (`saturate((v - 0.78) / 0.18)`, `saturate((v - 0.45) / 0.45)`) ; G/B/A ne sont pas des octaves et `N.gb - 0.5` est un décalage constant. Mesuré en exportant `source2d_texture` (AssetExportTask TGA). Placer les nuages avec le champ cellulaire, le bruit sert à creuser (remap Nubis) et éroder.
- Nuages volumétriques : **garder `r.VolumetricRenderTarget.Mode 0`** (quart de résolution + reconstruction temporelle). Le mode 1 (demi-résolution, sans reconstruction) scintille fort même à x1 (différence image à image 4,5 contre 1,2) ; le mode 2 est stable mais pixelisé. Mesurer avec `-AerosTourTimeScale=1 -AerosFlickerShots` (deux captures à 0,2 s, script de différence). Pas de détail de bruit sous ~200 m (les pas du rayon font 100-200 m : ça scintille), pas de bruit fin pour piloter une déformation (domain warp), pas de miettes hors du contour (`smoothstep` sur la couverture), cœur plein. Au-delà de 30-70 km les petits cumulus deviennent des carrés de pixels : fondre le champ en couverture douce. Le grain à x600 venait de la dérive trop rapide (plafonnée à x60).
- Les maillages translucides (rideau de pluie, etc.) sont dessinés **par-dessus** les nuages volumétriques (pas de test de profondeur avec eux) : un rideau sous un orage se voyait à travers l'orage depuis le dessus. Les masquer quand la caméra est au-dessus de la base du nuage. Pareil avec le rendu maison (`r.AerosClouds 1`, passe avant DOF) : l'éclair se voyait à travers l'enclume. Il s'estompe quand la caméra est à plus de 1 km au-dessus de la base du canal **et** à moins de 15-25 km (de côté et de loin, il reste visible sous l'orage).
- `TextureSample` en mode `TMVM_MIP_LEVEL` : l'entrée s'appelle `Level` pour `connect_material_expressions` (lister avec `get_material_expression_input_names`).
- Ombre des nuages : peu d'échantillons sur une longue distance (25 km / 10) = pas de 2,5 km, plus aucune ombre interne (enclume blanche plate). Garder ≈ 15 km et augmenter `ShadowViewSampleCountScale`.
- Les scripts Python qui écrivent dans le repo : les lancer depuis PowerShell (`python fichier.py`) ; le python de Git Bash échoue à l'écriture (`OSError 22`).
- Rendu maison : l'irradiance SH du ciel (`View.SkyIrradianceEnvironmentMap`) est **normalisée** ; la multiplier par `View.SkyLightColor` (sinon le ciel est ~100x trop faible et les faces à l'ombre sont noires).
- Rendu maison : les mips moyennés d'un bruit normalisé perdent leur contraste ; un bruit qui pilote une déformation, lu au mip 3-5, ne déforme plus rien (tours en boîtes). Lire au mip 2,5-3,5 et re-étirer (x1,6).
- `UAerosCloudManager` : `MotionCmS` est par seconde **simulée** ; tout ce qui utilise le temps réel (reprojection) doit multiplier par l'échelle de temps.
- RDG : `QueueTextureExtraction` garde un pointeur vers la cible ; si elle vit dans une `TMap` qui peut se réorganiser dans le même graphe (2e vue), la tenir par `TUniquePtr`.
- Visite : `stat gpu` ne s'affiche pas dans les captures ; utiliser `-AerosTourExec=profilegpu` (arbre GPU dans le log). Le moment des éclairs varie d'un lancement à l'autre : pas de comparaison « même éclair » entre deux runs ; `r.AerosClouds.Flash=-1` coupe la lueur des nuages pour mesurer le scintillement.
- PowerShell : `& script.ps1 | Select-Object -First N` **arrête** le script après N lignes (la suite, copie du log comprise, n'est pas faite). Ne pas tronquer la sortie d'un script qui travaille encore.
- Tornades : au-dessus de la mer, l'acteur `AAerosTornado` se pose sur le **fond marin** (la mer n'a pas de collision) ; son tube en maillage était sous l'eau. Le rendu cale l'entonnoir au niveau de la mer (`SeaLevelZ`).
- Tornades : la base de l'entonnoir doit être celle que dessine la grille voxel (LCL de l'orage borné à 0,3-3 km, orage parent sinon le plus proche). Le LCL local sous la pluie est presque nul, et forcer une autre base met l'entonnoir dans le nuage ou sous une base invisible.
- Tornades : tout ce qui entoure l'entonnoir (poussière, embruns, nuage-mur) doit rester léger et bas, surtout sous une base basse (300 m) : sinon il cache l'entonnoir.
- Visite à x300 : l'orage avance en temps simulé, la tornade en temps réel : elle se retrouve vite sous un ciel dégagé. Filmer une vie de tornade à x1 en deux morceaux (`-AerosTornadoAge=0` puis `0.85`).
- Shader : une variable lue après une boucle doit être déclarée hors du bloc `if` qui contient la boucle (erreur de compilation au lancement, le jeu affiche « Réessayer la compilation ? »).
- Voir une tour d'orage depuis le sol : les cumulus entre la caméra et la tour la cachent ; et la grille voxel (centrée caméra) s'estompe dès 35 km. Filmer à ≤ 25 km, au-dessus des sommets des cumulus (≈ 4 km).
- `VolumetricCloud` ne trace que 50 km par défaut : `SetTracingMaxDistance(180)`.
- Après un changement de matériau, le premier lancement compile les shaders : la visite attend (`GShaderCompilingManager`), sinon les captures montrent les matériaux par défaut.
- Lire la simulation en **bilinéaire** pour tout ce qui est rendu en 3D : la case la plus proche dessine des blocs de 2 km.
- Une grille voxel lisse donne des « colonnes » : déformer la lecture (domain warp ±1 km par le bruit) avant d'éroder.
- Durées de vie des acteurs (tornades, etc.) en secondes réelles : les faire vieillir avec `TimeScale / 60`, sinon ils s'accumulent quand le temps accélère.
- Pas de variable locale qui porte le nom d'une constante de `Aeros::Phys` (`Cp`, `G`, `Lv`, `Rd`, `K0`...) : `AerosSimulation.cpp` fait `using namespace Aeros::Phys` et UE traite le masquage (C4459/C4456/C4458) comme une **erreur**.
- **Unity build** : UE colle plusieurs `.cpp` ensemble. Un nom dans un namespace anonyme ou un `using namespace` au niveau fichier « fuit » dans les autres fichiers et masque leurs variables locales (erreur C4459). Mettre les helpers dans un namespace au nom unique (`AerosXxxInternal`) et les `using namespace` **dans les fonctions**.
- Une lambda ne capture pas une variable `static` (ex. `ConstructorHelpers::FObjectFinder`) : erreur C3495. L'utiliser sans capture.
- `FRandomStream::FRandRange` renvoie un `double` : dans une init avec accolades `{ ... }` vers un `float`, caster avec `float(...)` (erreur C2397).
- Un `UFUNCTION` ne renvoie pas de `const&` : renvoyer par valeur.
- Le destructeur de `UAerosWeatherSubsystem` est défini dans le `.cpp` (TUniquePtr d'un type incomplet).
- Assets optionnels : charger avec `Aeros::LoadOptional` (`AerosAssetUtils.h`) pour éviter les warnings « failed to find ».
- Le cœur est **déterministe** (multijoueur : même seed + mêmes commandes datées = même météo). Hasard uniquement via `Aeros::Phys::Hash01`, jamais `FMath::FRand` dans `Core/`.
- Toute nouvelle commande de simulation passe par `EAerosNetCommand` + `UAerosWeatherSubsystem::IssueCommand` (réplication).
- Paysage : le masque « sable » ne doit couvrir que le premier mètre au-dessus de l'eau. La plaine du départ est à 1,6-2,8 m au-dessus de la mer : une bande de 3,5 m la mettait tout entière en plage.
- `UMaterialExpressionVertexColor` n'a pas de sortie `RGBA` : multiplier un canal à la fois (`R`, `G`, `B`, `A`).

## Conventions

- Repère simulation : x = est, y = nord (m). Unreal : X = est, **−Y = nord** (cm). Les API publiques renvoient des vecteurs monde Unreal en m/s.
- Une cellule = `CellSizeKm` (2 km par défaut). Les profondeurs de crue sont des **moyennes de cellule**.
- La démo est pensée pour un **clavier AZERTY** (ZQSD, rangée `& é " ' ( - è _ ç à` pour les scénarios). Garder ça.
- Messages de commit en anglais, finir par les lignes d'attribution demandées par l'environnement.

## Phrase à donner au début d'une nouvelle session

> Lis `CLAUDE.md` à la racine du repo, puis `Aeros/Plugins/AerosWeather/README.md`. Active `/caveman`. On travaille sur la branche `claude/fervent-galileo-okll3m`.
