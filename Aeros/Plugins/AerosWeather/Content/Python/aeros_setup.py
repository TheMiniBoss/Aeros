# Copyright Aeros. All Rights Reserved.
#
# Aeros Weather - one-click asset setup (Unreal Editor, Python Script Plugin).
#
# Creates in the plugin content folder (/AerosWeather):
#   Materials/MPC_AerosWeather          Material Parameter Collection fed by the plugin
#   Materials/T_AerosDefaultWeather     tiny linear texture (default for texture parameters)
#   Materials/M_AerosVolumetricCloud    Volumetric Cloud material driven by the weather map
#   Materials/M_AerosGround             ground material: wetness, snow, hail, ash accumulation
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
#   N   3D Perlin-Worley shape noise (engine volume texture, 6 km tiles: ~1 km cumulus cells)
#   E   3D Worley erosion noise (0.8 km tiles)
# Placement: a cellular cumulus field (1.6 km cells, one dome-shaped puff per cell with probability =
# cover), merging into towers where cover is high. In the spirit of Horizon / Nubis, 3D noises then
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

// --- Where the clouds are: a cellular cumulus field (1.6 km cells). Each cell holds a cloud with
// probability = cover; its radius grows with cover so a 100 % sky (storms) merges into one mass.
// Each puff is a dome: its radius shrinks toward the top (cumulus) or stays wide (towers, conv).
float2 q = P.xy / 1.3 + (N.gb - 0.5) * 0.35;   // domain warp: no perfect circles
float2 qi = floor(q);
float puff = 0.0;
for (int j = -1; j <= 1; j++)
{
    for (int i = -1; i <= 1; i++)
    {
        float2 c = qi + float2(i, j);
        float3 r = frac(sin(float3(dot(c, float2(127.1, 311.7)), dot(c, float2(269.5, 183.3)), dot(c, float2(419.2, 371.9)))) * 43758.5453);
        if (r.z > cover * 1.15) continue;
        // Each cloud has its own top (50-100 % of the layer): a field of uneven cumulus
        float topCell = baseKm + (topKm - baseKm) * (0.5 + 0.5 * frac(r.y * 7.13));
        if (AltKm > topCell) continue;
        float hc = saturate((AltKm - baseKm) / max(topCell - baseKm, 0.1));
        float domeC = lerp(sqrt(saturate(1.0 - hc * hc)), 1.0 - 0.35 * hc, conv);
        float2 center = c + 0.15 + 0.7 * r.xy;
        float radius = (0.35 + 0.55 * cover) * (0.75 + 0.5 * r.x) * domeC + updraft * 0.4;
        radius *= lerp(1.0, 0.75 + 0.5 * N.r, conv);   // towers: bulging turrets up the sides
        puff = max(puff, saturate(1.0 - length(q - center) / max(radius, 0.01)));
    }
}

// Height profile: sharp flat base (100 m), soft top (the dome does the rest)
float profile = saturate((AltKm - baseKm) / 0.1) * saturate((topKm - AltKm) / max(0.15 * (topKm - baseKm), 0.15));
float cloud = saturate(puff * 2.2) * profile;

// --- Detail: the 3D noises only erode the edges. Billowy (inverted Worley) at the top,
// wispy at the base, low-frequency Perlin-Worley breaks the round outline.
float lowFbm  = N.g * 0.625 + N.b * 0.25 + N.a * 0.125;
float highFbm = E.r * 0.625 + E.g * 0.25 + E.b * 0.125;
float highMod = lerp(highFbm, 1.0 - highFbm, saturate(h * 3.0));
cloud = saturate((cloud - (1.0 - lowFbm) * 0.45) / 0.55);
cloud = saturate((cloud - highMod * 0.5) / 0.5);

// --- Anvil: thin flat sheet 9.8-12.4 km, ragged fibrous edges, thinner far downwind
float anvilBand = saturate((AltKm - 9.8) / 0.7) * saturate((12.4 - AltKm) / 0.35);
float anvil     = anvilBand * saturate(anvilCv * 1.3 - (1.0 - lowFbm) * 0.5) * (0.5 + 0.5 * highFbm);

