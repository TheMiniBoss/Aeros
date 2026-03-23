# 🌍 VISION GLOBALE : PROJET "AEROS"

**Aeros** n'est pas un système d'effets visuels scriptés. C'est un **moteur thermodynamique** tournant en arrière-plan. L'état de l'atmosphère (température, humidité, pression) génère des flux (vent), qui transportent l'énergie et l'eau. Ces variables atteignent des seuils critiques (CAPE - _Convective Available Potential Energy_), déclenchant la formation procédurale de nuages, de précipitations et d'événements extrêmes. Les visuels (Niagara, Volumetric Clouds) et les impacts physiques ne sont que l'interprétation de cette matrice de données.

---

# 🏗️ ARCHITECTURE LOGICIELLE ET TECHNIQUE

Pour garantir la scalabilité temporelle et spatiale, l'architecture repose sur le pattern **Subsystem** d'Unreal, couplé à une séparation stricte entre **Simulation (Données/CPU/GPU)** et **Représentation (Acteurs/VFX)**.

## 1. Diagramme de l'Architecture

Plaintext

```
[UWorldSubsystem: UWeatherSubsystem]  <-- Cerveau principal (Manager)
   |
   +-- [FAtmosphereGrid] <-- Structure de Données (Grille 2D/3D Macro)
   |      |-- FWeatherCell (Temp, Humidité, Pression, Vent, Updraft)
   |      |-- Mise à jour asynchrone (FRunnable / TaskGraph / Compute Shader)
   |
   +-- [UCloudManager] <-- Interprète la grille pour les Volumetric Clouds
   |      |-- Génère les Textures 3D / Render Targets pour le Material Nuage
   |
   +-- [UStormManager] <-- Détecte les anomalies (Cellules convectives)
          |-- Spawn: AStormSystemActor (Supercellule, Tornade, Ouragan)
          |-- Spawn: APrecipitationVolume (Pluie, Neige, Grêle locale)
```

## 2. La Grille Atmosphérique (Simulation)

Plutôt qu'une grille 3D complète (trop lourde), nous utiliserons une **Grille 2.5D** : une grille 2D haute résolution pour la surface, avec des profils verticaux (vecteurs de cisaillement, gradients de température) stockés dans chaque cellule.

- **Variables par cellule :** Pression ($P$), Température ($T$), Humidité ($q$), Vecteur Vent ($\vec{v}$), CAPE, Vitesse du courant ascendant, Altitude de l'isotherme 0°C.
    
- **Advection :** Déplacement des masses d'air calculé via des équations de mécanique des fluides simplifiées.
    

## 3. Rôles et Responsabilités

|**Classe C++**|**Type Unreal**|**Responsabilité**|
|---|---|---|
|`UWeatherSubsystem`|`UWorldSubsystem`|Initialise la grille, gère le "Tick météo" asynchrone, point d'accès global.|
|`UWeatherSimulationThread`|`FRunnable`|Thread dédié au calcul mathématique (Navier-Stokes, thermodynamique).|
|`FWeatherCell`|`USTRUCT`|Contient les données pures mathématiques d'un point géolocalisé.|
|`AStormSystem`|`AActor`|Représente une tempête. Gère son cycle de vie, les particules, les dégâts physiques et le réseau.|
|`AWeatherRadar`|`AActor`|Capteur scannant la grille pour écrire dans un _Render Target_ (UI).|

---

# ⛈️ IMPLÉMENTATION DES SYSTÈMES ET GAMEPLAY

## A. Événements Extrêmes et Tempêtes

Les tempêtes sont gérées par le `UStormManager` qui scanne la grille. Si le CAPE et le cisaillement du vent (Wind Shear) sont critiques, un `AStormSystem` est instancié.

- **Supercellule :** Calcul du _Storm Relative Helicity_ (SRH). Si le SRH dépasse un seuil, le nuage volumétrique entame une rotation.
    
- **Tornade :** Instanciée sous la supercellule (CAPE > 2500, fort SRH). Possède un composant de force physique (Radial Force) qui aspire les objets.
    
- **Ouragan :** Entité macroscopique imposant un flux cyclonique autour d'un œil à basse pression.
    

## B. Dynamique des Précipitations (Pluie, Neige, Grêle)

Les cellules ayant une saturation en humidité > 100% déclenchent des précipitations lues via un **Niagara Data Interface (NDI)**.

