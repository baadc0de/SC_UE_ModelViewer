// FSCMaterialBuilder::Describe: pure sidecar -> master/parameter mapping (decomposed-export-contract
// §Material Sidecars, mirroring starbreaker-3d mtl.rs ShaderFamily / AlphaConfig / roughness rules).
#include "SCMaterialBuilder.h"
#include "Materials/Material.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using FObj = TSharedPtr<FJsonObject>;
FObj Parse(const FString& Text)
{
    FObj O; if (!Text.IsEmpty()) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O); return O;
}
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty; const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
FObj Child(const FObj& O, const TCHAR* Key) { const FObj* C; return O && O->TryGetObjectField(Key, C) ? *C : nullptr; }
FString Str(const FObj& O, const TCHAR* Key) { FString S; if (O) O->TryGetStringField(Key, S); return S; }
double Num(const FObj& O, const TCHAR* Key, double Default) { double V; return O && O->TryGetNumberField(Key, V) ? V : Default; }
FLinearColor Rgb(const FObj& O, const TCHAR* Key, const FLinearColor& Default)
{
    const auto& A = Arr(O, Key);
    return A.Num() >= 3 ? FLinearColor(float(A[0]->AsNumber()), float(A[1]->AsNumber()), float(A[2]->AsNumber())) : Default;
}
bool ParseRgb(const FString& Text, FLinearColor& Out)
{
    TArray<FString> Parts; Text.ParseIntoArray(Parts, TEXT(","));
    if (Parts.Num() < 3) return false;
    Out = FLinearColor(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2])); return true;
}
/** Authored .mtl attributes (name/value pairs) and public params, both as strings. */
struct FAuthored
{
    TMap<FString, FString> Attr, Params;
    FLinearColor Color(const TCHAR* Key, const FLinearColor& Default) const
    { FLinearColor C; return Attr.Contains(Key) && ParseRgb(Attr[Key], C) ? C : Default; }
    float Float(const TCHAR* Key, float Default) const { return Attr.Contains(Key) ? FCString::Atof(*Attr[Key]) : Default; }
    float Param(const TCHAR* Key, float Default) const { return Params.Contains(Key) ? FCString::Atof(*Params[Key]) : Default; }
};
FAuthored Authored(const FObj& Sub)
{
    FAuthored A;
    for (const auto& V : Arr(Sub, TEXT("authored_attributes"))) if (V->Type == EJson::Object)
        A.Attr.Add(Str(V->AsObject(), TEXT("name")), Str(V->AsObject(), TEXT("value")));
    if (const FObj P = Child(Sub, TEXT("public_params")))
        for (const auto& Pair : P->Values) { FString S; if (Pair.Value->TryGetString(S)) A.Params.Add(FString(Pair.Key), S); }
    return A;
}
bool HasToken(const FObj& Sub, const TCHAR* Token)
{
    for (const auto& V : Arr(Child(Sub, TEXT("decoded_feature_flags")), TEXT("tokens")))
        if (V->AsString().Equals(Token, ESearchCase::IgnoreCase)) return true;
    return false;
}
/** First exported (non-virtual) texture slot with the semantic role, lowest slot number first. */
FObj Slot(const FObj& Sub, const TCHAR* Role)
{
    FObj Best; int32 BestSlot = MAX_int32;
    for (const auto& V : Arr(Sub, TEXT("texture_slots")))
    {
        const FObj T = V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!T || Str(T, TEXT("role")) != Role || Str(T, TEXT("export_path")).IsEmpty()) continue;
        const int32 Number = FCString::Atoi(*Str(T, TEXT("slot")).RightChop(7)); // "TexSlotN"
        if (Number < BestSlot) { Best = T; BestSlot = Number; }
    }
    return Best;
}
bool HasVirtualRole(const FObj& Sub, const TCHAR* Role)
{
    for (const auto& V : Arr(Sub, TEXT("texture_slots")))
        if (V->Type == EJson::Object && Str(V->AsObject(), TEXT("role")) == Role && Str(V->AsObject(), TEXT("export_path")).IsEmpty()) return true;
    return false;
}
/** derived_textures[] roughness ref paired with a DDNA source by normalized source_path. */
FString DerivedRoughness(const FObj& Sub, const FObj& Normal)
{
    if (!Normal) return FString();
    const FString Source = Str(Normal, TEXT("source_path"));
    for (const auto& V : Arr(Sub, TEXT("derived_textures")))
    {
        const FObj D = V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (D && Str(D, TEXT("role")) == TEXT("roughness") && Str(D, TEXT("source_path")).Equals(Source, ESearchCase::IgnoreCase))
            return Str(D, TEXT("export_path"));
    }
    return FString();
}
FLinearColor PaletteTint(const FSCPalette* Palette, const FString& Channel, bool& bFound)
{
    FLinearColor C = FLinearColor::White;
    bFound = FSCMaterialBuilder::PaletteColor(Palette, Channel, C);
    return bFound ? C : FLinearColor::White;
}
void Tex(FSCMaterialDesc& D, const TCHAR* Param, const FString& Path, ESCTextureRole Role)
{
    if (!Path.IsEmpty()) D.Textures.Add(Param, FSCTextureKey{Path, Role});
}
/** Substrate slab default roughness (MaterialExpressionSubstrate.h) as the masters' smoothness: r = sqrt(1 - s). */
constexpr float SubstrateDefaultSmoothness = 1.f - 0.5f * 0.5f;
/** Smoothness constants: authored Shininess/255 scales the map (or is the constant); a missing value is the
 *  multiplicative identity for the map and the Substrate default roughness for the constant. */
