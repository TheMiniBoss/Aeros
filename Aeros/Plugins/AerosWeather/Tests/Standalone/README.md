# Tests hors moteur

Compile le cœur de simulation (`Source/AerosWeather/Private/Core`) avec un compilateur C++20 normal, sans Unreal. Le dossier `shim/` remplace les quelques en-têtes Unreal utilisés (TArray, FMath, FVector, ParallelFor séquentiel...). Rien ici n'est compilé par Unreal Build Tool.

| Fichier | Rôle |
|---|---|
| `run_automation_tests.cpp` | lance les **mêmes tests** que l'éditeur (`Private/Tests/AerosSimulationTests.cpp`) : thermodynamique, supercellule, déterminisme, crue, tsunami, haboob, cendres, blizzard |
| `scenarios.cpp` | longues simulations des scénarios météo, résumé heure par heure (réglages) |
| `hazards.cpp` | longues simulations des catastrophes (crue, tsunami, sable, volcan, blizzard, onde de tempête) |

## Lancer

Linux, macOS, WSL ou Git Bash (g++ ou clang++) :

```bash
./build.sh
```

Windows avec Visual Studio (dans « x64 Native Tools Command Prompt ») :

```bat
build.bat
```

Puis par exemple `bin/scenarios supercell` ou `bin/hazards tsunami`. Sans argument, tous les cas tournent (plusieurs minutes).

Le code de retour de `run_automation_tests` vaut 0 si tout passe : utilisable en CI.

> `build.bat` (MSVC) n'a pas été testé ; `build.sh` est testé avec g++ et clang++.
