"""WP-U2: (re)create the SCBridge Substrate master materials and their blend/two-sided variants.

Run headless inside the Python commandlet with r.Substrate enabled (project default):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/create_materials.py \
      -unattended -nop4 -nosplash -AllowCommandletRendering -ddc=SCViewerDDC
(UE 5.9 refuses to compile materials under -nullrhi.) Everything lives under /SCBridge/Materials (ue/Plugins/SCBridge/Content/Materials). Parameter names are
SC-semantic and model-neutral; FSCMaterialBuilder (C++) sets them. No asset values are baked in here:
every default is a neutral identity (white, flat normal, 0/1) and the builder supplies sidecar data.
The graphs do carry shading rules of the game's own shaders (gloss -> roughness, layer blend order, macro
surface, HAL luminance range, HardSurface static wear, dirt); those are shader logic, the same for every asset, cited
where they appear. Game functions with branches or many terms are Custom (HLSL) nodes transcribed from the
shader text, so they can be read against it.
"""
import json
import os
import shutil
import struct
import tempfile
import zlib

import unreal

ROOT = "/SCBridge/Materials"
TEX = ROOT + "/Textures"
mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
MP = unreal.MaterialProperty
ST = unreal.MaterialSamplerType
LAYERS = 4  # M_SC_Layered: LayerBlend_V2 base layers 1..4 selected by blend-mask R/G/B over layer 4.


def png_bytes(rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * 4 for _ in range(4))
    chunk = lambda tag, data: struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def default_texture(name, rgba, srgb, compression):
    path = TEX + "/" + name
    folder = tempfile.mkdtemp(prefix="sc_u2_")
    file = os.path.join(folder, name + ".png")
    with open(file, "wb") as f:
        f.write(png_bytes(rgba))
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", file)
    task.set_editor_property("destination_path", TEX)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", False)
    tools.import_asset_tasks([task])
    texture = unreal.load_asset(path)
    texture.set_editor_property("srgb", srgb)
    texture.set_editor_property("compression_settings", compression)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    eal.save_loaded_asset(texture)
    return texture


