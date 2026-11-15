# Custom Cloud Renderer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Unreal's `VolumetricCloud` with the plugin's own ray-marcher (module `AerosCloudRender`), so that the existing voxel storms render without flicker or dither at x1 and in a time-lapse, with continuous level of detail, fresh lightning light and cloud shadows.

**Architecture:** A scene view extension hooked at `BeforeDOF` runs three compute passes:
- a half-resolution jittered trace;
- a temporal accumulation reprojected along the camera and the storm motion;
- a depth-aware composite that also adds lightning fresh each frame.

The game thread pushes one `FAerosCloudRenderParams` per frame (textures, rects, motion, flashes). The density code of the current material moves to a shader include and runs on our own generated noise.

**Tech Stack:** UE 5.8 C++ (public renderer API only: `SceneViewExtension.h`, RDG, global shaders), HLSL `.usf/.ush` under `Plugins/AerosWeather/Shaders`, standalone C++20 tests (`Tests/Standalone`), the visual tour (`-AerosTourQuit`, `-AerosFlickerShots`).

**Spec:** `docs/superpowers/specs/2026-11-15-custom-cloud-renderer-design.md`

## Global Constraints

- Branch `claude/clouds-v2`. Commit only the files a task names (explicit paths, never `git add -A`); `Aeros/Content/` is never committed.
- Sources 100 % ASCII. Helpers in uniquely named namespaces (`AerosCloudRenderInternal`), `using namespace` only inside functions (unity build).
- Public renderer headers only (`Engine/Public`, `Renderer/Public`, `RenderCore`). No private renderer headers.
- Build: `"E:\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" AerosEditor Win64 Development "-Project=C:\Users\TheMiniBoss\Documents\GitHub\Aeros\Aeros\Aeros.uproject" -WaitMutex` from PowerShell, with the editor closed. Expected: `Result: Succeeded` and no `warning C` lines from `AerosCloudRender`.
- Standalone tests: `Tests/Standalone/build.sh` (Git Bash). Expected: all tests pass (21 today, plus those this plan adds).
- Tour run (PowerShell `Start-Process`, never Git Bash):
  `UnrealEditor.exe "<repo>\Aeros\Aeros.uproject" -game -windowed -ResX=1600 -ResY=900 -AerosTourQuit -AerosTourOnly=<steps> -AerosTourTimeScale=<1|60> -AerosFlickerShots -dpcvars=r.AerosClouds=1 -log=AerosTour.log`
  Shots go to `Aeros/Saved/Screenshots/WindowsEditor/AerosTour/`. Flicker metric: `python <scratchpad>/flicker.py 01_atmosphere 02_orage 03_supercellule`. The script is copied into `Tests/Visual/flicker.py` by Task 1.
- Switch `r.AerosClouds`: 1 = new renderer (the controller hides the `UVolumetricCloudComponent`), 0 = old path. It stays 0 by default until Task 9.
- Budget: at most 4 ms for the `AerosClouds` RDG scope at 1336x751. The frame must be no slower than the old path on the same tour step.
- Flicker targets: mean diff ≤ 0.5 and 0 % of pixels above 12 at x1; mean diff ≤ 1.0 at x60.
- Units: shader world positions in km, Unreal axes (z up). The voxel rect and domain come in cm, as the material gets them today.

## Review Focus

1. **Camera cut / teleport** (tour step change, F2 jump): the history must reset that frame, so there is no one-second ghost of the previous view. The test is in Task 4.
2. **Several views and resizes** (editor viewport + game window, a resolution-scale change): each view keeps its own history, and a size change resets it rather than reading out of bounds. The test is in Task 4.
3. **Night with lightning**: only cloud pixels near a strike brighten. The sky, the ground and far clouds stay dark. The test is in Task 5.
4. **Camera inside a cloud, or above the anvil**: no near clipping, no opaque wall, no black frame. The test is in Task 3.
5. **Snapshot swap** (the voxel buffers swap every ~30 s and blend runs 0 to 1): no pop, and the clouds move no more than the old path does at the clock-minute (TODO item). The test is in Task 4.

