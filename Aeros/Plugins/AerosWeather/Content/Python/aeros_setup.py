# Copyright Aeros. All Rights Reserved.
#
# Aeros Weather - one-click asset setup (Unreal Editor, Python Script Plugin).
#
# Creates in the plugin content folder (/AerosWeather):
#   Materials/MPC_AerosWeather          Material Parameter Collection fed by the plugin
#   Materials/T_AerosDefaultWeather     tiny linear texture (default for texture parameters)
#   Materials/M_AerosVolumetricCloud    Volumetric Cloud material driven by the weather map
#   Materials/M_AerosGround             landscape ground: grass / sand / rock / snow masks + wetness, snow, hail, ash
#   Materials/M_AerosSea                opaque sea and lakes (plane at sea level), small moving ripples
#   Materials/M_AerosWater              translucent water (floods, storm surge, tsunami)
#   Materials/M_AerosEmissive           unlit glow (lightning bolts, lava)
#   Materials/M_AerosShaft              soft translucent volume (rain shafts, tornado funnel)
#   Materials/M_AerosLightning          lightning channel glow, per-segment brightness (instance custom data 0)
#   Materials/M_AerosTornadoFunnel      tornado condensation funnel: rotating striations, soft edges, vertex alpha
#   Maps/L_AerosDemo                    demo level, GameMode Override = AerosDemoGameMode
#
# Each step is independent: if one fails, the others still run, and a summary is
# printed at the end of the Output Log ("[Aeros] SUMMARY").
#
# Run it from the editor: Tools > Execute Python Script... > aeros_setup.py
# or in the Output Log (Python): exec(open(r"<path>/aeros_setup.py").read())
# Safe to run again: existing assets are reused.

import os
import struct
import tempfile
import zlib

import unreal

ROOT = "/AerosWeather"
MAT_DIR = ROOT + "/Materials"
MAP_PATH = ROOT + "/Maps/L_AerosDemo"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary

MPC_SCALARS = [
    "AerosWetness", "AerosSnowDepth", "AerosHailCover", "AerosRainIntensity", "AerosSnowIntensity",
    "AerosHailIntensity", "AerosCloudCover", "AerosTemperature", "AerosWindSpeed", "AerosLightningFlash",
    "AerosFogDensity", "AerosFloodDepth", "AerosDust", "AerosAshDepth", "AerosAshFall", "AerosBlowingSnow",
    "AerosShake", "AerosSeaLevel",
]
MPC_VECTORS = ["AerosWind", "AerosDomain"]


def log(msg):
    unreal.log("[Aeros] " + msg)


def get_or_create(name, path, cls, factory):
    full = path + "/" + name
    if eal.does_asset_exist(full):
        return eal.load_asset(full)
    return asset_tools.create_asset(name, path, cls, factory)


# ---------------------------------------------------------------- helpers
def node(material, cls, x, y, **props):
    expr = mel.create_material_expression(material, cls, x, y)
    for key, value in props.items():
        expr.set_editor_property(key, value)
    return expr


def link(src, src_out, dst, dst_in):
    mel.connect_material_expressions(src, src_out, dst, dst_in)


def mask(material, src, x, y, r=False, g=False, b=False, a=False, src_out=""):
    m = node(material, unreal.MaterialExpressionComponentMask, x, y, r=r, g=g, b=b, a=a)
    link(src, src_out, m, "")
    return m


def binop(material, cls, a, b, x, y, a_out="", b_out=""):
    n = node(material, cls, x, y)
    link(a, a_out, n, "A")
    link(b, b_out, n, "B")
    return n


def const(material, value, x, y):
    return node(material, unreal.MaterialExpressionConstant, x, y, r=value)


# ---------------------------------------------------------------- MPC
def create_mpc():
    mpc = get_or_create("MPC_AerosWeather", MAT_DIR, unreal.MaterialParameterCollection,
                        unreal.MaterialParameterCollectionFactoryNew())
    scalars = []
    for name in MPC_SCALARS:
        p = unreal.CollectionScalarParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", 0.0)
        scalars.append(p)
    vectors = []
    for name in MPC_VECTORS:
        p = unreal.CollectionVectorParameter()
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", unreal.LinearColor(0, 0, 1, 1))
        vectors.append(p)
    mpc.set_editor_property("scalar_parameters", scalars)
    mpc.set_editor_property("vector_parameters", vectors)
    eal.save_loaded_asset(mpc)
    log("MPC ready: " + mpc.get_path_name())
    return mpc


# ---------------------------------------------------------------- default linear texture
def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def create_default_texture():
    full = MAT_DIR + "/T_AerosDefaultWeather"
    if not eal.does_asset_exist(full):
        tmp = os.path.join(tempfile.gettempdir(), "T_AerosDefaultWeather.png")
        write_png(tmp, 4, 4, [0, 0, 0, 0])
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", tmp)
        task.set_editor_property("destination_path", MAT_DIR)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", False)
        asset_tools.import_asset_tasks([task])
    tex = eal.load_asset(full)
    tex.set_editor_property("srgb", False)
    tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    eal.save_loaded_asset(tex)
    return tex


