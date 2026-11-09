"""WP-U2: (re)create the SCBridge Substrate master materials and their blend/two-sided variants.

Run headless inside the Python commandlet with r.Substrate enabled (project default):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/create_materials.py \
      -unattended -nop4 -nosplash -ddc=SCViewerDDC
Everything lives under /SCBridge/Materials (ue/Plugins/SCBridge/Content/Materials). Parameter names are
SC-semantic and model-neutral; FSCMaterialBuilder (C++) sets them. No game values are baked in here:
every default is a neutral identity (white, flat normal, 0/1) and the builder supplies sidecar data.
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

    def uv(self, tiling):
        coords = (self.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0), "")
        self.last_uv = self.mul(coords, tiling)
        return self.last_uv

    def roughness(self, prefix, uv):
        """UE perceptual roughness from the exporter's grayscale map (sqrt(1 - smoothness)) or a constant.

        smoothness = lerp(<p>Smoothness, (1 - r^2) * <p>GlossMult, <p>UseRoughnessMap); roughness = sqrt(1 - smoothness).
        """
        r = (self.texture(prefix + "RoughnessMap", self.white_linear, ST.SAMPLERTYPE_MASKS, uv), "R")
        from_map = self.mul(self.one_minus(self.mul(r, r)), self.scalar(prefix + "GlossMult", 1.0))
        smooth = self.lerp(self.scalar(prefix + "Smoothness", 0.0), from_map, self.scalar(prefix + "UseRoughnessMap", 0.0))
        return self.sqrt(self.saturate(self.one_minus(smooth)))

    def link_any(self, a, out, b, names):
        for name in names:
            if mel.connect_material_expressions(a, out, b, name):
                return
        raise RuntimeError(f"{self.m.get_name()}: cannot link {a.get_name()} -> {b.get_name()}.{names}")

    def glow(self, colour, alpha):
        """Emission = colour x Emissive x EmissiveStrength (builder: Glow x host scale; 0 when Glow is absent),
        optionally from the emittance map, gated by texture alpha above AlphaGlowCutoff (step)."""
        source = self.lerp(colour, (self.texture("EmissiveMap", self.white, ST.SAMPLERTYPE_COLOR, self.last_uv), "RGB"),
                           self.scalar("UseEmissiveMap", 0.0))
        gate = self.saturate(self.mul(self.sub(alpha, self.scalar("AlphaGlowCutoff", 0.0)), self.const(1.0e4)))
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
    albedo = g.mul(base, g.one_minus(g.scalar("Metallic", 0.0)))
    emissive = g.glow(base, (diffuse, "A"))
    g.slab(albedo, f0, None, g.roughness("", uv), normal, emissive)
    mask = g.add(g.sub((diffuse, "A"), g.scalar("AlphaTest", 0.0)), g.const(0.5))
    g.out(*mask, MP.MP_OPACITY_MASK)
    return g.save()


def layered_master(name, count):
    """M_SC_HardSurface (1 layer) / M_SC_Layered (4): per-layer tiling, tint (palette x tint x diffuse), F0, metallic.

    Layers blend like LayerBlend_V2: start from layer N-1, lerp toward layer i by blend-mask channel i (R,G,B).
    UseBlendMask=0 forces mask (1,1,1) which selects Layer0.
    """
    g = Graph(name, unreal.BlendMode.BLEND_OPAQUE, nanite=True)
    layers = []
    for i in range(count):
        p = f"Layer{i}"
        uv = g.uv(g.scalar(p + "Tiling", 1.0, p))
        d = g.texture(p + "DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv, p)
        colour = g.mul((d, "RGB"), g.vector(p + "Tint", (1, 1, 1), p))
        metallic = g.scalar(p + "Metallic", 0.0, p)
        layers.append(dict(
            albedo=g.mul(colour, g.one_minus(metallic)),
            f0=g.vector(p + "Specular", (0.04, 0.04, 0.04), p),
            normal=(g.texture(p + "NormalGlossMap", g.flat_normal, ST.SAMPLERTYPE_NORMAL, uv, p), "RGB"),
            roughness=g.roughness(p, uv),
            colour=colour, alpha=(d, "A")))
    result = dict(layers[-1])
    if count > 1:
        mask_uv = g.uv(g.scalar("BlendMaskTiling", 1.0))
        mask = (g.texture("BlendMaskMap", g.white_linear, ST.SAMPLERTYPE_MASKS, mask_uv), "RGB")
        mask = g.lerp(g.const3((1, 1, 1)), mask, g.scalar("UseBlendMask", 0.0))
        channels = [g.op(unreal.MaterialExpressionComponentMask, mask, names=("",), r=c == 0, g=c == 1, b=c == 2)
                    for c in range(3)]
        for i in range(count - 2, -1, -1):
            for key in ("albedo", "f0", "normal", "roughness", "colour", "alpha"):
                result[key] = g.lerp(result[key], layers[i][key], channels[i])
        result["normal"] = g.normalize(result["normal"])
    g.uv(g.scalar("Tiling", 1.0))
    emissive = g.glow(result["colour"], result["alpha"])
    g.slab(result["albedo"], result["f0"], None, result["roughness"], result["normal"], emissive)
    g.out(*g.add(g.sub(result["alpha"], g.scalar("AlphaTest", 0.0)), g.const(0.5)), MP.MP_OPACITY_MASK)
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
        slab = g.slab(g.mul(base, g.one_minus(g.scalar("Metallic", 0.0))), f0, None, g.roughness("", uv), normal,
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
    slab = g.slab(g.mul(base, g.one_minus(g.scalar("Metallic", 0.0))), f0, None, g.roughness("", uv), normal,
                  g.glow(base, (diffuse, "A")), front=False)
    g.weight(slab, coverage)
    return g.save()


def unlit_master():
    """M_SC_Emissive_Unlit: additive emissive screens/holograms (diffuse x colour x emissive x strength)."""
    g = Graph("M_SC_Emissive_Unlit", unreal.BlendMode.BLEND_ADDITIVE)
    uv = g.uv(g.scalar("Tiling", 1.0))
    diffuse = g.texture("DiffuseMap", g.white, ST.SAMPLERTYPE_COLOR, uv)
    colour = g.mul(g.mul((diffuse, "RGB"), g.vector("DiffuseColor", (1, 1, 1))), g.vector("EmissiveColor", (1, 1, 1)))
    unlit = g.node(unreal.MaterialExpressionSubstrateUnlitBSDF)
    g.link(*g.mul(colour, g.scalar("EmissiveStrength", 0.0)), unlit, "EmissiveColor")
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
shutil.rmtree(CONTENT, ignore_errors=True)
Graph.white = default_texture("T_SC_White", (255, 255, 255, 255), True, unreal.TextureCompressionSettings.TC_DEFAULT)
Graph.white_linear = default_texture("T_SC_WhiteLinear", (255, 255, 255, 255), False, unreal.TextureCompressionSettings.TC_MASKS)
Graph.black_linear = default_texture("T_SC_BlackLinear", (0, 0, 0, 255), False, unreal.TextureCompressionSettings.TC_MASKS)
Graph.flat_normal = default_texture("T_SC_FlatNormal", (128, 128, 255, 255), False, unreal.TextureCompressionSettings.TC_NORMALMAP)
for opaque in (surface_master("M_SC_Illum"), layered_master("M_SC_HardSurface", 1),
               layered_master("M_SC_Layered", LAYERS), surface_master("M_SC_Fallback")):
    variants(opaque)
glass_master()
for sided in (decal_master("M_SC_MeshDecal", True), decal_master("M_SC_MeshDecal_Relief", False), translucent_master()):
    variants(sided, (("_TwoSided", False, True),))  # is_double_sided decals keep reverse-facing triangles
unlit_master()
unreal.log("WP-U2 MATERIALS_OK")
