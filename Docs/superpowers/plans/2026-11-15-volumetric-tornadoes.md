# Volumetric Tornadoes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw tornadoes (condensation funnel, rotating wall cloud, dust whirl) inside the custom cloud ray march, with the funnel's shape coming from a vortex pressure model, so that from the ground the funnel grows down out of the cloud base and ropes out naturally.

**Architecture:** A pure C++ funnel model (`Aeros::Funnel`, Core, unit-tested) gives the funnel radius at each height and the life-cycle laws. Each `AAerosTornado` builds a render descriptor from it (axis curve, Vmax, core, cloud base); the weather controller sends up to 4 descriptors through `FAerosCloudRenderParams`; the cloud density shader evaluates the same model near each tornado, with fine ray sub-steps inside the tornado's bounding cylinder. The mesh tube stays for `r.AerosClouds 0` and is hidden otherwise.

**Tech Stack:** UE 5.8 C++ (AerosWeather, AerosCloudRender, AerosWeatherDemo modules), HLSL (`Shaders/Private/AerosClouds.usf`, `AerosCloudDensity.ush`), standalone MSVC tests, tour screenshots.

**Spec:** `docs/superpowers/specs/2026-11-15-volumetric-tornadoes-design.md`

## Global Constraints

- Sources 100 % ASCII (`é` in `TEXT()`); helpers in uniquely named namespaces; `using namespace` only inside functions (unity build).
- No local variable named like an `Aeros::Phys` constant (`G`, `Cp`, `Lv`, `Rd`, `K0`...): C4459 is an error.
- Visual only: no change to the simulation, the tornado's wind, forces, EF, track, lifetime or replication.
- Budget: ≤ 1 ms more at `Quality 2`, 1336×751, with a mature tornado filling a third of the screen; ~0 with no tornado in range.
- Still-cloud flicker stays ≤ 0.5 on the x1 flicker pair (`r.AerosClouds.Flash=-1`, regions away from the funnel).
- `r.AerosClouds 0` keeps today's mesh visuals unchanged.
- Build with the editor closed (`Build.bat AerosEditor Win64 Development -Project=... -WaitMutex`); launch Unreal from PowerShell.
- Commit only explicit paths; never `Aeros/Content/`.

## Review Focus

1. Camera inside or right next to the funnel / dust whirl (ray starts inside a bounding cylinder): no black or NaN frame, and the sub-steps are capped so the cost stays bounded. Test: Task 5 Step 3.
2. A tornado entering or leaving the rendered set of 4 (distance order changes, spawn, despawn): it fades, it never pops. Test: Task 2 Step 5.
3. Cloud base at or near the ground (LCL → 0), Vmax = 0, very wide core: the model returns finite values and no funnel where it should not. Test: Task 1 Step 1 (`Funnel_DegenerateInputs`).
4. Time-lapse x60: the visual spin is capped so the striations do not strobe or alias backwards. Test: Task 5 Step 4.
5. Night and backlit views: the funnel is lit like the clouds (dark against a bright sky, rim-lit, reached by the flash glow), not an unlit grey shape. Test: Task 4 Step 5.

---

### Task 1: The funnel model (Core, tested)

**Files:**
- Create: `Aeros/Plugins/AerosWeather/Source/AerosWeather/Public/Core/AerosFunnel.h` (header-only, `CoreMinimal.h`)
- Modify: `Source/AerosWeather/Private/AerosTornado.cpp` (`GetStrength` uses `Aeros::Funnel::LifeStrength`)
- Test: `Tests/Standalone/run_automation_tests.cpp` (new `FunnelTests` namespace, registered in `main`)