class Graph:
    """Small helper over MaterialEditingLibrary that fails loudly on any unconnected pin."""

    def __init__(self, name, blend, lighting=None, two_sided=False, nanite=False):
        self.m = tools.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
        self.m.set_editor_property("blend_mode", blend)
        self.m.set_editor_property("two_sided", two_sided)
        if lighting is not None:
            self.m.set_editor_property("translucency_lighting_mode", lighting)
        if nanite:
            self.m.set_editor_property("used_with_nanite", True)
        # Skinned parts (WP-U3 rigs, WP-P2 player body and weapons) use the same masters. A standalone (-game)
        # session cannot add a usage flag at runtime: the proxy would fall back to the engine's default
        # (WorldGrid) material, so the flag is part of the asset.
        self.m.set_editor_property("used_with_skeletal_mesh", True)
        # Environments draw repeated rigid parts as instanced static meshes (FSCInstancing, LOOK-6); same reason.
        self.m.set_editor_property("used_with_instanced_static_meshes", True)
        # Hangars with baked lighting (bake_hangar_lighting.py, the web build) draw lightmapped meshes; same reason.
        self.m.set_editor_property("used_with_static_lighting", True)
        # Explicit clip: mask graphs output (alpha - AlphaTest + 0.5), i.e. a texel survives when alpha >= the
        # instance's authored AlphaTest. UE's default clip (0.3333, Material.cpp) would shift every threshold.
        self.m.set_editor_property("opacity_mask_clip_value", 0.5)
        self.x, self.y = -2400, -1600

    def node(self, cls, **props):
        self.y += 140
        if self.y > 2400:
            self.x, self.y = self.x + 320, -1600
        e = mel.create_material_expression(self.m, cls, self.x, self.y)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def link(self, a, out, b, inp):
        # UE 5.8 names the only input of single-input expressions (e.g. Transform) "", 5.9 names it "Input".
        if not mel.connect_material_expressions(a, out, b, inp) and not (
                inp == "Input" and mel.connect_material_expressions(a, out, b, "")):
            raise RuntimeError(f"{self.m.get_name()}: cannot link {a.get_name()}.{out!r} -> {b.get_name()}.{inp!r}")

    def out(self, a, out, prop):
        if not mel.connect_material_property(a, out, prop):
            raise RuntimeError(f"{self.m.get_name()}: cannot connect {a.get_name()}.{out!r} -> {prop}")

    def scalar(self, name, value, group="SC"):
        return (self.node(unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=value, group=group), "")

    def vector(self, name, value, group="SC"):
        return (self.node(unreal.MaterialExpressionVectorParameter, parameter_name=name,
                          default_value=unreal.LinearColor(*value), group=group), "RGB")

    def collection(self, name, vector=False):
        """A parameter of the viewer's dirt collection (MPC_SCDirt), set at run time by FSCDirt."""
        e = (self.node(unreal.MaterialExpressionCollectionParameter, collection=Graph.dirt_collection, parameter_name=name), "")
        return self.op(unreal.MaterialExpressionComponentMask, e, names=("",), r=True, g=True, b=True, a=False) if vector else e

    def primitive_scalar(self, name, index, value, group="SC"):
        """A scalar read from the component's custom primitive data slot `index` (FSCDirt writes it)."""
        return (self.node(unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=value, group=group,
                          use_custom_primitive_data=True, primitive_data_index=index), "")

    def dirt(self, albedo, f0, smooth, blend_map, vertex_wear, vertex_ao, object_space=True, detail=None):
        """layerLib.cfi CalcDirtBlendAlpha + LayerAttribs_ApplyDirt with the viewer's dirt: albedo -> DirtColor,
        F0 -> DIRT_SPECULAR, smoothness -> DIRT_GLOSS by the dirt alpha. Returns (albedo, f0, smooth, alpha)."""
        # vertexLib.cfi: objectDirt = SH2_SampleValue_Clamped(normalize(objectNormal x MeshInvSquareNormalizedBounds),
        # dirtAmountSH); the viewer's coefficients: w = DirtAmount, xyz = DirtSH (FSCDirt::DirectionCoefficients).
        # The deferred-decal domain has no world -> local transform or object bounds: decals use the world normal (the
        # viewer's ships stand level) and unit bounds.
        normal_ws = (self.node(unreal.MaterialExpressionVertexNormalWS), "")
        if object_space:
            normal = self.op(unreal.MaterialExpressionTransform, normal_ws, names=("Input",),
                             transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD,
                             transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
            extents = (self.node(unreal.MaterialExpressionObjectLocalBounds), "Half Extents")
        else:
            normal, extents = normal_ws, self.const3((1, 1, 1))
        object_dirt = self.custom(DIRT_SH, unreal.CustomMaterialOutputType.CMOT_FLOAT1, Normal=normal, HalfExtents=extents,
                                  Amount=self.collection("DirtAmount"), SH=self.collection("DirtSH", vector=True))
        amount = self.mul(object_dirt, self.primitive_scalar("DirtExterior", DIRT_EXTERIOR_INDEX, 0.0, "Dirt"))
        colour = self.collection("DirtColor", vector=True)
        alpha = self.custom(DIRT_ALPHA, unreal.CustomMaterialOutputType.CMOT_FLOAT1, Amount=amount, VertexWear=vertex_wear,
                            BlendMap=blend_map, VertexAO=vertex_ao, DirtColor=colour,
                            Detail=detail if detail is not None else self.const(0.5))  # 0.5 = no details (identity)
        return (self.lerp(albedo, colour, alpha), self.lerp(f0, self.const3(DIRT_SPECULAR), alpha),
                self.lerp(smooth, self.const(DIRT_GLOSS), alpha), alpha)

    def vertex_illum(self):
        """Illum.cfx %VERTCOLORS (WP-U2c): (albedo factor, glow factor, blending-opacity factor) by VertexColorMode
        (0 none, 1 rgb, 2 blue/green under %USE_DAMAGE_MAP, 3 alpha) and UseVertexAlpha (set by FSCMaterialBuilder)."""
        v = self.node(unreal.MaterialExpressionVertexColor)
        mode = self.scalar("VertexColorMode", 0.0, "Vertex")
        pins = dict(R=(v, "R"), G=(v, "G"), B=(v, "B"), A=(v, "A"), Mode=mode)
        albedo = self.custom(VERTEX_ALBEDO, unreal.CustomMaterialOutputType.CMOT_FLOAT3, **pins)
        glow = self.custom(VERTEX_GLOW, unreal.CustomMaterialOutputType.CMOT_FLOAT3, **pins)
        opacity = self.lerp(self.const(1.0), (v, "A"), self.scalar("UseVertexAlpha", 0.0, "Vertex"))
        return albedo, glow, opacity

    def texture(self, name, default, sampler, uv, group="SC"):
        e = self.node(unreal.MaterialExpressionTextureSampleParameter2D, parameter_name=name, texture=default,
                      sampler_type=sampler, group=group,
                      sampler_source=unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
        self.link(*uv, e, "UVs")
        return e

    def op(self, cls, *inputs, names=("A", "B"), **props):
        e = self.node(cls, **props)
        for (src, out), inp in zip(inputs, names):
            self.link(src, out, e, inp)
        return (e, "")

    def mul(self, a, b): return self.op(unreal.MaterialExpressionMultiply, a, b)
    def add(self, a, b): return self.op(unreal.MaterialExpressionAdd, a, b)
    def sub(self, a, b): return self.op(unreal.MaterialExpressionSubtract, a, b)
    def lerp(self, a, b, t): return self.op(unreal.MaterialExpressionLinearInterpolate, a, b, t, names=("A", "B", "Alpha"))
    def one_minus(self, a): return self.op(unreal.MaterialExpressionOneMinus, a, names=("",))
    def saturate(self, a): return self.op(unreal.MaterialExpressionSaturate, a, names=("",))
    def sqrt(self, a): return self.op(unreal.MaterialExpressionSquareRoot, a, names=("",))
    def normalize(self, a): return self.op(unreal.MaterialExpressionNormalize, a, names=("VectorInput",))
    def const(self, v): return (self.node(unreal.MaterialExpressionConstant, r=v), "")
    def const3(self, v): return (self.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(*v, 1.0)), "")

    def custom(self, code, output, **inputs):
        """Custom HLSL node; `inputs` are name=(expression, output) pins, usable by name in `code`."""
        e = self.node(unreal.MaterialExpressionCustom, code=code, output_type=output)
        pins = []
        for name in inputs:
            pin = unreal.CustomInput()
            pin.set_editor_property("input_name", name)
            pins.append(pin)
        e.set_editor_property("inputs", pins)
        for name, source in inputs.items():
            self.link(*source, e, name)
        return (e, "")

    def uv(self, tiling):
        coords = (self.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0), "")
        self.last_uv = self.mul(coords, tiling)
        return self.last_uv

    def map_smoothness(self, name, uv, group="SC"):
        """The DDNA smoothness (gloss) map as the game stores it: the _ddna alpha mips (BC4 / R8), red channel,
        filtered in smoothness space as the game samples it (WP-T1)."""
        return (self.texture(name, self.white_linear, ST.SAMPLERTYPE_MASKS, uv, group), "R")

    def smoothness(self, prefix, uv):
        """Game smoothness (gloss): lerp(<p>Smoothness, map x <p>GlossMult, <p>UseSmoothnessMap)."""
        from_map = self.mul(self.map_smoothness(prefix + "SmoothnessMap", uv), self.scalar(prefix + "GlossMult", 1.0))
        return self.lerp(self.scalar(prefix + "Smoothness", 0.0), from_map, self.scalar(prefix + "UseSmoothnessMap", 0.0))

    def gloss_to_roughness(self, smooth):
        """UE roughness from game smoothness. Game ShadeLib: GlossToRoughness(g) = (1 - min(g, 251/255))^2 is the
        GGX alpha (NDF_GGX); UE's GGX alpha is Roughness^2, so Roughness = 1 - min(g, 251/255)."""
        return self.saturate(self.one_minus(self.op(unreal.MaterialExpressionMin, smooth, self.const(251.0 / 255.0))))

    def roughness(self, prefix, uv):
        return self.gloss_to_roughness(self.smoothness(prefix, uv))

    def conserve(self, albedo, f0):
        """Game diffuse / specular energy split, on the final surface attributes (TiledShading.cfx deferred
        resolve after DecodeGBuffer, Illum.cfx forward): Albedo *= saturate(1 - GetLuminance(Reflectance)),
        GetLuminance = Rec. 709. This is the spec workflow's only "metalness": albedo and F0 are authored
        separately. UE's own view-dependent form of the same attenuation (r.Shading.EnergyPreservation) is off
        in DefaultEngine.ini so the split is not applied twice."""
        lum = self.op(unreal.MaterialExpressionDotProduct, f0, self.const3((0.2126, 0.7152, 0.0722)))
        return self.mul(albedo, self.saturate(self.one_minus(lum)))

    def reconstruct_z(self, n):
        """Game NormalsCommon ReconstructNormalZ: keep XY, z = sqrt(max(1e-6, 1 - x^2 - y^2))."""
        xy = self.op(unreal.MaterialExpressionComponentMask, n, names=("",), r=True, g=True, b=False)
        zz = self.op(unreal.MaterialExpressionMax, self.one_minus(self.op(unreal.MaterialExpressionDotProduct, xy, xy)), self.const(1.0e-6))
        return self.op(unreal.MaterialExpressionAppendVector, xy, self.sqrt(zz))

    def detail_normal(self, base, detail):
        """Game NormalsCommon AddDetailNormals (reoriented normal mapping): `detail` rotated onto `base`."""
        t = self.add(base, self.const3((0, 0, 1)))
        u = self.mul(detail, self.const3((-1, -1, 1)))
        tz = self.op(unreal.MaterialExpressionComponentMask, t, names=("",), r=False, g=False, b=True)
        dot = self.op(unreal.MaterialExpressionDotProduct, t, u)
        return self.normalize(self.sub(self.mul(t, dot), self.mul(u, tz)))

    def link_any(self, a, out, b, names):
        for name in names:
            if mel.connect_material_expressions(a, out, b, name):
                return
        raise RuntimeError(f"{self.m.get_name()}: cannot link {a.get_name()} -> {b.get_name()}.{names}")

    def glow(self, colour, alpha, ramp=False):
        """Emission = colour x Emissive x EmissiveStrength (builder: cd/m2 from the game's glow rule, FSCStarEngine; 0 when the material does not emit),
        optionally from the emittance map, gated by texture alpha above AlphaGlowCutoff: a step, or with `ramp` the
        HardSurface.cfx form saturate((alpha - cutoff) / (1 - cutoff))."""
        source = self.lerp(colour, (self.texture("EmissiveMap", self.white, ST.SAMPLERTYPE_COLOR, self.last_uv), "RGB"),
                           self.scalar("UseEmissiveMap", 0.0))
        cutoff = self.scalar("AlphaGlowCutoff", 0.0)
        slope = self.op(unreal.MaterialExpressionDivide, self.const(1.0), self.op(
            unreal.MaterialExpressionMax, self.one_minus(cutoff), self.const(1.0e-4))) if ramp else self.const(1.0e4)
        gate = self.saturate(self.mul(self.sub(alpha, cutoff), slope))
        return self.mul(self.mul(self.mul(source, self.vector("EmissiveColor", (0, 0, 0))), self.scalar("EmissiveStrength", 0.0)), gate)

    def slab(self, albedo=None, f0=None, f90=None, roughness=None, normal=None, emissive=None, mfp=None, front=True):
        e = self.node(unreal.MaterialExpressionSubstrateSlabBSDF)
        for value, pin in ((albedo, "Diffuse Albedo"), (f0, "F0"), (f90, "F90"), (roughness, "Roughness"),
                           (normal, "Normal"), (emissive, "Emissive Color"), (mfp, "SSS MFP")):
            if value is not None:
                self.link(*value, e, pin)
        if front:
            self.out(e, "", MP.MP_FRONT_MATERIAL)
        return e

    def weight(self, slab, coverage):
        """Substrate coverage (MP_Opacity is only live for AlphaComposite under Substrate)."""
        w = self.node(unreal.MaterialExpressionSubstrateWeight)
        self.link(slab, "", w, "A")
        self.link(*coverage, w, "Weight")
        self.out(w, "", MP.MP_FRONT_MATERIAL)

    def save(self):
        mel.layout_material_expressions(self.m)
        mel.recompile_material(self.m)
        eal.save_loaded_asset(self.m)
        stats = mel.get_statistics(self.m)
        unreal.log(f"SC_MATERIAL {self.m.get_path_name()} expressions={mel.get_num_material_expressions(self.m)} "
                   f"vs={stats.num_vertex_shader_instructions} ps={stats.num_pixel_shader_instructions} "
                   f"samplers={stats.num_samplers}")
        return self.m