void Gloss(FSCMaterialDesc& D, const FString& Prefix, const TOptional<float>& Shininess, float Mult, bool bMap)
{
    const float Authored = Shininess.IsSet() ? FMath::Clamp(*Shininess / 255.f, 0.f, 1.f) : 1.f;
    D.Scalars.Add(*(Prefix + TEXT("UseRoughnessMap")), bMap ? 1.f : 0.f);
    D.Scalars.Add(*(Prefix + TEXT("GlossMult")), Authored * Mult);
    // TODO(WP-R5): upstream should emit a scalar gloss/roughness when Shininess is absent.
    D.Scalars.Add(*(Prefix + TEXT("Smoothness")), Shininess.IsSet() ? Authored * Mult : SubstrateDefaultSmoothness);
}
TOptional<float> OptionalAttr(const FAuthored& A, const TCHAR* Key) { return A.Attr.Contains(Key) ? TOptional<float>(A.Float(Key, 0.f)) : TOptional<float>(); }
/** Normal + derived roughness + smoothness constants for a direct (non-layer) material. */
void SurfaceGloss(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const TCHAR* NormalRole = TEXT("normal_gloss"))
{
    const FObj Normal = Slot(Sub, NormalRole);
    if (Normal) Tex(D, TEXT("NormalGlossMap"), Str(Normal, TEXT("export_path")), ESCTextureRole::Normal);
    const FString Rough = DerivedRoughness(Sub, Normal);
    Tex(D, TEXT("RoughnessMap"), Rough, ESCTextureRole::Linear);
    Gloss(D, FString(), OptionalAttr(A, TEXT("Shininess")), 1.f, !Rough.IsEmpty()); // authored, no placeholder rule
}
float AuthoredGlow(const FAuthored& A) { return FMath::Max(0.f, A.Float(TEXT("Glow"), A.Float(TEXT("GlowAmount"), 0.f))); }
/** Emission = Emissive x Glow x (Diffuse x base texture | emittance map), gated by alpha above AlphaGlowCutoff
 *  (the masters multiply by the surface colour). Glow absent or 0 emits nothing even with Emissive = 1,1,1;
 *  this deliberately does not use mtl.rs emissive_factor()'s Glow=1 fallback. */
