# Custom Cloud Renderer: Design

Branch: `claude/clouds-v2` (from `claude/voxel-clouds`). Date: 2026-11-15.

## Intent

The voxel clouds have the right shapes, but the picture still flickers, shows a dither pattern when clouds move, and smears when the light changes (lightning, the sun in a time-lapse). The user finds it painful to watch. They also want detail that follows distance: rich up close, simpler but still realistic far away, with no visible switch between the two.

Root cause (read in `Engine/Shaders/Private/VolumetricRenderTarget.usf`): Unreal's `VolumetricCloud` traces a fraction of the pixels each frame and fills the rest from previous frames, reprojected with the **camera motion only**. Anything that moves or changes on its own (wind drift, a storm morphing between simulation states, a lightning flash, a fast-moving sun) makes that history wrong, and the result is dither and flicker. Tuning cannot fix this, because the reconstruction is engine code.

So we replace `VolumetricCloud` with our own renderer in the plugin, while keeping the cloud *content* we already have (voxel storm grid, cumulus field, anvils, rain veil).

## Goals

- No visible flicker or dither at x1 and in a time-lapse (up to about x60), seen from the ground and when flying through or above the clouds.
- Lightning lights the clouds with no trail, no dither and no black flashes.
- Continuous level of detail: noise octaves, step length and light samples change smoothly with distance. Up close you see detail; far away you see a correct, simpler cloud; nothing pops.
- The same storms as today, driven by the same simulation data.
- Faster than today, or at least the same: at most 4 ms for the cloud passes at 1336x751, and a total frame no slower than the current 12 ms (ground) / 17-20 ms (above cumulus).

## Non-goals