- **Pluie et Neige :** Asservies à l'isotherme 0°C.
    
- **Grêle (La Physique) :** Se forme dans de violents courants ascendants (Updrafts). La vitesse verticale ($W$) est corrélée au CAPE. Si $W$ est supérieur à la vitesse terminale de chute d'un grêlon, il gèle au-dessus de 0°C, retombe, accumule de l'eau, remonte et grossit. La taille finale ($D$) dépend de $W$.
    
- **Interaction et Gameplay (Grêle) :** La grêle nécessite des _Mesh Particles_. Elle génère du son au rebond et des dégâts de zone (système Chaos) sur les véhicules, vitres et végétation si les grêlons dépassent 30 mm.
    
- **Accumulation Environnementale :** Mise à jour d'un _Global Material Parameter Collection_ ou d'un _Render Target 2D_ :
    
    - _Eau :_ Pilote la Roughness (flaques).
        
    - _Neige :_ Pilote le World Position Offset (épaisseur).
        
    - _Grêle :_ Augmente l'Albedo (blanchiment granuleux), modifie la Normal Map, avec une fonte rapide.
        

## C. Saisons et Soleil

Calcul astronomique dans le `UWeatherSubsystem`. L'angle d'élévation solaire ($h$) modifie l'apport énergétique local ($E$) :

$$E = E_{max} \cdot \sin(h) \cdot (1 - \text{Albedo}) \cdot \text{FacteurNuageux}$$

Cela génère naturellement les variations saisonnières.

## D. Radar Météo (Gameplay & UI)

Conversion de l'intensité des précipitations d'une zone en décibels de Z (dBZ). Les cellules sont traduites en pixels sur un _Texture Render Target_, affichées via une UI Doppler classique (vert=pluie, magenta=grêle/tornade).

---

# 💻 STRUCTURE DE DONNÉES ET ALGORITHMES (C++)

## 1. La Structure FWeatherCell Unifiée

Structure alignée et optimisée en mémoire.

C++

```
#pragma once
#include "CoreMinimal.h"
#include "WeatherTypes.generated.h"

USTRUCT(BlueprintType)
struct FWeatherCell
{
    GENERATED_BODY()

    UPROPERTY() float Temperature; // Celsius
    UPROPERTY() float Humidity; // 0.0 - 1.0
    UPROPERTY() float Pressure; // hPa
    UPROPERTY() FVector2D WindVector;
    UPROPERTY() float CAPE; 
    UPROPERTY() float PrecipitationIntensity; // mm/h

    // Données thermodynamiques pour la grêle
    UPROPERTY() float UpdraftVelocity; // Vitesse verticale (m/s)
    UPROPERTY() float FreezingLevelAltitude; // Altitude du 0°C (m)
    UPROPERTY() float HailstoneDiameter; // mm (0 = pas de grêle)

    FWeatherCell() : Temperature(15.f), Humidity(0.5f), Pressure(1013.f), WindVector(FVector2D::ZeroVector), CAPE(0.f), PrecipitationIntensity(0.f), UpdraftVelocity(0.f), FreezingLevelAltitude(2500.f), HailstoneDiameter(0.f) {}
};
```

## 2. Thermodynamique : Formation de la Grêle et Tornades

**Advection du vent (équation) :**

Déplacement stable via advection semi-lagrangienne :

$$\frac{\partial q}{\partial t} + \vec{v} \cdot \nabla q = 0$$

**Algorithmes de formation (Thread Météo) :**

C++

```
// 1. Calcul de la Grêle
void UWeatherSimulationThread::ComputePrecipitationType(FWeatherCell& Cell)
{
    if (Cell.PrecipitationIntensity > 0.f && Cell.CAPE > 1000.f)
    {
        // Vitesse de l'updraft dérivée du CAPE
        Cell.UpdraftVelocity = FMath::Sqrt(2.f * Cell.CAPE) * 0.5f; 

        if (Cell.UpdraftVelocity > 15.f) // Plus de 15 m/s = risque de grêle
        {
            Cell.HailstoneDiameter = (Cell.UpdraftVelocity - 15.f) * 2.5f; 
        }
        else { Cell.HailstoneDiameter = 0.f; }
    }
}

// 2. Détection de Tornade
bool UWeatherSimulationThread::CheckTornadoFormation(const FWeatherCell& Cell, float WindShear)
{
    float SRH = CalculateSRH(WindShear); 
    if (Cell.CAPE > 2500.f && SRH > 250.f) 
    {
        return FMath::FRand() > 0.85f; // 15% de chance si conditions critiques
    }
    return false;
}
```