void Emission(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A)
{
    // Authored values only: Emissive = 0,0,0 or Glow absent/0 emits nothing.
    // TODO(WP-R5): Glow > 0 with Emissive = 0,0,0 ("glows", "buttons") emits nothing by this rule; confirm
    // whether the engine treats it as diffuse glow and, if so, emit the effective emissive colour upstream.
    const FLinearColor E = A.Color(TEXT("Emissive"), FLinearColor::Black);
    const bool bColour = E.R > 0.f || E.G > 0.f || E.B > 0.f;
    D.Vectors.Add(TEXT("EmissiveColor"), E);
    D.Scalars.Add(TEXT("EmissiveStrength"), bColour ? AuthoredGlow(A) * FSCMaterialBuilder::GlowToEmissive : 0.f);
    D.Scalars.Add(TEXT("AlphaGlowCutoff"), A.Param(TEXT("AlphaGlowCutoff"), 0.f));
    if (HasToken(Sub, TEXT("EMITTANCE_MAP")))
    {
        // TextureSemanticRole has no emittance role (mtl.rs): route the base-colour slot until it does.
        // TODO(WP-R5): emit an `emittance` texture role for %EMITTANCE_MAP slots.
        if (const FObj Map = Slot(Sub, TEXT("base_color")))
        { Tex(D, TEXT("EmissiveMap"), Str(Map, TEXT("export_path")), ESCTextureRole::Color); D.Scalars.Add(TEXT("UseEmissiveMap"), 1.f); }
    }
}
void Direct(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const FSCPalette* Palette)
{
    if (const FObj Base = Slot(Sub, TEXT("base_color"))) Tex(D, TEXT("DiffuseMap"), Str(Base, TEXT("export_path")), ESCTextureRole::Color);
    if (const FObj Spec = Slot(Sub, TEXT("specular_support"))) Tex(D, TEXT("SpecularMap"), Str(Spec, TEXT("export_path")), ESCTextureRole::Color);
    // Authored Diffuse multiplies the texture; without either, the Substrate default albedo (0.18) applies.
    FLinearColor Diffuse = A.Attr.Contains(TEXT("Diffuse")) || D.Textures.Contains(TEXT("DiffuseMap"))
        ? A.Color(TEXT("Diffuse"), FLinearColor::White) : FLinearColor(0.18f, 0.18f, 0.18f);
    bool bPalette;
    const FLinearColor Tint = PaletteTint(Palette, Str(Child(Child(Sub, TEXT("palette_routing")), TEXT("material_channel")), TEXT("name")), bPalette);
    if (bPalette) Diffuse *= Tint;
    D.Vectors.Add(TEXT("DiffuseColor"), Diffuse);
    // CryEngine spec workflow: F0 is the authored Specular colour (x spec map), used as authored. A missing
    // Specular keeps the masters' F0 default, which is Substrate's (0.04).
    // TODO(WP-R5): resolve the Diffuse = Specular = 1,1,1 sentinel (response in textures) in the sidecar.
    if (A.Attr.Contains(TEXT("Specular"))) D.Vectors.Add(TEXT("SpecularColor"), A.Color(TEXT("Specular"), FLinearColor::Black));
    D.Scalars.Add(TEXT("Metallic"), 0.f); // no metallic in the spec workflow: albedo and F0 are authored directly
    D.Scalars.Add(TEXT("AlphaTest"), A.Float(TEXT("AlphaTest"), 0.f));
    SurfaceGloss(D, Sub, A);
    Emission(D, Sub, A);
}
template <typename V> void CopyPrefix(TMap<FName, V>& Map, const FString& From, const FString& To)
{
    TArray<TPair<FName, V>> Add;
    for (const auto& Pair : Map) if (Pair.Key.ToString().StartsWith(From)) Add.Emplace(FName(To + Pair.Key.ToString().RightChop(From.Len())), Pair.Value);
    for (auto& Pair : Add) Map.Add(Pair.Key, Pair.Value);
}
/** Stencil decal: TexSlot7 stencil texture, or for $TintPaletteDecal the palette's decal texture; colours are
 *  palettes.json decal red/green/blue x StencilDiffuseColor{,2,3}; coverage x StencilOpacity. */