- Perfect quality at x600. At very high speed some smearing is acceptable (user's call).
- Changing the simulation or the voxel grid builder (`FAerosCloudVolume`), except for small additions the renderer needs.
- The "clouds jump every clock minute" issue (TODO.md). It is a data timing issue and is tracked separately. The renderer must not make it worse.
- Mobile or forward rendering. Desktop SM5/SM6 deferred only.
- Niagara: sprites would look flat and fail when flying through, and Niagara Fluids cannot cover 100 km.

## Anti-goals (a failure even if it "works")

- Trading flicker for blur: clouds that look smeared or ghosted at x1.
- Visible bands, rings or rectangles where the level of detail changes.
- Clouds that glow at night, or a whole sky that whitens when lightning strikes.
- Losing cloud shadows on the ground compared with today.

## Constraints

- UE 5.8 binary engine: only public headers (`Engine/Public`, `Renderer/Public`, `RenderCore`). No private renderer headers.
- ASCII-only C++ sources. Deterministic simulation (renderer-side randomness is fine, it is not simulation state).
- Plugin shaders live in `Aeros/Plugins/AerosWeather/Shaders`, mapped to `/Plugin/AerosWeather` by a `PostConfigInit` module.
- Existing translucent effects (lightning bolt meshes, rain particles, tornado tube) must keep drawing correctly against the clouds.

## Approaches considered

| | Approach | Trade-off |
|---|---|---|
| A | Keep `VolumetricCloud` and work around it (lightning out of the material, slower changes, hidden stability cvars) | A few days, no risk. The engine never reprojects along cloud motion, so the dither remains in time-lapse. |
| **B** | **Own ray-marcher in the plugin (chosen)** | Weeks. Full control over reprojection, lighting and level of detail. We must handle haze, cloud shadows and translucency order ourselves. |
| C | Hybrid: `VolumetricCloud` far, ours near | Two systems to stitch with no visible seam. The hardest option. |

The user picked **B**, with a throwaway spike first.

### Spike result (2026-11-15)

The spike ran a synthetic cloud field (cumulus plus one tower with an anvil) through the full pipeline described below, in module `AerosCloudRender`.

- The hook works from a project plugin with public headers only (`SubscribeToPostProcessingPass(BeforeDOF)`). The plugin shader directory compiles, and the clouds sit correctly in front of and behind the terrain.
- Flicker metric (two shots 0.2 s apart, `-AerosFlickerShots`): **0.2-0.4** on tour steps 1-2, against 1.1 for the tuned `VolumetricCloud`. Step 3 measured 1.7, but with no pixel above the threshold, so it is likely camera motion. Same 0 % of pixels above the threshold.
- GPU: about 8 ms for the whole frame including clouds (1336x751, measured with the game running).
- A test flash leaves nothing in the next frame.
- Problems found:
  - Streaks on the tower at 600 m/s drift.
  - The flash is far too bright in daylight, with a white rim at cloud edges.
  - No haze on distant clouds.
  - Flat lighting.
  - The flat cloud base cuts 4 km mountains: this is the test field. The real data uses the simulation's base.

## Design

### 1. Module and hook

- Module `AerosCloudRender` (Runtime, `PostConfigInit`), already created by the spike:
  - it maps the shader directory;
  - it creates `FAerosCloudViewExtension` after engine init.
- The extension subscribes to `EPostProcessingPass::BeforeDOF`. The pass reads scene color and scene depth and returns a new scene color with the clouds composited.
  - At this point, opaque geometry and before-DOF translucency are already in scene color.
  - After-DOF translucency (the default for translucent materials) still draws over the clouds, which is how it behaves today. The `aeros_setup.py` translucent materials (lightning, rain curtain, shaft, tornado) leave the pass at its default, so nothing changes for them.
- TSR / TAA runs after this pass and treats cloud pixels as static sky (camera-only velocity), exactly as it did for `VolumetricCloud`. Check for TSR ghosting on fast clouds. If it shows, output a velocity for cloud pixels or lower the TSR history weight for them.
- Switch: `r.AerosClouds` (1 = ours, 0 = old path).
  - When it is 1, `AAerosWeatherController` hides its `UVolumetricCloudComponent`.
  - When it is 0, the controller shows it again.
  - Default 0 until parity is reached, then 1.

### 2. Data from the game thread

`UAerosWeatherController` builds a plain struct every tick and sends it to the extension with `ENQUEUE_RENDER_COMMAND`. The extension keeps the latest copy on the render thread.

The struct contains:
- the two voxel volume textures and their rects (already advected by the mean storm motion, see `GetCloudVolumeRect`), the blend and the weight;
- the cloud shape map, the weather map, the domain and the noise offset;
- the mean storm motion (cm/s) used for reprojection;
- the active lightning strikes: up to 8 per frame, each with a position, a start time, an intensity and a channel path endpoint.

Textures are passed as `FTextureRHIRef` taken from the `UTexture` resources (`GetResource()->TextureRHI`) and registered in RDG with `RegisterExternalTexture`. The time passed is continuous display time, so motion between simulation snapshots stays smooth.

### 3. Cloud density (ported, not redesigned)

The HLSL density code of the current cloud material (the Custom node in `aeros_setup.py`) moves to `Shaders/Private/AerosCloudDensity.ush`. It keeps the same fields and tuning:
- cumulus loop with grow and fade;
- voxel lookup with xy/z warp;
- ice and anvil fibres;
- 2D anvil fallback;
- rain veil, including the rain in the G channel when R = 0.

The material-specific inputs become shader parameters. The current material stays in place for the old path.

Noise: generate our own tileable noise textures once at startup with a compute pass:
- a 128³ RGBA8 Perlin-Worley shape texture, one octave per channel;
- a 32³ RGBA8 Worley detail texture.

This replaces the engine's single-channel noises and removes the stretch-remap hacks. The textures are deterministic and kept as pooled render targets.

### 4. Continuous level of detail

All of these are smooth functions of the ray distance `t`:
- **Detail octaves**: octave *i* blends from its mean value to its real value as `Octaves(t)` passes from *i* to *i+1*, so the shape gets smoother with distance without changing size. This is the spike's `DetailNoise`.
- **Step length**: `clamp(0.06 + 0.012 t, 0.06, 0.5)` km (spike values, to tune), with a jittered start every frame.
- **Light samples**: 6 near, fewer far away, with the count blended by weight and never switched abruptly.
- **Far field (beyond about 60 km)**: the same density function with detail off and longer steps. No second renderer, so there is no seam.
- **Near field when flying**: tracing starts at the camera (t = 0), with steps of at most 60 m and no near clip. Inside a cloud you see a lit fog with depth, not a wall.

### 5. Lighting

- **Sun:**
  - direction and illuminance from the view (`AtmosphereLightDirection[0]`, `AtmosphereLightIlluminanceOnGroundPostTransmittance[0]`);
  - Beer-Lambert extinction plus three multiple-scattering orders (spike);
  - a dual Henyey-Greenstein phase function;
  - tuned so the side away from the sun is clearly darker (a TODO item today).
- **Ambient**: from the sky irradiance in the view uniform buffer (`SkyIrradianceEnvironmentMap`), with more light at the top and less at the base. It is darkened by a cheap upward occlusion estimate (a few samples), so storm bases go dark.
- **Haze**: apply the sky atmosphere's aerial perspective to the cloud color at its front depth. The 5.8 view uniform buffer exposes `CameraAerialPerspectiveVolume` and its slice parameters (`SkyAtmosphereCameraAerialPerspectiveVolume*`), and `SkyIrradianceEnvironmentMap` for the ambient. Apply the height fog on top when `AerosFogDensity` is high.
- **Night**: everything scales with the sun and sky values, so clouds go dark at night. There is no unlit term.

### 6. Temporal accumulation

- Trace at half resolution with a per-frame jitter (interleaved gradient noise or blue noise, using the frame index).
- Reproject each pixel using its front depth, with:
  - camera motion (current translated world position to the previous view-projection matrix, without the anti-aliasing jitter);
  - cloud motion: the mean storm motion times dt (the wind for the cumulus field).
- History is clamped to the current neighbourhood using variance clipping in YCoCg plus the alpha (transmittance) channel.
- **Motion-adaptive weight**: the new-frame weight rises with the reprojection distance in pixels and with the change between the previous and current simulation snapshots. This fixes the time-lapse streaks without blurring the x1 view.
- Lighting changes the history cannot follow (the sun moving fast in a time-lapse) raise the new-frame weight too.

### 7. Lightning: added fresh, never accumulated

The composite pass adds the flash glow from the front depth and transmittance of each pixel. It handles up to 8 flashes, with a falloff around each channel and a short in-cloud glow. The glow is never written to the history, so it cannot leave a trail.

The intensity is scaled to the current exposure, so a flash is bright at night and visible but not blinding by day. The spike flash was far too strong.

The flash needs a little blur to avoid the edge rim. To get it, use the upsampled transmittance, not the raw half-resolution one.

### 8. Composite

- Upsample with depth weights (spike): bilinear taps, down-weighted when the half-resolution sample saw a different scene depth.
- Fix the bright or dark rim along cloud edges against the sky (seen in the spike) with a 3x3 transmittance-aware filter.
- Output: `scene * transmittance + cloud light (+ lightning)`.

### 9. Cloud shadows on the ground

`VolumetricCloud` used to cast cloud shadows on the terrain. We keep that with a cloud shadow map:
- a 2D top-down transmittance texture (for example 512², covering 100 km around the camera), traced along the sun direction every few frames at low cost;
- written into a `UTextureRenderTarget2D` owned by the controller;
- read by a light function material on the directional light. That material is created by `aeros_setup.py`.

The sky light cloud occlusion (`SkyLightCloudBottomOcclusion` today) becomes a scalar from the cloud cover above the camera, pushed to the sky light intensity.

### 10. Performance

- Budget: at most 4 ms for all cloud passes at 1336x751 on the user's GPU.
- Measure with `stat gpu` and the RDG event scope `AerosClouds`.
- Levers:
  - empty-space skipping on the cheap shape term (the spike already exits before detail);
  - early exit at transmittance < 0.01;
  - fewer light samples far away;
  - a quality cvar `r.AerosClouds.Quality` 0-3 for step count and resolution scale.

### 11. Testing

- The tour becomes the regression test:
  - shots of steps 1-3 at x1 with `-AerosFlickerShots`. Target: mean diff ≤ 0.5 and 0 % of pixels above 12 at x1; ≤ 1.0 at x60 (`-AerosTourTimeScale=60`);
  - a flash shot;
  - an in-cloud shot (`-AerosSupercellCam` with the camera inside the layer).
- Compare against the old path with `r.AerosClouds 0` on the same steps.
- The standalone tests are unaffected. A new automation test only checks that the shaders compile and the extension registers.
- The user judges the final look in game. The metric only guards against regressions.

### 12. Rollout

1. Real data in the new renderer, with parity in shape against the old one.
2. Lighting, haze and lightning.
3. Cloud shadows.
4. Default `r.AerosClouds 1`.
5. Remove the old material path later, on request.

## Calls made without asking (check these)

- Budget of 4 ms for the cloud passes, and "no slower than today" for the frame.
- Keep cloud shadows on the ground (light function), and treat losing them as a failure.
- Generate our own multi-channel noise textures instead of the engine ones.
- `r.AerosClouds` stays 0 by default until the new path matches the old one, then switches to 1. The old material path is removed only on request.
- Flicker targets: ≤ 0.5 at x1 and ≤ 1.0 at x60, with the existing metric.

## Left to the builder

- The exact noise generation and the step and weight constants: tune them against the tour shots.
- Sampling details of the aerial perspective volume and the sky irradiance buffer (they are exposed in the view uniform buffer; follow the engine's `SkyAtmosphereCommon.ush` usage).
- The pooled render target and history management per view (key by view state, reset on camera cuts and teleports).
- Whether rain particles need a depth hint so they sort sensibly against clouds. Check in game with rain, which the spike did not cover.