// --- Shelf cloud: layered wedge between 0.4 and 1.9 km along the gust front
float shelfBand = saturate((AltKm - 0.45) / 0.08) * saturate((1.9 - AltKm) / 0.6);
float layers    = 0.6 + 0.4 * sin(AltKm * 38.0 + N.g * 5.0);
float shelf     = gust * shelfBand * layers * saturate(lowFbm * 1.5 - 0.2);

// --- Voxel grid (storm anatomy synthesised from the simulation), eroded by the same noises.
// Inside the grid storms / anvils / shelves come from the voxels; the 2D path only keeps the
// fair-weather cumulus there, and takes over beyond the grid (35-40 km fade).
float2 edgeUV = abs(VU.xy - 0.5) * 2.0;
float voxIn   = Weight * (1.0 - smoothstep(0.875, 1.0, max(edgeUV.x, edgeUV.y)));
float voxRaw  = lerp(VP.r, VC.r, Blend);
float voxDark = lerp(VP.g, VC.g, Blend);
float vox     = saturate((voxRaw - (1.0 - lowFbm) * 0.5) / 0.5);   // billows at the km scale
vox = saturate((vox - highMod * 0.45) / 0.55);                        // cauliflower / wisps

float full2D      = max(cloud, max(anvil * 0.4, shelf * 0.9));
float cumulusOnly = cloud * (1.0 - smoothstep(0.2, 0.4, conv));
float density = lerp(full2D, max(cumulusOnly, vox), voxIn);
float dark    = lerp(saturate(Wx.a * 1.2 + conv * h * 0.3), max(saturate(Wx.a * 0.5), voxDark), voxIn);
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
    uvw_erosion = binop(mat, unreal.MaterialExpressionMultiply, pos, const(mat, 1.0 / 0.8, -1800, 750), -1600, 700)
    noise_shape = node(mat, unreal.MaterialExpressionTextureSample, -1300, 800,
                       texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseShape128"))
    link(uvw_shape, "", noise_shape, "UVs")
    noise_erosion = node(mat, unreal.MaterialExpressionTextureSample, -1300, 1050,
                         texture=unreal.load_asset("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32"))
    link(uvw_erosion, "", noise_erosion, "UVs")

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
        warp = mask(mat, noise_shape, -1700, y + 200, g=True, b=True, src_out="RGBA")
        warp = binop(mat, unreal.MaterialExpressionSubtract, warp, const(mat, 0.5, -1700, y + 260), -1600, y + 200)
        warp = binop(mat, unreal.MaterialExpressionMultiply, warp, const(mat, 240000.0, -1700, y + 300), -1500, y + 200)
        warp = binop(mat, unreal.MaterialExpressionDivide, warp, r_z, -1400, y + 200)
        uv_v = binop(mat, unreal.MaterialExpressionAdd, uv_v, warp, -1450, y + 40)
        w_v = binop(mat, unreal.MaterialExpressionDivide, alt, r_w, -1550, y + 120)
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
    for name in ["AltKm", "P", "Wx", "Sx", "N", "E", "VP", "VC", "VU", "Blend", "Weight"]:
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
    link(node(mat, unreal.MaterialExpressionConstant3Vector, -150, 50, constant=unreal.LinearColor(0.95, 0.95, 0.97, 1)), "", albedo, "A")
    link(node(mat, unreal.MaterialExpressionConstant3Vector, -150, 150, constant=unreal.LinearColor(0.75, 0.77, 0.8, 1)), "", albedo, "B")
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
                       ("multi_scattering_approximation_octave_count", 2), ("const_multi_scattering_contribution", 0.5),
                       ("const_multi_scattering_occlusion", 0.4), ("const_multi_scattering_eccentricity", 0.5)]:
        try:
            advanced.set_editor_property(key, value)
        except Exception as e:
            unreal.log_warning("[Aeros] cloud advanced output: could not set {}: {}".format(key, e))

    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Cloud material ready: " + mat.get_path_name())
    return mat


