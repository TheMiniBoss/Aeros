# Voxel Clouds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 2.5D cloud look with a Nubis-3-style 3D density grid around the camera, synthesised from the simulation, so every storm has a real anatomy, while staying fluid.

**Architecture:** A pure C++ builder in `Core/` (`FAerosCloudVolume`) turns a snapshot into an RG8 voxel grid (R density, G darkness). `UAerosCloudManager` rebuilds it ~1 Hz on a worker task into one of two transient `UVolumeTexture`s and cross-fades. The cloud material samples the grid (eroded by engine 3D noise), adds the procedural cumulus field, falls back to the 2D path beyond the grid, and uses the Volumetric Advanced output for lighting.

**Tech Stack:** UE 5.8 C++ (Core pure C++, game layer UObjects), `UVolumeTexture::CreateTransient` + `RHIUpdateTexture3D`, `UE::Tasks`, material graph built by `aeros_setup.py` (HLSL Custom node), standalone MSVC tests.

**Spec:** `docs/superpowers/specs/2026-11-13-voxel-clouds-design.md`

## Global Constraints

- Grid: 256 x 256 x 48 voxels, 80 x 80 km x 16 km, centred on the viewer, recentred in 10 km steps.
- Rebuild ~1 Hz on a worker thread, double buffer, material cross-fade (no popping).
- Format RG8: R = large-scale density 0-1, G = darkness 0-1.
- Beyond 40 km from the grid centre: 2D fallback, faded over 5 km.
- Sources 100 % ASCII; helpers in uniquely named namespaces (unity build); no `FMath::FRand` in `Core/`.
- Materials only via `aeros_setup.py`; run headless and expect every `[Aeros] SUMMARY` line OK.
- Standalone tests: `cmd //c "\"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" && Aeros\Plugins\AerosWeather\Tests\Standalone\build.bat"` must end with `failures: 0`.
- Editor closed before `Build.bat` (save via MCP `save_assets([])`, then close).

## Review Focus

1. Teleport farther than 40 km (keys F/V/I, tour): the old grid must not be drawn at the new place; expected: voxel weight 0 until the new grid is ready, far-field 2D path visible meanwhile.
2. Fair weather, no storm: grid all zero, cumulus still visible (no empty sky).
3. Simulation domain recentring (`bFollowPlayer`): storms keep their world position in the grid.
4. Storm partly outside the grid: clipped cleanly, no wrap-around at the opposite edge.
5. Huge time scale (x7680): rebuilds stay at most ~1 Hz, game thread never waits for the builder.

---

### Task 1: Core voxel builder (`FAerosCloudVolume`)

**Files:**
- Create: `Aeros/Plugins/AerosWeather/Source/AerosWeather/Public/Core/AerosCloudVolume.h`
- Create: `Aeros/Plugins/AerosWeather/Source/AerosWeather/Private/Core/AerosCloudVolume.cpp`
- Modify: `Aeros/Plugins/AerosWeather/Source/AerosWeather/Private/Tests/AerosSimulationTests.cpp` (new test)
- Modify: `Aeros/Plugins/AerosWeather/Tests/Standalone/run_automation_tests.cpp`, `build.bat`, `build.sh` (add the new .cpp and test)

**Interfaces:**
- Produces:
  - `struct FAerosCloudVolumeDesc { int32 NX = 256, NY = 256, NZ = 48; float SizeKm = 80.f; float HeightKm = 16.f; FVector Center; }` (Center = world cm, Z ignored)
  - `class FAerosCloudVolume { public: static void Build(const FAerosSnapshot& Snap, const FAerosCloudVolumeDesc& Desc, TArray<uint8>& OutRG8); static FVector SnapCenter(const FVector& Viewer, float StepKm = 10.f); static float SampleDensity(const TArray<uint8>& RG8, const FAerosCloudVolumeDesc& Desc, const FVector& World, float AltKm); };`
  - Layout: index = 2 * (X + Y * NX + Z * NX * NY); Y grows with world +Y (south), Z grows with altitude (voxel z centre = (z + 0.5) * HeightKm / NZ).
  - `Build` is parallel over Z slices (`ParallelFor` exists in the shim? if not, plain loop under `#if` — check `Tests/Standalone/shim`).

- [ ] **Step 1: Write the failing test `FAerosCloudVolumeTest` ("Aeros.Clouds.VoxelAnatomy")**

Build a supercell outbreak simulation for 3 simulated hours (same setup as `FAerosSupercellTest`), take the strongest storm S, build a grid centred on `S.UpdraftLocation`, then assert:

```cpp
const float Tower  = FAerosCloudVolume::SampleDensity(Grid, Desc, S.UpdraftLocation, 0.5f * (LCLkm + ELkm));
const float Above  = FAerosCloudVolume::SampleDensity(Grid, Desc, S.UpdraftLocation, ELkm + 2.5f);
const FVector Downwind = S.UpdraftLocation + WorldDir500 * 2500000.0; // 25 km along the 500 hPa wind
const FVector Upwind   = S.UpdraftLocation - WorldDir500 * 3500000.0; // 35 km against it
TestTrue(TEXT("Tower is dense at mid height"), Tower > 0.5f);
TestTrue(TEXT("Nothing 2.5 km above the EL"), Above < 0.05f);
TestTrue(TEXT("Anvil downwind near the EL"), FAerosCloudVolume::SampleDensity(Grid, Desc, Downwind, ELkm - 0.8f) > 0.2f);
TestTrue(TEXT("No anvil far upwind"), FAerosCloudVolume::SampleDensity(Grid, Desc, Upwind, ELkm - 0.8f) < 0.05f);
TestTrue(TEXT("Clear below the cloud base away from storms"), FAerosCloudVolume::SampleDensity(Grid, Desc, Upwind, 0.3f) < 0.05f);
```

Plus edge cases (Review Focus 2 and 4):

```cpp
// Fair weather: a fresh FairWeather simulation after 1 h -> every voxel density < 0.05
// Storm outside the grid: centre the grid 200 km away from every storm -> max density < 0.05
// Snap: SnapCenter(FVector(1234567, -987654, 0)) == FVector(1000000, -1000000, 0)
```

Use `ELkm = S.CloudTop * 0.001f`, `LCLkm = S.LCL * 0.001f`, `WorldDir500 = FAerosSnapshot::SimToWorldDir(W500.X, W500.Y).GetSafeNormal()`.

- [ ] **Step 2: Run the standalone tests, expect a compile failure (`FAerosCloudVolume` undefined)**

- [ ] **Step 3: Implement `FAerosCloudVolume`**

Per voxel: sample the snapshot cell at (x, y) (nearest cell is enough, the grid is finer than the 2 km sim), then take the max of these components (each 0-1):
- Stratiform / cumulus envelope: `CloudCover` between `CloudBase` and `CloudTop` only where `Conv < 0.2` (density 0.35 * cover; cumulus detail comes from the shader).
- For each storm within 60 km of the grid (precompute the list once per build):
  - Tower: horizontal distance d to the axis `UpdraftLocation + Shear * (alt - LCL)` where `Shear = WorldDir500 * 0.25 km per km`; radius `Rt = clamp(RadiusKm * 0.35, 2, 6)` km; density `smoothstep(Rt, 0.6 Rt, d)` between LCL and EL.
  - Overshooting top: dome of radius `0.5 Rt` above EL, height `clamp((MaxUpdraft - 30) / 20, 0, 1) * 2` km.
  - Anvil: shape-map logic re-implemented here (splat downwind along `WorldDir500`, 15-70 km long, widening 0.18 km per km), thickness 1.5-2.5 km under EL, density 0.6 fading with distance; mammatus: under the anvil base within 30 km, `0.4 * (sin(x*1.9)+sin(y*2.3)) ` lobes 300-600 m deep.
  - Precipitation core: ellipse around `HailCoreLocation`, radius `RadiusKm * 0.6`, from LCL to 0.7 EL, density 0.8, darkness = `sqrt(MaxRainRate / 100)`.
  - Wall cloud: if `Mesocyclone > 0.4`, disc radius 1.5-3 km under the updraft from LCL - 0.4 km to LCL.
  - Flanking line: 4 towers spaced 4 km behind-right of the updraft (`-Motion + Perp`), heights 0.8, 0.6, 0.45, 0.3 of (EL - LCL).
- Shelf cloud: gust front from the snapshot (same convergence rule as `UAerosCloudManager::UpdateShapeMap`, move it into a shared helper `AerosCloudVolumeInternal::GustFront(Snap, I, J)` used by both), wedge 0.4-1.9 km.
- G (darkness) = max(precip-core darkness, 0.3 * conv * height fraction).

- [ ] **Step 4: Run the standalone tests, expect `failures: 0`**

- [ ] **Step 5: Commit** — `git commit -m "Add FAerosCloudVolume: voxel storm anatomy from the simulation"`

### Task 2: GPU grid in `UAerosCloudManager`