bool Stencil(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const FSCPalette* Palette)
{
    if (!HasToken(Sub, TEXT("STENCIL_MAP")) && !HasVirtualRole(Sub, TEXT("tint_palette_decal"))) return false;
    const FObj PaletteJson = Palette ? Parse(Palette->Json) : nullptr;
    const FObj Decal = Child(PaletteJson, TEXT("decal"));
    FString Map;
    if (const FObj Slot7 = Slot(Sub, TEXT("stencil"))) Map = Str(Slot7, TEXT("export_path"));
    if (Map.IsEmpty() && HasVirtualRole(Sub, TEXT("tint_palette_decal"))) Map = Str(Decal, TEXT("export_path"));
    if (Map.IsEmpty()) return false; // TODO(WP-R5): palettes.json decal.export_path is null in r1
    Tex(D, TEXT("StencilMap"), Map, ESCTextureRole::Linear);
    const TCHAR* Channels[] = {TEXT("red"), TEXT("green"), TEXT("blue")};
    const TCHAR* Params[] = {TEXT("DecalRed"), TEXT("DecalGreen"), TEXT("DecalBlue")};
    const TCHAR* Authored[] = {TEXT("StencilDiffuseColor"), TEXT("StencilDiffuseColor2"), TEXT("StencilDiffuseColor3")};
    const TCHAR* Tints[] = {TEXT("StencilTint1"), TEXT("StencilTint2"), TEXT("StencilTint3")};
    for (int32 I = 0; I < 3; ++I)
    {
        if (Decal && Decal->HasField(Channels[I])) D.Vectors.Add(Params[I], Rgb(Decal, Channels[I], FLinearColor::White));
        FLinearColor C; if (A.Params.Contains(Authored[I]) && ParseRgb(A.Params[Authored[I]], C)) D.Vectors.Add(Tints[I], C);
    }
    D.Scalars.Add(TEXT("StencilOpacity"), A.Param(TEXT("StencilOpacity"), 1.f));
    D.Scalars.Add(TEXT("UseStencil"), 1.f);
    return true;
}
bool IsWear(const FObj& Layer) { return Str(Layer, TEXT("name")).StartsWith(TEXT("Wear"), ESearchCase::IgnoreCase); }
/** Base layers of a HardSurface / LayerBlend_V2 manifest (wear layers need the wear factor: TODO(WP-U2)). */
int32 Layers(FSCMaterialDesc& D, const FObj& Sub, const FSCPalette* Palette, int32 Max)
{
    int32 Count = 0;
    for (const auto& V : Arr(Sub, TEXT("layer_manifest")))
    {
        const FObj L = V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!L || IsWear(L) || Count == Max) continue;
        const FString P = FString::Printf(TEXT("Layer%d"), Count++);
        const FObj Snap = Child(L, TEXT("layer_snapshot"));
        bool bPalette;
        const FLinearColor Channel = PaletteTint(Palette, Str(Child(L, TEXT("palette_channel")), TEXT("name")), bPalette);
        const FLinearColor Tint = Rgb(Snap, TEXT("diffuse"), FLinearColor::White) * Rgb(L, TEXT("tint_color"), FLinearColor::White) * Channel;
        const FString Rough = Str(L, TEXT("roughness_export_path"));
        double Shininess = 0;
        const TOptional<float> Shine = Snap && Snap->TryGetNumberField(TEXT("shininess"), Shininess) ? TOptional<float>(float(Shininess)) : TOptional<float>();
        Tex(D, *(P + TEXT("DiffuseMap")), Str(L, TEXT("diffuse_export_path")), ESCTextureRole::Color);
        Tex(D, *(P + TEXT("NormalGlossMap")), Str(L, TEXT("normal_export_path")), ESCTextureRole::Normal);
        Tex(D, *(P + TEXT("RoughnessMap")), Rough, ESCTextureRole::Linear);
        D.Vectors.Add(*(P + TEXT("Tint")), Tint);
        if (Snap && Snap->HasField(TEXT("specular"))) D.Vectors.Add(*(P + TEXT("Specular")), Rgb(Snap, TEXT("specular"), FLinearColor::Black));
        D.Scalars.Add(*(P + TEXT("Metallic")), float(Num(Snap, TEXT("metallic"), 0.0)));
        D.Scalars.Add(*(P + TEXT("Tiling")), float(Num(L, TEXT("uv_tiling"), 1.0)));
        Gloss(D, P, Shine, float(Num(L, TEXT("gloss_mult"), 1.0)), !Rough.IsEmpty());
    }
    // Unused blend slots repeat the last base layer so any mask channel resolves to authored data.
    for (int32 I = Count; I > 0 && I < Max; ++I)
    {
        const FString From = FString::Printf(TEXT("Layer%d"), Count - 1), To = FString::Printf(TEXT("Layer%d"), I);
        CopyPrefix(D.Scalars, From, To); CopyPrefix(D.Vectors, From, To); CopyPrefix(D.Textures, From, To);
    }
    return Count;
}
}

