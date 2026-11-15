# Volumetric tornadoes in the custom cloud renderer — design

Date: 2026-11-15. Branch: a new branch cut from `claude/clouds-sharp`.

## Intent

The user watches from the ground, a few kilometres from a storm, and wants to **see the tornado come down out of the cloud**: the dark base lowers into a rotating wall cloud, a condensation funnel grows down from it, touches the ground or stays suspended, and later ropes out and lifts. It must look natural and vary from one tornado to the next. The user chose that the funnel's shape comes **from physics** (pressure drop in the vortex against the cloud base height), not from a list of artist shapes.

Today the tornado is a translucent procedural tube (`AAerosTornado::FunnelMesh`, 8 hand-made shapes picked by EF and seed) hung under the storm, with a flat mesh wall cloud and a dust mesh. It is not part of the cloud, it draws over the clouds (no depth test against them), and the voxel grid (312 m cells) is far too coarse to hold a funnel 50-1000 m wide.

## Goals

1. The funnel, the wall cloud and the ground dust whirl are part of the custom cloud ray march (`r.AerosClouds 1`): same lighting, shadows, haze and rain veil as the clouds, correct occlusion both ways.
2. The funnel's shape follows a vortex model: wider, longer and lower with a stronger vortex, a wider core and a lower cloud base; shorter and thinner with a weak vortex or a high base. Touching the ground or not emerges from the model.
3. The life cycle reads naturally from the ground: wall cloud lowers and rotates, the funnel grows down from it (no pop), matures, then thins into a tilted, sinuous rope and lifts.
4. Visible rotation: striations and the wall cloud turn at a speed tied to the vortex; the dust whirl turns at the ground.
5. Variety without hand-made shapes: cone, rope, stovepipe, wedge and multi-vortex appear from the parameters (EF, core radius, cloud base, life stage, seed).

## Non-goals

- Supercell variety (low-precipitation / classic / high-precipitation, striated towers): a second project, after this one.
- Changing the tornado simulation: genesis, EF rating, track, lifetime, wind field, forces, damage and sound stay as they are.
- Waterspouts, landspouts, gustnadoes; rain-wrapped (HP) tornadoes hidden in the precipitation curtain.
- The old engine path (`r.AerosClouds 0`) keeps the existing mesh tube unchanged.

## Anti-goals (a failure even if it technically works)

- A funnel that pops into existence, or a smooth plastic cone.
- A funnel or wall cloud drawn over clouds or rain that should hide it, or hidden by clouds that are behind it.
- Smearing or ghosting of the spinning funnel (the temporal accumulation must not blur it into a grey column).
- Any frame cost when no tornado is near the view.
- Flicker of the still clouds around it (the x1 targets of the renderer stay: ≤ 0.5 on the flicker pair).

## Constraints

- Sources 100 % ASCII; helpers in uniquely named namespaces (unity build); no local named like an `Aeros::Phys` constant.
- Visual only: no change to the deterministic simulation or its replication. The tornado parameters the renderer reads are the replicated actor properties (`MaxWind`, `CoreRadius`, `Age`, `Lifetime`, `TrackVelocity`, `Seed`, `EFRating`) plus the cloud base, so every client draws the same funnel.
- Budget: ≤ 1 ms more at `Quality 2`, 1336×751, with a mature tornado filling a third of the screen; ~0 when none is within range.
- Data reaches the renderer through `FAerosCloudRenderParams` (game thread → render thread), like the rest of the cloud data.

## Approaches considered

1. **Analytic funnel in the shader (chosen).** The game sends up to 4 tornado descriptors per frame; the density function evaluates the vortex model at each sample near a tornado. Sharp, animated, fully integrated; cost limited to rays that pass near a funnel (fine sub-steps only there).
2. **A fine local voxel grid per tornado** (≈ 30 m cells, rebuilt on the CPU every frame): simple shader, but CPU and upload cost, still blocky at 30 m, and rotation would stutter at the rebuild rate.
3. **Keep the mesh tube, make it depth-aware against the clouds:** least work, but the shape does not come from physics and the funnel cannot grow out of the cloud base.

## The physics (decided)

Vortex: tangential wind `v(r) = Vmax · 2 (r/rc) / (1 + (r/rc)²)` (maximum `Vmax` at the core radius `rc`). Cyclostrophic balance gives the pressure deficit `Δp(r) = ρ Vmax² / (1 + (r/rc)²)` (≈ 110 hPa at the centre for 100 m/s, 27 hPa for 50 m/s).

Condensation: air at height `z` (above the ground) condenses when its pressure falls to the cloud-base pressure, i.e. where `K · Δp(r) ≥ ρ g (zLCL − z)`. Hence:

- funnel radius at height `z`: `rf(z) = rc · sqrt(K Vmax² / (g (zLCL − z)) − 1)` where positive, 0 otherwise;
- funnel tip height: `zTip = zLCL − K Vmax² / g` (on the ground when ≤ 0).

`K ≥ 1` is a calibration factor for what the bare model leaves out (the moist, cooled low-level inflow; stronger near-ground winds). It is tuned against the target table below, not chosen per tornado.