**Files:**
- Modify: `.../Public/AerosCloudManager.h`, `.../Private/AerosCloudManager.cpp`
- Modify: `.../Private/AerosWeatherController.cpp` (material parameters)
- Modify: `.../Source/AerosWeatherDemo/Private/AerosDemoPawn.cpp` (dump slices with `-AerosDumpMaps`)

**Interfaces:**
- Consumes: `FAerosCloudVolume::Build`, `FAerosCloudVolumeDesc`, `SnapCenter`.
- Produces (cloud manager):
  - `UVolumeTexture* GetCloudVolume(int32 Index) const` (Index 0 = previous, 1 = current)
  - `FVector4 GetCloudVolumeRect() const` (CenterX, CenterY cm, SizeXY cm, HeightKm)
  - `float GetCloudVolumeBlend() const` (0 = previous, 1 = current; 0..1 over 1 s)
  - `float GetCloudVolumeWeight() const` (0 after a > 40 km jump until the first grid at the new place is uploaded, else 1)
  - `void DumpVolumeSlices(const FString& Prefix) const` (PNGs: horizontal at 2 km and 10 km, vertical through the grid centre along X)
- Material parameter names (controller sets them on the cloud MID): `AerosCloudVolumePrev`, `AerosCloudVolumeCur` (textures), `AerosCloudVolumeRect` (vector), `AerosCloudVolumeBlend`, `AerosCloudVolumeWeight` (scalars).

- [ ] **Step 1: Create the two transient volume textures (`PF_R8G8`, 256x256x48, bilinear, clamp) on first use**
- [ ] **Step 2: Launch `UE::Tasks::Launch` builds at most once per second when a new snapshot exists and no build is running; keep the snapshot pointer alive in the task lambda**
- [ ] **Step 3: On completion (game thread), upload with `ENQUEUE_RENDER_COMMAND` + `RHIUpdateTexture3D` into the older texture, swap roles, restart the blend**
- [ ] **Step 4: Weight rule for Review Focus 1: if the snapped centre moved more than 40 km, set weight 0 until the next upload**
- [ ] **Step 5: Build (`Build.bat`), run the tour with `-AerosTourOnly=1,2 -AerosDumpMaps`, check `Saved/AerosDebug/02_orage_vox_*.png` show towers / anvil**
- [ ] **Step 6: Commit** — `git commit -m "Upload the voxel cloud grid to double-buffered volume textures"`

### Task 3: Cloud material on the voxel grid

**Files:**
- Modify: `Aeros/Plugins/AerosWeather/Content/Python/aeros_setup.py` (`create_cloud_material`, `CLOUD_HLSL`)

**Interfaces:**
- Consumes: the material parameter names of Task 2.
- Produces: `M_AerosVolumetricCloud` with the same 2D parameters as today plus the voxel ones.

- [ ] **Step 1: Sample both volumes at `UVW = ((WorldXY - Rect.xy) / Rect.z + 0.5, AltKm / HeightKm)`, lerp by Blend, multiply by Weight and by the 5 km edge fade (inside 35-40 km)**
- [ ] **Step 2: HLSL density = max(voxel R eroded by engine noise (cauliflower above 40 % of the column, wisps below), cumulus cellular field from the 2D maps); far field uses today's 2D density times (1 - edge fade)**
- [ ] **Step 3: Replace the plain outputs with `MaterialExpressionVolumetricAdvancedMaterialOutput`: phase G 0.6, phase G2 -0.25, phase blend 0.3, multi-scattering contribution 0.5, occlusion 0.4, eccentricity 0.5; extinction scaled by (1 + 3 * darkness)**
- [ ] **Step 4: Regenerate headless, expect all SUMMARY lines OK; run `-AerosTourOnly=1,2,4`; captures must show a leaning tower with an anvil and no full overcast in fair areas**
- [ ] **Step 5: Commit** — `git commit -m "Cloud material: voxel storms, cumulus field, advanced volumetric lighting"`

### Task 4: Tour, tuning, docs

**Files:**
- Modify: `.../Source/AerosWeatherDemo/Private/AerosDemoPawn.cpp` (new step "une supercellule": side view at 40 km, 1.5 km altitude; then from below at 8 km)
- Modify: `Aeros/Plugins/AerosWeather/README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the tour step (12 steps total), screenshot both views**
- [ ] **Step 2: Tune shape constants against the slices and captures (max 3 iterations), re-run standalone tests after any Core change**
- [ ] **Step 3: Measure cost: `stat gpu` in a capture run (log `VolumetricCloud` ms before/after), note it in the README**
- [ ] **Step 4: Update README (clouds section) and CLAUDE.md (state, lessons)**
- [ ] **Step 5: Full tour run, then commit and push `claude/voxel-clouds`**