# ---------------------------------------------------------------- volumetric cloud material
# Density is computed in HLSL (Custom node) from:
#   Wx  weather map  (R cover, G base 0-5 km, B top 0-16 km, A precipitation)
#   Sx  shape map    (R anvil cover, G updraft, B gust front / shelf cloud, A convective activity)
#   VP/VC voxel grids (previous / current) around the viewer, built from the simulation (FAerosCloudVolume)
#   N   3D Perlin-Worley shape noise (engine volume texture, single channel, 6 km tiles: km-scale lobes)
#   W   same shape noise at 9 km tiles, offset: smooth decorrelated warp (N and W read at mip >= 3)
#   N2  same shape noise at 4 km tiles (mip >= 2): 250-800 m turrets; nothing finer than ~200 m, which the
#       100-200 m ray steps cannot resolve (it flickered as glitter even at x1)
#   E   3D Worley erosion noise (4 km tiles; smaller tiles alias into grain at a few km)
#   F   same erosion noise stretched (16 km x 16 km x 3 km tiles): fibrous ice in anvils
#   N2, E, F are read at a mip chosen by the distance to the camera (1 + log2(d / 4 km), 1-4)
# Placement: a cellular cumulus field (1.1 km cells, one puff per cell with probability = cover), shallow
# in fair weather, growing into towers where the column is convective. In the spirit of Horizon / Nubis, 3D noises then
# erode the edges (billowy cauliflower at the top, wispy at the base), flat dark bases. Extra layers: anvils near the tropopause
# spreading downwind, layered shelf clouds along gust fronts.
CLOUD_HLSL = r"""
float cover   = saturate(Wx.r);
float baseKm  = Wx.g * 5.0;
float topKm   = max(Wx.b * 16.0, baseKm + 0.25);
float anvilCv = Sx.r;
float updraft = Sx.g;
float gust    = Sx.b;
float conv    = Sx.a;
float h       = saturate((AltKm - baseKm) / (topKm - baseKm));

// --- Where the clouds are: a cellular cumulus field (1.1 km cells). Each cell holds a cloud with
// probability = cover. Fair-weather cumulus stay shallow (humilis / mediocris, 0.6-2.5 km deep);
// only where the column is convective (conv) do they grow into congestus and towers up to the top.
float towerK  = smoothstep(0.3, 0.7, conv);
float cuDepth = lerp(0.6 + 1.9 * cover, topKm - baseKm, towerK);

// The engine cloud noises are single-channel (R = G = B, A = 1) and squeezed into a narrow range
// (shape: 80 % of the texels between 0.83 and 0.94, erosion: 0.57-0.82): stretch them to 0-1.
float lowFbm  = saturate((N.r  - 0.78) / 0.18);   // billowy Perlin-Worley, 6 km tiles: km-scale lobes
float bubbles = saturate((N2.r - 0.78) / 0.18);   // same noise, 4 km tiles, mip >= 2: 250-800 m turrets
float highFbm = saturate((E.r  - 0.45) / 0.45);   // Worley, 4 km tiles: 100 m detail
float highMod = lerp(highFbm, 1.0 - highFbm, saturate(h * 3.0));   // wispy base, billowy top
float fib     = saturate((F.r  - 0.45) / 0.45);   // erosion noise stretched 10x horizontally: ice fibres

float warpY   = saturate((W.r - 0.78) / 0.18);   // decorrelated smooth twin of the shape noise (9 km tiles)
float2 q = P.xy / 1.1 + float2(lowFbm - 0.5, warpY - 0.5) * 0.6;   // domain warp by smooth noise only
float2 qi = floor(q);
float shape = -1.0;   // 1 at a cloud centre, 0 on its outline, < 0 outside
for (int j = -1; j <= 1; j++)
{
    for (int i = -1; i <= 1; i++)
    {
        float2 c = qi + float2(i, j);
        float3 r = frac(sin(float3(dot(c, float2(127.1, 311.7)), dot(c, float2(269.5, 183.3)), dot(c, float2(419.2, 371.9)))) * 43758.5453);
        if (r.z > cover * 1.1) continue;
        // Each cloud has its own top (45-100 % of the depth): a field of uneven cumulus
        float topCell = baseKm + cuDepth * (0.45 + 0.55 * frac(r.y * 7.13));
        topCell -= (topCell - baseKm) * 0.4 * (1.0 - bubbles);   // turrets of uneven height
        if (AltKm > topCell) continue;
        float hc = saturate((AltKm - baseKm) / max(topCell - baseKm, 0.1));
        // Near-vertical sides, rounded top: a cumulus, not a hemisphere
        float side = sqrt(saturate(1.0 - hc * hc * hc));
        float2 center = c + 0.2 + 0.6 * r.xy;
        float radius = (0.3 + 0.35 * cover) * (0.7 + 0.6 * r.x) * side + updraft * 0.4;
        radius *= lerp(1.0, 0.75 + 0.5 * lowFbm, towerK);   // towers: bulging turrets up the sides
        shape = max(shape, 1.0 - length(q - center) / max(radius, 0.01));
    }
}

// Nubis-style carving: the cell field only gives the coverage; Worley bubbles (150-600 m) decide
// what is cloud. In the core every bubble survives, near the outline only the biggest ones:
// a cauliflower made of many turrets, not one ball. Then 100 m erosion and a crisp, dense edge.
float baseCut = saturate((AltKm - baseKm) / 0.08);
float cv      = saturate(shape * 1.6 + 0.35) * baseCut;
// Far away (30-70 km) a 1 km cumulus is a few traced pixels and turned into square blocks: the field melts
// into a soft coverage (a hazy cumulus carpet on the horizon, as seen in reality)
float farK    = smoothstep(30.0, 70.0, DistKm);
float cvFar   = saturate(cover * 1.4 - (1.0 - lowFbm) * 0.6) * baseCut * step(AltKm, baseKm + cuDepth);
cv = lerp(cv, cvFar, farK);
// Solid core (no holes for the jittered ray steps to flicker on); near the outline only the larger
// bubbles survive. Gentle slope (x2): hard thresholds on noise alias into glitter.
float cloud   = saturate((bubbles - (1.0 - cv) * 1.2 - highMod * 0.1) * 2.0 + cv * 0.6 - 0.1);
cloud *= lerp(smoothstep(0.25, 0.45, cv), saturate(cv * 2.0), farK);   // no crumbs outside the outline (white glitter)

// --- Anvil: flat sharp top at the tropopause, ragged base, thinner where the anvil cover fades
float anvilBase = 12.4 - (0.5 + 2.0 * anvilCv) * (0.75 + 0.5 * lowFbm);
float anvilBand = saturate((AltKm - anvilBase) / 0.6) * saturate((12.4 - AltKm) / 0.25);
// Ragged edges: the noise eats the thin parts first (distant anvils were flat glass plates)
float anvil     = anvilBand * saturate((anvilCv * 1.5 - (1.0 - lowFbm) * 0.6 - (1.0 - fib) * 0.35) * 2.0);

// --- Shelf cloud: layered wedge between 0.4 and 1.9 km along the gust front
float shelfBand = saturate((AltKm - 0.45) / 0.08) * saturate((1.9 - AltKm) / 0.6);
float layers    = 0.6 + 0.4 * sin(AltKm * 38.0 + lowFbm * 5.0);
float shelf     = gust * shelfBand * layers * saturate(lowFbm * 1.5 - 0.2);

// --- Voxel grid (storm anatomy synthesised from the simulation), eroded by the same noises.
// Inside the grid storms / anvils / shelves come from the voxels; the 2D path only keeps the
// fair-weather cumulus there, and takes over beyond the grid (35-40 km fade).
float2 edgeUV = abs(VU.xy - 0.5) * 2.0;
float voxIn   = Weight * (1.0 - smoothstep(0.875, 1.0, max(edgeUV.x, edgeUV.y)));
float voxRaw  = lerp(VP.r, VC.r, Blend);
float voxDark = lerp(VP.g, VC.g, Blend);
// Liquid cloud (below ~8 km) bubbles into cauliflower turrets; glaciated cloud above (anvil, upper
// tower) is smooth and fibrous: eroded by the horizontally stretched noise, not carved by bubbles.
float iceK    = smoothstep(9.0, 11.5, AltKm);
float voxCv   = saturate(voxRaw * 2.0);   // a narrow carved rim, solid core
float voxCu   = saturate((bubbles - (1.0 - voxCv) * 1.2 - highMod * 0.1) * 2.0 + voxCv * 0.6 - 0.1);   // turrets on a solid core
voxCu *= smoothstep(0.15, 0.35, voxCv);   // no crumbs outside the outline
float voxIce  = saturate((voxRaw - (1.0 - lowFbm) * 0.2 - (1.0 - fib) * 0.12) * 2.5);
float vox     = lerp(voxCu, voxIce, iceK);

float full2D      = max(cloud, max(anvil, shelf * 0.9));
float cumulusOnly = cloud * (1.0 - smoothstep(0.2, 0.4, conv));
float density = lerp(full2D, max(cumulusOnly, vox), voxIn);
// Rain greys only the lower cloud (liquid, below ~6 km); the glaciated tops stay white
float rainDark = saturate(Wx.a * 1.2) * (1.0 - smoothstep(4.0, 7.0, AltKm));
float dark    = lerp(rainDark, max(rainDark * 0.4, voxDark), voxIn);
return float4(density, dark, anvil, shelf);
"""