---

### Task 1: Render parameters from the game thread, real data replaces the test field

**Files:**
- Create: `Source/AerosCloudRender/Public/AerosCloudRender.h`
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.h/.cpp`, `Source/AerosCloudRender/Private/AerosCloudRenderModule.cpp`
- Modify: `Source/AerosWeather/AerosWeather.Build.cs` (private dependency `AerosCloudRender`)
- Modify: `Source/AerosWeather/Private/AerosWeatherController.cpp` (push params, hide `UVolumetricCloudComponent` when `r.AerosClouds` is 1)
- Modify: `Source/AerosWeather/Public/AerosCloudManager.h`, `Private/AerosCloudManager.cpp` (expose mean storm motion)
- Create: `Tests/Visual/flicker.py` (copy of the scratchpad metric, paths unchanged)

**Interfaces:**
- Produces, in `AerosCloudRender.h`:
  ```cpp
  struct FAerosCloudFlash { FVector3f PosKm; float Intensity; };   // Intensity 0-1, as reported today
  struct FAerosCloudRenderParams
  {
      FTextureResource* WeatherMap = nullptr;   // RGBA: cover, base/5 km, top/16 km, rain
      FTextureResource* ShapeMap = nullptr;     // RGBA: anvil cover, updraft, gust, convection
      FTextureResource* VolumePrev = nullptr;   // RG8 voxel grids
      FTextureResource* VolumeCur = nullptr;
      FVector4f DomainCm;                       // xy centre, zw size (cm), as AerosDomain
      FVector4f RectPrevCm, RectCurCm;          // xy centre (cm), z size (cm), w height (km)
      float VolumeBlend = 1.f, VolumeWeight = 0.f;
      FVector3f NoiseOffsetKm;                  // AerosNoiseOffset
      FVector3f MotionKmS;                      // mean storm motion (reprojection + flash drift)
      TArray<FAerosCloudFlash, TInlineAllocator<8>> Flashes;
  };
  namespace AerosCloudRender { AEROSCLOUDRENDER_API void SetParams(const FAerosCloudRenderParams& Params); }
  ```
  `SetParams` is called on the game thread. It copies the params to the extension's render-thread copy with `ENQUEUE_RENDER_COMMAND`.
- Produces in `UAerosCloudManager`: `FVector GetCloudMotionCmS() const` (the `MotionCmS` stored at the last grid build).
- Shader parameters (Task 2 and Task 3 bind them): `WeatherMap`, `ShapeMap`, `VolumePrev`, `VolumeCur` (`Texture2D`/`Texture3D`), the same names as the material parameters without the `Aeros` prefix.

- [ ] **Step 1:** Add the header and `SetParams`. In `Render_RenderThread`, read the latest params and register the four textures (`RegisterExternalTexture(CreateRenderTarget(Resource->TextureRHI, ...))`, or `GSystemTextures` black when null). Replace `CVarWind` with `Params.MotionKmS` (keep the cvar as an override when > 0, for tests).
- [ ] **Step 2:** In `AAerosWeatherController::Tick`, next to the existing `CloudMID` block, fill the params from the same sources (`GetWeatherMapTexture`, `GetCloudShapeMap`, `GetCloudVolume(0/1)`, `GetCloudVolumeRect(0/1)`, blend, weight, `GetDomainRect`, `GetCloudNoiseOffsetKm`, `GetCloudMotionCmS() / 1e5`) and call `AerosCloudRender::SetParams`. `Flashes` holds the single current flash (`GetLightningFlash`, `GetLightningLocation`) until Task 5 extends it to 8. When `r.AerosClouds` is 1, `SetHiddenInGame(true)` / `SetVisibility(false)` on the `UVolumetricCloudComponent`. Restore it when the cvar goes back to 0.
- [ ] **Step 3:** In the trace shader, temporarily replace the synthetic field with a debug density: voxel `R` only, at the voxel UVW (`(WorldXY - Rect.xy)/Rect.z + 0.5`, `AltKm / Rect.w`). This proves the data path.
- [ ] **Step 4:** Build. Expected: `Result: Succeeded`.
- [ ] **Step 5:** Tour `-AerosTourOnly=2,3 -AerosTourTimeScale=1`. Expected: step 3 shows the supercell as a smooth voxel blob at the same place as in the old path (`r.AerosClouds=0` run, same steps). The log has no `AerosClouds` errors.
- [ ] **Step 6:** Commit `feat(clouds-v2): feed simulation data to the custom cloud renderer`.

### Task 2: Generated tileable noise

**Files:**
- Create: `Source/AerosCloudRender/Private/AerosCloudNoise.h`, `AerosCloudNoise.cpp` (pure C++: only `<cstdint>`, `<vector>`, `<cmath>`; no UE headers, so the standalone build can compile it)
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (upload once to two `FRHITexture` 3D, kept as pooled render targets)
- Modify: `Tests/Standalone/build.sh`, `build.bat`, `run_automation_tests.cpp` (compile `AerosCloudNoise.cpp`, add tests)

**Interfaces:**
- Produces:
  ```cpp
  namespace AerosCloudNoise
  {
      // RGBA8, x fastest. Shape 128^3: R = Perlin-Worley (billowy, 4 cells per tile), G/B/A = Worley at
      // 8/16/32 cells. Detail 32^3: R/G/B = Worley at 4/8/16 cells, A = their fbm. Tileable on all axes.
      std::vector<uint8_t> MakeShape(int Size = 128, uint32_t Seed = 1);
      std::vector<uint8_t> MakeDetail(int Size = 32, uint32_t Seed = 2);
  }
  ```
  Each channel is normalised so its 5th-95th percentile spans 13-242 (5 %-95 % of 0-255). This keeps the old thresholds meaningful without the old stretch hacks.
- Shader parameters: `ShapeNoise`, `DetailNoise` (`Texture3D`), `NoiseSampler` (trilinear, wrap, mips).

- [ ] **Step 1: Failing tests** in `run_automation_tests.cpp`:
  - `CloudNoise_Tileable`: for every channel, mean |v(x=0) - v(x=Size-1 wrapped neighbour)| equals that channel's mean |v(x) - v(x+1)| within 25 %. Same on y and z.
  - `CloudNoise_Range`: for every channel of both textures, P5 ≤ 20 and P95 ≥ 235.
  - `CloudNoise_ChannelsDiffer`: the correlation between any two channels of the shape texture is < 0.9 (the engine noise had R = G = B).
  - `CloudNoise_Deterministic`: two calls with the same seed are byte-identical.
- [ ] **Step 2:** `Tests/Standalone/build.sh`. Expected: link error or FAIL on the four new tests.
- [ ] **Step 3:** Implement `MakeShape` / `MakeDetail`. Use a hash-based Worley with a wrapped cell lattice and a gradient Perlin with a wrapped lattice. PW = remap(perlin, worley - 1, 1, 0, 1) (Schneider). Then do the percentile normalisation per channel.
- [ ] **Step 4:** `Tests/Standalone/build.sh`. Expected: all pass. Generation takes under 3 s for 128³ in `-O2`.
- [ ] **Step 5:** Upload on first use on the render thread (`RHICreateTexture` 3D RGBA8 with mips built by the CPU box filter, or `GenerateMips`). Bind it to the trace shader and sample `ShapeNoise` in the debug density from Task 1 (`density *= ShapeNoise.r`) to prove the binding.
- [ ] **Step 6:** Build plus tour step 3. Expected: the blob shows lumpy noise, not a constant.
- [ ] **Step 7:** Commit `feat(clouds-v2): tileable multi-channel cloud noise`.

### Task 3: Port the cloud density with continuous LOD

**Files:**
- Create: `Shaders/Private/AerosCloudDensity.ush`
- Modify: `Shaders/Private/AerosClouds.usf` (trace uses it; the synthetic field and spike constants are removed)
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (parameters)

**Interfaces:**
- Consumes: Task 1 textures and rects, Task 2 noise.
- Produces, in `AerosCloudDensity.ush`:
  ```hlsl
  struct FAerosCloudSample { float Density; float Dark; };   // Density 0-1, Dark 0-1 (rain greying)
  FAerosCloudSample AerosCloudDensity(float3 PosKm, float DistKm, float Octaves);
  float AerosCloudOctaves(float DistKm);    // 4 near -> 1 far, continuous (spike formula as start)
  float AerosCloudStepKm(float DistKm);     // clamp(0.06 + 0.012 * DistKm, 0.06, 0.5)
  ```
  The body is `CLOUD_HLSL` from `aeros_setup.py` (lines 161-269), with these changes:
  - `AltKm` = `PosKm.z`, `P` = `PosKm - NoiseOffsetKm`, and `Wx`/`Sx`/`VP`/`VC`/`VU` sampled in the function;
  - `N` → `ShapeNoise.r` at 6 km tiles, `W` → `ShapeNoise.r` at 9 km tiles + offset `(0.37, 0.11, 0.53)`;
  - `N2` (bubbles) → `ShapeNoise` 4 km tiles, `dot(gba, w)` where octave weights fade with `Octaves` (spike `DetailNoise` rule: an octave blends to its mean, 0.5, as `Octaves` drops below its index);
  - `E` → `DetailNoise.a` at 4 km tiles, faded to 0.5 the same way, `F` → `DetailNoise.r` at (16, 16, 3) km;
  - the `saturate((v - 0.78) / 0.18)` and `saturate((v - 0.45) / 0.45)` stretches are removed, since the noise is already normalised;
  - the mip/LOD selection moves from `TextureSample` levels to `Octaves`;
  - the voxel xy warp (±0.8 km from `N`, `W`) and the z-warp (±0.5 / ±0.2 km above 6-9 km) are kept;
  - extinction per km = `Density * 80 * (1 + 3 * Dark)` (the material's 0.08 / m × dark multiplier).

- [ ] **Step 1:** Write the include. Have `TraceCS` call it and use `AerosCloudStepKm` and `AerosCloudOctaves`. The march starts at t = 0 when the camera is between the lowest base and 16 km, with steps of at most 0.06 km near the camera. The slab is [0, 16] km (the voxel height), plus the 2D anvil top at 12.4 km.
- [ ] **Step 2:** Build. Expected: `Result: Succeeded`, with no shader compile error in `AerosTour.log` (`grep -i "AerosClouds.usf"`).
- [ ] **Step 3:** Tour `-AerosTourOnly=1,2,3 -AerosTourTimeScale=1` with `r.AerosClouds=1`, then the same with `r.AerosClouds=0`. Expected: the same clouds in the same places (cumulus field, supercell tower, anvil, rain veil under the storm). Copy both sets of shots side by side in the scratchpad and compare them visually. Lighting may still differ.
- [ ] **Step 4 (Review Focus 4): in-cloud test.** Tour `-AerosTourOnly=3 -AerosSupercellCam=6,2.5,2.5,6` (camera inside the cumulus layer) and `-AerosSupercellCam=10,13.5,11,10` (above the anvil). Expected: no black or opaque frame. Inside you see a fog with depth; above, the anvil top has relief.
- [ ] **Step 5:** Flicker metric on step 1-3 shots. Expected: ≤ 0.5 mean, 0 % > 12.
- [ ] **Step 6:** Commit `feat(clouds-v2): port the cloud density with continuous LOD`.

### Task 4: Robust temporal accumulation

**Files:**
- Modify: `Shaders/Private/AerosClouds.usf` (`TemporalCS`), `Source/AerosCloudRender/Private/AerosCloudViewExtension.h/.cpp`

**Interfaces:**
- Consumes: `Params.MotionKmS`, `Params.VolumeBlend` (Task 1).
- Produces: a per-view history `TMap<uint32, FAerosCloudHistory>` keyed by `View.State ? View.State->GetViewKey() : 0`, with
  `struct FAerosCloudHistory { TRefCountPtr<IPooledRenderTarget> Color, Depth; FMatrix ViewProj; double Time; FVector ViewOrigin; float Blend; uint32 LastFrame; };`
  Entries not used for 120 frames are dropped.

- [ ] **Step 1:** Reset rules (history invalid this frame):
  - the size differs from the current half size;
  - `View.bCameraCut`;
  - the camera moved more than 2 km since the last frame;
  - dt ≤ 0 or > 0.5 s.

  Log `AerosClouds: history reset (<reason>)` with `UE_LOG(LogTemp, Verbose, ...)`, plus a `Log`-level line when `r.AerosClouds.LogResets 1`.
- [ ] **Step 2:** In `TemporalCS`:
  - variance clipping in YCoCg on rgb, with the 3x3 mean ± 1.25 σ, plus min/max on alpha;
  - reprojection with `MotionKmS`;
  - new-frame weight `w = clamp(NewWeight + PixelMotion * 0.02 + abs(Blend - PrevBlend) * 4, NewWeight, 0.6)`, where `PixelMotion` is the reprojection offset in half-res pixels.
- [ ] **Step 3:** Build.
- [ ] **Step 4 (Review Focus 1):** tour `-AerosTourOnly=1,2,3` with `-dpcvars=r.AerosClouds=1,r.AerosClouds.LogResets=1`. Expected: `grep -c "history reset (camera cut)\|history reset (teleport)"` in `AerosTour.log` ≥ 2 (one per step change). The first shot of each step shows no ghost of the previous view.
- [ ] **Step 5 (Review Focus 2):** run the tour once at `-ResX=1600` and once with `-dpcvars=...,r.ScreenPercentage=50`. Expected: no crash, and a `history reset (size)` line on the first frame.
- [ ] **Step 6 (Review Focus 5):** tour `-AerosTourOnly=2 -AerosTourTimeScale=1` with a long step (`-AerosTourStepSeconds=70` if it exists; otherwise add that pawn option in this task, defaulting to the current durations) and `-AerosFlickerShots` timed across a voxel swap. Expected: flicker ≤ 0.5 on the pair that straddles the swap (the swap time is in the log from `UAerosCloudManager`; add a `Verbose` line there if none exists).
- [ ] **Step 7:** Time-lapse: tour `-AerosTourOnly=1,2,3 -AerosTourTimeScale=60 -AerosFlickerShots`. Expected: flicker ≤ 1.0 mean, and no visible streaks on the step 3 tower (compare with the spike's x60 shot).
- [ ] **Step 8:** Commit `feat(clouds-v2): per-view, motion-adaptive temporal accumulation`.

### Task 5: Lighting, haze, night, lightning

**Files:**
- Modify: `Shaders/Private/AerosClouds.usf` (`TraceCS` lighting, `CompositeCS` haze, flashes, edge filter), `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp`
- Modify: `Source/AerosWeather/Public/AerosCloudManager.h`, `Private/AerosCloudManager.cpp` (keep the 8 brightest flashes, not 1)
- Create: `Source/AerosWeather/Public/Core/AerosFlashList.h` (pure C++), with tests in `Tests/Standalone/run_automation_tests.cpp`

**Interfaces:**
- Produces: `namespace Aeros { struct FFlashEntry { float Intensity; FVector Location; }; template<int N> struct TFlashList { void Add(float Intensity, const FVector& Location); void Reset(); int Num() const; const FFlashEntry& operator[](int I) const; /* sorted by intensity, desc */ }; }`. `UAerosCloudManager::GetFlashes()` returns `TArrayView<const Aeros::FFlashEntry>` (Location in cm), reset each frame where `PendingFlash` is. `ReportLightningFlash` keeps its signature and feeds the list. The controller converts the entries to `FAerosCloudFlash` (km) for `SetParams`.
- Shader params: `FlashPosKm[8]` (`float4`: xyz, w = intensity × exposure scale), `NumFlashes`.

- [ ] **Step 1: Failing tests** `FlashList_KeepsBrightest` (add 20 flashes with intensities 0.05 × i, Num = 8, the first is 0.95, the last is 0.6) and `FlashList_IgnoresZero` (adding 0 changes nothing).
- [ ] **Step 2:** `build.sh`. Expected: FAIL. Then implement it. Expected: PASS.
- [ ] **Step 3: Lighting in `TraceCS`:**
  - sun from `View.AtmosphereLightDirection[0]` / `AtmosphereLightIlluminanceOnGroundPostTransmittance[0]`;
  - 6 light samples near, down to 3 far, blended by weight;
  - three scattering orders with a dual Henyey-Greenstein phase (g 0.6 / -0.25, blend 0.3, from the old material), multi-scattering contribution 0.25, occlusion 0.8;
  - ambient = sky irradiance from `View.SkyIrradianceEnvironmentMap` (the SH evaluated up and down), times `lerp(0.3, 1, height in cloud)`, times an upward occlusion of 3 samples at 0.3 / 0.8 / 1.6 km;
  - albedo `lerp(0.92, 0.55, Dark)`.
- [ ] **Step 4: Haze in `CompositeCS`:** apply the aerial perspective volume (`View.CameraAerialPerspectiveVolume`, slice via `SkyAtmosphereCameraAerialPerspectiveVolumeDepthSliceLengthKmInv`) at the cloud front depth: `rgb = rgb * T_ap + L_ap * (1 - T)`. Follow the engine's `SkyAtmosphereCommon.ush` sampling.
- [ ] **Step 5: Flashes in `CompositeCS`:** for each flash, glow = `w * exp(-|P - FlashPos| / 2.5 km) * (1 - T_up)`, where `P` = camera + dir × min(front, scene) and `T_up` = the upsampled transmittance. Physical scale, so it is subtle by day and bright at night: `w = Intensity * min(400 * View.PreExposure, 1.5)` (400 cd/m² in-cloud flash luminance; the 1.5 cap in pre-exposed units keeps the night sky from whitening). The spike's `4000 * PreExposure` was blinding by day. Also apply a 3x3 transmittance-aware filter on cloud edges against the sky, to remove the spike's rim.
- [ ] **Step 6:** Build.
- [ ] **Step 7:** Day shots: tour `-AerosTourOnly=1,2,3`. Expected: the side away from the sun is clearly darker than the lit side, storm bases are dark grey, and the 50 km clouds are hazy and bluish, not pure white.
- [ ] **Step 8 (Review Focus 3):** night lightning: tour `-AerosTourOnly=4` (`04_eclairs`, shot on a flash). Expected: the clouds around the strike glow, while the sky away from clouds, the ground and the clouds more than 20 km away stay dark. Measure the mean value of the sky band (rows 170-300) at least 300 px away from the flash: it must be within 10 % of a non-flash frame.
- [ ] **Step 9:** Commit `feat(clouds-v2): physical cloud lighting, haze and lightning`.

### Task 6: Cloud shadows on the ground

**Files:**
- Modify: `Shaders/Private/AerosClouds.usf` (new `ShadowMapCS`), `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp`
- Modify: `Source/AerosCloudRender/Public/AerosCloudRender.h` (`FAerosCloudRenderParams::ShadowTarget` = `FTextureRenderTargetResource*`, `ShadowRectCm` = `FVector4f`)
- Modify: `Source/AerosWeather/Private/AerosWeatherController.cpp` (create a 512² `UTextureRenderTarget2D` R16F, set the light function on `SunLight`, push the rect: centred on the camera, 100 km, snapped to 1 km)
- Modify: `Content/Python/aeros_setup.py` (new `M_AerosCloudShadow`: light function domain, samples `AerosCloudShadowMap` at `(WorldXY - Rect.xy) / Rect.zw + 0.5`, outputs transmittance, 1 outside the rect)

**Interfaces:**
- Consumes: `AerosCloudDensity` (Task 3).
- Produces: `ShadowMapCS`. One thread per texel marches along the sun direction through [0, 16] km with 24 steps, octaves = 1, and writes the transmittance. It runs every 4th frame.

- [ ] **Step 1:** Implement `ShadowMapCS`, the render target plumbing and the light function material (run `aeros_setup.py` headless as in CLAUDE.md; expected `[Aeros] SUMMARY` with the new material OK).
- [ ] **Step 2:** Set the sky light occlusion: `SkyLight` intensity × `lerp(1, 0.75, cover above camera)` (replaces `SkyLightCloudBottomOcclusion` when `r.AerosClouds` is 1).
- [ ] **Step 3:** Build.
- [ ] **Step 4:** Tour `-AerosTourOnly=1,3` with `r.AerosClouds=1` and with `=0`. Expected: cloud shadows are visible on the terrain in both runs, with similar darkness and position. No shadow seam at the 100 km rect edge in step 3.
- [ ] **Step 5:** Commit `feat(clouds-v2): cloud shadows on the ground`.

### Task 7: Performance and quality levels

**Files:**
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp`, `Shaders/Private/AerosClouds.usf`

