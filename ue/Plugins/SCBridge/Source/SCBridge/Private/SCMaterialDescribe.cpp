// FSCMaterialBuilder::Describe: pure sidecar -> master/parameter mapping (decomposed-export-contract
// §Material Sidecars, mirroring starbreaker-3d mtl.rs ShaderFamily / AlphaConfig / roughness rules).
#include "SCMaterialBuilder.h"
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
/** Families whose authored Shininess=255 is a placeholder for per-pixel DDNA smoothness (mtl.rs). */
bool PerPixelSmoothness(const FString& Family)
{
    return Family == TEXT("HardSurface") || Family.StartsWith(TEXT("LayerBlend")) || Family == TEXT("Illum") || Family == TEXT("GlassPBR");
}
/** Normal + derived roughness + smoothness constants for a direct (non-layer) material. */
void SurfaceGloss(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const TCHAR* NormalRole = TEXT("normal_gloss"))
{
    const FObj Normal = Slot(Sub, NormalRole);
    if (Normal) Tex(D, TEXT("NormalGlossMap"), Str(Normal, TEXT("export_path")), ESCTextureRole::Normal);
    const FString Rough = DerivedRoughness(Sub, Normal);
    const float Gloss = FMath::Clamp(A.Float(TEXT("Shininess"), 255.f) / 255.f, 0.f, 1.f);
    Tex(D, TEXT("RoughnessMap"), Rough, ESCTextureRole::Linear);
    D.Scalars.Add(TEXT("UseRoughnessMap"), Rough.IsEmpty() ? 0.f : 1.f);
    D.Scalars.Add(TEXT("GlossMult"), Gloss);
    // mtl.rs: roughness 0 from a placeholder Shininess=255 without per-pixel data reads as a mirror;
    // it substitutes roughness 0.5. Masters compute roughness = sqrt(1 - smoothness), so smoothness 0.75.
    D.Scalars.Add(TEXT("Smoothness"), Rough.IsEmpty() && Gloss >= 1.f && PerPixelSmoothness(D.Family) ? 0.75f : Gloss);
}
float AuthoredGlow(const FAuthored& A) { return FMath::Max(0.f, A.Float(TEXT("Glow"), A.Float(TEXT("GlowAmount"), 0.f))); }
/** Emission = Emissive x Glow x (Diffuse x base texture | emittance map), gated by alpha above AlphaGlowCutoff
 *  (the masters multiply by the surface colour). Glow absent or 0 emits nothing even with Emissive = 1,1,1;
 *  this deliberately does not use mtl.rs emissive_factor()'s Glow=1 fallback. */