def create_cloud_material(default_tex):
    mat = get_or_create("M_AerosVolumetricCloud", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_VOLUME)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)

    # UV = (WorldXY - Domain.xy) / Domain.zw + 0.5   (vector parameters: B/A come from the RGBA output)
    wp = node(mat, unreal.MaterialExpressionWorldPosition, -2200, 0)
    domain = node(mat, unreal.MaterialExpressionVectorParameter, -2200, 200, parameter_name="AerosDomain",
                  default_value=unreal.LinearColor(0, 0, 32000000, 32000000))
    wp_xy = mask(mat, wp, -2000, 0, r=True, g=True)
    d_xy = mask(mat, domain, -2000, 200, r=True, g=True, src_out="RGBA")
    d_zw = mask(mat, domain, -2000, 300, b=True, a=True, src_out="RGBA")
    rel = binop(mat, unreal.MaterialExpressionSubtract, wp_xy, d_xy, -1800, 50)
    uv0 = binop(mat, unreal.MaterialExpressionDivide, rel, d_zw, -1650, 100)
    uv = binop(mat, unreal.MaterialExpressionAdd, uv0, const(mat, 0.5, -1650, 250), -1500, 100)

    weather = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1300, 0,
                   parameter_name="AerosWeatherMap", texture=default_tex,
                   sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(uv, "", weather, "UVs")
    shape_map = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, -1300, 250,
                     parameter_name="AerosCloudShapeMap", texture=default_tex,
                     sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(uv, "", shape_map, "UVs")

    # Altitude (km) inside the layer set by the controller: 0.3 km + 15 km * normalized height
    attrib = node(mat, unreal.MaterialExpressionCloudSampleAttribute, -1300, 500)
    alt = binop(mat, unreal.MaterialExpressionMultiply, attrib, const(mat, 15.0, -1300, 650), -1100, 500, a_out="NormAltitudeInLayer")
    alt = binop(mat, unreal.MaterialExpressionAdd, alt, const(mat, 0.3, -1100, 650), -950, 500)

    # 3D noise in km, drifting with the simulated wind
    wp_km = binop(mat, unreal.MaterialExpressionMultiply, wp, const(mat, 0.00001, -2200, 450), -2000, 450)
    drift = node(mat, unreal.MaterialExpressionVectorParameter, -2200, 600, parameter_name="AerosNoiseOffset",
                 default_value=unreal.LinearColor(0, 0, 0, 0))
    pos = binop(mat, unreal.MaterialExpressionSubtract, wp_km, drift, -1800, 500)
    uvw_shape = binop(mat, unreal.MaterialExpressionMultiply, pos, const(mat, 1.0 / 6.0, -1800, 650), -1600, 550)
    uvw_erosion = binop(mat, unreal.MaterialExpressionMultiply, pos, const(mat, 1.0 / 4.0, -1800, 750), -1600, 700)
    uvw_bubbles = binop(mat, unreal.MaterialExpressionMultiply, pos, const(mat, 1.0 / 4.0, -1800, 1300), -1600, 1300)
    # Mip 1 for the shape noises: their 1-2 texel detail is sub-pixel a few km away and sparkles.
    # The detail noises (bubbles, erosion, fibres) go further: mip = 1 + log2(distance / 4 km), 1-4,
    # a real LOD (at 20+ km even mip 1 detail was sub-pixel speckle on anvils and towers).
    mip1 = dict(mip_value_mode=unreal.TextureMipValueMode.TMVM_MIP_LEVEL, const_mip_value=1)
    cam_dist = node(mat, unreal.MaterialExpressionDistance, -2200, 1700)
    link(wp, "", cam_dist, "A")
    link(node(mat, unreal.MaterialExpressionCameraPositionWS, -2350, 1750), "", cam_dist, "B")
    lod = binop(mat, unreal.MaterialExpressionDivide, cam_dist, const(mat, 400000.0, -2100, 1800), -2000, 1700)
    lod = binop(mat, unreal.MaterialExpressionMax, lod, const(mat, 1.0, -2000, 1800), -1900, 1700)
    lod_log = node(mat, unreal.MaterialExpressionLogarithm2, -1800, 1700)
    link(lod, "", lod_log, "")
    lod = binop(mat, unreal.MaterialExpressionAdd, lod_log, const(mat, 1.0, -1800, 1800), -1700, 1700)
    lod = binop(mat, unreal.MaterialExpressionMin, lod, const(mat, 3.0, -1700, 1800), -1600, 1700)
    # Filtered + wrapped sampler from the world texture group: with the asset's own sampler the coarse
    # mips came out as 1 km square blocks on distant cumulus
    mip_lod = dict(mip_value_mode=unreal.TextureMipValueMode.TMVM_MIP_LEVEL,
                   sampler_source=unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)

    def link_mip(sample):
        for pin in ["Level", "MipLevel"]:   # the pin is named "Level" in MipLevel mode
            try:
                if mel.connect_material_expressions(lod, "", sample, pin):
                    return
            except Exception:
                pass
        unreal.log_warning("[Aeros] cloud material: could not connect the LOD mip input")
    # Shape noise and a decorrelated twin (9 km tiles, offset) at mip >= 3 (~375 m detail): they drive the
    # warps, and a warp driven by fine detail turns the lookups into glitter
    lod_smooth = binop(mat, unreal.MaterialExpressionAdd, lod, const(mat, 2.0, -1600, 1950), -1500, 1900)

    def link_lod(src, sample):
        for pin in ["Level", "MipLevel"]:
            if mel.connect_material_expressions(src, "", sample, pin):
                return
        unreal.log_warning("[Aeros] cloud material: could not connect a LOD mip input")
    noise_shape = node(mat, unreal.MaterialExpressionTextureSample, -1300, 800, **mip_lod,
                       texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape128"))
    link(uvw_shape, "", noise_shape, "UVs")
    link_lod(lod_smooth, noise_shape)
    uvw_warp = binop(mat, unreal.MaterialExpressionMultiply, pos, const(mat, 1.0 / 9.0, -1800, 2000), -1700, 2000)
    uvw_warp = binop(mat, unreal.MaterialExpressionAdd, uvw_warp,
                     node(mat, unreal.MaterialExpressionConstant3Vector, -1800, 2100, constant=unreal.LinearColor(0.37, 0.11, 0.53, 1)),
                     -1600, 2000)
    noise_warp = node(mat, unreal.MaterialExpressionTextureSample, -1300, 2000, **mip_lod,
                      texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape128"))
    link(uvw_warp, "", noise_warp, "UVs")
    link_lod(lod_smooth, noise_warp)
    noise_erosion = node(mat, unreal.MaterialExpressionTextureSample, -1300, 1050, **mip_lod,
                         texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32"))
    link(uvw_erosion, "", noise_erosion, "UVs")
    link_mip(noise_erosion)
    uvw_fib = binop(mat, unreal.MaterialExpressionMultiply, pos,
                    node(mat, unreal.MaterialExpressionConstant3Vector, -1800, 1550, constant=unreal.LinearColor(1.0 / 16.0, 1.0 / 16.0, 1.0 / 3.0, 1)),
                    -1600, 1500)
    noise_fib = node(mat, unreal.MaterialExpressionTextureSample, -1300, 1500, **mip_lod,
                     texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32"))
    link(uvw_fib, "", noise_fib, "UVs")
    link_mip(noise_fib)
    noise_bubbles = node(mat, unreal.MaterialExpressionTextureSample, -1300, 1250, **mip_lod,
                         texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape128"))
    link(uvw_bubbles, "", noise_bubbles, "UVs")
    lod_bubbles = binop(mat, unreal.MaterialExpressionAdd, lod, const(mat, 1.0, -1600, 1850), -1500, 1800)
    for pin in ["Level", "MipLevel"]:
        if mel.connect_material_expressions(lod_bubbles, "", noise_bubbles, pin):
            break

    # Voxel domain warp (+-1.2 km): the engine noise is single-channel, so take R of two samples at
    # different scales and stretch them (0.78-0.96 -> 0-1)
    warp_xy = node(mat, unreal.MaterialExpressionAppendVector, -1900, 1300)
    link(mask(mat, noise_shape, -2050, 1250, r=True, src_out="RGBA"), "", warp_xy, "A")
    link(mask(mat, noise_warp, -2050, 1320, r=True, src_out="RGBA"), "", warp_xy, "B")
    warp_xy = binop(mat, unreal.MaterialExpressionSubtract, warp_xy, const(mat, 0.78, -1900, 1380), -1800, 1300)
    warp_xy = binop(mat, unreal.MaterialExpressionDivide, warp_xy, const(mat, 0.18, -1800, 1380), -1700, 1300)
    warp_sat = node(mat, unreal.MaterialExpressionSaturate, -1600, 1300)
    link(warp_xy, "", warp_sat, "")
    warp_c = binop(mat, unreal.MaterialExpressionSubtract, warp_sat, const(mat, 0.5, -1600, 1380), -1500, 1300)
    warp_xy = binop(mat, unreal.MaterialExpressionMultiply, warp_c, const(mat, 160000.0, -1500, 1380), -1400, 1300)
    # Vertical warp of the lookup (km), upper levels only (bases stay flat): +-0.5 km swells (6 km tiles)
    # and +-0.2 km lumps (3 km tiles) -> the anvil top is no longer a flat white plate
    z_warp = node(mat, unreal.MaterialExpressionDotProduct, -1500, 1450)
    link(warp_c, "", z_warp, "A")
    link(node(mat, unreal.MaterialExpressionConstant2Vector, -1600, 1500, r=1.0, g=0.4), "", z_warp, "B")
    z_fade = binop(mat, unreal.MaterialExpressionSubtract, alt, const(mat, 6.0, -1600, 1580), -1500, 1550)
    z_fade = binop(mat, unreal.MaterialExpressionDivide, z_fade, const(mat, 3.0, -1500, 1620), -1400, 1550)
    z_fade_sat = node(mat, unreal.MaterialExpressionSaturate, -1300, 1550)
    link(z_fade, "", z_fade_sat, "")
    z_warp = binop(mat, unreal.MaterialExpressionMultiply, z_warp, z_fade_sat, -1300, 1450)
    alt_warped = binop(mat, unreal.MaterialExpressionAdd, alt, z_warp, -1200, 1450)

    # Voxel grids: UVW = ((WorldXY - Rect.xy) / Rect.z + 0.5, AltKm / Rect.w), one rect per buffer
    default_volume = unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape64")
    vox_samples = {}
    vox_uvw = {}
    for k, (tex_name, rect_name) in enumerate([("AerosCloudVolumePrev", "AerosCloudVolumeRectPrev"),
                                               ("AerosCloudVolumeCur", "AerosCloudVolumeRectCur")]):
        y = 1400 + 350 * k
        rect = node(mat, unreal.MaterialExpressionVectorParameter, -2200, y, parameter_name=rect_name,
                    default_value=unreal.LinearColor(0, 0, 8000000, 16))
        r_xy = mask(mat, rect, -2000, y, r=True, g=True, src_out="RGBA")
        r_z = mask(mat, rect, -2000, y + 80, b=True, src_out="RGBA")
        r_w = mask(mat, rect, -2000, y + 160, a=True, src_out="RGBA")
        rel_v = binop(mat, unreal.MaterialExpressionSubtract, wp_xy, r_xy, -1850, y)
        uv_v = binop(mat, unreal.MaterialExpressionDivide, rel_v, r_z, -1700, y)
        uv_v = binop(mat, unreal.MaterialExpressionAdd, uv_v, const(mat, 0.5, -1700, y + 80), -1550, y)
        # Domain warp by the shape noise (+-1.2 km): breaks the voxel cylinders into bulging turrets
        warp = binop(mat, unreal.MaterialExpressionDivide, warp_xy, r_z, -1400, y + 200)
        uv_v = binop(mat, unreal.MaterialExpressionAdd, uv_v, warp, -1450, y + 40)
        w_v = binop(mat, unreal.MaterialExpressionDivide, alt_warped, r_w, -1550, y + 120)
        uvw_v = node(mat, unreal.MaterialExpressionAppendVector, -1400, y)
        link(uv_v, "", uvw_v, "A")
        link(w_v, "", uvw_v, "B")
        sample = node(mat, unreal.MaterialExpressionTextureSampleParameterVolume, -1250, y,
                      parameter_name=tex_name, texture=default_volume)
        link(uvw_v, "", sample, "UVs")
        vox_samples[k] = sample
        vox_uvw[k] = uvw_v
    blend = node(mat, unreal.MaterialExpressionScalarParameter, -1250, 2150, parameter_name="AerosCloudVolumeBlend", default_value=1.0)
    weight = node(mat, unreal.MaterialExpressionScalarParameter, -1250, 2250, parameter_name="AerosCloudVolumeWeight", default_value=0.0)

    custom = node(mat, unreal.MaterialExpressionCustom, -700, 400)
    custom.set_editor_property("description", "AerosCloudDensity")
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    inputs = []
    for name in ["AltKm", "P", "Wx", "Sx", "N", "N2", "W", "E", "F", "VP", "VC", "VU", "Blend", "Weight", "DistKm"]:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    custom.set_editor_property("code", CLOUD_HLSL)
    link(alt, "", custom, "AltKm")
    link(pos, "", custom, "P")
    link(weather, "RGBA", custom, "Wx")
    link(shape_map, "RGBA", custom, "Sx")
    link(noise_shape, "RGBA", custom, "N")
    link(noise_erosion, "RGBA", custom, "E")
    link(noise_bubbles, "RGBA", custom, "N2")
    link(noise_warp, "RGBA", custom, "W")
    link(binop(mat, unreal.MaterialExpressionDivide, cam_dist, const(mat, 100000.0, -900, 2300), -800, 2300), "", custom, "DistKm")
    link(noise_fib, "RGBA", custom, "F")
    link(vox_samples[0], "RGBA", custom, "VP")
    link(vox_samples[1], "RGBA", custom, "VC")
    link(vox_uvw[1], "", custom, "VU")
    link(blend, "", custom, "Blend")
    link(weight, "", custom, "Weight")

    density = mask(mat, custom, -450, 300, r=True)
    dark = mask(mat, custom, -450, 450, g=True)

    # Extinction (1/m): denser in precipitation cores
    dark_mul = binop(mat, unreal.MaterialExpressionMultiply, dark, const(mat, 3.0, -300, 550), -150, 500)
    dark_mul = binop(mat, unreal.MaterialExpressionAdd, dark_mul, const(mat, 1.0, -150, 600), 0, 500)
    ext = binop(mat, unreal.MaterialExpressionMultiply, density, const(mat, 0.08, -300, 350), -150, 300)
    ext = binop(mat, unreal.MaterialExpressionMultiply, ext, dark_mul, 50, 350)
    mel.connect_material_property(ext, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)

    # Albedo: bright white tops, slightly grey in storm cores
    albedo = node(mat, unreal.MaterialExpressionLinearInterpolate, 50, 100)
    link(node(mat, unreal.MaterialExpressionConstant3Vector, -150, 50, constant=unreal.LinearColor(0.92, 0.92, 0.94, 1)), "", albedo, "A")
    link(node(mat, unreal.MaterialExpressionConstant3Vector, -150, 150, constant=unreal.LinearColor(0.55, 0.57, 0.6, 1)), "", albedo, "B")
    link(dark, "", albedo, "Alpha")
    mel.connect_material_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # Lightning: local glow, (1 - d / 12 km)^2 around the strike
    flash = node(mat, unreal.MaterialExpressionScalarParameter, -450, 900, parameter_name="AerosLightningFlash", default_value=0.0)
    lpos = node(mat, unreal.MaterialExpressionVectorParameter, -450, 1000, parameter_name="AerosLightningPos",
                default_value=unreal.LinearColor(0, 0, -1.0e9, 1))
    dist = node(mat, unreal.MaterialExpressionDistance, -300, 1000)
    link(wp, "", dist, "A")
    link(lpos, "", dist, "B")
    ratio = binop(mat, unreal.MaterialExpressionDivide, dist, const(mat, 1200000.0, -300, 1100), -150, 1050)
    near = node(mat, unreal.MaterialExpressionOneMinus, 0, 1050)
    link(ratio, "", near, "")
    near_sat = node(mat, unreal.MaterialExpressionSaturate, 100, 1050)
    link(near, "", near_sat, "")
    falloff = binop(mat, unreal.MaterialExpressionMultiply, near_sat, near_sat, 200, 1050)
    glow = binop(mat, unreal.MaterialExpressionMultiply, density, flash, 50, 900)
    glow = binop(mat, unreal.MaterialExpressionMultiply, glow, falloff, 300, 950)
    glow = binop(mat, unreal.MaterialExpressionMultiply, glow, const(mat, 6.0, 300, 1100), 450, 950)
    mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    advanced = node(mat, unreal.MaterialExpressionVolumetricAdvancedMaterialOutput, 700, 1200)
    for key, value in [("const_phase_g", 0.6), ("const_phase_g2", -0.25), ("const_phase_blend", 0.3),
                       ("multi_scattering_approximation_octave_count", 2), ("const_multi_scattering_contribution", 0.25),
                       ("const_multi_scattering_occlusion", 0.8), ("const_multi_scattering_eccentricity", 0.5)]:
        try:
            advanced.set_editor_property(key, value)
        except Exception as e:
            unreal.log_warning("[Aeros] cloud advanced output: could not set {}: {}".format(key, e))

    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Cloud material ready: " + mat.get_path_name())
    return mat


def sampler_for(tex):
    """Material sampler type matching a texture (a mismatch fails the compile -> default material)."""
    S = unreal.MaterialSamplerType
    TC = unreal.TextureCompressionSettings
    comp = tex.get_editor_property("compression_settings")
    srgb = tex.get_editor_property("srgb")
    if comp == TC.TC_NORMALMAP:
        return S.SAMPLERTYPE_NORMAL
    if comp == TC.TC_GRAYSCALE:
        return S.SAMPLERTYPE_GRAYSCALE if srgb else S.SAMPLERTYPE_LINEAR_GRAYSCALE
    if comp == TC.TC_ALPHA:
        return S.SAMPLERTYPE_ALPHA
    if comp == TC.TC_MASKS:
        return S.SAMPLERTYPE_MASKS
    return S.SAMPLERTYPE_COLOR if srgb else S.SAMPLERTYPE_LINEAR_COLOR


NOISE_TEX = "/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq"


def world_xy(mat, x, y):
    wp = node(mat, unreal.MaterialExpressionWorldPosition, x, y)
    return mask(mat, wp, x + 150, y, r=True, g=True)


def noise_sample(mat, tex, xy, tile_cm, x, y, offset=None):
    """Noise texture sampled in world space: one tile = tile_cm (plus an optional UV offset node)."""
    uv = binop(mat, unreal.MaterialExpressionDivide, xy, const(mat, tile_cm, x - 150, y + 60), x, y)
    if offset is not None:
        uv = binop(mat, unreal.MaterialExpressionAdd, uv, offset, x + 80, y)
    s = node(mat, unreal.MaterialExpressionTextureSample, x + 200, y, texture=tex, sampler_type=sampler_for(tex))
    link(uv, "", s, "UVs")
    return s


# ---------------------------------------------------------------- ground material
def create_ground_material(mpc):
    """Procedural landscape ground (AAerosDemoTerrain). Vertex colour = surface masks computed on the CPU:
    R sand (beaches, deserts), G rock (steep slopes, high ground), B snow (peaks), A dry grass.
    World-space noise breaks the flat colours. Then the weather: wet, snow / hail cover, volcanic ash.
    Rough everywhere (>= 0.45 even wet): no blurry sky mirror at grazing angles."""
    mat = get_or_create("M_AerosGround", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)

    def p(name, x, y):
        return node(mat, unreal.MaterialExpressionCollectionParameter, x, y, collection=mpc, parameter_name=name)

    def color(r, g, b, x, y):
        return node(mat, unreal.MaterialExpressionConstant3Vector, x, y, constant=unreal.LinearColor(r, g, b, 1))

    def lerp(a, b, alpha, x, y, a_out="", b_out="", alpha_out=""):
        n = node(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
        link(a, a_out, n, "A"); link(b, b_out, n, "B"); link(alpha, alpha_out, n, "Alpha")
        return n

    # Surface masks (x TerrainMasks: 0 on meshes without vertex colours, which read as white)
    vc = node(mat, unreal.MaterialExpressionVertexColor, -2000, 0)
    use_masks = node(mat, unreal.MaterialExpressionScalarParameter, -2000, 200, parameter_name="TerrainMasks", default_value=1.0)
    # VertexColor has no RGBA output: one masked channel at a time
    vm = {ch: binop(mat, unreal.MaterialExpressionMultiply, vc, use_masks, -1800, 100 * i, a_out=ch)
          for i, ch in enumerate("RGBA")}

    grass = lerp(color(0.075, 0.13, 0.035, -1800, -400), color(0.2, 0.18, 0.075, -1800, -300), vm["A"], -1600, -350)
    col = lerp(grass, color(0.5, 0.43, 0.3, -1600, -200), vm["R"], -1400, -300)
    col = lerp(col, color(0.2, 0.185, 0.17, -1400, -150), vm["G"], -1200, -250)
    col = lerp(col, color(0.82, 0.84, 0.88, -1200, -100), vm["B"], -1000, -200)

    # World-space variation: 7 m, 61 m and 530 m noise (albedo x 0.7-1.25)
    if eal.does_asset_exist(NOISE_TEX):
        tex = unreal.load_asset(NOISE_TEX)
        xy = world_xy(mat, -2000, 500)
        n1 = noise_sample(mat, tex, xy, 700.0, -1700, 450)
        n2 = noise_sample(mat, tex, xy, 6100.0, -1700, 600)
        n3 = noise_sample(mat, tex, xy, 53000.0, -1700, 750)
        s12 = binop(mat, unreal.MaterialExpressionAdd, n1, n2, -1300, 500, a_out="R", b_out="R")
        s123 = binop(mat, unreal.MaterialExpressionAdd, s12, n3, -1150, 550, b_out="R")
        n = binop(mat, unreal.MaterialExpressionMultiply, s123, const(mat, 0.3333, -1150, 650), -1000, 550)
        var = lerp(const(mat, 0.7, -900, 450), const(mat, 1.25, -900, 500), n, -800, 500)
        col = binop(mat, unreal.MaterialExpressionMultiply, col, var, -800, -150)
    else:
        unreal.log_warning("[Aeros] ground: no noise texture " + NOISE_TEX + ", flat colours")

    wet = p("AerosWetness", -900, 0)
    snow = p("AerosSnowDepth", -900, 150)
    hail = p("AerosHailCover", -900, 300)
    ash = p("AerosAshDepth", -900, 900)
    white = color(0.9, 0.92, 0.95, -900, 750)

    # Wet: darker albedo
    darken = lerp(const(mat, 1.0, -800, -100), const(mat, 0.6, -800, -50), wet, -600, -100)
    wet_col = binop(mat, unreal.MaterialExpressionMultiply, col, darken, -450, -150)

    # Snow cover and hail whitening
    snow_a = node(mat, unreal.MaterialExpressionSaturate, -600, 150)
    link(binop(mat, unreal.MaterialExpressionDivide, snow, const(mat, 4.0, -800, 220), -700, 150), "", snow_a, "")
    hail_a = binop(mat, unreal.MaterialExpressionMultiply, hail, const(mat, 0.8, -800, 360), -600, 300)
    ash_a = node(mat, unreal.MaterialExpressionSaturate, -600, 900)
    link(binop(mat, unreal.MaterialExpressionDivide, ash, const(mat, 20.0, -800, 980), -700, 900), "", ash_a, "")
    cover_a = node(mat, unreal.MaterialExpressionMax, -450, 220)
    link(snow_a, "", cover_a, "A"); link(hail_a, "", cover_a, "B")
    base = lerp(wet_col, white, cover_a, -300, 0)
    base_ash = lerp(base, color(0.22, 0.21, 0.2, -300, -200), ash_a, -150, 0)
    mel.connect_material_property(base_ash, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # Roughness: grass / sand 0.92, rock 0.82, snow 0.65; wet down to 0.45 (glossy, not a mirror)
    r0 = lerp(const(mat, 0.92, -600, 400), const(mat, 0.82, -600, 450), vm["G"], -450, 420)
    r1 = lerp(r0, const(mat, 0.65, -450, 500), vm["B"], -300, 450)
    r2 = lerp(r1, const(mat, 0.45, -300, 550), wet, -150, 450)
    r3 = lerp(r2, const(mat, 0.6, -150, 550), cover_a, 0, 450)
    mel.connect_material_property(r3, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(const(mat, 0.35, 0, 600), "", unreal.MaterialProperty.MP_SPECULAR)

    wpo_z = binop(mat, unreal.MaterialExpressionMultiply, snow, const(mat, 1.0, -450, 700), -300, 700)
    wpo = node(mat, unreal.MaterialExpressionAppendVector, -150, 700)
    link(node(mat, unreal.MaterialExpressionConstant2Vector, -300, 800, r=0.0, g=0.0), "", wpo, "A")
    link(wpo_z, "", wpo, "B")
    mel.connect_material_property(wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Ground material ready: " + mat.get_path_name())
    return mat


# ---------------------------------------------------------------- sea material
def create_sea_material():
    """Opaque sea and lakes (one plane at sea level under the landscape). Dark water, small moving
    ripples (normal from the gradient of two scrolling noise layers), ripples fade with distance so
    the far sea does not sparkle; specular reflection of the sky only at grazing angles."""
    mat = get_or_create("M_AerosSea", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    color = node(mat, unreal.MaterialExpressionConstant3Vector, -400, -200, constant=unreal.LinearColor(0.012, 0.04, 0.055, 1))
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(const(mat, 0.14, -400, -80), "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(const(mat, 0.5, -400, -20), "", unreal.MaterialProperty.MP_SPECULAR)

    if eal.does_asset_exist(NOISE_TEX):
        tex = unreal.load_asset(NOISE_TEX)
        xy = world_xy(mat, -2200, 200)
        t = node(mat, unreal.MaterialExpressionTime, -2200, 400)
        d = 1.0 / 64.0  # one texel of the 64 x 64 noise

        def layer(tile_cm, speed, y):
            drift = binop(mat, unreal.MaterialExpressionMultiply, t,
                          node(mat, unreal.MaterialExpressionConstant2Vector, -2050, y + 120, r=speed[0], g=speed[1]), -1900, y + 100)
            h0 = noise_sample(mat, tex, xy, tile_cm, -1700, y, drift)
            dx = binop(mat, unreal.MaterialExpressionAdd, drift,
                       node(mat, unreal.MaterialExpressionConstant2Vector, -1900, y + 200, r=d, g=0.0), -1800, y + 180)
            dy = binop(mat, unreal.MaterialExpressionAdd, drift,
                       node(mat, unreal.MaterialExpressionConstant2Vector, -1900, y + 300, r=0.0, g=d), -1800, y + 280)
            hx = noise_sample(mat, tex, xy, tile_cm, -1700, y + 180, dx)
            hy = noise_sample(mat, tex, xy, tile_cm, -1700, y + 280, dy)
            gx = binop(mat, unreal.MaterialExpressionSubtract, h0, hx, -1300, y + 100, a_out="R", b_out="R")
            gy = binop(mat, unreal.MaterialExpressionSubtract, h0, hy, -1300, y + 200, a_out="R", b_out="R")
            g = node(mat, unreal.MaterialExpressionAppendVector, -1150, y + 150)
            link(gx, "", g, "A"); link(gy, "", g, "B")
            return g

        g1 = layer(2400.0, (0.010, 0.006), 0)
        g2 = layer(700.0, (-0.017, 0.011), 450)
        g = binop(mat, unreal.MaterialExpressionAdd, g1, g2, -1000, 250)
        # Ripple strength fades from 2.5 near the camera to 0.3 at 3 km
        depth = node(mat, unreal.MaterialExpressionPixelDepth, -1150, 650)
        fade = node(mat, unreal.MaterialExpressionSaturate, -900, 650)
        link(binop(mat, unreal.MaterialExpressionDivide, depth, const(mat, 300000.0, -1150, 720), -1000, 650), "", fade, "")
        k = node(mat, unreal.MaterialExpressionLinearInterpolate, -750, 600)
        link(const(mat, 2.5, -900, 550), "", k, "A"); link(const(mat, 0.3, -900, 600), "", k, "B"); link(fade, "", k, "Alpha")
        gk = binop(mat, unreal.MaterialExpressionMultiply, g, k, -600, 300)
        nrm = node(mat, unreal.MaterialExpressionAppendVector, -450, 300)
        link(gk, "", nrm, "A"); link(const(mat, 1.0, -600, 400), "", nrm, "B")
        nn = node(mat, unreal.MaterialExpressionNormalize, -300, 300)
        link(nrm, "", nn, "")
        mel.connect_material_property(nn, "", unreal.MaterialProperty.MP_NORMAL)
    else:
        unreal.log_warning("[Aeros] sea: no noise texture " + NOISE_TEX + ", flat water")

    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Sea material ready: " + mat.get_path_name())
    return mat


# ---------------------------------------------------------------- water material
def create_water_material():
    mat = get_or_create("M_AerosWater", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE)
    color = node(mat, unreal.MaterialExpressionConstant3Vector, -400, 0, constant=unreal.LinearColor(0.05, 0.12, 0.1, 1))
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(const(mat, 0.05, -400, 150), "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(const(mat, 0.85, -400, 250), "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Water material ready: " + mat.get_path_name())
    return mat


# ---------------------------------------------------------------- effect materials
def create_emissive_material():
    """Unlit glow (lightning bolts, lava, markers): Color * Intensity."""
    mat = get_or_create("M_AerosEmissive", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    color = node(mat, unreal.MaterialExpressionVectorParameter, -500, 0, parameter_name="Color",
                 default_value=unreal.LinearColor(0.8, 0.85, 1.0, 1.0))
    inten = node(mat, unreal.MaterialExpressionScalarParameter, -500, 200, parameter_name="Intensity", default_value=20.0)
    glow = binop(mat, unreal.MaterialExpressionMultiply, color, inten, -250, 80)
    mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Emissive material ready: " + mat.get_path_name())
    return mat


def create_lightning_material():
    """Lightning channel: Color * Intensity * per-instance brightness (custom data 0, default 1)."""
    mat = get_or_create("M_AerosLightning", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    color = node(mat, unreal.MaterialExpressionVectorParameter, -700, 0, parameter_name="Color",
                 default_value=unreal.LinearColor(0.75, 0.82, 1.0, 1.0))
    inten = node(mat, unreal.MaterialExpressionScalarParameter, -700, 200, parameter_name="Intensity", default_value=50.0)
    seg = node(mat, unreal.MaterialExpressionPerInstanceCustomData, -700, 320, data_index=0, const_default_value=1.0)
    glow = binop(mat, unreal.MaterialExpressionMultiply, color, inten, -450, 80)
    glow = binop(mat, unreal.MaterialExpressionMultiply, glow, seg, -250, 150)
    mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Lightning material ready: " + mat.get_path_name())
    return mat


def create_tornado_material():
    """Tornado funnel on a procedural tube (UV: U = around, V = height; vertex alpha = fade).
    Rotating striations: 3D noise sampled on a cylinder whose angle advances with SpinTurns (turns/s)."""
    mat = get_or_create("M_AerosTornadoFunnel", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL)
    mat.set_editor_property("two_sided", True)

    uv = node(mat, unreal.MaterialExpressionTextureCoordinate, -1400, 0)
    u = mask(mat, uv, -1250, 0, r=True)
    v = mask(mat, uv, -1250, 100, g=True)
    spin = node(mat, unreal.MaterialExpressionScalarParameter, -1400, 200, parameter_name="SpinTurns", default_value=0.1)
    t = node(mat, unreal.MaterialExpressionTime, -1400, 300)
    turn = binop(mat, unreal.MaterialExpressionMultiply, t, spin, -1250, 250)
    # Vertical shear of the spin: the top turns a bit later than the base, the streaks spiral
    lag = binop(mat, unreal.MaterialExpressionMultiply, v, const(mat, 0.35, -1250, 350), -1100, 330)
    angle = binop(mat, unreal.MaterialExpressionAdd, u, turn, -1100, 100)
    angle = binop(mat, unreal.MaterialExpressionAdd, angle, lag, -950, 150)
    cos_a = node(mat, unreal.MaterialExpressionCosine, -800, 100, period=1.0)
    link(angle, "", cos_a, "")
    sin_a = node(mat, unreal.MaterialExpressionSine, -800, 200, period=1.0)
    link(angle, "", sin_a, "")
    cx = binop(mat, unreal.MaterialExpressionMultiply, cos_a, const(mat, 2.5, -800, 300), -650, 100)
    cy = binop(mat, unreal.MaterialExpressionMultiply, sin_a, const(mat, 2.5, -800, 350), -650, 200)
    cz = binop(mat, unreal.MaterialExpressionMultiply, v, const(mat, 14.0, -800, 450), -650, 300)
    xy = node(mat, unreal.MaterialExpressionAppendVector, -500, 150)
    link(cx, "", xy, "A")
    link(cy, "", xy, "B")
    xyz = node(mat, unreal.MaterialExpressionAppendVector, -380, 200)
    link(xy, "", xyz, "A")
    link(cz, "", xyz, "B")
    noise = node(mat, unreal.MaterialExpressionNoise, -250, 200, scale=1.0, levels=4, output_min=0.0, output_max=1.0)
    link(xyz, "", noise, "Position")

    streaks = node(mat, unreal.MaterialExpressionScalarParameter, -250, 400, parameter_name="Streaks", default_value=0.6)
    streak = node(mat, unreal.MaterialExpressionLinearInterpolate, -50, 300)
    link(const(mat, 1.0, -150, 300), "", streak, "A")
    link(noise, "", streak, "B")
    link(streaks, "", streak, "Alpha")

    color = node(mat, unreal.MaterialExpressionVectorParameter, -250, -150, parameter_name="Color",
                 default_value=unreal.LinearColor(0.45, 0.46, 0.48, 1.0))
    shade = binop(mat, unreal.MaterialExpressionMultiply, color, streak, 100, -100)
    mel.connect_material_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(const(mat, 0.9, 100, 0), "", unreal.MaterialProperty.MP_ROUGHNESS)

    opacity = node(mat, unreal.MaterialExpressionScalarParameter, -250, 550, parameter_name="Opacity", default_value=0.8)
    fresnel = node(mat, unreal.MaterialExpressionFresnel, -250, 650, exponent=2.5)
    soft = node(mat, unreal.MaterialExpressionOneMinus, -100, 650)
    link(fresnel, "", soft, "")
    vcol = node(mat, unreal.MaterialExpressionVertexColor, -250, 750)
    alpha = binop(mat, unreal.MaterialExpressionMultiply, opacity, streak, 50, 500)
    alpha = binop(mat, unreal.MaterialExpressionMultiply, alpha, soft, 200, 550)
    alpha = binop(mat, unreal.MaterialExpressionMultiply, alpha, vcol, 350, 600, b_out="A")
    mel.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Tornado material ready: " + mat.get_path_name())
    return mat


def create_shaft_material():
    """Soft translucent volume (rain shafts, tornado funnel, dust walls): edges fade out."""
    mat = get_or_create("M_AerosShaft", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    # Lit (not unlit): an emissive colour glows at night. Volumetric lighting = lit by sun, sky and lightning.
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL)
    mat.set_editor_property("two_sided", True)
    color = node(mat, unreal.MaterialExpressionVectorParameter, -600, 0, parameter_name="Color",
                 default_value=unreal.LinearColor(0.25, 0.28, 0.32, 1.0))
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    opacity = node(mat, unreal.MaterialExpressionScalarParameter, -600, 200, parameter_name="Opacity", default_value=0.5)
    fresnel = node(mat, unreal.MaterialExpressionFresnel, -600, 320, exponent=1.5)
    soft = node(mat, unreal.MaterialExpressionOneMinus, -400, 320)
    link(fresnel, "", soft, "")
    alpha = binop(mat, unreal.MaterialExpressionMultiply, opacity, soft, -200, 250)
    # No caps: faces looking up / down fade out (from above a cylinder cap was a flat glass ellipse)
    n_z = mask(mat, node(mat, unreal.MaterialExpressionVertexNormalWS, -600, 450), -450, 450, b=True)
    n_abs = node(mat, unreal.MaterialExpressionAbs, -350, 450)
    link(n_z, "", n_abs, "")
    side = node(mat, unreal.MaterialExpressionOneMinus, -250, 450)
    link(n_abs, "", side, "")
    alpha = binop(mat, unreal.MaterialExpressionMultiply, alpha, side, -100, 300)
    mel.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Shaft material ready: " + mat.get_path_name())
    return mat


# ---------------------------------------------------------------- demo level
def demo_game_mode_class():
    cls = unreal.load_class(None, "/Script/AerosWeatherDemo.AerosDemoGameMode")
    if cls is None:
        raise RuntimeError("AerosDemoGameMode class not found: is the AerosWeather plugin compiled and enabled?")
    return cls


def create_demo_map(cloud_mat, ground_mat):
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    world_settings_target = None
    if eal.does_asset_exist(MAP_PATH):
        les.load_level(MAP_PATH)
        log("Demo map exists, opened it (game mode is re-applied): " + MAP_PATH)
    else:
        les.new_level(MAP_PATH)

        def spawn(cls, loc=(0, 0, 0), rot=(0, 0, 0)):
            return actors.spawn_actor_from_class(cls, unreal.Vector(*loc), unreal.Rotator(*rot))

        def try_set(obj, prop, value):
            # Cosmetic settings: a renamed property must not abort the whole map.
            try:
                obj.set_editor_property(prop, value)
            except Exception as e:
                unreal.log_warning("[Aeros] could not set {}: {}".format(prop, e))

        sun = spawn(unreal.DirectionalLight, (0, 0, 100000), (0, -45, 0))
        try_set(sun.light_component, "mobility", unreal.ComponentMobility.MOVABLE)
        try_set(sun.light_component, "atmosphere_sun_light", True)
        spawn(unreal.SkyAtmosphere)
        sky = spawn(unreal.SkyLight, (0, 0, 50000))
        try_set(sky.light_component, "mobility", unreal.ComponentMobility.MOVABLE)
        try_set(sky.light_component, "real_time_capture", True)
        fog = spawn(unreal.ExponentialHeightFog)
        try_set(fog.component, "enable_volumetric_fog", True)
        clouds = spawn(unreal.VolumetricCloud)
        if cloud_mat:
            try_set(clouds.volumetric_cloud_component, "material", cloud_mat)

        spawn(unreal.PlayerStart, (0, 0, 300))
        # No ground: AerosDemoGameMode builds the procedural landscape (and the weather controller,
        # radar, volcano and props) at runtime.

    # Old maps had a flat 200 km plane: remove it (the game mode builds the landscape instead)
    for actor in actors.get_all_level_actors():
        if isinstance(actor, unreal.StaticMeshActor):
            mesh = actor.static_mesh_component.get_editor_property("static_mesh")
            if mesh and mesh.get_path_name() == "/Engine/BasicShapes/Plane.Plane":
                actors.destroy_actor(actor)
                log("Removed the old flat ground plane from the demo map")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", demo_game_mode_class())
    les.save_current_level()
    log("Demo map saved with GameMode Override = AerosDemoGameMode: " + MAP_PATH)


def main():
    results = []
    assets = {}
    # Headless runs (-run=pythonscript) start with an empty asset registry: scan our folder so
    # existing assets are reused instead of failing to be re-created.
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([ROOT], True)

    def step(name, fn, *args):
        try:
            assets[name] = fn(*args)
            results.append((name, "OK"))
        except Exception as e:  # keep going: report everything at the end
            assets[name] = None
            results.append((name, "FAILED: " + str(e)))
            unreal.log_error("[Aeros] {} failed: {}".format(name, e))

    step("MPC", create_mpc)
    step("Default texture", create_default_texture)
    step("Cloud material", create_cloud_material, assets.get("Default texture"))
    step("Ground material", create_ground_material, assets.get("MPC"))
    step("Water material", create_water_material)
    step("Sea material", create_sea_material)
    step("Emissive mat.", create_emissive_material)
    step("Shaft material", create_shaft_material)
    step("Lightning mat.", create_lightning_material)
    step("Tornado mat.", create_tornado_material)
    step("Demo map", create_demo_map, assets.get("Cloud material"), assets.get("Ground material"))

    log("SUMMARY")
    for name, status in results:
        log("  {:<16} {}".format(name, status))
    log("Open /AerosWeather/Maps/L_AerosDemo and press Play (or set GameMode Override = AerosDemoGameMode in any level).")


main()