**Interfaces:**
- Produces: cvar `r.AerosClouds.Quality` 0-3 (default 2). It scales the max steps (96 / 128 / 192 / 256), the light samples (3 / 4 / 6 / 8) and the trace resolution (Quality 0 = quarter resolution).

- [ ] **Step 1:** Implement the cvar. Make sure the march skips detail and lighting when the cheap shape term is 0 (spike pattern) and exits at T < 0.01.
- [ ] **Step 2:** Build. Tour `-AerosTourOnly=1,3 -ExecCmds="stat gpu"` with `r.AerosClouds=1`, then `=0`. Read the `AerosClouds` line and the frame time from the shots.
- [ ] **Step 3:** Expected:
  - `AerosClouds` ≤ 4 ms at Quality 2, 1336x751;
  - the frame time is no higher than with `r.AerosClouds=0` on the same step.

  If it is above budget, lower the defaults for steps and light samples first, and re-measure flicker (Task 3 Step 5 target).
- [ ] **Step 4:** Commit `perf(clouds-v2): quality levels and budget`.

### Task 8: In-game checks the spike did not cover

**Files:**
- Modify only if a check fails: `Content/Python/aeros_setup.py` (translucency pass), `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp`

- [ ] **Step 1: Rain order.** Tour `-AerosTourOnly=2,10` (storm, flood: rain under the camera). Expected: rain particles and the lightning bolt draw sensibly against the clouds, and no rain shows *through* a cloud seen from above (`-AerosSupercellCam=10,13.5,11,10`). If they do, set the offending material's translucency pass explicitly and note it in CLAUDE.md rules.
- [ ] **Step 2: TSR ghosting.** Tour step 3 at `-AerosTourTimeScale=60` with `r.AntiAliasingMethod=4` (TSR) and with `=2` (TAA). Expected: no ghost trail behind moving towers. If there is one, stop and show the user the shots: the real fix needs a velocity write for cloud pixels, which this post-process hook cannot do, so it is a design decision, not a task step.
- [ ] **Step 3:** Commit the fixes, if any, with `fix(clouds-v2): ...`.

### Task 9: Switch on and document

**Files:**
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (`r.AerosClouds` default 1)
- Modify: `Aeros/Plugins/AerosWeather/README.md` (renderer section, cvars, quality), `CLAUDE.md` (state + new rules learned), `TODO.md` (tick flicker/dither, salty edges, shadow side; add anything left)

- [ ] **Step 1:** Default 1. Full tour `-AerosTourQuit` (all 12 steps). Expected: 12 shots, no shader error, no crash. The flicker metric on 1-3 is within the targets.
- [ ] **Step 2:** Update the docs.
- [ ] **Step 3:** Commit `feat(clouds-v2): custom cloud renderer on by default` and push `claude/clouds-v2`.