FSCMaterialDesc FSCMaterialBuilder::Describe(const FSCSubmaterial& In, const FSCPalette* Palette)
{
    FSCMaterialDesc D; D.Name = In.Name; D.Family = In.ShaderFamily;
    const FObj Sub = Parse(In.Json);
    const FAuthored A = Authored(Sub);
    D.Shader = A.Attr.FindRef(TEXT("Shader"));
    const FString& F = D.Family;
    if (F == TEXT("Illum")) D.Master = ESCMaster::Illum;
    else if (F == TEXT("HardSurface")) D.Master = ESCMaster::HardSurface;
    else if (F == TEXT("LayerBlend_V2") || F == TEXT("Layer")) D.Master = ESCMaster::Layered;
    else if (F == TEXT("GlassPBR")) D.Master = ESCMaster::Glass;
    else if (F == TEXT("MeshDecal")) D.Master = ESCMaster::MeshDecal;
    else if (F == TEXT("DisplayScreen") || F == TEXT("UIPlane") || F == TEXT("Monitor") || F == TEXT("UIMesh")
        || F == TEXT("Hologram") || F == TEXT("HologramCIG") || F == TEXT("Shield_Holo")) D.Master = ESCMaster::EmissiveUnlit;
    else if (F != TEXT("NoDraw"))
        D.Warnings.Add(FString::Printf(TEXT("Unknown shader family '%s' (shader '%s'); using M_SC_Fallback"), *F, *D.Shader));
    D.bHidden = F == TEXT("NoDraw") || Str(Child(Sub, TEXT("activation_state")), TEXT("state")) == TEXT("inactive");

    // mtl.rs alpha_config: NoDraw/decal -> blend; AlphaTest -> mask; Opacity < 1 -> blend. CryEngine's
    // MTL_FLAG_REQUIRE_FORWARD_RENDERING (0x8000) marks glow cards over the hull: their diffuse alpha is masked
    // coverage (they stay opaque-pass Nanite surfaces, so their emission feeds Lumen).
    // TODO(WP-R5): carry the forward/coverage decision in the sidecar's alpha_config.
    const bool bDecal = F == TEXT("MeshDecal") || HasToken(Sub, TEXT("DECAL"));
    const float Opacity = A.Float(TEXT("Opacity"), 1.f);
    const float AlphaTest = A.Float(TEXT("AlphaTest"), 0.f);
    const bool bForward = (FCString::Strtoui64(*A.Attr.FindRef(TEXT("MtlFlags")), nullptr, 10) & 0x8000) != 0;
    if (bDecal || In.bBlended || (Opacity < 1.f && AlphaTest <= 0.f)) D.Blend = ESCBlend::Translucent;
    else if (AlphaTest > 0.f || bForward) D.Blend = ESCBlend::Masked;
    // Cutoff: authored AlphaTest; a forward card without one uses UE's default opacity-mask clip value.
    const float Cutoff = AlphaTest > 0.f ? AlphaTest : GetDefault<UMaterial>()->OpacityMaskClipValue;
    D.bTwoSided = bDecal; // mtl.rs is_double_sided
    if (D.Master == ESCMaster::Glass || D.Master == ESCMaster::EmissiveUnlit) D.Blend = ESCBlend::Translucent;
    const bool bOpaqueFamily = D.Master == ESCMaster::Illum || D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered || D.Master == ESCMaster::Fallback;
    // Decal surfaces (MeshDecal, %DECAL sheets) modify the host through the DBuffer; other blended surfaces of
    // the opaque families (Opacity < 1, alpha_config blend) are lit translucent surfaces.
    if (bDecal) D.Master = ESCMaster::MeshDecal;
    else if (D.Blend == ESCBlend::Translucent && bOpaqueFamily) D.Master = ESCMaster::Translucent;

    if (D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered)
    {
        const int32 Count = Layers(D, Sub, Palette, D.Master == ESCMaster::HardSurface ? 1 : 4);
        if (Count == 0)
        {
            D.Warnings.Add(TEXT("Layered family without base layers; using M_SC_Fallback with authored colours"));
            D.Master = ESCMaster::Fallback; D.Textures.Reset(); D.Scalars.Reset(); D.Vectors.Reset();
            Direct(D, Sub, A, Palette);
        }
        else
        {
            const FObj Mask = Slot(Sub, TEXT("blend_mask"));
            if (D.Master == ESCMaster::Layered && Mask) Tex(D, TEXT("BlendMaskMap"), Str(Mask, TEXT("export_path")), ESCTextureRole::Linear);
            D.Scalars.Add(TEXT("UseBlendMask"), D.Textures.Contains(TEXT("BlendMaskMap")) ? 1.f : 0.f);
            D.Scalars.Add(TEXT("BlendMaskTiling"), A.Param(TEXT("BlendMapTiling"), 1.f));
            Emission(D, Sub, A);
        }
    }
    else if (D.Master == ESCMaster::Glass)
    {
        SurfaceGloss(D, Sub, A);
        D.Vectors.Add(TEXT("SpecularColor"), A.Color(TEXT("Specular"), FLinearColor(0.04f, 0.04f, 0.04f)));
        bool bPalette;
        const FLinearColor Glass = PaletteTint(Palette, Str(Child(Child(Sub, TEXT("palette_routing")), TEXT("material_channel")), TEXT("name")), bPalette);
        D.Vectors.Add(TEXT("GlassTint"), bPalette ? Glass : A.Color(TEXT("Diffuse"), FLinearColor::White));
        D.Scalars.Add(TEXT("GlassTintStrength"), A.Param(TEXT("GlassTintStrength"), 1.f));
        D.Scalars.Add(TEXT("ThinFilmThickness"), HasToken(Sub, TEXT("THIN_FILM")) ? A.Param(TEXT("ThinFilmThickness"), 0.f) : 0.f);
        D.Scalars.Add(TEXT("Opacity"), Opacity);
    }
    else
    {
        Direct(D, Sub, A, Palette);
        if (D.Master == ESCMaster::MeshDecal || D.Master == ESCMaster::Translucent)
        {
            if (const FObj Height = Slot(Sub, TEXT("height"))) Tex(D, TEXT("HeightMap"), Str(Height, TEXT("export_path")), ESCTextureRole::Linear);
            const bool bPom = HasToken(Sub, TEXT("PARALLAX_OCCLUSION_MAPPING")) && D.Textures.Contains(TEXT("HeightMap"));
            D.Scalars.Add(TEXT("PomDisplacement"), bPom ? A.Param(TEXT("PomDisplacement"), 0.f) : 0.f);
            // Blender add-on (non-FPS packages): POM decals take the host paint channel as Host Tint.
            D.bHostTinted = HasToken(Sub, TEXT("PARALLAX_OCCLUSION_MAPPING"));
            D.Vectors.Add(TEXT("HostTint"), FLinearColor::White);
            D.Scalars.Add(TEXT("Opacity"), Opacity * A.Param(TEXT("DecalAlphaMultiplier"), 1.f));
            // Contract (Blender importer note): POM without decal/stencil/tint-mask signals is control-only:
            // TexSlot1 is coverage, TexSlot3 normal-gloss, TexSlot4 height. With no colour data at all the decal
            // must not change the host's base colour: normal + roughness only.
            const bool bColourSignals = HasToken(Sub, TEXT("DECAL")) || HasToken(Sub, TEXT("STENCIL_MAP"))
                || Slot(Sub, TEXT("stencil")) || HasVirtualRole(Sub, TEXT("tint_palette_decal"));
            const bool bControlOnlyPom = HasToken(Sub, TEXT("PARALLAX_OCCLUSION_MAPPING")) && !bColourSignals;
            const bool bStencil = Stencil(D, Sub, A, Palette);
            if (bStencil && D.bHidden && Str(Child(Sub, TEXT("activation_state")), TEXT("reason")) == TEXT("missing_base_color_texture"))
                D.bHidden = false; // the palette supplies the colour source the exporter could not see
            if (D.Master == ESCMaster::MeshDecal && !bStencil && (bControlOnlyPom || !D.Textures.Contains(TEXT("DiffuseMap"))))
                D.Master = ESCMaster::MeshDecalRelief;
            if (D.Master == ESCMaster::MeshDecal) // colour decals: sheet alpha x DecalAlphaMultiplier x DecalDiffuseOpacity
                D.Scalars.Add(TEXT("Opacity"), Opacity * A.Param(TEXT("DecalAlphaMultiplier"), 1.f) * A.Param(TEXT("DecalDiffuseOpacity"), 1.f));
        }
        if (D.Master == ESCMaster::EmissiveUnlit && !D.Textures.Contains(TEXT("DiffuseMap")))
        {
            // Screens draw render-to-texture content bound per instance (ui_bindings, WP later); dark until then.
            D.Scalars.Add(TEXT("EmissiveStrength"), 0.f);
            if (!HasVirtualRole(Sub, TEXT("render_to_texture"))) D.Warnings.Add(TEXT("Emissive family without an image; drawn dark"));
        }
        // Screens with an image emit by the same authored rule (Emissive x Glow); no Glow, no emission.
    }
    if (D.Blend == ESCBlend::Masked) D.Scalars.Add(TEXT("AlphaTest"), Cutoff);
    if (D.bHidden)
    {
        D.Master = ESCMaster::EmissiveUnlit; D.Blend = ESCBlend::Translucent;
        D.Scalars.Add(TEXT("EmissiveStrength"), 0.f);
    }
    D.bNonNanite = D.Blend == ESCBlend::Translucent || D.Master == ESCMaster::Glass || D.Master == ESCMaster::MeshDecal
        || D.Master == ESCMaster::MeshDecalRelief || D.Master == ESCMaster::Translucent || D.Master == ESCMaster::EmissiveUnlit;
    return D;
}

