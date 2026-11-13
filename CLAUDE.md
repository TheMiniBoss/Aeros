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
7. **Nuages** : refonte voxel sur la branche `claude/voxel-clouds` (plan `docs/superpowers/plans/2026-11-13-voxel-clouds.md`, ledger `.superpowers/sdd/2026-11-13-voxel-clouds/progress.md`). Fait : grille 3D + anatomie d'orage + matériau, puis réglage du rendu (cumulus Nubis en tourelles, bruits normalisés, tracé demi-résolution). Coût GPU mesuré (`stat unit`, 1336×751) : ≈ 12 ms vu du sol, 17-20 ms au-dessus d'un champ de cumulus (`CloudSampleScale` = 1,25 ; 2 donne 26 ms). Puis enclumes : plus de marches entre enclumes qui se recouvrent (test `Overlapping anvils`), mammatus irréguliers, glace lisse et fibreuse au-dessus de 9-11,5 km, grille élargie à 100 km (orages en 3D jusqu'à ≈ 45 km), dérive du bruit plafonnée à x60 (grain à x600 nettement réduit). Encore faible : le dessus des enclumes est un blanc uniforme (peu de relief), du sol les cumulus proches cachent encore les tours lointaines (l'étape « supercellule » filme depuis 4 km), pas d'ombre franche côté opposé au soleil. Tornades : tube procédural fait (étape 3), galerie de test : `-AerosTourOnly=1,5 -AerosTornadoShape=k -AerosTornadoAge=0.35`. Visuel encore faible / à faire : nuage-mur en disque (devrait venir des nuages volumétriques), pluie de secours (rectangles sombres), vague de tsunami peu visible, sol plat qui reflète le ciel en vue rasante (pas de landscape), effet « Séisme » qui dure longtemps après un M9 (répliques).

## Règles apprises (erreurs à ne pas refaire)

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
- Nuages volumétriques : le mode par défaut (`r.VolumetricRenderTarget.Mode 0`, quart de résolution + temporel) donne du grain quand le temps accéléré fait bouger les nuages vite ; le contrôleur passe en mode 1 (`bSharpCloudTracing`). Les détails de bruit < 50 m scintillent à quelques km : lire les bruits de forme au mip 1, érosion à 4 km par tuile.
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

## Conventions

- Repère simulation : x = est, y = nord (m). Unreal : X = est, **−Y = nord** (cm). Les API publiques renvoient des vecteurs monde Unreal en m/s.
- Une cellule = `CellSizeKm` (2 km par défaut). Les profondeurs de crue sont des **moyennes de cellule**.
- La démo est pensée pour un **clavier AZERTY** (ZQSD, rangée `& é " ' ( - è _ ç à` pour les scénarios). Garder ça.
- Messages de commit en anglais, finir par les lignes d'attribution demandées par l'environnement.

## Phrase à donner au début d'une nouvelle session

> Lis `CLAUDE.md` à la racine du repo, puis `Aeros/Plugins/AerosWeather/README.md`. Active `/caveman`. On travaille sur la branche `claude/fervent-galileo-okll3m`.
