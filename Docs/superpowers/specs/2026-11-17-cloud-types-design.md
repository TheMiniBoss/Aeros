# Cloud types (genera) and realistic shapes — design

Branch `claude/cloud-types` (from `claude/storm-chaser`). Request: clouds look "cartoon" (balls); render every
cloud genus, each forming only under its own conditions and at its own altitude. What is not done now goes to
TODO.md.

## Goals

1. Less "ball", more real: cumulus shaped by 3D noise (Nubis remap), flat base, cauliflower top, wispy
   eroded edges; no analytic sphere per cell.
2. Genera by layer, diagnosed per simulation cell from its physics (pure C++, tested):
   * low (base < 2 km): Cu humilis / mediocris / congestus, St, Sc, Ns;
   * middle (2-7 km): Ac, As;
   * high (6-12 km): Ci, Cs, Cc;
   * vertical: Cu congestus, Cb (existing storms, voxel grid).
3. HUD names the clouds above the viewer (genus + species).

## Conditions (inputs the simulation already has, plus one new field)

| Genus | Forms when | Base / top |
|---|---|---|
| Cu | daytime convective boundary layer (surface heat flux, BL top above LCL), RH > 0.45 | base LCL; depth from CAPE: humilis < 0.7 km, mediocris 0.7-2.5 km, congestus deeper (convection) |
| St | stable surface layer (ground colder than air or weak lapse rate), RH > 0.93, some wind (calm = fog) | base 80-400 m, 0.3-0.7 km thick |
| Sc | moist BL capped by an inversion (lapse rate 0-1.5 km < 4 K/km), dry above; or Cu spread under a low lid | base LCL 0.4-1.5 km, top at the lid |
| Ns | stratiform precipitation (no convection), RH850 > 0.9 | base 0.3-1.5 km, top 4.5-7 km |
| As | large-scale ascent (fronts) with moist mid levels | base 2.5-4 km, 2-3.5 km thick, lowers as it moistens |
| Ac | moderately moist mid levels, steep mid-level lapse rate (castellanus when steeper) | base 2.5-5 km, 0.3-0.8 km thick |
| Ci | upper troposphere moist w.r.t. ice (new prognostic `UpperRH`, advected by the 500 hPa wind, fed by fronts and anvils) | base where T = -38 C (6-11 km), 0.5-1.5 km |
| Cs | as Ci, wide and saturated under large-scale ascent (far ahead of a warm front) | same, 1-2 km |
| Cc | as Ci with an unstable / wavy upper layer (steep lapse rate) | same, thin |

## Data to the renderer

Two new RGBA8 maps next to the weather map (`LayerMapA`, `LayerMapB`):
A = mid cover, mid base (/10 km), mid form (0 As .. 1 Ac), low form (0 St .. 0.5 Sc .. 1 Cu);
B = high cover, high base (/16 km), high form (0 Cs .. 0.5 Ci .. 1 Cc), spare.

## Not now (TODO)

Species and accessories beyond the above: lenticularis (orographic waves), castellanus / floccus as separate
shapes, uncinus hooks, volutus, pileus, velum, pannus, virga, cavum, asperitas, fluctus, murus flumen / cauda,
banner cloud, foehn wall, sea of clouds, contrails, iridescence, varieties (undulatus, radiatus, lacunosus...),
nacreous and noctilucent clouds.