## 3. L'Acteur AStormSystem : Multijoueur et Dégâts Chaos

Gère la représentation d'une tempête locale. Ne réplique que le macro-état.

C++

```
// ... Setup initial, bReplicates = true, NetUpdateFrequency = 1.0f ...

void AStormSystem::OnRep_StormIntensity()
{
    if (StormVFX) { StormVFX->SetFloatParameter(FName("SpawnRate"), StormIntensity * 1000.f); }
}

void AStormSystem::ProcessExtremeHailDamage()
{
    if (CurrentCellData.HailstoneDiameter > 30.f) // Grêle destructrice
    {
        float DamageRadius = 5000.f; 
        float DamageAmount = CurrentCellData.HailstoneDiameter * 0.5f;

        TArray<AActor*> IgnoredActors;
        UGameplayStatics::ApplyRadialDamage(
            this, DamageAmount, GetActorLocation(), DamageRadius, 
            UDamageType::StaticClass(), IgnoredActors, this, nullptr, true
        );

        // Déclenche Chaos sur les GC (Geometry Collections) à portée
        TriggerChaosFracturesInRadius(DamageRadius, DamageAmount);
    }
}
```

---

# 🚀 OPTIMISATION & PERFORMANCES

1. **Time-Slicing & Threading :** Résolution des équations en arrière-plan via `FRunnable`.
    
2. **Compute Shaders :** Sur des grilles 3D denses, déport de l'advection vers le GPU (RDG) et stockage dans un `UTexture3D`.
    
3. **LOD Météo & Collisions GPU :** La pluie locale instancie des Acteurs. Au loin, on utilise du brouillard volumétrique. Les particules de grêle utilisent le _Global Distance Field_ ou le _Depth Buffer_ de Niagara pour calculer des dizaines de milliers de rebonds sans impacter le CPU.
    

---

# 🗺️ ROADMAP DE DÉVELOPPEMENT PRIORISÉE

- **PHASE 1 : Fondations (Mois 1-2)**
    
    - Architecture : `UWeatherSubsystem`, `FWeatherCell`.
        
    - Grille mathématique et système temporel asynchrone.
        
- **PHASE 2 : Dynamique des fluides (Mois 3-4)**
    
    - Advection semi-lagrangienne, convection, génération d'humidité.
        
    - Écriture des Render Targets globaux.
        
- **PHASE 3 : Visuels & Accumulation (Mois 5-6)**
    
    - Volumetric Clouds procéduraux.
        
    - Matériaux dynamiques (accumulation eau, neige, assèchement).
        
- **PHASE 4 : Événements Extrêmes & Grêle (Mois 7-8)**
    
    - Calcul Updrafts/CAPE et formation de la grêle.
        
    - VFX de grêle (Mesh particles, Collisions GDF) et audio.
        
    - Logique serveur (Dégâts physiques) et Multijoueur (Réplication `AStormSystem`).
        
- **PHASE 5 : Polish & Destruction (Mois 9-10)**
    
    - Tornades (forces radiales) et intégration Chaos Destruction.
        
    - Radar Météo UI en temps réel.
        
- **PHASE 6 : Finalisation (Mois 11-12)**
    
    - Passage en Compute Shaders (si nécessaire pour la résolution finale).
        
    - Lissage réseau et profiling (Unreal Insights).
        

---

# ⚠️ RISQUES TECHNIQUES ET MITIGATIONS

1. **Chute de performances réseau (Net Saturation) :** _Mitigation:_ Ne jamais répliquer la grille brute. Synchroniser un "Seed" RNG et l'état macro des `AStormSystem`. L'extrapolation visuelle est locale et déterministe.
    
2. **Volumetric Cloud trop lourd :** _Mitigation:_ Réduire les _Ray Marching Steps_, masquer par _Distance Fields_, utiliser un LOD HDRI lointain.
    
3. **Complexité mathématique :** _Mitigation:_ Utiliser des modèles simplifiés d'automates cellulaires et une advection "fake 3D" plutôt qu'une simulation académique exhaustive.
    
	