def surface_master(name):
    """M_SC_Illum / M_SC_Fallback: diffuse x colour, DDNA normal, roughness, F0 = specular colour x spec map, glow."""
    g = Graph(name, unreal.BlendMode.BLEND_OPAQUE, nanite=True)
    uv = g.uv(g.scalar("Tiling", 1.0))
    diffuse = g.texture("DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv)
    base = g.mul((diffuse, "RGB"), g.vector("DiffuseColor", (1, 1, 1)))
    normal = (g.texture("NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv), "RGB")
    spec = (g.texture("SpecularMap", g.white, ST.SAMPLERTYPE_COLOR, uv), "RGB")
    f0 = g.mul(g.vector("SpecularColor", (0.04, 0.04, 0.04)), spec)
    vertex_albedo, vertex_glow, _ = g.vertex_illum()  # opaque: vertex alpha only scales blending opacity
    emissive = g.mul(g.glow(base, (diffuse, "A")), vertex_glow)
    base = g.mul(base, vertex_albedo)
    g.slab(g.conserve(base, f0), f0, None, g.roughness("", uv), normal, emissive)
    # Coverage = diffuse alpha x a separate opacity map (HairPBR cards: TexSlot1 `opacity`, red channel);
    # the white default leaves every other family's coverage unchanged.
    coverage = g.mul((diffuse, "A"), (g.texture("OpacityMap", g.white_linear, ST.SAMPLERTYPE_MASKS, uv), "R"))
    mask = g.add(g.sub(coverage, g.scalar("AlphaTest", 0.0)), g.const(0.5))
    g.out(*mask, MP.MP_OPACITY_MASK)
    return g.save()


def layered_master(name, count):
    """M_SC_Layered (4 layers): per-layer tiling, tint (palette x tint x diffuse) and F0.

    Layers blend like LayerBlend_V2: start from layer N-1, lerp toward layer i by blend-mask channel i (R,G,B).
    UseBlendMask=0 forces mask (1,1,1) which selects Layer0. Smoothness blends as smoothness (layerLib), and
    the material-level maps apply over the blended result (LayerAttribs_AddMacroSurface, then AO):
      MacroNormalMap / MacroSmoothnessMap (UseMacroGloss) at UV x MacroTiling (the builder sets 1: LayerBlend
        macro maps are untiled): detail-normal blend of the layer normal onto the macro normal; smoothness x
        macro smoothness.
      HalMap at the base UVs (UseHalMap): G = AO over the blend; B = luminance shift, applied to each layer
        before it is accumulated (Layer_ApplyLayerEffects): saturate(colour x (B x 0.4 + 0.8)) on albedo and
        F0 (LayerBlend_V2: lumShift = B x 2, HAL_LUM_MAX = 0.2). The R hue shift (HueLUT) and RotateLayerUVs
        are not implemented.
    Blended layer normals accumulate XY and rebuild Z (LayerAttribs_BlendNormalize -> ReconstructNormalZ).
    """
    g = Graph(name, unreal.BlendMode.BLEND_OPAQUE, nanite=True)
    base_uv = g.uv(g.scalar("Tiling", 1.0))  # HAL map and emittance map (g.glow) UVs
    hal = g.texture("HalMap", g.white_linear, ST.SAMPLERTYPE_MASKS, base_uv, "Macro")
    use_hal = g.scalar("UseHalMap", 0.0, "Macro")
    ao = g.lerp(g.const(1.0), (hal, "G"), use_hal)
    lum = g.lerp(g.const(1.0), g.add(g.mul((hal, "B"), g.const(0.4)), g.const(0.8)), use_hal)
    layers = []
    for i in range(count):
        p = f"Layer{i}"
        uv = g.uv(g.scalar(p + "Tiling", 1.0, p))
        d = g.texture(p + "DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv, p)
        colour = g.mul((d, "RGB"), g.vector(p + "Tint", (1, 1, 1), p))
        f0 = g.vector(p + "Specular", (0.04, 0.04, 0.04), p)
        albedo, f0 = g.saturate(g.mul(colour, lum)), g.saturate(g.mul(f0, lum))
        layers.append(dict(
            albedo=albedo, f0=f0,
            normal=(g.texture(p + "NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv, p), "RGB"),
            smoothness=g.smoothness(p, uv),
            colour=colour, alpha=(d, "A")))
    result = dict(layers[-1])
    mask_uv = g.uv(g.scalar("BlendMaskTiling", 1.0))
    mask = (g.texture("BlendMaskMap", g.white_linear, ST.SAMPLERTYPE_MASKS, mask_uv), "RGB")
    mask = g.lerp(g.const3((1, 1, 1)), mask, g.scalar("UseBlendMask", 0.0))
    channels = [g.op(unreal.MaterialExpressionComponentMask, mask, names=("",), r=c == 0, g=c == 1, b=c == 2)
                for c in range(3)]
    for i in range(count - 2, -1, -1):
        for key in ("albedo", "f0", "normal", "smoothness", "colour", "alpha"):
            result[key] = g.lerp(result[key], layers[i][key], channels[i])
    result["normal"] = g.reconstruct_z(result["normal"])
    macro_uv = g.uv(g.scalar("MacroTiling", 1.0, "Macro"))
    macro = (g.texture("MacroNormalMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, macro_uv, "Macro"), "RGB")
    normal = g.detail_normal(macro, result["normal"])
    macro_gloss = g.lerp(g.const(1.0), g.map_smoothness("MacroSmoothnessMap", macro_uv, "Macro"), g.scalar("UseMacroGloss", 0.0, "Macro"))
    smooth = g.mul(result["smoothness"], macro_gloss)
    # LayerBlend_V2.cfx: dirt after the macro surface, before AO. blendMap = WearMap (TexSlot11, R = wear/curvature) at
    # the base UVs; %WEAR_DIRT_AO_VERTEX (UseWearDirtAOVertex): vertex R = wear -> vertex dirt, vertex B multiplies AO.
    wear_map = (g.texture("WearMap", g.white_linear, ST.SAMPLERTYPE_MASKS, base_uv, "Dirt"), "R")
    colour_v = g.node(unreal.MaterialExpressionVertexColor)
    use_vertex = g.scalar("UseWearDirtAOVertex", 0.0, "Dirt")
    vertex_wear = g.lerp(g.const(0.0), (colour_v, "R"), use_vertex)
    ao = g.mul(ao, g.lerp(g.const(1.0), (colour_v, "B"), use_vertex))
    # GetAlphaBlendDetailMaps: G of the engine detail map at baseUV x DirtDetailScale (UseDirtDetail; 0.5 = none).
    detail_uv = g.op(unreal.MaterialExpressionMultiply, (g.node(unreal.MaterialExpressionTextureCoordinate), ""),
                     g.scalar("DirtDetailScale", 1.0, "Dirt"))
    detail = g.lerp(g.const(0.5), (g.texture("DirtDetailMap", g.white_linear, ST.SAMPLERTYPE_MASKS, detail_uv, "Dirt"), "G"),
                    g.scalar("UseDirtDetail", 0.0, "Dirt"))
    albedo, f0, smooth, dirt = g.dirt(result["albedo"], result["f0"], smooth, wear_map, vertex_wear, ao, detail=detail)
    albedo, f0 = g.mul(albedo, ao), g.mul(f0, ao)
    g.last_uv = base_uv
    emissive = g.mul(g.glow(result["colour"], result["alpha"]), g.one_minus(dirt))  # Emissive *= 1 - dirtAmount
    g.slab(g.conserve(albedo, f0), f0, None, g.gloss_to_roughness(smooth), normal, emissive)
    g.out(*g.add(g.sub(result["alpha"], g.scalar("AlphaTest", 0.0)), g.const(0.5)), MP.MP_OPACITY_MASK)
    return g.save()


# Illum.cfx %VERTCOLORS (WP-U2c): layer.Albedo *= Vert.Color.rgb / .bbb (%USE_DAMAGE_MAP) / .aaa (wind, transmittance,
# ocean sway); vertGlowMask = .rgb / .ggg / .aaa multiplies the glow, which is taken from the albedo before that multiply.
VERTEX_ALBEDO = """if (Mode < 0.5) return float3(1, 1, 1);
if (Mode < 1.5) return float3(R, G, B);
if (Mode < 2.5) return float3(B, B, B);
return float3(A, A, A);"""
VERTEX_GLOW = """if (Mode < 0.5) return float3(1, 1, 1);
if (Mode < 1.5) return float3(R, G, B);
if (Mode < 2.5) return float3(G, G, G);
return float3(A, A, A);"""


# Dirt (WP-U2b, docs/work-packages/WP-U2b-dirt.md). common.cfi: DIRT_SPECULAR, DIRT_GLOSS. The game drives the amount and
# colour per instance at run time (a ship's default `Dirt` is 0); the viewer feeds its own amount and colour through
# MPC_SCDirt, and FSCDirt marks exterior components (custom primitive data slot DIRT_EXTERIOR_INDEX: 1 = exterior).
DIRT_SPECULAR = (0.03, 0.03, 0.03)
DIRT_GLOSS = 0.0
DIRT_EXTERIOR_INDEX = 0
# vertexLib.cfi ENABLE_DIRT + SphericalHarmonics.cfi SH2_SampleValue_Clamped(direction, c) = c.w + dot(direction, c.xyz),
# direction = normalize(object normal x MeshInvSquareNormalizedBounds), read as 1 / (normalised half extents)^2.
DIRT_SH = """float3 b = normalize(max(abs(HalfExtents), 1e-4));
float3 d = normalize(Normal / (b * b));
return Amount + dot(d, SH);"""
# Decals without an exported blend texture: a neutral mid mask (viewer choice, owner allowance 2026-11-12).
DECAL_DIRT_MASK = 0.5
# layerLib.cfi: `if (objectDirt)`, dirt = saturate(objectDirt - CalculateVertexDirt(wear)) with
# CalculateVertexDirt(v) = 0.95 v^2, then CalcDirtBlendAlpha(dirtColor, dirt, blendMap, AO): mask = blendMap AO^2,
# a = CalcBlendAlpha(mask, dirt, softness 1), slope = 1 - (1 - a)^2, result = lerp(slope, a,
# InverseLerpNormalized(0, 0.15, GetLuminance(dirtColor))); GetLuminance is Rec.709 (common.cfi); then
# AddBlendAlphaDetails(alpha, Detail): saturate(0.25 alpha / Detail^2), the identity at Detail 0.5 (no detail map).
DIRT_ALPHA = """if (Amount <= 0) return 0;
float dirt = saturate(Amount - 0.95 * VertexWear * VertexWear);
float mask = BlendMap * VertexAO * VertexAO;
const float maxSlopeRatio = 1.0 / 255.0;
float inMap = mask * (1 - maxSlopeRatio - 0.001) + maxSlopeRatio + 0.001;
float softSlope = saturate(2 * dirt);
float hardSlope = 1.0 / max(softSlope, maxSlopeRatio);
float softAlpha = inMap * softSlope + 1 - softSlope;
float hardAlpha = inMap * hardSlope;
float blendIn = saturate(lerp(hardAlpha, softAlpha, 1.0));
float blendOut = lerp(1.0, mask * mask, saturate(2 * dirt - 1));
float a = 1.0 - blendIn * blendOut;
float slope = 1 - (1 - a) * (1 - a);
float luminance = dot(DirtColor, float3(0.2126, 0.7152, 0.0722));
float blendAlpha = lerp(slope, a, saturate(luminance / 0.15));
return saturate(0.25 * blendAlpha / (Detail * Detail));  // AddBlendAlphaDetails (identity at Detail 0.5)"""


# Game layerLib.cfi CalcBlendAlpha(blendMap, blendAmt, softness) = 1 - CalcBlendInAlpha x CalcBlendOutAlpha.
WEAR_ALPHA = """const float maxSlopeRatio = 1.0 / 255.0;
float inMap = BlendMap * (1 - maxSlopeRatio - 0.001) + maxSlopeRatio + 0.001;
float softSlope = saturate(2 * Amount);
float hardSlope = 1.0 / max(softSlope, maxSlopeRatio);
float softAlpha = inMap * softSlope + 1 - softSlope;
float hardAlpha = inMap * hardSlope;
float blendIn = saturate(lerp(hardAlpha, softAlpha, Softness));
float blendOut = lerp(1.0, BlendMap * BlendMap, saturate(2 * Amount - 1));
return 1.0 - blendIn * blendOut;"""
# Game layerLib.cfi Layer_RemapWear: paint (SHADING_PAINT / SHADING_CLEARCOAT_PAINT) starts the wear layer at
# 1 - PaintPrimerStart (0.5), cloth at 1 - ClothComplementStart (0.75); other shading models are unchanged.
WEAR_REMAP = """float startWear = Paint * (1.0 - 0.5) + Cloth * (1.0 - 0.75);
return saturate((WearAlpha - startWear) * (1 / (1 - startWear)));"""
# Game layerLib.cfi Layer_ApplyLayerEffects, paint branch (PaintPrimerStart 0.5, PaintComplementStart 0.9), on
# the tinted primary layer; wearAmt = 1 - CalcBlendAlpha. rgb = albedo, a = smoothness. The complement-hue
# shift (Layer_ApplyComplementWearAndHAL: a 3D HueLUT lookup) between wearAmt 0.9 and 1 is not drawn.
# stencilLib.cfib CalcStencilParams + GetStencilLayer_Premultiplied + ApplyStencilAsTint (non-sticker), applied to
# the Primary layer after its primer: three distance fields -> anti-aliased channel alphas (x over y over z), x
# StencilOpacity; albedo = albedo x (1 - total) + sum(colour_i x alpha_i). The breakup texture is neutral (see
# FSCMaterialBuilder HardSurfaceStencil): CalcStencilWearAlpha = 1 and no diffuse overlay. fwidth is 0 where the
# hardware has no derivatives (ray tracing; the game zeroes it under subpixel shading too): a hard step there.
STENCIL_TINT = """float3 w = max(fwidth(Dist), 1e-5);
float3 a = saturate((Dist - 0.5 + 0.5 * w) / w);
a.y *= 1.0 - a.x;
a.z *= 1.0 - (a.x + a.y);
a *= Opacity;
return Albedo * (1.0 - (a.x + a.y + a.z)) + C1 * a.x + C2 * a.y + C3 * a.z;
"""
STENCIL_PORTED = {
    "CalcStencilWearAlpha": "c8522957b2f7e32643e453e0c9fd7b85f7682444879144d71dbbf8a1d8eb8f80",
    "CalcStencilParams": "7b8d0df8643d8260eeb3f23d671f87b8f9c7178eb4ef7ca03409b60d87528a63",
    "GetStencilLayer_Premultiplied": "0106f02b8747bbf42dd223554f44b66dc9d6c93139fdc41e91ab8980b4412148",
    "ApplyStencilAsTint": "ba5b95a64be866590adbff096b8d0d47ef8213904fcc3dfaa3b7b35c3d428441",
    "GetPaletteStencilColors": "54c2f9aaf555e19d45bbeea0435e3dda0888132110b1cafb704af3c9558abbbc",
}
WEAR_PRIMER = """float primerAmount = (1 - smoothstep(0.5, 0.9, PrimaryWear)) * Paint;
float3 baseColor = sqrt(Albedo);
float maxCol = max(baseColor.r, max(baseColor.g, baseColor.b));
float minCol = min(baseColor.r, min(baseColor.g, baseColor.b));
float doubleLuminance = maxCol + minCol;
float saturation = (maxCol - minCol) / (1.0 + (1.0e-4) - abs(doubleLuminance - 1.0));
float luminance = 0.5 * doubleLuminance;
float3 blendGrey = lerp(luminance, baseColor, 0.5);
float lumBias = pow(luminance, 5);
float lumDelta = lerp(0.25, 0.33, saturation) - 0.35 * lumBias;
float3 blendSat = lerp(blendGrey, lumDelta > 0 ? 1.0 : 0.0, abs(lumDelta));
float3 primerAlbedo = pow(blendSat, 2);
float boostedGloss = 0.75 + (Smoothness * 0.25);
float loweredGloss = 0 + (Smoothness * 0.75);
float primerGloss = lerp(boostedGloss, loweredGloss, Smoothness);
return float4(lerp(Albedo, primerAlbedo, primerAmount), lerp(Smoothness, primerGloss, primerAmount));"""


def hardsurface_master():
    """M_SC_HardSurface: Primary layer (Layer0*) + wear layer (Wear*), blended by the game's static wear.

    Game HardSurface.cfx GetLayerAttribs / CalcAlphas_BlendBased / BlendLayers_BranchGradWear:
      vertex colour (%VERTCOLORS -> UseVertexColors; else damage 0, glow 1, AO 1, wear 0): G = glow mask,
        B = AO, A = wear; wearAmt = saturate(WearBlendBase - A x 0.95) (CalculateVertexWear; the per-instance
        wear of a flown ship is 0 here).
      wearAlpha = CalcBlendAlpha(WearBlendMap.r at UV x Layer0Tiling, wearAmt, WearFalloff)   [WEAR_ALPHA]
      Primary: (diffuse x tint, F0 = specular x Layer0SpecularMap, gloss), then Layer_ApplyLayerEffects with
        1 - wearAlpha: paint wears to primer first [WEAR_PRIMER]; weight 1 - Layer_RemapWear(wearAlpha).
      Wear: the same attributes from the Wear* parameters (the builder fills them from the separate Wear layer
        under %WEAR_LAYER, else from the Primary textures with the layer's wear tints); weight
        Layer_RemapWear(wearAlpha) [WEAR_REMAP].
      Normal XY, albedo, F0 and smoothness accumulate by weight, Z is rebuilt (LayerAttribs_BlendNormalize).
      Then LayerAttribs_AddMacroSurface (MacroNormalMap / MacroSmoothnessMap at UV x MacroTiling),
        albedo, F0 x AO, AO = vertex B x MacroAOMap.r, and the deferred energy split (Graph.conserve).
      Opacity (alpha test) is the Primary diffuse alpha; the glow is masked by vertex G (GetMaterialAttribs).
    Stencil (%STENCIL_MAP, stencilLib.cfib non-sticker): the Primary albedo after its primer is tinted by three
      anti-aliased distance-field channels of StencilMap (STENCIL_TINT), before the wear blend.
    Dirt: the viewer's uniform amount and colour (MPC_SCDirt) on exterior components (Graph.dirt), not the game's
      direction-dependent instance dirt.
    Not drawn: per-instance wear/damage, detail maps, the paint/cloth complement hue (HueLUT),
    retroreflective normals, %SECONDARY_LAYER, the stencil breakup texture and %STENCIL_AS_STICKER, iridescence.
    """
    g = Graph("M_SC_HardSurface", unreal.BlendMode.BLEND_OPAQUE, nanite=True)
    base_uv = g.uv(g.scalar("Tiling", 1.0))  # emittance map (g.glow) UVs
    colour_v = g.node(unreal.MaterialExpressionVertexColor)
    use_vertex = g.scalar("UseVertexColors", 0.0, "Wear")
    vertex = lambda channel, default: g.lerp(g.const(default), (colour_v, channel), use_vertex)
    vertex_glow, vertex_ao, vertex_wear = vertex("G", 1.0), vertex("B", 1.0), vertex("A", 0.0)

    def layer(p):
        uv = g.uv(g.scalar(p + "Tiling", 1.0, p))
        d = g.texture(p + "DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv, p)
        spec = (g.texture(p + "SpecularMap", g.white, ST.SAMPLERTYPE_COLOR, uv, p), "RGB")
        return dict(
            uv=uv, alpha=(d, "A"), colour=g.mul((d, "RGB"), g.vector(p + "Tint", (1, 1, 1), p)),
            f0=g.mul(g.vector(p + "Specular", (0.04, 0.04, 0.04), p), spec),
            normal=(g.texture(p + "NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv, p), "RGB"),
            smoothness=g.smoothness(p, uv))

    primary, wear = layer("Layer0"), layer("Wear")
    wear_amount = g.saturate(g.sub(g.scalar("WearBlendBase", 0.0, "Wear"), g.mul(vertex_wear, g.const(0.95))))
    blend_map = (g.texture("WearBlendMap", g.white_linear, ST.SAMPLERTYPE_MASKS, primary["uv"], "Wear"), "R")
    wear_alpha = g.custom(WEAR_ALPHA, unreal.CustomMaterialOutputType.CMOT_FLOAT1,
                          BlendMap=blend_map, Amount=wear_amount, Softness=g.scalar("WearFalloff", 0.5, "Wear"))
    paint = g.scalar("WearPaint", 0.0, "Wear")
    wear_weight = g.custom(WEAR_REMAP, unreal.CustomMaterialOutputType.CMOT_FLOAT1,
                           WearAlpha=wear_alpha, Paint=paint, Cloth=g.scalar("WearCloth", 0.0, "Wear"))
    worn = g.custom(WEAR_PRIMER, unreal.CustomMaterialOutputType.CMOT_FLOAT4, Albedo=primary["colour"],
                    Smoothness=primary["smoothness"], PrimaryWear=g.one_minus(wear_alpha), Paint=paint)
    worn_colour = g.op(unreal.MaterialExpressionComponentMask, worn, names=("",), r=True, g=True, b=True, a=False)
    worn_smooth = g.op(unreal.MaterialExpressionComponentMask, worn, names=("",), r=False, g=False, b=False, a=True)
    # %STENCIL_MAP (STENCIL_TINT): TexSlot7 at (UseUV2ForStencil ? UV1 : UV0) x StencilTiling; colours resolved by
    # the builder (StencilTintOverride: authored, palette main tints or palette decal colours).
    check_ported(STENCIL_PORTED)
    stencil_uv = g.mul(g.lerp((g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0), ""),
                              (g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1), ""),
                              g.scalar("StencilUV2", 0.0, "Stencil")),
                       g.scalar("StencilTiling", 1.0, "Stencil"))
    stencil = (g.texture("StencilMap", g.white_linear, ST.SAMPLERTYPE_MASKS, stencil_uv, "Stencil"), "RGB")
    worn_colour = g.custom(STENCIL_TINT, unreal.CustomMaterialOutputType.CMOT_FLOAT3, Albedo=worn_colour, Dist=stencil,
                           Opacity=g.mul(g.scalar("StencilOpacity", 1.0, "Stencil"), g.scalar("UseStencil", 0.0, "Stencil")),
                           C1=g.vector("StencilColor1", (1, 1, 1), "Stencil"), C2=g.vector("StencilColor2", (1, 1, 1), "Stencil"),
                           C3=g.vector("StencilColor3", (1, 1, 1), "Stencil"))
    blend = lambda a, b: g.lerp(a, b, wear_weight)  # weights (1 - w, w) sum to 1
    albedo = blend(worn_colour, wear["colour"])
    f0 = blend(primary["f0"], wear["f0"])
    smooth = blend(worn_smooth, wear["smoothness"])
    layer_normal = g.reconstruct_z(blend(primary["normal"], wear["normal"]))

    macro_uv = g.uv(g.scalar("MacroTiling", 1.0, "Macro"))
    macro = (g.texture("MacroNormalMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, macro_uv, "Macro"), "RGB")
    normal = g.detail_normal(macro, layer_normal)
    macro_gloss = g.lerp(g.const(1.0), g.map_smoothness("MacroSmoothnessMap", macro_uv, "Macro"), g.scalar("UseMacroGloss", 0.0, "Macro"))
    smooth = g.mul(smooth, macro_gloss)
    ao = g.mul(vertex_ao, (g.texture("MacroAOMap", g.white_linear, ST.SAMPLERTYPE_MASKS, macro_uv, "Macro"), "R"))
    # HardSurface.cfx GetLayerAttribs: dirt after the macro surface, before Albedo/Reflectance *= AO; the blend map is
    # the wear blend map, the vertex dirt comes from the vertex wear and the dirt AO is the vertex AO.
    albedo, f0, smooth, dirt = g.dirt(albedo, f0, smooth, blend_map, vertex_wear, vertex_ao)
    g.last_uv = base_uv
    albedo, f0 = g.mul(albedo, ao), g.mul(f0, ao)
    # HardSurface.cfx GetMaterialAttribs: glowColour = attribs.Albedo, the wear-blended albedo after the AO multiply
    # (before the lighting energy split), x the alpha ramp x the vertex glow mask.
    emissive = g.mul(g.mul(g.glow(albedo, primary["alpha"], ramp=True), vertex_glow), g.one_minus(dirt))  # x (1 - dirt)
    g.slab(g.conserve(albedo, f0), f0, None, g.gloss_to_roughness(smooth), normal, emissive)
    g.out(*g.add(g.sub(primary["alpha"], g.scalar("AlphaTest", 0.0)), g.const(0.5)), MP.MP_OPACITY_MASK)
    return g.save()


# glassLib.cfib GlassShared. transmissiveColour = MatDifColor (palette-tinted by the builder), x IN.Color.rgb under
# %PER_VERTEX_TINT, x tintTexture under %TINT_MAP. glass.Height = saturate((IN.Color.b - 0.25) * 3) under
# %PER_VERTEX_CURVATURE, x heightTexture under %HEIGHT_MAP. colourDensity = (1 / max(colour, 0.01) - 1) * 0.1 and
# transmittanceColour = exp(-colourDensity * path). The numbers arrive as instance parameters from FSCStarEngine;
# the master defaults are inert (no absorption). This is the transmittance at normal incidence (path = Height).
GLASS_TRANSMITTANCE = """
float3 colour = Colour * lerp(float3(1, 1, 1), Vertex.rgb, UseVertexTint) * lerp(float3(1, 1, 1), TintMap, UseTintMap);
float height = lerp(1.0, saturate((Vertex.b - HeightBias) * HeightScale), UseVertexHeight) * lerp(1.0, HeightMap, UseHeightMap);
float3 density = (1.0 / max(colour, Floor) - 1.0) * DensityScale;
return exp(-density * height);
"""
# glassLib.cfib under %HEIGHT_MAP: clip(heightMap - 0.01).
GLASS_COVERAGE = "return lerp(1.0, HeightMap < Clip ? 0.0 : 1.0, UseHeightMap);"
# The glass graph is a port of these game functions. StarEngineRender.json pins their token hashes and
# scripts/verify_star_engine_render.py re-checks them against the installed game; the master is not built from a
# fixture that no longer carries the hashes the port was written against.
GLASS_PORTED = {
    "GlassShared": "c9e71e7cb2bf0a6c75d5ae829f849a7763d8508210d41bbec4a6ddb432281ad4",
    "ReflectanceToIOR": "96a922e87560903c7cd13017ebded86cdaca5bc81e1957ce83c9671b8da33634",
}


def check_ported(ported):
    fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Plugins", "SCBridge", "Resources", "StarEngineRender.json")
    with open(fixture, encoding="utf-8") as handle:
        functions = json.load(handle)["shader_functions"]
    for name, sha in ported.items():
        if functions.get(name, {}).get("sha256") != sha:
            raise RuntimeError(f"{name}: the game shader changed since the port was written; re-derive the graph")


def glass_master():
    """M_SC_Glass, the game's GlassPBR (ue/verification/WP-U3b.md §14): a clear dielectric slab, albedo 0, F0 =
    SpecularColor, whose only colour is absorption through its height. The slab is a Substrate *simple volume*: UE
    evaluates it like the game does (GlassShared), refracting the view ray with the IOR of F0, taking Beer-Lambert
    transmittance along the refracted path and multiplying by (1 - Fresnel). TransmittanceToMFP converts the
    normal-incidence transmittance into the slab's mean free path. Any other sub-surface type has no transmittance
    in a translucent blend mode and draws the slab opaque. Coverage is 1: the authored Opacity is not read. Every
    glass map is read at the game's glassUV = baseUV * GlassTiling, v *= GlassVStretch."""
    check_ported(GLASS_PORTED)
    g = Graph("M_SC_Glass", unreal.BlendMode.BLEND_TRANSLUCENT_COLORED_TRANSMITTANCE,
              unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    uv = g.mul(g.uv(g.scalar("GlassTiling", 1.0)),
               g.op(unreal.MaterialExpressionAppendVector, g.const(1.0), g.scalar("GlassVStretch", 1.0)))
    g.last_uv = uv
    normal = (g.texture("NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv), "RGB")
    vertex = (g.node(unreal.MaterialExpressionVertexColor), "")
    height_map = (g.texture("HeightMap", g.white_linear, ST.SAMPLERTYPE_MASKS, uv), "R")
    use_height_map = g.scalar("UseHeightMap", 0.0)
    f1, f3 = unreal.CustomMaterialOutputType.CMOT_FLOAT1, unreal.CustomMaterialOutputType.CMOT_FLOAT3
    transmittance = g.custom(
        GLASS_TRANSMITTANCE, f3, Colour=g.vector("GlassColor", (1, 1, 1)), Vertex=vertex,
        UseVertexTint=g.scalar("UseVertexTint", 0.0), TintMap=(g.texture("TintMap", g.white, ST.SAMPLERTYPE_COLOR, uv), "RGB"),
        UseTintMap=g.scalar("UseTintMap", 0.0), HeightBias=g.scalar("GlassVertexHeightBias", 0.0),
        HeightScale=g.scalar("GlassVertexHeightScale", 1.0), UseVertexHeight=g.scalar("UseVertexHeight", 0.0),
        HeightMap=height_map, UseHeightMap=use_height_map, Floor=g.scalar("GlassColorFloor", 1.0),
        DensityScale=g.scalar("GlassDensityScale", 0.0))
    mfp = g.node(unreal.MaterialExpressionSubstrateTransmittanceToMFP)
    g.link(*transmittance, mfp, "TransmittanceColor")
    # Fresnel: the game's lerp(Reflectance, 1, Schlick) (F0 = SpecularColor, F90 = 1); under %THIN_FILM (UseThinFilm)
    # UE's thin-film node instead. The node is not an identity at zero thickness: it always adds the film's own
    # dielectric interface (IOR 1.44) and so raises a clear glass's F0 several-fold, which drew a milky reflection over
    # every canopy (WP-U3b.md section 14.8). It is therefore selected, not fed a zero thickness.
    f0 = g.vector("SpecularColor", (0.04, 0.04, 0.04))
    f90 = g.const3((1, 1, 1))
    film = g.node(unreal.MaterialExpressionSubstrateThinFilm)
    g.link(*normal, film, "Normal")
    g.link(*f0, film, "F0")
    g.link(*f90, film, "F90")
    g.link(*g.scalar("ThinFilmThickness", 0.0), film, "Thickness")
    use_film = g.scalar("UseThinFilm", 0.0)
    slab = g.slab(g.const3((0, 0, 0)), g.lerp(f0, (film, "Specular Color"), use_film),
                  g.lerp(f90, (film, "Edge Specular Color"), use_film), g.roughness("", uv), normal, None, (mfp, "MFP"),
                  front=False)
    slab.set_editor_property("sub_surface_type", unreal.MaterialSubSurfaceType.MSS_SIMPLE_VOLUME)
    g.weight(slab, g.custom(GLASS_COVERAGE, f1, HeightMap=height_map, UseHeightMap=use_height_map,
                            Clip=g.scalar("GlassHeightClip", 0.0)))
    return g.save()


def decal_surface(g):
    """Shared decal inputs: POM (bump offset from height) UVs, TexSlot1 sample, coverage = alpha x Opacity."""
    base_uv = g.uv(g.scalar("Tiling", 1.0))
    height = (g.texture("HeightMap", g.black_linear, ST.SAMPLERTYPE_MASKS, base_uv), "R")
    bump = g.node(unreal.MaterialExpressionBumpOffset)
    g.link(*base_uv, bump, "Coordinate")
    g.link(*height, bump, "Height")
    g.link(*g.scalar("PomDisplacement", 0.0), bump, "HeightRatioInput")
    uv = (bump, "")
    g.last_uv = uv
    diffuse = g.texture("DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv)
    normal = (g.texture("NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv), "RGB")
    coverage = g.saturate(g.mul((diffuse, "A"), g.scalar("Opacity", 1.0)))
    return uv, diffuse, normal, coverage


def decal_master(name, colour):
    """Mesh decals rendered into the DBuffer (Substrate Convert-To-Decal: deferred-decal domain on the split
    non-Nanite mesh), so they modify the host under the decal's coverage instead of drawing their own surface.

    M_SC_MeshDecal writes colour (decal texture x colour x host tint), F0, roughness, normal and glow.
    M_SC_MeshDecal_Relief writes normal + roughness only: control-only POM / colourless decals never change
    the host's base colour, which is what produces the dark panel grooves.
    """
    g = Graph(name, unreal.BlendMode.BLEND_TRANSLUCENT)
    uv, diffuse, normal, coverage = decal_surface(g)
    if colour:
        base = g.mul(g.mul((diffuse, "RGB"), g.vector("DiffuseColor", (1, 1, 1))), g.vector("HostTint", (1, 1, 1)))
        # Stencil decals ($TintPaletteDecal): palettes.json decal red/green/blue chosen by the stencil channels,
        # each x StencilDiffuseColor{,2,3}; coverage = channel sum x StencilOpacity.
        stencil = g.texture("StencilMap", g.black_linear, ST.SAMPLERTYPE_MASKS, uv)
        st_colour = None
        for c, (pal, tint) in zip("RGB", (("DecalRed", "StencilTint1"), ("DecalGreen", "StencilTint2"), ("DecalBlue", "StencilTint3"))):
            term = g.mul(g.mul((stencil, c), g.vector(pal, (1, 1, 1))), g.vector(tint, (1, 1, 1)))
            st_colour = term if st_colour is None else g.add(st_colour, term)
        st_cover = g.mul(g.saturate(g.add(g.add((stencil, "R"), (stencil, "G")), (stencil, "B"))), g.scalar("StencilOpacity", 1.0))
        use = g.scalar("UseStencil", 0.0)
        base = g.lerp(base, st_colour, use)
        coverage = g.lerp(coverage, st_cover, use)
        f0 = g.mul(g.vector("SpecularColor", (0.04, 0.04, 0.04)),
                   (g.texture("SpecularMap", g.white, ST.SAMPLERTYPE_COLOR, uv), "RGB"))
        # MeshDecal.cfx dirt (WP-U2b): the decal's own attributes take the viewer dirt, AO 1. BlendTexture at baseTC x
        # WearTiling when exported (UseDirtBlendMap); without one the mask is DECAL_DIRT_MASK (viewer choice: the
        # game's default binding is not recoverable from the shader text). Vertex wear under %VERTCOLORS.
        blend_uv = g.uv(g.scalar("DirtBlendTiling", 1.0, "Dirt"))
        blend = g.lerp(g.const(DECAL_DIRT_MASK), (g.texture("DirtBlendMap", g.white_linear, ST.SAMPLERTYPE_MASKS, blend_uv, "Dirt"), "R"),
                       g.scalar("UseDirtBlendMap", 0.0, "Dirt"))
        vertex_wear = g.lerp(g.const(0.0), (g.node(unreal.MaterialExpressionVertexColor), "A"), g.scalar("UseVertexColors", 0.0, "Dirt"))
        smooth = g.smoothness("", uv)  # dirt works on smoothness (DIRT_GLOSS); converted with the game's mapping below
        g.last_uv = uv
        vertex_albedo, vertex_glow, vertex_alpha = g.vertex_illum()  # Illum decals' %VERTCOLORS (WP-U2c)
        glow = g.mul(g.glow(base, (diffuse, "A")), vertex_glow)
        base = g.mul(base, vertex_albedo)
        coverage = g.mul(coverage, vertex_alpha)
        base, f0, smooth, dirt = g.dirt(base, f0, smooth, blend, vertex_wear, g.const(1.0), object_space=False)
        slab = g.slab(g.conserve(base, f0), f0, None, g.gloss_to_roughness(smooth), normal,
                      g.mul(glow, g.one_minus(dirt)), front=False)
    else:
        slab = g.slab(None, None, None, g.roughness("", uv), normal, front=False)
    decal = g.node(unreal.MaterialExpressionSubstrateConvertToDecal)
    g.link_any(slab, "", decal, ("DecalMaterial", "Decal Material"))
    g.link_any(*coverage, decal, ("Coverage",))
    g.out(decal, "", MP.MP_FRONT_MATERIAL)
    return g.save()


def translucent_master():
    """M_SC_Translucent: lit translucent surface for faded / blended Illum (Opacity < 1, alpha_config blend):
    coverage = diffuse alpha x Opacity through a Substrate weight, glow = colour x emissive x strength."""
    g = Graph("M_SC_Translucent", unreal.BlendMode.BLEND_TRANSLUCENT,
              unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    uv, diffuse, normal, coverage = decal_surface(g)
    base = g.mul((diffuse, "RGB"), g.vector("DiffuseColor", (1, 1, 1)))
    f0 = g.mul(g.vector("SpecularColor", (0.04, 0.04, 0.04)),
               (g.texture("SpecularMap", g.white, ST.SAMPLERTYPE_COLOR, uv), "RGB"))
    vertex_albedo, vertex_glow, vertex_alpha = g.vertex_illum()  # Illum %VERTCOLORS (WP-U2c)
    emissive = g.mul(g.glow(base, (diffuse, "A")), vertex_glow)
    base = g.mul(base, vertex_albedo)
    slab = g.slab(g.conserve(base, f0), f0, None, g.roughness("", uv), normal, emissive, front=False)
    g.weight(slab, g.mul(coverage, vertex_alpha))
    return g.save()


def unlit_master():
    """M_SC_Emissive_Unlit: additive emissive screens/holograms and forward glow cards
    (diffuse x colour x emissive x strength, optionally weighted by texture alpha)."""
    g = Graph("M_SC_Emissive_Unlit", unreal.BlendMode.BLEND_ADDITIVE)
    uv = g.uv(g.scalar("Tiling", 1.0))
    diffuse = g.texture("DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv)
    colour = g.mul(g.mul((diffuse, "RGB"), g.vector("DiffuseColor", (1, 1, 1))), g.vector("EmissiveColor", (1, 1, 1)))
    # Glow cards (forward-flag Illum): texture alpha weights the additive glow so soft gradients survive,
    # gated at AlphaGlowCutoff. Screens keep AlphaWeight = 0 (full image).
    alpha = (diffuse, "A")
    gate = g.saturate(g.mul(g.sub(alpha, g.scalar("AlphaGlowCutoff", 0.0)), g.const(1.0e4)))
    weight = g.lerp(g.const(1.0), g.mul(alpha, gate), g.scalar("AlphaWeight", 0.0))
    # Illum %VERTCOLORS on a glow card (WP-U2c): the glow mask and the blending opacity from the vertex colour.
    _, vertex_glow, vertex_alpha = g.vertex_illum()
    weight = g.mul(weight, vertex_alpha)
    unlit = g.node(unreal.MaterialExpressionSubstrateUnlitBSDF)
    g.link(*g.mul(g.mul(g.mul(colour, vertex_glow), g.scalar("EmissiveStrength", 0.0)), weight), unlit, "EmissiveColor")
    g.out(unlit, "", MP.MP_FRONT_MATERIAL)
    return g.save()


def variants(master, combos=(("_Masked", True, False), ("_TwoSided", False, True), ("_Masked_TwoSided", True, True))):
    """Constant instances that only override blend mode / two-sidedness (shader permutations built here)."""
    for suffix, masked, two_sided in combos:
        name = "MI" + master.get_name()[1:] + suffix
        mi = tools.create_asset(name, ROOT, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mel.set_material_instance_parent(mi, master)
        o = mi.get_editor_property("base_property_overrides")
        if masked:
            o.set_editor_property("override_blend_mode", True)
            o.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        if two_sided:
            o.set_editor_property("override_two_sided", True)
            o.set_editor_property("two_sided", True)
        mi.set_editor_property("base_property_overrides", o)
        mel.update_material_instance(mi)
        eal.save_loaded_asset(mi)
        unreal.log(f"SC_MATERIAL {mi.get_path_name()} parent={master.get_name()}")


if not unreal.SystemLibrary.get_console_variable_bool_value("r.Substrate"):
    raise RuntimeError("r.Substrate is off: the SC masters are Substrate graphs (see ue/verification/WP-U2.md)")
# Regenerate from scratch so the checked-in assets always equal this script's output. Packages that the
# commandlet has not loaded are removed on disk first (editor-side deletion leaves them in this session).
CONTENT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Plugins", "SCBridge", "Content", "Materials")
# M_SC_LightFunction belongs to create_light_material.py (WP-U3) and M_SC_DisplayMapping to
# create_display_mapping_material.py (WP-U3b §13); both are kept.
KEEP = {"M_SC_LightFunction.uasset", "M_SC_DisplayMapping.uasset"}
for entry in (os.listdir(CONTENT) if os.path.isdir(CONTENT) else []):
    path = os.path.join(CONTENT, entry)
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)
    elif entry not in KEEP:
        os.remove(path)
def dirt_collection():
    """MPC_SCDirt: the viewer's dirt amount and colour (FSCDirt sets them from USCViewerSettings at run time)."""
    mpc = tools.create_asset("MPC_SCDirt", ROOT, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
    amount = unreal.CollectionScalarParameter()
    amount.set_editor_property("parameter_name", "DirtAmount")
    amount.set_editor_property("default_value", 0.0)
    colour = unreal.CollectionVectorParameter()
    colour.set_editor_property("parameter_name", "DirtColor")
    colour.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    mpc.set_editor_property("scalar_parameters", [amount])
    sh = unreal.CollectionVectorParameter()
    sh.set_editor_property("parameter_name", "DirtSH")
    sh.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    mpc.set_editor_property("vector_parameters", [colour, sh])
    eal.save_loaded_asset(mpc)
    unreal.log(f"SC_MATERIAL {mpc.get_path_name()} collection")
    return mpc


Graph.dirt_collection = dirt_collection()
Graph.white = default_texture("T_SC_White", (255, 255, 255, 255), True, unreal.TextureCompressionSettings.TC_DEFAULT)
Graph.white_linear = default_texture("T_SC_WhiteLinear", (255, 255, 255, 255), False, unreal.TextureCompressionSettings.TC_MASKS)
Graph.black_linear = default_texture("T_SC_BlackLinear", (0, 0, 0, 255), False, unreal.TextureCompressionSettings.TC_MASKS)
Graph.flat_normal = default_texture("T_SC_FlatNormal", (128, 128, 255, 255), False, unreal.TextureCompressionSettings.TC_NORMALMAP)
for opaque in (surface_master("M_SC_Illum"), hardsurface_master(),
               layered_master("M_SC_Layered", LAYERS), surface_master("M_SC_Fallback")):
    variants(opaque)
glass_master()
for sided in (decal_master("M_SC_MeshDecal", True), decal_master("M_SC_MeshDecal_Relief", False), translucent_master()):
    variants(sided, (("_TwoSided", False, True),))  # is_double_sided decals keep reverse-facing triangles
unlit_master()
unreal.log("WP-U2 MATERIALS_OK")
