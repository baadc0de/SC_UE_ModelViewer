// FSCMaterialBuilder::Describe: pure sidecar -> master/parameter mapping (decomposed-export-contract
// §Material Sidecars, mirroring starbreaker-3d mtl.rs ShaderFamily / AlphaConfig / roughness rules).
#include "SCMaterialBuilder.h"
#include "SCStarEngine.h"
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
/** Exported texture slot by number: for slots whose meaning is fixed by the shader family (macro maps), where the
 *  lowest slot with a role is a different texture (LayerBlend_V2 TexSlot2 = POM decal normal, TexSlot3 = macro normal). */
FObj SlotAt(const FObj& Sub, int32 Number)
{
    const FString Name = FString::Printf(TEXT("TexSlot%d"), Number);
    for (const auto& V : Arr(Sub, TEXT("texture_slots")))
        if (V->Type == EJson::Object && Str(V->AsObject(), TEXT("slot")) == Name && !Str(V->AsObject(), TEXT("export_path")).IsEmpty()) return V->AsObject();
    return nullptr;
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
/** palettes.json `finish.<channel>`: the palette entry's specular tint (linear) and gloss factor (0..1), the
 *  other half of the game's SPackedTintEntry { diffuse, specular, gloss }. A field the export does not carry
 *  (null) stays unset. */
struct FPaletteFinish { TOptional<FLinearColor> Specular; TOptional<float> Gloss; };
FPaletteFinish PaletteFinish(const FSCPalette* Palette, const FString& Channel)
{
    FPaletteFinish F;
    const FObj Entry = Palette && !Channel.IsEmpty() ? Child(Child(Parse(Palette->Json), TEXT("finish")), *Channel) : nullptr;
    if (Entry && Entry->HasTypedField<EJson::Array>(TEXT("specular"))) F.Specular = Rgb(Entry, TEXT("specular"), FLinearColor::Black);
    double Gloss = 0;
    if (Entry && Entry->TryGetNumberField(TEXT("glossiness"), Gloss)) F.Gloss = FMath::Clamp(float(Gloss), 0.f, 1.f);
    return F;
}
/** A layer .mtl's public params (manifest `resolved_material.authored_public_params`: name / value pairs). */
TMap<FString, FString> LayerParams(const FObj& L)
{
    TMap<FString, FString> P;
    for (const auto& V : Arr(Child(L, TEXT("resolved_material")), TEXT("authored_public_params"))) if (V->Type == EJson::Object)
        P.Add(Str(V->AsObject(), TEXT("name")), Str(V->AsObject(), TEXT("value")));
    return P;
}
void Tex(FSCMaterialDesc& D, const TCHAR* Param, const FString& Path, ESCTextureRole Role)
{
    if (!Path.IsEmpty()) D.Textures.Add(Param, FSCTextureKey{Path, Role});
}
/** Substrate slab default roughness 0.5 (MaterialExpressionSubstrate.h) as the masters' smoothness: r = 1 - s
 *  (game ShadeLib GlossToRoughness: GGX alpha = (1 - gloss)^2 = UE Roughness^2). */
constexpr float SubstrateDefaultSmoothness = 0.5f;
/** Smoothness constants: authored Shininess/255 scales the map (or is the constant); a missing value is the
 *  multiplicative identity for the map and the Substrate default roughness for the constant. */
void Gloss(FSCMaterialDesc& D, const FString& Prefix, const TOptional<float>& Shininess, float Mult, bool bMap)
{
    const float Authored = Shininess.IsSet() ? FMath::Clamp(*Shininess / 255.f, 0.f, 1.f) : 1.f;
    D.Scalars.Add(*(Prefix + TEXT("UseRoughnessMap")), bMap ? 1.f : 0.f);
    D.Scalars.Add(*(Prefix + TEXT("GlossMult")), Authored * Mult);
    // TODO(WP-R5): upstream should emit a scalar gloss/roughness when Shininess is absent.
    // Mult (layer GlossMult x palette gloss) scales the constant either way: paletteLib `Smoothness *= layerGloss * gloss`.
    D.Scalars.Add(*(Prefix + TEXT("Smoothness")), (Shininess.IsSet() ? Authored : SubstrateDefaultSmoothness) * Mult);
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
/** CryEngine MTL_FLAG_REQUIRE_FORWARD_RENDERING. TODO(WP-R5): carry it in the sidecar's alpha_config. */
bool IsForward(const FAuthored& A) { return (FCString::Strtoui64(*A.Attr.FindRef(TEXT("MtlFlags")), nullptr, 10) & 0x8000) != 0; }
float AuthoredGlow(const FAuthored& A) { return FMath::Max(0.f, A.Float(TEXT("Glow"), A.Float(TEXT("GlowAmount"), 0.f))); }
/** Emission parameters (D11 final rule, amended; docs/03-unreal-export-design.md): the masters emit
 *  surface colour (Diffuse x base texture, or the emittance map) x EmissiveColor (tint) x EmissiveStrength,
 *  gated by alpha above AlphaGlowCutoff. Tint is applied once. EmissiveStrength is in cd/m2 and follows the
 *  game shader (FSCStarEngine; ue/verification/WP-U3b.md §13): an authored Glow emits MaterialGlowToLinearGlow(Glow);
 *  a material that emits without one (forward glow card, emittance map) is driven by the instance glow term of
 *  CalculateEmissive, taken as one scene unit (fallback register, WP-U2.md).
 *  v2 sidecars carry the rule in `emissive` {strength, color, uses_base_texture, emittance_texture,
 *  alpha_glow_cutoff, forward_rendering}; v1 sidecars are evaluated from the authored attributes. */
void Emission(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A)
{
    const FObj Em = D.bV2 ? Child(Sub, TEXT("emissive")) : nullptr;
    float Strength = 0.f, Glow = AuthoredGlow(A); FLinearColor Tint = FLinearColor::White; FString Map; double Cutoff = 0;
    bool bEmittance = HasToken(Sub, TEXT("EMITTANCE_MAP"));
    if (Em)
    {
        Strength = float(Num(Em, TEXT("strength"), 0.0));
        Glow = float(FMath::Max(0., Num(Em, TEXT("glow"), Glow)));
        Tint = Rgb(Em, TEXT("color"), FLinearColor::White);
        Map = Str(Em, TEXT("emittance_texture"));
        bEmittance |= !Map.IsEmpty();
        Cutoff = Em->TryGetNumberField(TEXT("alpha_glow_cutoff"), Cutoff) ? Cutoff : A.Param(TEXT("AlphaGlowCutoff"), 0.f);
        bool bBase = true;
        if (Em->TryGetBoolField(TEXT("uses_base_texture"), bBase) && !bBase && Strength > 0.f)
            D.Warnings.Add(TEXT("emissive.uses_base_texture = false is not supported by the masters; base texture still applied")); // TODO(WP-U2)
    }
    else
    {
        // v1: strength = Glow if > 0; else a forward-rendered Illum-family glow card with a non-black Emissive
        // emits 1 (Emissive is its tint, applied once); else 1 with an emittance map; else 0. LayerBlend and
        // non-forward materials never emit from Emissive alone. Black Emissive = no tint.
        const FLinearColor E = A.Color(TEXT("Emissive"), FLinearColor::Black);
        const bool bTint = E.R > 0.f || E.G > 0.f || E.B > 0.f;
        const bool bGlowCard = (D.Family == TEXT("Illum") || D.Family == TEXT("HardSurface")) && D.bForward && bTint;
        Strength = Glow > 0.f ? Glow : (bGlowCard || bEmittance) ? 1.f : 0.f;
        Tint = bTint ? E : FLinearColor::White;
        Cutoff = A.Param(TEXT("AlphaGlowCutoff"), 0.f);
    }
    D.Vectors.Add(TEXT("EmissiveColor"), Tint);
    const FSCStarEngine& Game = FSCStarEngine::Get();
    D.Scalars.Add(TEXT("EmissiveStrength"), float(Strength <= 0.f ? 0. : Glow > 0.f ? Game.GlowNits(Glow) : Game.InstanceGlowNits(Strength)));
    D.Scalars.Add(TEXT("AlphaGlowCutoff"), float(Cutoff));
    if (bEmittance)
    {
        // TextureSemanticRole has no emittance role (mtl.rs): the base-colour slot unless v2 names the map.
        if (Map.IsEmpty()) if (const FObj Base = Slot(Sub, TEXT("base_color"))) Map = Str(Base, TEXT("export_path"));
        if (!Map.IsEmpty()) { Tex(D, TEXT("EmissiveMap"), Map, ESCTextureRole::Color); D.Scalars.Add(TEXT("UseEmissiveMap"), 1.f); }
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
    D.Scalars.Add(TEXT("AlphaTest"), A.Float(TEXT("AlphaTest"), 0.f));
    SurfaceGloss(D, Sub, A);
    Emission(D, Sub, A);
}
/** UE HairShadingCommon.ush GetHairColorFromMelanin (absorption model, [0..1] melanin), on the CPU. */
FLinearColor HairColorFromMelanin(float Melanin01, float Redness, const FLinearColor& Dye)
{
    constexpr float B = 0.3f;
    const float D = 5.969f - 0.215f * B + 2.532f * B * B - 10.73f * B * B * B + 5.574f * B * B * B * B + 0.245f * B * B * B * B * B;
    const float Melanin = -FMath::Loge(FMath::Max(1.f - FMath::Clamp(Melanin01, 0.f, 1.f), 0.0001f));
    const float Eu = Melanin * (1.f - FMath::Clamp(Redness, 0.f, 1.f)), Pheo = Melanin * FMath::Clamp(Redness, 0.f, 1.f);
    const float EuA[3] = {0.506f, 0.841f, 1.653f}, PheoA[3] = {0.343f, 0.733f, 1.924f}, DyeC[3] = {Dye.R, Dye.G, Dye.B};
    float Out[3];
    for (int32 I = 0; I < 3; ++I)
    {
        const float DyeAbs = FMath::Square(FMath::Loge(FMath::Clamp(DyeC[I], 1e-4f, 1.f)) / D);
        Out[I] = FMath::Exp(-FMath::Sqrt(Eu * EuA[I] + Pheo * PheoA[I] + DyeAbs) * D);
    }
    return FLinearColor(Out[0], Out[1], Out[2]);
}
/** HairPBR: card coverage from the `opacity` slot (red channel) and, when the material authors BaseMelanin,
 *  colour = melanin model (BaseMelanin, BaseMelaninRedness, DyeColor x DyeAmount) x BaseTintColor; otherwise
 *  the authored Diffuse (hair caps: black). */
void Hair(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A)
{
    if (const FObj Opacity = Slot(Sub, TEXT("opacity"))) Tex(D, TEXT("OpacityMap"), Str(Opacity, TEXT("export_path")), ESCTextureRole::Linear);
    if (!A.Params.Contains(TEXT("BaseMelanin"))) return;
    FLinearColor Dye = FLinearColor::White, Tint = FLinearColor::White;
    if (A.Params.Contains(TEXT("DyeColor"))) ParseRgb(A.Params[TEXT("DyeColor")], Dye);
    if (A.Params.Contains(TEXT("BaseTintColor"))) ParseRgb(A.Params[TEXT("BaseTintColor")], Tint);
    const float DyeAmount = FMath::Clamp(A.Param(TEXT("DyeAmount"), 0.f), 0.f, 1.f);
    D.Vectors.Add(TEXT("DiffuseColor"), HairColorFromMelanin(A.Param(TEXT("BaseMelanin"), 0.f), A.Param(TEXT("BaseMelaninRedness"), 0.f),
        FMath::Lerp(FLinearColor::White, Dye, DyeAmount)) * Tint);
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
/** Material-level maps applied over the blended layers (game HardSurface.cfx / LayerBlend_V2.cfx):
 *  - macro normal + gloss: TexSlot3 ddna, compiled in by %MACRO_NORMAL_MAP (HardSurface, at baseTC x MacroTiling)
 *    or %NORMAL_MAP (LayerBlend, untiled); LayerAttribs_AddMacroSurface = detail-normal blend, smoothness x gloss;
 *  - HardSurface %MACRO_AO_MAP: TexSlot1, single channel, same UVs (exported with the base_color role);
 *  - LayerBlend TexSlot13: R hue shift, G AO, B luminance shift, sampled unconditionally at baseTC. */
void MacroSurface(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A)
{
    const bool bHard = D.Master == ESCMaster::HardSurface;
    FObj Normal = HasToken(Sub, bHard ? TEXT("MACRO_NORMAL_MAP") : TEXT("NORMAL_MAP")) ? SlotAt(Sub, 3) : nullptr;
    if (Normal && Str(Normal, TEXT("role")) != TEXT("normal_gloss")) Normal = nullptr;
    const FString Rough = DerivedRoughness(Sub, Normal);
    if (Normal) Tex(D, TEXT("MacroNormalMap"), Str(Normal, TEXT("export_path")), ESCTextureRole::Normal);
    Tex(D, TEXT("MacroRoughnessMap"), Rough, ESCTextureRole::Linear);
    D.Scalars.Add(TEXT("UseMacroGloss"), Rough.IsEmpty() ? 0.f : 1.f);
    D.Scalars.Add(TEXT("MacroTiling"), bHard ? A.Param(TEXT("MacroTiling"), 1.f) : 1.f);
    if (bHard)
    {
        if (HasToken(Sub, TEXT("MACRO_AO_MAP"))) if (const FObj AO = SlotAt(Sub, 1)) Tex(D, TEXT("MacroAOMap"), Str(AO, TEXT("export_path")), ESCTextureRole::Linear);
        if (HasToken(Sub, TEXT("USE_UV2_FOR_MACRO_MAPS")) && (D.Textures.Contains(TEXT("MacroNormalMap")) || D.Textures.Contains(TEXT("MacroAOMap"))))
            D.Warnings.Add(TEXT("%USE_UV2_FOR_MACRO_MAPS is not supported by the masters; macro maps use the first UV set"));
    }
    else
    {
        const FObj Hal = SlotAt(Sub, 13);
        if (Hal && Str(Hal, TEXT("role")) == TEXT("hal_control")) Tex(D, TEXT("HalMap"), Str(Hal, TEXT("export_path")), ESCTextureRole::Linear);
        D.Scalars.Add(TEXT("UseHalMap"), D.Textures.Contains(TEXT("HalMap")) ? 1.f : 0.f);
        if (A.Param(TEXT("RotateLayerUVs"), 0.f) != 0.f)
            D.Warnings.Add(TEXT("RotateLayerUVs is not supported by the masters; layer UVs are not rotated"));
    }
}
/** Game layer tint modes (Layer.cfx `TintMode` UIEnum), authored on the layer .mtl; the shader default is 0. */
enum class ESCTintMode { DiffuseAndSpecular = 0, DiffuseOnly = 1, SpecularOnly = 2 };
/** One manifest layer as `<P>*` parameters: maps, tint, F0, tiling, gloss.
 *  Tints follow the game's paletteLib.cfi LayerAttribs_ApplyTintsAndPalette (HardSurface Primary / Wear and every
 *  LayerBlend_V2 layer). Without a palette entry (no palette, or the layer names no channel):
 *    albedo x layer diffuse, F0 = layer specular, gloss x layer gloss.
 *  With one (ApplyPaletteParams):
 *    albedo x layer diffuse, and x the entry's diffuse tint when the layer tints diffuse;
 *    F0 = the entry's specular tint when the layer tints specular (it takes the place of the layer specular),
 *         else the layer specular;
 *    gloss x layer gloss x the entry's gloss, always.
 *  Which of the two a layer tints is its TintMode. The engine packs that into the tint index bits the shader
 *  tests (0x4 diffuse, 0x8 specular); the packing code is not in the shader cache, so the mapping from the enum
 *  (0 both, 1 diffuse only, 2 specular only) is taken from the enum's names.
 *  A palette that lacks the entry's specular / gloss keeps the layer's own value, with a warning.
 *
 *  The layer's spec map (its TexSlot6) scales F0. A shader that samples it (HardSurface %USE_SPECULAR_MAPS:
 *  LayerAttribs_ReadSpecular, declared per layer as `UseLayerSpecMap`) gets it as `<P>SpecularMap`. One that does
 *  not (HardSurface without the token; LayerBlend_V2, which has no layer spec sampler at all) gets the map's mean
 *  colour times F0 instead (FSCMaterialDesc::VectorMeans). That fold is inferred, not read from the shader cache:
 *  layers authored for a spec map carry Specular = 1,1,1 and keep their real F0 in the map, the same layer .mtl is
 *  used under both kinds of shader, and the engine tells its layer packing which kind it is (UseLayerSpecMap);
 *  taking Specular as authored would turn every such layer into a mirror. See ue/verification/WP-U2.md. */
void Layer(FSCMaterialDesc& D, const FObj& L, const FString& P, const FSCPalette* Palette, bool bSamplesSpecMap)
{
    const FObj Snap = Child(L, TEXT("layer_snapshot"));
    const FString ChannelName = Str(Child(L, TEXT("palette_channel")), TEXT("name"));
    bool bPalette;
    const FLinearColor Channel = PaletteTint(Palette, ChannelName, bPalette);
    const ESCTintMode Mode = ESCTintMode(FCString::Atoi(*LayerParams(L).FindRef(TEXT("TintMode"))));
    const bool bTintDiffuse = bPalette && Mode != ESCTintMode::SpecularOnly, bTintSpecular = bPalette && Mode != ESCTintMode::DiffuseOnly;
    const FPaletteFinish Finish = bPalette ? PaletteFinish(Palette, ChannelName) : FPaletteFinish();
    if (bTintSpecular && !Finish.Specular.IsSet())
        D.Warnings.Add(FString::Printf(TEXT("palette %s has no specular tint: layer %s keeps its own specular"), *ChannelName, *Str(L, TEXT("name"))));
    if (bPalette && !Finish.Gloss.IsSet())
        D.Warnings.Add(FString::Printf(TEXT("palette %s has no gloss: layer %s gloss is not scaled"), *ChannelName, *Str(L, TEXT("name"))));
    FLinearColor Tint = Rgb(Snap, TEXT("diffuse"), FLinearColor::White) * Rgb(L, TEXT("tint_color"), FLinearColor::White);
    if (bTintDiffuse) Tint *= Channel;
    const FString Rough = Str(L, TEXT("roughness_export_path"));
    double Shininess = 0;
    const TOptional<float> Shine = Snap && Snap->TryGetNumberField(TEXT("shininess"), Shininess) ? TOptional<float>(float(Shininess)) : TOptional<float>();
    Tex(D, *(P + TEXT("DiffuseMap")), Str(L, TEXT("diffuse_export_path")), ESCTextureRole::Color);
    Tex(D, *(P + TEXT("NormalGlossMap")), Str(L, TEXT("normal_export_path")), ESCTextureRole::Normal);
    Tex(D, *(P + TEXT("RoughnessMap")), Rough, ESCTextureRole::Linear);
    D.Vectors.Add(*(P + TEXT("Tint")), Tint);
    if (bTintSpecular && Finish.Specular.IsSet()) D.Vectors.Add(*(P + TEXT("Specular")), *Finish.Specular);
    else if (Snap && Snap->HasField(TEXT("specular"))) D.Vectors.Add(*(P + TEXT("Specular")), Rgb(Snap, TEXT("specular"), FLinearColor::Black));
    D.Scalars.Add(*(P + TEXT("Tiling")), float(Num(L, TEXT("uv_tiling"), 1.0)));
    Gloss(D, P, Shine, float(Num(L, TEXT("gloss_mult"), 1.0)) * Finish.Gloss.Get(1.f), !Rough.IsEmpty());
    if (const FObj Spec = SlotAt(L, 6))
    {
        const FSCTextureKey Key{Str(Spec, TEXT("export_path")), ESCTextureRole::Color};
        if (bSamplesSpecMap) D.Textures.Add(*(P + TEXT("SpecularMap")), Key);
        else if (D.Vectors.Contains(*(P + TEXT("Specular")))) D.VectorMeans.Add(*(P + TEXT("Specular")), Key);
        else D.Warnings.Add(FString::Printf(TEXT("layer %s has a spec map but no specular colour: the map is not used"), *Str(L, TEXT("name"))));
    }
}
/** Base layers of a HardSurface / LayerBlend_V2 manifest. Wear layers are not base layers: LayerBlend wear is a
 *  runtime per-instance amount (zero on a pristine item); HardSurface static wear is described by StaticWear. */
int32 Layers(FSCMaterialDesc& D, const FObj& Sub, const FSCPalette* Palette, int32 Max, bool bSamplesSpecMaps)
{
    int32 Count = 0;
    for (const auto& V : Arr(Sub, TEXT("layer_manifest")))
    {
        const FObj L = V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!L || IsWear(L) || Count == Max) continue;
        Layer(D, L, FString::Printf(TEXT("Layer%d"), Count++), Palette, bSamplesSpecMaps);
    }
    // Unused blend slots repeat the last base layer so any mask channel resolves to authored data.
    for (int32 I = Count; I > 0 && I < Max; ++I)
    {
        const FString From = FString::Printf(TEXT("Layer%d"), Count - 1), To = FString::Printf(TEXT("Layer%d"), I);
        CopyPrefix(D.Scalars, From, To); CopyPrefix(D.Vectors, From, To); CopyPrefix(D.Textures, From, To);
        CopyPrefix(D.VectorMeans, From, To);
    }
    return Count;
}
/** Game shading models of a layer .mtl (layerLibConstants.cfi SHADING_*), authored as the `ShadingModel` param. */
enum class ESCLayerShading { Standard = 0, ClearCoat = 1, Paint = 2, ClearCoatPaint = 3, Cloth = 4, RetroReflective = 5 };
/** HardSurface vertex data and static wear (game HardSurface.cfx GetLayerAttribs, CalcAlphas_BlendBased,
 *  BlendLayers_BranchGradWear; the blend itself is in M_SC_HardSurface, see create_materials.py):
 *  - %VERTCOLORS: vertex colour G = glow mask, B = AO, A = wear (UseVertexColors);
 *  - wear amount = WearBlendBase - vertex wear, shaped by the Primary layer's blend map (its TexSlot8, sampled
 *    with the Primary tiling) and WearDirtBlendFalloff; the Primary layer's ShadingModel selects the paint /
 *    cloth remap and the paint primer;
 *  - %WEAR_LAYER: the worn surface is the manifest's Wear layer (BlendLayers_SeparatePrimaryAndWear); otherwise
 *    it is the Primary layer's textures with the layer's wear tints instead of its own
 *    (BlendLayers_TintedPrimaryWear + LayerAttribs_ApplyWearTints): albedo x wear diffuse, F0 = wear specular,
 *    gloss x wear gloss. The engine packs those three per layer; taking them as the layer .mtl's
 *    WearDiffuseColor / WearSpecularColor / WearGlossiness times the submaterial's per-layer WearTint / WearGloss
 *    is inferred from the names (the packing code is not in the shader cache);
 *  - %USE_SPECULAR_MAPS: each layer's TexSlot6 multiplies its F0 (LayerAttribs_ReadSpecular). Without the token
 *    the read is not compiled in and the map's mean colour scales F0 instead (see Layer). The tinted wear
 *    surface is the Primary layer's textures, so it takes the Primary spec map either way.
 *  A missing input leaves static wear off with a warning rather than guessing a shader default. */
void StaticWear(FSCMaterialDesc& D, const FObj& Sub, const FAuthored& A, const FSCPalette* Palette)
{
    FObj Primary, Wear;
    for (const auto& V : Arr(Sub, TEXT("layer_manifest")))
    {
        const FObj L = V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (L && IsWear(L)) { if (!Wear) Wear = L; }
        else if (L && !Primary) Primary = L;
    }
    const TMap<FString, FString> P = LayerParams(Primary);
    D.Scalars.Add(TEXT("UseVertexColors"), HasToken(Sub, TEXT("VERTCOLORS")) ? 1.f : 0.f);

    const float Base = A.Param(TEXT("WearBlendBase"), 0.f);
    const FObj Blend = SlotAt(Primary, 8);
    const bool bSeparate = HasToken(Sub, TEXT("WEAR_LAYER"));
    FLinearColor WearDiffuse = FLinearColor::White, WearSpecular = FLinearColor::White;
    FString Missing;
    if (!Blend) Missing = TEXT("the Primary layer has no blend map (TexSlot8)");
    else if (!P.Contains(TEXT("WearDirtBlendFalloff"))) Missing = TEXT("the Primary layer has no WearDirtBlendFalloff");
    else if (bSeparate && !Wear) Missing = TEXT("%WEAR_LAYER without a Wear layer in the manifest");
    else if (!bSeparate && !(P.Contains(TEXT("WearGlossiness")) && ParseRgb(P.FindRef(TEXT("WearDiffuseColor")), WearDiffuse)
        && ParseRgb(P.FindRef(TEXT("WearSpecularColor")), WearSpecular))) Missing = TEXT("the Primary layer has no wear tints");
    const bool bWear = Base > 0.f && Missing.IsEmpty();
    if (Base > 0.f && !bWear) D.Warnings.Add(FString::Printf(TEXT("WearBlendBase %.3f is not drawn: %s"), Base, *Missing));
    D.Scalars.Add(TEXT("WearBlendBase"), bWear ? Base : 0.f);
    if (!bWear) return;

    Tex(D, TEXT("WearBlendMap"), Str(Blend, TEXT("export_path")), ESCTextureRole::Linear);
    D.Scalars.Add(TEXT("WearFalloff"), FCString::Atof(*P[TEXT("WearDirtBlendFalloff")]));
    const ESCLayerShading Shading = ESCLayerShading(FCString::Atoi(*P.FindRef(TEXT("ShadingModel"))));
    const bool bPaint = Shading == ESCLayerShading::Paint || Shading == ESCLayerShading::ClearCoatPaint;
    const bool bCloth = Shading == ESCLayerShading::Cloth;
    D.Scalars.Add(TEXT("WearPaint"), bPaint ? 1.f : 0.f);
    D.Scalars.Add(TEXT("WearCloth"), bCloth ? 1.f : 0.f);
    if (bPaint || bCloth) D.Warnings.Add(TEXT("worn paint / cloth complement hue (HueLUT) is not supported by the masters; the hue is not shifted"));
    if (Shading == ESCLayerShading::RetroReflective) D.Warnings.Add(TEXT("retroreflective layer shading is not supported by the masters"));
    if (bSeparate)
    {
        Layer(D, Wear, TEXT("Wear"), Palette, HasToken(Sub, TEXT("USE_SPECULAR_MAPS")));
    }
    else
    {
        const float WearGloss = FMath::Clamp(FCString::Atof(*P[TEXT("WearGlossiness")]) * float(Num(Primary, TEXT("wear_gloss"), 1.0)), 0.f, 1.f);
        CopyPrefix(D.Textures, TEXT("Layer0"), TEXT("Wear")); // the Primary diffuse, normal, gloss and spec maps
        D.Vectors.Add(TEXT("WearTint"), WearDiffuse * Rgb(Primary, TEXT("wear_tint"), FLinearColor::White));
        D.Vectors.Add(TEXT("WearSpecular"), WearSpecular);
        if (const FSCTextureKey* Mean = D.VectorMeans.Find(TEXT("Layer0Specular"))) D.VectorMeans.Add(TEXT("WearSpecular"), FSCTextureKey(*Mean));
        D.Scalars.Add(TEXT("WearTiling"), D.Scalars.FindRef(TEXT("Layer0Tiling")));
        D.Scalars.Add(TEXT("WearUseRoughnessMap"), D.Scalars.FindRef(TEXT("Layer0UseRoughnessMap")));
        D.Scalars.Add(TEXT("WearGlossMult"), WearGloss);
        D.Scalars.Add(TEXT("WearSmoothness"), WearGloss);
    }
}
}

FSCMaterialDesc FSCMaterialBuilder::Describe(const FSCSubmaterial& In, const FSCPalette* Palette, bool bUseV2Objects)
{
    FSCMaterialDesc D; D.Name = In.Name; D.Family = In.ShaderFamily;
    const FObj Sub = Parse(In.Json);
    D.bV2 = bUseV2Objects && Sub && Child(Sub, TEXT("emissive")) && Child(Sub, TEXT("alpha_config"));
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
    // Character families (WP-P2 round 2): plain lit mapping of their textures through the Illum master, no
    // subsurface / DNA / wrinkle work. HumanSkin_V2 and Eye: base colour, DDNA normal, spec map, authored F0.
    // HairPBR: card coverage from the `opacity` slot (masked), colour from the authored melanin parameters.
    else if (F == TEXT("HumanSkin_V2") || F == TEXT("Eye") || F == TEXT("HairPBR")) D.Master = ESCMaster::Illum;
    else if (F != TEXT("NoDraw"))
        D.Warnings.Add(FString::Printf(TEXT("Unknown shader family '%s' (shader '%s'); using M_SC_Fallback"), *F, *D.Shader));
    D.bHidden = F == TEXT("NoDraw") || Str(Child(Sub, TEXT("activation_state")), TEXT("state")) == TEXT("inactive");

    // mtl.rs alpha_config: NoDraw/decal -> blend; AlphaTest -> mask; Opacity < 1 -> blend. CryEngine's
    // MTL_FLAG_REQUIRE_FORWARD_RENDERING (0x8000) marks glow cards over the hull: their diffuse alpha is masked
    // coverage (they stay opaque-pass Nanite surfaces, so their emission feeds Lumen).
    // TODO(WP-R5): carry the forward/coverage decision in the sidecar's alpha_config.
    const bool bDecal = F == TEXT("MeshDecal") || HasToken(Sub, TEXT("DECAL"));
    // v2 `alpha_config` {alpha_test, opacity, two_sided, forward_rendering} when present; v1: authored attributes.
    const FObj AC = D.bV2 ? Child(Sub, TEXT("alpha_config")) : nullptr;
    const float Opacity = AC ? float(Num(AC, TEXT("opacity"), 1.0)) : A.Float(TEXT("Opacity"), 1.f);
    const float AlphaTest = AC ? float(Num(AC, TEXT("alpha_test"), 0.0)) : A.Float(TEXT("AlphaTest"), 0.f);
    bool bForward = IsForward(A);
    if (AC) AC->TryGetBoolField(TEXT("forward_rendering"), bForward);
    D.bForward = bForward;
    const bool bHairCards = F == TEXT("HairPBR") && Slot(Sub, TEXT("opacity"));
    if (bDecal || In.bBlended || (Opacity < 1.f && AlphaTest <= 0.f)) D.Blend = ESCBlend::Translucent;
    else if (AlphaTest > 0.f || bForward || bHairCards) D.Blend = ESCBlend::Masked;
    // Cutoff: authored AlphaTest; a forward card without one uses UE's default opacity-mask clip value.
    const float Cutoff = AlphaTest > 0.f ? AlphaTest : GetDefault<UMaterial>()->OpacityMaskClipValue;
    D.bTwoSided = bDecal; // v1: mtl.rs is_double_sided
    if (AC) AC->TryGetBoolField(TEXT("two_sided"), D.bTwoSided); // v2: authored MtlFlags two-sided
    if (D.Master == ESCMaster::Glass || D.Master == ESCMaster::EmissiveUnlit) D.Blend = ESCBlend::Translucent;
    const bool bOpaqueFamily = D.Master == ESCMaster::Illum || D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered || D.Master == ESCMaster::Fallback;
    // Decal surfaces (MeshDecal, %DECAL sheets) modify the host through the DBuffer; other blended surfaces of
    // the opaque families (Opacity < 1, alpha_config blend) are lit translucent surfaces.
    // A forward-flag Illum with %DECAL is a glow-only overlay (CryEngine: drawn over the host, its diffuse scaled
    // by DecalDiffuseOpacity): the family stays Illum and it takes the glow-card path below, never the DBuffer
    // decal master, and decal opacity parameters never touch its emission or coverage.
    const bool bForwardIllum = F == TEXT("Illum") && bForward;
    if (bDecal && !bForwardIllum) D.Master = ESCMaster::MeshDecal;
    else if (D.Blend == ESCBlend::Translucent && bOpaqueFamily && !bForwardIllum) D.Master = ESCMaster::Translucent;

    if (D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered)
    {
        // HardSurface.cfx samples the layer spec maps under %USE_SPECULAR_MAPS; LayerBlend_V2.cfx never does.
        const bool bHard = D.Master == ESCMaster::HardSurface;
        const int32 Count = Layers(D, Sub, Palette, bHard ? 1 : 4, bHard && HasToken(Sub, TEXT("USE_SPECULAR_MAPS")));
        if (Count == 0)
        {
            D.Warnings.Add(TEXT("Layered family without base layers; using M_SC_Fallback with authored colours"));
            D.Master = ESCMaster::Fallback; D.Textures.Reset(); D.Scalars.Reset(); D.Vectors.Reset(); D.VectorMeans.Reset();
            Direct(D, Sub, A, Palette);
        }
        else
        {
            const FObj Mask = Slot(Sub, TEXT("blend_mask"));
            if (D.Master == ESCMaster::Layered) // M_SC_HardSurface has no blend mask: one base layer + static wear
            {
                if (Mask) Tex(D, TEXT("BlendMaskMap"), Str(Mask, TEXT("export_path")), ESCTextureRole::Linear);
                D.Scalars.Add(TEXT("UseBlendMask"), D.Textures.Contains(TEXT("BlendMaskMap")) ? 1.f : 0.f);
                D.Scalars.Add(TEXT("BlendMaskTiling"), A.Param(TEXT("BlendMapTiling"), 1.f));
            }
            else StaticWear(D, Sub, A, Palette);
            MacroSurface(D, Sub, A);
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
        if (F == TEXT("HairPBR")) Hair(D, Sub, A);
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
    // Emitting forward-flag Illum surfaces are glow cards: additive unlit emission weighted by texture alpha
    // (soft gradients survive, no clip), split into the non-Nanite mesh like glass. Non-emitting forward
    // materials (glass trims, runtime-driven engine glow) keep the masked opaque path.
    // A %DECAL overlay card is a card even at strength 0 (runtime-driven engine glow); its non-emissive colour
    // (Diffuse x DecalDiffuseOpacity) is not drawn by the additive master. TODO(WP-U2): DecalDiffuseOpacity > 0.
    if (bForwardIllum && !D.bHidden && (D.Scalars.FindRef(TEXT("EmissiveStrength")) > 0.f || bDecal))
    {
        D.Master = ESCMaster::EmissiveUnlit; D.Blend = ESCBlend::Translucent; D.bGlowCard = true;
        D.Scalars.Add(TEXT("AlphaWeight"), 1.f);
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

bool FSCMaterialBuilder::RequiresNonNanite(const FSCSubmaterial& Sub, bool bEngineGlowTarget)
{
    FSCMaterialDesc D = Describe(Sub, nullptr);
    if (bEngineGlowTarget) ApplyEngineGlow(D, 0.f);
    return D.bNonNanite;
}

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

float FSCMaterialBuilder::EngineGlowToEmissive(const FSCEngineGlow& C, float Value)
{
    if (C.MaxStrength > C.MinStrength) Value = FMath::Clamp(Value, C.MinStrength, C.MaxStrength);
    return C.Units == TEXT("material_property_glow") ? float(FSCStarEngine::Get().InstanceGlowNits(Value)) : Value;
}

void FSCMaterialBuilder::ApplyEngineGlow(FSCMaterialDesc& D, float Strength)
{
    // EmissiveColor already follows the rule (Emissive when non-black, else no tint).
    D.Scalars.Add(TEXT("EmissiveStrength"), Strength);
    if (D.bForward && D.Family == TEXT("Illum") && !D.bHidden && !D.bGlowCard)
    {
        D.Master = ESCMaster::EmissiveUnlit; D.Blend = ESCBlend::Translucent; D.bGlowCard = true; D.bNonNanite = true;
        D.Scalars.Add(TEXT("AlphaWeight"), 1.f);
    }
}