Life cycle, from the replicated actor state: `Vmax(t) = MaxWind · GetStrength()`, `rc(t) = CoreRadius · lerp(1, 0.4, ropeOut)` (thinner as it ropes out). Because `zTip` follows `Vmax²`, the funnel lowers as the vortex spins up and rises as it decays: the descent from the cloud comes from the model.

Multi-vortex: for violent tornadoes (EF4+, chosen by the seed), 3-6 sub-vortices orbit inside at 0.6-0.8 `rc`, each with its own small core and the same condensation rule; the parent funnel becomes thinner and more translucent.

Axis: the funnel hangs from the mesocyclone (ahead of the ground point along the storm motion) and its base drags behind; the tilt grows during the rope-out and the axis bends slowly. The axis is a function of height (centre offset as a curve of `z / zLCL`), which holds for tilts up to ~60°; the radial distance is corrected by the cosine of the local tilt.

Cloud base `zLCL`: the parent storm's LCL (`FAerosStormData::LCL`), else the local weather LCL at the tornado.

### Target table (what tuning must reproduce)

| Case | Expected look |
|---|---|
| EF0-1, LCL ~1.5 km | funnel hangs a third to half way down; a dust whirl turns on the ground under it |
| EF2-3, LCL ~0.8 km | cone that reaches the ground |
| EF4-5, LCL ~0.5 km, wide core | wedge, wider than tall, with a dust and debris skirt |
| Rope-out (any) | thin, tilted, sinuous rope, then it lifts from the ground up |
| EF4+ with the multi-vortex seed | several thin vortices orbiting inside a translucent parent |

## Rendering (decided)

- **Descriptors:** up to 4 tornadoes in `FAerosCloudRenderParams` (km, world axes): ground point, `zLCL`, axis curve control points, `Vmax`, `rc`, `K`, life stage, spin phase, number of sub-vortices and their phase, dust tint. Filled by the weather controller from the `AAerosTornado` actors nearest the viewer.
- **Density:** the cloud density function adds (max) three terms near a tornado, each behind a cheap bounding-cylinder test:
  - **funnel**: dense inside `rf(z)` with a soft skin of 5-15 m; the edge radius is modulated ±10 % by noise in cylindrical coordinates rotating with the vortex (striations, wobble);
  - **wall cloud**: a lowered, rotating part of the base around the mesocyclone (radius ~1-2 km, 200-600 m below the base, lowering with strength), with spiral striations and ragged edges; it replaces the flat voxel wall cloud near tornadoes;
  - **dust whirl**: low density, earth-tinted albedo, where the surface wind `v(r)` exceeds ~30 m/s, up to 100-300 m high with `Vmax`, rotating.
- **Ray march:** inside a tornado's bounding cylinder the step drops to the scale of the funnel skin (≈ 15-30 m) so the edge is sharp and does not shimmer; outside, the march is unchanged.
- **Lighting:** the same lighting as the clouds (the light march sees the funnel too); the funnel is dark grey against a bright background, lit at the edges when backlit, and the flash glow reaches it.
- **Temporal:** funnel pixels must not smear: they get a faster history response (or reprojection with the vortex's rotation), so the striations stay crisp.
- **Mesh fallback:** with `r.AerosClouds 1` the tube, the wall cloud mesh and the dust mesh are hidden; physics, debris and sound stay. With `r.AerosClouds 0` nothing changes.

## Testing (decided)

- The funnel model (`rf(z)`, `zTip`, the life-cycle laws) lives in a pure C++ Core header so it is unit-tested in `Tests/Standalone` (monotonic with `Vmax`, `rc`, `zLCL`; tip on the ground exactly when `K Vmax² ≥ g zLCL`; no funnel when `Vmax` = 0) and mirrored in the shader.
- A ground-level tour gallery with overrides (`-AerosTornadoVmax`, `-AerosTornadoLCL`, `-AerosTornadoAge`, existing `-AerosTornadoShape` replaced or kept for the mesh path) produces one shot per row of the target table; the shots are judged against it.
- A time sequence of a full life (spin-up → rope-out) from the ground, at x1 with the moving-camera options, to check the descent, the absence of pops and of smearing.
- Flicker of the still clouds and the GPU budget measured as in the renderer project (`-AerosFlickerShots`, `-AerosTourExec=profilegpu`).

## Calls made by the designer (not by the user)

- Up to 4 tornadoes rendered at once (the nearest to the viewer); a farther one keeps no funnel in the custom path.
- One global `K`, tuned on the target table.
- The rotating wall cloud is drawn only around tornadoes; supercells without a tornado keep the voxel wall cloud.
- Axis tilt is handled up to ~60°; an almost horizontal rope at the very end is drawn at 60°.
- Dust whirl threshold ~30 m/s surface wind.
- Work on a new branch `claude/volumetric-tornadoes`, cut from `claude/clouds-sharp`.

## Left to the builder

- The exact noise used for striations and scud, the skin width, the dust tint per surface type, the wall cloud's spiral pattern.
- How the tornado descriptors are packed for the shader, and the bounding-cylinder test.
- The value of `K` (from the target table) and how the faster temporal response is done for funnel pixels.
- Whether the voxel builder's wall cloud is removed or blended near tornadoes.