void Emission(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A)
{
    const float Glow = AuthoredGlow(A);
    // Glow without an Emissive colour (Emissive=0,0,0, e.g. orange "glows" Illum) is CryEngine's diffuse glow:
    // the surface colour itself emits, i.e. Emissive acts as white.
    const FLinearColor E = A.Color(TEXT("Emissive"), FLinearColor::Black);
    D.Vectors.Add(TEXT("EmissiveColor"), Glow > 0.f && E.R <= 0.f && E.G <= 0.f && E.B <= 0.f ? FLinearColor::White : E);
    D.Scalars.Add(TEXT("EmissiveStrength"), Glow * FSCMaterialBuilder::GlowToEmissive);
    D.Scalars.Add(TEXT("AlphaGlowCutoff"), A.Param(TEXT("AlphaGlowCutoff"), 0.f));
    if (HasToken(Sub, TEXT("EMITTANCE_MAP")))
    {
        FObj Map = Slot(Sub, TEXT("emittance")); if (!Map) Map = Slot(Sub, TEXT("emissive"));
        if (Map) { Tex(D, TEXT("EmissiveMap"), Str(Map, TEXT("export_path")), ESCTextureRole::Color); D.Scalars.Add(TEXT("UseEmissiveMap"), 1.f); }
        else D.Warnings.Add(TEXT("%EMITTANCE_MAP without an exported emittance slot; emission uses the base colour"));
    }
}
void Direct(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const FSCPalette* Palette)
{
    if (const FObj Base = Slot(Sub, TEXT("base_color"))) Tex(D, TEXT("DiffuseMap"), Str(Base, TEXT("export_path")), ESCTextureRole::Color);
    if (const FObj Spec = Slot(Sub, TEXT("specular_support"))) Tex(D, TEXT("SpecularMap"), Str(Spec, TEXT("export_path")), ESCTextureRole::Color);
    FLinearColor Diffuse = A.Color(TEXT("Diffuse"), FLinearColor::White);
    bool bPalette;
    const FLinearColor Tint = PaletteTint(Palette, Str(Child(Child(Sub, TEXT("palette_routing")), TEXT("material_channel")), TEXT("name")), bPalette);
    if (bPalette) Diffuse *= Tint;
    D.Vectors.Add(TEXT("DiffuseColor"), Diffuse);
    // CryEngine spec workflow: F0 is the authored specular colour. Diffuse = Specular = (1,1,1) is the
    // authored sentinel whose response lives in textures (mtl.rs layer_metallic): without a spec map it
    // is a dielectric, so the master's dielectric default F0 applies.
    const bool bSentinel = A.Color(TEXT("Diffuse"), FLinearColor::Black).Equals(FLinearColor::White)
        && A.Color(TEXT("Specular"), FLinearColor::Black).Equals(FLinearColor::White);
    if (!(bSentinel && !D.Textures.Contains(TEXT("SpecularMap"))))
        D.Vectors.Add(TEXT("SpecularColor"), A.Color(TEXT("Specular"), FLinearColor(0.04f, 0.04f, 0.04f)));
    D.Scalars.Add(TEXT("Metallic"), 0.f);
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
        const float Gloss = float(Num(L, TEXT("gloss_mult"), 1.0) * FMath::Clamp(Num(Snap, TEXT("shininess"), 255.0) / 255.0, 0.0, 1.0));
        const FString Rough = Str(L, TEXT("roughness_export_path"));
        Tex(D, *(P + TEXT("DiffuseMap")), Str(L, TEXT("diffuse_export_path")), ESCTextureRole::Color);
        Tex(D, *(P + TEXT("NormalGlossMap")), Str(L, TEXT("normal_export_path")), ESCTextureRole::Normal);
        Tex(D, *(P + TEXT("RoughnessMap")), Rough, ESCTextureRole::Linear);
        D.Vectors.Add(*(P + TEXT("Tint")), Tint);
        D.Vectors.Add(*(P + TEXT("Specular")), Rgb(Snap, TEXT("specular"), FLinearColor(0.04f, 0.04f, 0.04f)));
        D.Scalars.Add(*(P + TEXT("Metallic")), float(Num(Snap, TEXT("metallic"), 0.0)));
        D.Scalars.Add(*(P + TEXT("Tiling")), float(Num(L, TEXT("uv_tiling"), 1.0)));
        D.Scalars.Add(*(P + TEXT("GlossMult")), Gloss);
        D.Scalars.Add(*(P + TEXT("Smoothness")), Gloss);
        D.Scalars.Add(*(P + TEXT("UseRoughnessMap")), Rough.IsEmpty() ? 0.f : 1.f);
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
    // MTL_FLAG_REQUIRE_FORWARD_RENDERING (0x8000) marks surfaces drawn in the forward/transparent pass (glow
    // cards over the hull): their diffuse alpha is coverage. TODO(WP-U2): carry this in the sidecar's alpha_config.
    const bool bDecal = F == TEXT("MeshDecal") || HasToken(Sub, TEXT("DECAL"));
    const float Opacity = A.Float(TEXT("Opacity"), 1.f);
    const bool bForward = (FCString::Strtoui64(*A.Attr.FindRef(TEXT("MtlFlags")), nullptr, 10) & 0x8000) != 0;
    if (bDecal || In.bBlended || bForward || (Opacity < 1.f && A.Float(TEXT("AlphaTest"), 0.f) <= 0.f)) D.Blend = ESCBlend::Translucent;
    else if (A.Float(TEXT("AlphaTest"), 0.f) > 0.f) D.Blend = ESCBlend::Masked;
    D.bTwoSided = bDecal; // mtl.rs is_double_sided
    if (D.Master == ESCMaster::Glass || D.Master == ESCMaster::EmissiveUnlit) D.Blend = ESCBlend::Translucent;
    const bool bOpaqueFamily = D.Master == ESCMaster::Illum || D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered || D.Master == ESCMaster::Fallback;
    // Decal surfaces (MeshDecal, %DECAL sheets) modify the host through the DBuffer; other blended surfaces of
    // the opaque families (forward glow cards, Opacity < 1) are lit translucent surfaces.
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
            D.Scalars.Add(TEXT("AlphaTest"), A.Float(TEXT("AlphaTest"), 0.f));
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
            if (D.Master == ESCMaster::MeshDecal && (bControlOnlyPom || !D.Textures.Contains(TEXT("DiffuseMap"))))
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
        else if (D.Master == ESCMaster::EmissiveUnlit)
        {
            // Screens show their image: authored Emissive tint (white when unset) x Glow, unit brightness without Glow.
            const FLinearColor E = A.Color(TEXT("Emissive"), FLinearColor::Black);
            if (E.R <= 0 && E.G <= 0 && E.B <= 0) D.Vectors.Add(TEXT("EmissiveColor"), FLinearColor::White);
            D.Scalars.Add(TEXT("EmissiveStrength"), AuthoredGlow(A) > 0 ? AuthoredGlow(A) * FSCMaterialBuilder::GlowToEmissive : 1.f);
        }
    }
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
