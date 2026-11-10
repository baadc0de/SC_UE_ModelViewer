"""WP-U2: (re)create the SCBridge Substrate master materials and their blend/two-sided variants.

Run headless inside the Python commandlet with r.Substrate enabled (project default):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/create_materials.py \
      -unattended -nop4 -nosplash -ddc=SCViewerDDC
Everything lives under /SCBridge/Materials (ue/Plugins/SCBridge/Content/Materials). Parameter names are
SC-semantic and model-neutral; FSCMaterialBuilder (C++) sets them. No asset values are baked in here:
every default is a neutral identity (white, flat normal, 0/1) and the builder supplies sidecar data.
The graphs do carry shading rules of the game's own shaders (gloss -> roughness, layer blend order, macro
surface, HAL luminance range, HardSurface static wear); those are shader logic, the same for every asset, cited
where they appear. Game functions with branches or many terms are Custom (HLSL) nodes transcribed from the
shader text, so they can be read against it.
"""
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
        if not mel.connect_material_expressions(a, out, b, inp):
            raise RuntimeError(f"{self.m.get_name()}: cannot link {a.get_name()}.{out!r} -> {b.get_name()}.{inp!r}")

    def out(self, a, out, prop):
        if not mel.connect_material_property(a, out, prop):
            raise RuntimeError(f"{self.m.get_name()}: cannot connect {a.get_name()}.{out!r} -> {prop}")

    def scalar(self, name, value, group="SC"):
        return (self.node(unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=value, group=group), "")

    def vector(self, name, value, group="SC"):
        return (self.node(unreal.MaterialExpressionVectorParameter, parameter_name=name,
                          default_value=unreal.LinearColor(*value), group=group), "RGB")

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
        """Smoothness stored by the exporter as a grayscale map r = sqrt(1 - smoothness): 1 - r^2."""
        r = (self.texture(name, self.white_linear, ST.SAMPLERTYPE_MASKS, uv, group), "R")
        return self.one_minus(self.mul(r, r))

    def smoothness(self, prefix, uv):
        """Game smoothness (gloss): lerp(<p>Smoothness, map x <p>GlossMult, <p>UseRoughnessMap)."""
        from_map = self.mul(self.map_smoothness(prefix + "RoughnessMap", uv), self.scalar(prefix + "GlossMult", 1.0))
        return self.lerp(self.scalar(prefix + "Smoothness", 0.0), from_map, self.scalar(prefix + "UseRoughnessMap", 0.0))

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
    emissive = g.glow(base, (diffuse, "A"))
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
      MacroNormalMap / MacroRoughnessMap (UseMacroGloss) at UV x MacroTiling (the builder sets 1: LayerBlend
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
    macro_gloss = g.lerp(g.const(1.0), g.map_smoothness("MacroRoughnessMap", macro_uv, "Macro"), g.scalar("UseMacroGloss", 0.0, "Macro"))
    smooth = g.mul(result["smoothness"], macro_gloss)
    albedo, f0 = g.mul(result["albedo"], ao), g.mul(result["f0"], ao)
    g.last_uv = base_uv
    emissive = g.glow(result["colour"], result["alpha"])
    g.slab(g.conserve(albedo, f0), f0, None, g.gloss_to_roughness(smooth), normal, emissive)
    g.out(*g.add(g.sub(result["alpha"], g.scalar("AlphaTest", 0.0)), g.const(0.5)), MP.MP_OPACITY_MASK)
    return g.save()


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
      Then LayerAttribs_AddMacroSurface (MacroNormalMap / MacroRoughnessMap at UV x MacroTiling),
        albedo, F0 x AO, AO = vertex B x MacroAOMap.r, and the deferred energy split (Graph.conserve).
      Opacity (alpha test) is the Primary diffuse alpha; the glow is masked by vertex G (GetMaterialAttribs).
    Not drawn: per-instance wear/dirt/damage, detail maps, the paint/cloth complement hue (HueLUT),
    retroreflective normals, %SECONDARY_LAYER, stencils, iridescence.
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
    blend = lambda a, b: g.lerp(a, b, wear_weight)  # weights (1 - w, w) sum to 1
    albedo = blend(worn_colour, wear["colour"])
    f0 = blend(primary["f0"], wear["f0"])
    smooth = blend(worn_smooth, wear["smoothness"])
    layer_normal = g.reconstruct_z(blend(primary["normal"], wear["normal"]))

    macro_uv = g.uv(g.scalar("MacroTiling", 1.0, "Macro"))
    macro = (g.texture("MacroNormalMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, macro_uv, "Macro"), "RGB")
    normal = g.detail_normal(macro, layer_normal)
    macro_gloss = g.lerp(g.const(1.0), g.map_smoothness("MacroRoughnessMap", macro_uv, "Macro"), g.scalar("UseMacroGloss", 0.0, "Macro"))
    smooth = g.mul(smooth, macro_gloss)
    ao = g.mul(vertex_ao, (g.texture("MacroAOMap", g.white_linear, ST.SAMPLERTYPE_MASKS, macro_uv, "Macro"), "R"))
    g.last_uv = base_uv
    albedo, f0 = g.mul(albedo, ao), g.mul(f0, ao)
    # HardSurface.cfx GetMaterialAttribs: glowColour = attribs.Albedo, the wear-blended albedo after the AO multiply
    # (before the lighting energy split), x the alpha ramp x the vertex glow mask.
    emissive = g.mul(g.glow(albedo, primary["alpha"], ramp=True), vertex_glow)
    g.slab(g.conserve(albedo, f0), f0, None, g.gloss_to_roughness(smooth), normal, emissive)
    g.out(*g.add(g.sub(primary["alpha"], g.scalar("AlphaTest", 0.0)), g.const(0.5)), MP.MP_OPACITY_MASK)
    return g.save()


def glass_master():
    """M_SC_Glass: colored-transmittance slab, MFP from (palette) tint, thin-film F0/F90, DDNA normal + roughness."""
    g = Graph("M_SC_Glass", unreal.BlendMode.BLEND_TRANSLUCENT_COLORED_TRANSMITTANCE,
              unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    uv = g.uv(g.scalar("Tiling", 1.0))
    normal = (g.texture("NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv), "RGB")
    tint = g.lerp(g.const3((1, 1, 1)), g.vector("GlassTint", (1, 1, 1)), g.scalar("GlassTintStrength", 0.0))
    mfp = g.node(unreal.MaterialExpressionSubstrateTransmittanceToMFP)
    g.link(*tint, mfp, "TransmittanceColor")
    film = g.node(unreal.MaterialExpressionSubstrateThinFilm)
    g.link(*normal, film, "Normal")
    g.link(*g.vector("SpecularColor", (0.04, 0.04, 0.04)), film, "F0")
    g.link(*g.const3((1, 1, 1)), film, "F90")
    g.link(*g.scalar("ThinFilmThickness", 0.0), film, "Thickness")
    slab = g.slab(g.const3((0, 0, 0)), (film, "Specular Color"), (film, "Edge Specular Color"), g.roughness("", uv), normal,
                  None, (mfp, "MFP"), front=False)
    g.weight(slab, g.saturate(g.scalar("Opacity", 1.0)))
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
        slab = g.slab(g.conserve(base, f0), f0, None, g.roughness("", uv), normal,
                      g.glow(base, (diffuse, "A")), front=False)
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
    slab = g.slab(g.conserve(base, f0), f0, None, g.roughness("", uv), normal,
                  g.glow(base, (diffuse, "A")), front=False)
    g.weight(slab, coverage)
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
    unlit = g.node(unreal.MaterialExpressionSubstrateUnlitBSDF)
    g.link(*g.mul(g.mul(colour, g.scalar("EmissiveStrength", 0.0)), weight), unlit, "EmissiveColor")
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
# M_SC_LightFunction belongs to create_light_material.py (WP-U3) and is kept.
KEEP = {"M_SC_LightFunction.uasset"}
for entry in (os.listdir(CONTENT) if os.path.isdir(CONTENT) else []):
    path = os.path.join(CONTENT, entry)
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)
    elif entry not in KEEP:
        os.remove(path)
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