**Interfaces:**
- Produces:
  ```cpp
  namespace Aeros { namespace Funnel {
    struct FParams { float VmaxMs = 0.f; float CoreRadiusM = 80.f; float CloudBaseM = 1000.f; float K = 1.f; };
    float TangentialWindMs(float VmaxMs, float RadiusM, float CoreRadiusM);   // Vmax * 2(r/rc) / (1 + (r/rc)^2)
    float PressureDeficitPa(float VmaxMs, float RadiusM, float CoreRadiusM);  // 1.1 * Vmax^2 / (1 + (r/rc)^2)
    float RadiusAtM(const FParams& P, float HeightM);   // rc * sqrt(K Vmax^2 / (9.81 (base - h)) - 1), 0 if none; >= base -> wall of the cloud: returns the radius at base - 1 m
    float TipHeightM(const FParams& P);                 // max(0, base - K Vmax^2 / 9.81)
    float LifeStrength(float Age01);                    // today's GetStrength law: spin-up over 12 %, rope-out over the last 25 %
    float RopeOut(float Age01);                         // 0 -> 1 over the last 35 % of the life
    float CoreRadiusAt(float CoreRadiusM, float Age01); // CoreRadiusM * lerp(1, 0.4, RopeOut)
  } }
  ```

- [ ] **Step 1: Write the failing tests** in `FunnelTests::Run()`:
  - `Funnel_NoVortexNoFunnel`: Vmax 0 → `RadiusAtM` 0 at every height and `TipHeightM` == base.
  - `Funnel_TipOnGround`: base 800 m, K 1 → Vmax 89 m/s gives tip 0 (K V²/g = 807 ≥ 800); Vmax 85 gives tip > 0.
  - `Funnel_RadiusMonotonic`: at h = 200 m, base 1000: radius grows with Vmax (40→60→80), with rc (50→100→200), and shrinks with base (800→1000→1500, other inputs fixed).
  - `Funnel_NarrowsDownward`: for Vmax 70, rc 100, base 1000, K 1.5: radius at 900 m > at 500 m > at the tip height (== 0 within 1e-3).
  - `Funnel_DegenerateInputs` (Review Focus 3): base 0, base 1 m, h = base, h > base, rc 2000, Vmax 0 → all results finite (`std::isfinite`), none negative.
  - `Funnel_LifeMatchesTornado`: `LifeStrength` at Age01 0, 0.06, 0.5, 0.875, 1 gives 0, 0.5, 1, 0.5, 0 (today's `GetStrength` law: smoothstep spin-up over 0.12, rope-out from 0.75 over 0.25).
- [ ] **Step 2:** Run the standalone suite (`Tests/Standalone/build.bat` from an x64 VS prompt). Expected: compile error, `Core/AerosFunnel.h` not found.
- [ ] **Step 3:** Implement `AerosFunnel.h` (inline functions; `9.81` and `1.1` as named constants inside `Aeros::Funnel`, not `G`). Clamp `base - h` to ≥ 1 m before dividing.
- [ ] **Step 4:** Run the suite. Expected: all `Funnel_*` PASS, `failures: 0`.
- [ ] **Step 5:** Replace the body of `AAerosTornado::GetStrength` with `Aeros::Funnel::LifeStrength(Age01)`. Build the editor target. Expected: `Result: Succeeded`.
- [ ] **Step 6:** Commit `feat(tornado): pure C++ funnel model (condensation from the vortex pressure drop)`.

### Task 2: Tornado descriptors to the renderer

**Files:**
- Modify: `Source/AerosCloudRender/Public/AerosCloudRender.h` (`FAerosCloudTornado`, `FAerosCloudRenderParams::Tornadoes`)
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (shader parameters)
- Modify: `Source/AerosWeather/Public/AerosTornado.h`, `Private/AerosTornado.cpp` (`MakeRenderDescriptor`, mesh hidden when `AerosCloudRender::IsEnabled()`)
- Modify: `Source/AerosWeather/Private/AerosWeatherController.cpp` (collect, select, fade)
- Modify: `Shaders/Private/AerosCloudDensity.ush` (tornado parameters, bounding test, debug density)

**Interfaces:**
- Consumes: `Aeros::Funnel` (Task 1).
- Produces:
  ```cpp
  struct FAerosCloudTornado {
    FVector3f GroundKm;          // world km, z = ground height
    float CloudBaseKm = 1.f;     // above the ground
    FVector4f AxisKm;            // cubic Bezier in xy over t = h / base: P1.xy, P2.xy (offsets from the ground point)
    FVector2f TopKm;             // P3.xy offset (P0 = 0)
    float VmaxMs = 0.f, CoreRadiusM = 80.f, K = 1.f, RopeOut = 0.f;
    float SpinPhase = 0.f;       // radians, wrapped
    int32 NumSubVortices = 0; float SubPhase = 0.f;
    FVector3f DustTint = FVector3f(0.45f, 0.38f, 0.3f);
    float Fade = 1.f;            // 0-1, selection fade
  };
  // FAerosCloudRenderParams: TArray<FAerosCloudTornado, TInlineAllocator<4>> Tornadoes;
  FAerosCloudTornado AAerosTornado::MakeRenderDescriptor(float CloudBaseM, float K) const;
  ```
  Shader: `float4 TornadoA[4]` (GroundKm.xyz, CloudBaseKm), `TornadoB[4]` (AxisKm), `TornadoC[4]` (TopKm.xy, VmaxMs, CoreRadiusKm), `TornadoD[4]` (K, SpinPhase, RopeOut, NumSubVortices), `TornadoE[4]` (DustTint.rgb, SubPhase), `float TornadoFade[4]`, `uint NumTornadoes`; cvar `r.AerosClouds.TornadoDebug` (1 = draw each bounding cylinder as faint density).

- [ ] **Step 1:** Move the axis logic of `UpdateFunnelMesh` (top ahead along the motion, lean growing with the rope-out, slow sinuous bend) into `MakeRenderDescriptor`, expressed as the Bezier offsets above, with `Vmax = MaxWind * LifeStrength`, `rc = CoreRadiusAt`, `SpinPhase` advanced by `min(Vmax / rc, 6 rad/s) * dt` (real seconds). `NumSubVortices` keeps today's rule (multi-vortex shape → 3-5) but from `EFRating >= 4` and the seed, not the picked shape.
- [ ] **Step 2:** In `AAerosTornado::Tick`, when `AerosCloudRender::IsEnabled()`, hide `FunnelMesh`, `FallbackFunnel`, `FallbackWallCloud`, `FallbackDustCloud` (show them again when it is 0). Debris and sound unchanged.
- [ ] **Step 3:** In `UpdateCustomClouds`: gather the tornado actors, keep the 4 nearest within 60 km, cloud base = parent storm `LCL` from the snapshot (else `GetWeatherAtLocation(...).LCL`), `K` from cvar `r.AerosClouds.TornadoK` (default from Task 3), command-line overrides `-AerosTornadoVmax=`, `-AerosTornadoLCL=` (metres) for the gallery. Selection fade: each tornado's `Fade` moves toward 1 when selected and toward 0 when dropped at 1/s, so the set changes without a pop (keep a small map by actor).
- [ ] **Step 4:** Shader: declare the parameters, add `AerosTornadoBounds(i)` (axis centre at height h, bounding radius = max funnel radius + dust radius + 200 m, height range 0 → base + 0.6 km) and, with the debug cvar, a faint density inside the bounds. Build; tour `-AerosTourOnly=1,6 -dpcvars=r.AerosClouds.TornadoDebug=1`. Expected: a faint column stands exactly where the tornado mesh stood in a run with `r.AerosClouds=0`, from the ground to the cloud base, leaning with the storm motion; the mesh is gone in the custom run.
- [ ] **Step 5 (Review Focus 2):** Tour `-AerosTourOnly=1,6 -AerosTourStepSeconds=40 -AerosTourTimeScale=60` (tornadoes spawn and die). Log each selection change (`Verbose`, plus `Log` with `r.AerosClouds.LogResets 1`). Expected: in the log, every tornado that leaves or joins the set has its fade ramp over ≥ 0.9 s (print fade at change time and 1 s later).
- [ ] **Step 6:** Commit `feat(tornado): send tornado descriptors to the cloud renderer`.

### Task 3: The condensation funnel in the ray march

**Files:**
- Modify: `Shaders/Private/AerosCloudDensity.ush` (funnel density), `Shaders/Private/AerosClouds.usf` (sub-steps in `TraceCS`)
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (cvar `r.AerosClouds.TornadoK`)
- Modify: `Source/AerosWeatherDemo/Private/AerosDemoPawn.cpp` (gallery framing option)

**Interfaces:**
- Consumes: Task 2 parameters; the shader mirrors `Aeros::Funnel::RadiusAtM` exactly (same constants, same clamp).
- Produces: `float AerosTornadoDensity(float3 PosKm, out float Dark)` in `AerosCloudDensity.ush`, added (max) into `AerosCloudDensity`; `bool AerosNearTornado(float3 PosKm)` used by the march.

- [ ] **Step 1:** Funnel density: horizontal distance to the axis at the sample's height (corrected by the cosine of the local tilt); funnel radius from the model, times `1 + 0.1 * striation`, where the striation noise is read in cylindrical coordinates (angle − `SpinPhase`, height stretched ×4, smooth noise only: no detail under ~5 m); soft skin `smoothstep(rf, rf − skin, d)` with skin 5-15 m growing with `rf`; dense core (extinction like a cumulus core), slightly darker than the cloud base. Multi-vortex: `NumSubVortices` small funnels orbiting at 0.7 `rc` with phase `SubPhase`, each with core `rc / 5` and the same rule; the parent's density × 0.4 when sub-vortices exist. All × `Fade`.
- [ ] **Step 2:** `TraceCS`: when `AerosNearTornado(P)`, the step becomes `clamp(skin-scale, 0.015, 0.03) km` (cap: at most 160 sub-steps per ray in tornado bounds), back to the normal step outside.
- [ ] **Step 3:** Pawn: `-AerosTornadoCam=DistKm,AltM,LookAltM` overrides the step-6 framing (camera at that distance from the tornado, looking at that height). Build.
- [ ] **Step 4:** Gallery: run step 6 once per row of the spec's target table, with `-AerosTornadoVmax`, `-AerosTornadoLCL`, `-AerosTornadoAge` (0.5 mature, 0.92 rope-out) and `-AerosTornadoCam=4,150,500`; EF4+ multi-vortex with `-AerosTornadoVmax=95 -AerosTornadoLCL=500` and a seed that yields sub-vortices. Tune `r.AerosClouds.TornadoK` (one value) until every row matches its expected look. Expected: the table rows, judged on the shots: hanging funnel + nothing on the ground (EF1, LCL 1.5 km), cone to the ground (EF2-3, 0.8 km), wedge wider than tall (EF5, 0.5 km), thin tilted rope (age 0.92), orbiting thin vortices (multi-vortex). Record the chosen K as the cvar default.
- [ ] **Step 5:** Commit `feat(tornado): physical condensation funnel in the cloud ray march`.

### Task 4: Wall cloud and dust whirl

**Files:**
- Modify: `Shaders/Private/AerosCloudDensity.ush`, `Shaders/Private/AerosClouds.usf` (dust albedo)

**Interfaces:**
- Consumes: Task 2/3 parameters.
- Produces: wall cloud and dust terms inside `AerosTornadoDensity` (dust returns its own albedo tint through an `out float3 Tint` used by the trace's lighting).

- [ ] **Step 1:** Wall cloud: a lowered disk under the base around the axis top (radius `clamp(15 rc, 1, 2) km`, bottom at base − `lerp(0.2, 0.6, strength)` km, ragged edge by smooth noise), spiral striations rotating with `SpinPhase * 0.15`, density blending into the base above. The voxel wall cloud stays for storms without a selected tornado.
- [ ] **Step 2:** Dust whirl: where `TangentialWindMs(Vmax, d, rc) > 30 m/s` and height < `lerp(100, 300, Vmax / 100)` m, low density (≈ 0.15 of a cumulus core), tint `DustTint`, rotating noise; it shows under a hanging funnel too (EF1 case).
- [ ] **Step 3:** `TraceCS`: the albedo of a sample is `lerp(cloud albedo, DustTint, dust share)`.
- [ ] **Step 4:** Build; rerun the Task 3 gallery. Expected: a rotating lowered wall cloud above every funnel, a brown whirl at the ground in the EF1 / EF2-3 / wedge shots, larger with Vmax.
- [ ] **Step 5 (Review Focus 5):** Night: tour `-AerosTourOnly=1,4,6 -AerosTornadoCam=4,150,500` (step 4 moves the clock to night, so step 6 runs at night), and a backlit framing (camera on the sun side reversed: `-AerosTornadoCam=-4,150,500`). Expected: at night the funnel is dark and lit by flashes like the clouds around; backlit it is a dark silhouette with a bright rim, not a flat grey shape.
- [ ] **Step 6:** Commit `feat(tornado): rotating wall cloud and dust whirl`.

### Task 5: Motion, budget, life sequence

**Files:**
- Modify: `Shaders/Private/AerosClouds.usf` (`TraceCS` writes a tornado mask; `TemporalCS` uses it)
- Modify: `Source/AerosCloudRender/Private/AerosCloudViewExtension.cpp` (only if a texture channel is added)

**Interfaces:**
- Consumes: `AerosNearTornado` (Task 3).
- Produces: a per-pixel "fast" mask from the trace (share of the ray's opacity inside tornado bounds), used by the temporal pass for a faster response (new-frame weight ≥ 0.5 where the mask is 1).

- [ ] **Step 1:** Store the mask (e.g. in a spare channel of the trace depth target) and use it in `TemporalCS` so funnel pixels do not smear. Build.
- [ ] **Step 2:** Life sequence: tour step 6 with `-AerosTourStepSeconds=60 -AerosTourTimeScale=1 -AerosTornadoAge=0` and a new pawn option `-AerosTourShotEvery=N` (an extra shot `<shot>_tNN.png` every N seconds of the step; here N = 10, plus the end shot). Expected: the wall cloud lowers, the funnel grows down from it (no frame where it appears whole), matures, thins into a tilted rope, lifts; the funnel striations stay crisp on the flicker pair (no grey smear).
- [ ] **Step 3 (Review Focus 1):** Camera in the funnel and in the dust: `-AerosTornadoCam=0.05,50,300` and `-AerosTornadoCam=0.3,20,100`. Expected: no black, NaN or frozen frame; `profilegpu` shows `AerosClouds` ≤ 6 ms there (sub-step cap holds).
- [ ] **Step 4 (Review Focus 4):** Time-lapse: step 6 at `-AerosTourTimeScale=60`. Expected: the visual spin stays at ≤ 6 rad/s (striations visibly turning the right way on two shots 0.2 s apart), no strobing.
- [ ] **Step 5:** Budget: `-AerosTourExec=profilegpu` on the gallery's cone shot (funnel ≈ a third of the screen) and on step 3 (no tornado in range). Expected: `AerosClouds` ≤ 4.7 ms with the tornado (the renderer measured 3.1-3.7 ms on `claude/clouds-sharp`, + 1 ms budget), unchanged ± 0.1 ms on step 3 without a tornado in range; still-cloud flicker ≤ 0.5 on steps 2-3 with `r.AerosClouds.Flash=-1`.
- [ ] **Step 6:** Commit `feat(tornado): crisp spinning funnel, life sequence and budget`.

### Task 6: Docs and push

**Files:**
- Modify: `Aeros/Plugins/AerosWeather/README.md` (tornado section: model, cvars, gallery options), `CLAUDE.md` (state + rules learned), `TODO.md`

- [ ] **Step 1:** Full tour (12 steps). Expected: 12 shots, no shader error, no crash.
- [ ] **Step 2:** Update the docs: the funnel model and its target table, `r.AerosClouds.TornadoK`, `r.AerosClouds.TornadoDebug`, the gallery options (`-AerosTornadoVmax`, `-AerosTornadoLCL`, `-AerosTornadoCam`, `-AerosTourShotEvery`); TODO: tick the funnel items, add what is left (supercell variety project, HP rain-wrapped tornadoes).
- [ ] **Step 3:** Commit `docs(tornado): volumetric tornadoes` and push `claude/volumetric-tornadoes`.
