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
  Content/Python/aeros_setup.py           crée MPC, matériaux (nuages, sol, eau) et la carte démo dans l'éditeur
  Tests/Standalone/                       tests hors moteur (shim des en-têtes UE) : build.sh / build.bat
```

## État actuel (à vérifier / à faire en priorité)

1. **Le plugin n'a jamais été compilé entièrement dans Unreal 5.8.** Une seule erreur a été remontée et corrigée (C4459). Sur le PC de l'utilisateur : compiler en premier, corriger les erreurs, recommencer.
   - Commande type : `"<UE_5.8>\Engine\Build\BatchFiles\Build.bat" AerosEditor Win64 Development -Project="<chemin>\Aeros\Aeros.uproject" -WaitMutex`
2. Le cœur de simulation est compilé et testé hors moteur : 21 tests passent (`Tests/Standalone/build.sh`). Les relancer après toute modification de `Private/Core` ou `Public/Core`.
3. Le script Python (`aeros_setup.py`) n'a pas été vérifié de bout en bout dans l'éditeur : regarder le bloc `[Aeros] SUMMARY` dans l'Output Log.
4. La météo réelle (Open-Meteo, `UAerosLiveWeather`) n'a jamais été testée : le réseau du cloud bloquait l'API.
5. Si le plugin **Unreal MCP** est connecté (éditeur ouvert), l'utiliser pour compiler, lancer la démo en PIE, lire les logs et créer les assets.

## Règles apprises (erreurs à ne pas refaire)

- **Sources 100 % ASCII** (MSVC) : pas d'accent dans les `.h/.cpp`. Dans les chaînes, utiliser `é` etc. dans `TEXT("...")`.
- Pas de variable locale qui porte le nom d'une constante de `Aeros::Phys` (`Cp`, `G`, `Lv`, `Rd`, `K0`...) : `AerosSimulation.cpp` fait `using namespace Aeros::Phys` et UE traite le masquage (C4459/C4456/C4458) comme une **erreur**.
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