bool FSCMaterialBuilder::RequiresNonNanite(const FSCSubmaterial& Sub) { return Describe(Sub, nullptr).bNonNanite; }

FString FSCMaterialBuilder::HostChannel(const FSCSubmaterial& Sub)
{
    if (RequiresNonNanite(Sub)) return FString();
    const FString Channel = Str(Child(Child(Parse(Sub.Json), TEXT("palette_routing")), TEXT("material_channel")), TEXT("name"));
    return Channel == TEXT("primary") || Channel == TEXT("secondary") || Channel == TEXT("tertiary") ? Channel : FString();
}

bool FSCMaterialBuilder::PaletteColor(const FSCPalette* Palette, const FString& Channel, FLinearColor& Out)
{
    if (!Palette || Channel.IsEmpty()) return false;
    const FObj P = Parse(Palette->Json);
    if (!P || !P->HasTypedField<EJson::Array>(*Channel)) return false;
    Out = Rgb(P, *Channel, FLinearColor::White); return true;
}

void FSCMaterialBuilder::ApplyEngineGlow(FSCMaterialDesc& D, float Strength)
{
    D.Vectors.Add(TEXT("EmissiveColor"), FLinearColor::White);
    D.Scalars.Add(TEXT("EmissiveStrength"), Strength);
}
