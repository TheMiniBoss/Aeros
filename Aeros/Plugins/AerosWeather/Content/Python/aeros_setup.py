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
def create_cloud_material(default_tex):
    mat = get_or_create("M_AerosVolumetricCloud", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_VOLUME)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)

    # UV = (WorldXY - Domain.xy) / Domain.zw + 0.5
    wp = node(mat, unreal.MaterialExpressionWorldPosition, -1800, 0)
    domain = node(mat, unreal.MaterialExpressionVectorParameter, -1800, 200, parameter_name="AerosDomain",
                  default_value=unreal.LinearColor(0, 0, 32000000, 32000000))
    wp_xy = mask(mat, wp, -1600, 0, r=True, g=True)
    d_xy = mask(mat, domain, -1600, 200, r=True, g=True)
    d_zw = mask(mat, domain, -1600, 300, b=True, a=True)
    rel = binop(mat, unreal.MaterialExpressionSubtract, wp_xy, d_xy, -1400, 50)
    uv0 = binop(mat, unreal.MaterialExpressionDivide, rel, d_zw, -1250, 100)
    half = const(mat, 0.5, -1250, 250)
    uv = binop(mat, unreal.MaterialExpressionAdd, uv0, half, -1100, 100)

    tex = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, 100,
               parameter_name="AerosWeatherMap", texture=default_tex,
               sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(uv, "", tex, "UVs")

    cover = mask(mat, tex, -650, 0, r=True, src_out="RGBA")
    base01 = mask(mat, tex, -650, 100, g=True, src_out="RGBA")
    top01 = mask(mat, tex, -650, 200, b=True, src_out="RGBA")
    precip = mask(mat, tex, -650, 300, a=True, src_out="RGBA")

    # Altitude (km) inside the cloud layer set by the controller: 0.3 km + 15 km * norm
    attrib = node(mat, unreal.MaterialExpressionCloudSampleAttribute, -900, 450)
    alt = binop(mat, unreal.MaterialExpressionMultiply, attrib, const(mat, 15.0, -900, 600), -700, 450, a_out="NormAltitudeInLayer")
    alt = binop(mat, unreal.MaterialExpressionAdd, alt, const(mat, 0.3, -700, 600), -550, 450)

    base_km = binop(mat, unreal.MaterialExpressionMultiply, base01, const(mat, 5.0, -500, 150), -450, 100)
    top_km = binop(mat, unreal.MaterialExpressionMultiply, top01, const(mat, 16.0, -500, 250), -450, 200)
    base_top = binop(mat, unreal.MaterialExpressionAdd, base_km, const(mat, 0.4, -450, 330), -300, 100)
    top_low = binop(mat, unreal.MaterialExpressionSubtract, top_km, const(mat, 1.2, -450, 380), -300, 200)

    rise = node(mat, unreal.MaterialExpressionSmoothStep, -150, 100)
    link(base_km, "", rise, "Min"); link(base_top, "", rise, "Max"); link(alt, "", rise, "Value")
    fall = node(mat, unreal.MaterialExpressionSmoothStep, -150, 250)
    link(top_low, "", fall, "Min"); link(top_km, "", fall, "Max"); link(alt, "", fall, "Value")
    fall_inv = node(mat, unreal.MaterialExpressionOneMinus, 0, 250)
    link(fall, "", fall_inv, "")
    profile = binop(mat, unreal.MaterialExpressionMultiply, rise, fall_inv, 150, 150)

    # Billowy detail: procedural noise (cheap settings)
    noise = node(mat, unreal.MaterialExpressionNoise, -500, 600, scale=0.00008, levels=3, output_min=0.0, output_max=1.0)
    link(wp, "", noise, "Position")
    one_minus_noise = node(mat, unreal.MaterialExpressionOneMinus, -300, 600)
    link(noise, "", one_minus_noise, "")
    erosion = binop(mat, unreal.MaterialExpressionMultiply, one_minus_noise, const(mat, 0.55, -300, 700), -150, 600)
    cover_boost = binop(mat, unreal.MaterialExpressionMultiply, cover, const(mat, 1.3, -450, 0), -300, 0)
    dens0 = binop(mat, unreal.MaterialExpressionSubtract, cover_boost, erosion, 0, 450)
    dens_sat = node(mat, unreal.MaterialExpressionSaturate, 150, 450)
    link(dens0, "", dens_sat, "")
    density = binop(mat, unreal.MaterialExpressionMultiply, dens_sat, profile, 300, 300)

    # Extinction: storms (precipitation) are optically thicker
    dark = binop(mat, unreal.MaterialExpressionMultiply, precip, const(mat, 3.0, 300, 500), 450, 450)
    dark = binop(mat, unreal.MaterialExpressionAdd, dark, const(mat, 1.0, 450, 550), 600, 450)
    ext = binop(mat, unreal.MaterialExpressionMultiply, density, const(mat, 0.12, 300, 600), 600, 300)
    ext = binop(mat, unreal.MaterialExpressionMultiply, ext, dark, 750, 300)
    mel.connect_material_property(ext, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)

    albedo = node(mat, unreal.MaterialExpressionConstant3Vector, 750, 100, constant=unreal.LinearColor(0.9, 0.9, 0.92, 1))
    mel.connect_material_property(albedo, "", unreal.MaterialProperty.MP_BASE_COLOR)

    flash = node(mat, unreal.MaterialExpressionScalarParameter, 450, 700, parameter_name="AerosLightningFlash", default_value=0.0)
    glow = binop(mat, unreal.MaterialExpressionMultiply, density, flash, 750, 650)
    glow = binop(mat, unreal.MaterialExpressionMultiply, glow, const(mat, 20.0, 750, 800), 900, 650)
    mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    mel.layout_material_expressions(mat)
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
    link(const(mat, 0.85, -450, 400), "", rough, "A"); link(const(mat, 0.08, -450, 450), "", rough, "B"); link(wet, "", rough, "Alpha")
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

        sun = spawn(unreal.DirectionalLight, (0, 0, 100000), (0, -45, 0))
        sun.light_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        sun.light_component.set_editor_property("atmosphere_sun_light", True)
        spawn(unreal.SkyAtmosphere)
        sky = spawn(unreal.SkyLight, (0, 0, 50000))
        sky.light_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        sky.light_component.set_editor_property("real_time_capture", True)
        fog = spawn(unreal.ExponentialHeightFog)
        fog.component.set_editor_property("volumetric_fog", True)
        clouds = spawn(unreal.VolumetricCloud)
        if cloud_mat:
            clouds.volumetric_cloud_component.set_editor_property("material", cloud_mat)

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
    step("Demo map", create_demo_map, assets.get("Cloud material"), assets.get("Ground material"))

    log("SUMMARY")
    for name, status in results:
        log("  {:<16} {}".format(name, status))
    log("Open /AerosWeather/Maps/L_AerosDemo and press Play (or set GameMode Override = AerosDemoGameMode in any level).")


main()
