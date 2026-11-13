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
4. **Boucle de test visuelle automatique** : `UnrealEditor.exe <uproject> -game -windowed -ResX=1600 -ResY=900 -AerosTourQuit -log=AerosTour.log` (lancer avec PowerShell `Start-Process`, pas Git Bash). La visite guidée (11 étapes, `-AerosTourOnly=1,2,3` pour n'en jouer que certaines ; l'étape 1 fait naître les orages) enregistre une capture par étape dans `Aeros/Saved/Screenshots/WindowsEditor/AerosTour/` puis quitte. Lire les PNG pour juger le rendu ; les lignes `Aeros tour:` / `Aeros demo:` du log disent ce qu'il y a sous la caméra.
5. La météo réelle (Open-Meteo, `UAerosLiveWeather`) n'a jamais été testée.
6. **Unreal MCP** : plugins activés, `.mcp.json` à la racine (http://127.0.0.1:8000/mcp), démarrage auto dans `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini`. Le serveur n'a pas encore été vu en marche : si `unreal-mcp` n'apparaît pas, taper `ModelContextProtocol.StartServer` dans la console de l'éditeur puis recharger la session Claude.
7. Visuel encore faible / à faire : tornades (étape 3 : formes rope/cône/wedge, déformation, cycle de vie), vague de tsunami peu visible, sol plat qui reflète le ciel en vue rasante (pas de landscape), effet « Séisme » qui dure longtemps après un M9 (répliques).

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
- Le bruit Perlin-Worley du moteur (`T_VolumeNoiseShape128`) a surtout des valeurs hautes : un seuil « 1 - couverture » dessus donne un ciel couvert. Placer les nuages avec le champ cellulaire du nœud HLSL et garder le bruit pour l'érosion.
- `VolumetricCloud` ne trace que 50 km par défaut : `SetTracingMaxDistance(180)`.
- Après un changement de matériau, le premier lancement compile les shaders : la visite attend (`GShaderCompilingManager`), sinon les captures montrent les matériaux par défaut.
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