# ---------------------------------------------------------------- ground material
def create_ground_material(mpc):
    mat = get_or_create("M_AerosGround", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)

    def p(name, x, y):
        return node(mat, unreal.MaterialExpressionCollectionParameter, x, y, collection=mpc, parameter_name=name)

    wet = p("AerosWetness", -900, 0)
    snow = p("AerosSnowDepth", -900, 150)
    hail = p("AerosHailCover", -900, 300)
    ash = p("AerosAshDepth", -900, 520)

    grass = node(mat, unreal.MaterialExpressionConstant3Vector, -900, -200, constant=unreal.LinearColor(0.13, 0.2, 0.07, 1))
    white = node(mat, unreal.MaterialExpressionConstant3Vector, -900, 450, constant=unreal.LinearColor(0.9, 0.92, 0.95, 1))

    # Wet: darker albedo, glossy (roadmap: water drives roughness / puddles)
    darken = node(mat, unreal.MaterialExpressionLinearInterpolate, -600, -150)
    link(const(mat, 1.0, -800, -100), "", darken, "A"); link(const(mat, 0.55, -800, -50), "", darken, "B"); link(wet, "", darken, "Alpha")
    wet_col = binop(mat, unreal.MaterialExpressionMultiply, grass, darken, -450, -150)

    # Snow cover (roadmap: snow drives WPO thickness) and hail whitening
    snow_a = node(mat, unreal.MaterialExpressionSaturate, -600, 150)
    link(binop(mat, unreal.MaterialExpressionDivide, snow, const(mat, 4.0, -800, 220), -700, 150), "", snow_a, "")
    hail_a = binop(mat, unreal.MaterialExpressionMultiply, hail, const(mat, 0.8, -800, 360), -600, 300)
    ash_a = node(mat, unreal.MaterialExpressionSaturate, -600, 520)
    link(binop(mat, unreal.MaterialExpressionDivide, ash, const(mat, 20.0, -800, 600), -700, 520), "", ash_a, "")
    cover_a = node(mat, unreal.MaterialExpressionMax, -450, 220)
    link(snow_a, "", cover_a, "A"); link(hail_a, "", cover_a, "B")
    base = node(mat, unreal.MaterialExpressionLinearInterpolate, -300, 0)
    link(wet_col, "", base, "A"); link(white, "", base, "B"); link(cover_a, "", base, "Alpha")
    ash_col = node(mat, unreal.MaterialExpressionConstant3Vector, -300, -200, constant=unreal.LinearColor(0.22, 0.21, 0.2, 1))
    base_ash = node(mat, unreal.MaterialExpressionLinearInterpolate, -150, 0)
    link(base, "", base_ash, "A"); link(ash_col, "", base_ash, "B"); link(ash_a, "", base_ash, "Alpha")
    mel.connect_material_property(base_ash, "", unreal.MaterialProperty.MP_BASE_COLOR)

    rough = node(mat, unreal.MaterialExpressionLinearInterpolate, -300, 250)
    # Wet grass is darker and a bit glossy, not a mirror (0.08 reflected the whole sky)
    link(const(mat, 0.85, -450, 400), "", rough, "A"); link(const(mat, 0.35, -450, 450), "", rough, "B"); link(wet, "", rough, "Alpha")
    rough2 = node(mat, unreal.MaterialExpressionLinearInterpolate, -150, 250)
    link(rough, "", rough2, "A"); link(const(mat, 0.6, -300, 450), "", rough2, "B"); link(cover_a, "", rough2, "Alpha")
    mel.connect_material_property(rough2, "", unreal.MaterialProperty.MP_ROUGHNESS)

    wpo_z = binop(mat, unreal.MaterialExpressionMultiply, snow, const(mat, 1.0, -450, 600), -300, 600)
    wpo = node(mat, unreal.MaterialExpressionAppendVector, -150, 600)
    link(node(mat, unreal.MaterialExpressionConstant2Vector, -300, 700, r=0.0, g=0.0), "", wpo, "A")
    link(wpo_z, "", wpo, "B")
    mel.connect_material_property(wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("Ground material ready: " + mat.get_path_name())
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

        ground = spawn(unreal.StaticMeshActor)
        ground.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Plane"))
        ground.set_actor_scale3d(unreal.Vector(200000, 200000, 1))
        if ground_mat:
            ground.static_mesh_component.set_material(0, ground_mat)
        spawn(unreal.PlayerStart, (0, 0, 300))
        # AerosDemoGameMode adds the weather controller, radar, volcano and props at runtime.

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
