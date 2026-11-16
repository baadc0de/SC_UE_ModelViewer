// SCFxBuild: the game's particle effects (CryEngine ParticleLibrary, read through the FFI as sc_particle_effect JSON)
// rebuilt as Niagara systems of lightweight (stateless) emitters, saved under the gitignored /Game/SC/FX (WP-K4c).
//
//   UnrealEditor-Cmd.exe SCViewer.uproject -run=SCFxBuild [-Effects=<ref>;<ref>] [-MaterialEffects=<lib:effect>;...]
//
// Without arguments the lists come from Config/FX/effects.json. An effect `<GUID>|<library>.<effect>` becomes
// /Game/SC/FX/<library>/<effect> (dots -> underscores); a material effect (FXLib entry) builds the particle effects it
// plays. Each enabled game emitter with something to draw becomes one lightweight emitter duplicated from the engine's
// FountainLightweight template (sprite) and, when it carries a LightSource, a second one with a light renderer (a
// light shares no colour with its sprite). Every rule below is per parameter; nothing is keyed on an effect or asset.
//
// Units and axes. Game values are metres / seconds; UE centimetres (x100). Game vectors map like the importer's
// (FSCMeshImporter::Position, LightBasis): game (x, y, z) -> UE (y, x, z). The emitter's forward, CryEngine's +Y (the
// focus / emit axis), is therefore the system's +X. Spline-guided emitters are laid along a 100 cm line from the
// origin along +X: the runtime orients the component along the beam and scales X to the beam length (the stateless
// shape and spawn rate take no per-instance length; see SC_FX_SYSTEM lines).
//
// Mapping (JSON field -> Niagara):
//  spawn: burst -> Burst spawn info (count x parent count for Spawn=FromParentBirth children; PulsePeriod -> loop);
//         continuous -> Rate = Count / min(lifetime, EmitterLifeTime) (CryEngine's Count is the number alive at once);
//         per_distance -> Rate = min(EmitPerDistance x 1 m, Count) / lifetime (along the 1 m beam); SpawnDelay -> delay.
//  lifetime (+Random) -> InitializeParticle Lifetime range [v(1-r), v].
//  size (+Random, Aspect) -> Sprite Size: CryEngine's Size is a radius -> 2 x 100 x size; age_curve -> ScaleSpriteSize.
//  color x EmitterStrength curve at strength 1 x (EmissiveLighting + DiffuseLighting x LitSceneUnits) -> particle colour
//         in scene units; the material instance's EmissiveScale is the game's scene unit (FSCStarEngine::SceneUnitNits,
//         cd/m2), so a sprite's luminance is the game's. Color Random -> one-random colour range (brightness, not hue);
//         FakeLight lights are skipped (they feed the flare only); age curves (colour, alpha) ->
//         ScaleColor.
//  alpha (+Random, strength curve at 1) -> colour alpha (the additive master weights by it, the translucent covers).
//  speed (+Random) + EmitAngle / FocusAngle -> AddVelocity cone about +X (Niagara's cone angle is the full angle).
//  gravity_scale -> GravityForce (0, 0, -981 x scale) cm/s2, world space; Drag / AirResistance -> Drag.
//  position_offset_m -> ShapeLocation offset; random_offset_m (EmissionSize) -> box of 2 x the offset. Spline-guided:
//         a box 0..100 cm along +X, as wide as 2 x the spline noise amplitude; their offsets (the spline start) dropped.
//  facing Camera -> face camera (OrientToVelocity: velocity aligned); Free -> custom facing along +X.
//  tiles / anim -> renderer SubImageSize + SubUVAnimation (lifetime rate: linear over life; fps: loops; AnimRate 0:
//         a random fixed frame in the first-frame interval).
//  texture -> imported once to /Game/SC/FX/Textures/<archive path>; material instance per (master, texture) under
//         /Game/SC/FX/Materials (blend Additive -> M_SC_Particle_Additive, else M_SC_Particle_Translucent).
//  light -> LightAttributes radius (+ age curve) and a light renderer; colour = particle colour x pi x SceneUnitNits x
//         Intensity cd (FSCStarEngine::PointCandela's far field, no bulb) x 100^2 (UE's internal cd -> cm units).
//  RandomAngles y / RandomRotationRate y (raw) -> sprite rotation / rotation rate ranges.
// EmitterStrength curves are baked at strength 1 (the stable beam); a User float SC.Strength is exposed, unbound.
// Emitters invisible at strength 1 are added disabled. Every raw attribute not mapped is listed in `skipped`.
#include "SCFxBuildCommandlet.h"
#include "SCBridgeClient.h" // scbridge.h
#include "SCStarEngine.h"
#include "SCTextureImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Stateless/Modules/NiagaraStatelessModule_AddVelocity.h"
#include "Stateless/Modules/NiagaraStatelessModule_Drag.h"
#include "Stateless/Modules/NiagaraStatelessModule_GravityForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_InitializeParticle.h"
#include "Stateless/Modules/NiagaraStatelessModule_LightAttributes.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleColor.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSize.h"
#include "Stateless/Modules/NiagaraStatelessModule_ShapeLocation.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteFacingAndAlignment.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteRotationRate.h"
#include "Stateless/Modules/NiagaraStatelessModule_SubUVAnimation.h"
#include "Stateless/NiagaraStatelessEmitter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCFx, Log, All);

namespace
{
using FJson = TSharedPtr<FJsonObject>;
const TCHAR* const TemplatePath = TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight");
const TCHAR* const AdditiveMaster = TEXT("/SCBridge/Materials/M_SC_Particle_Additive.M_SC_Particle_Additive");
const TCHAR* const TranslucentMaster = TEXT("/SCBridge/Materials/M_SC_Particle_Translucent.M_SC_Particle_Translucent");
const TCHAR* const Root = TEXT("/Game/SC/FX");
constexpr double Cm = 100.0;
constexpr double BeamLengthM = 1.0; // spline-guided emitters span 0..100 cm along +X (header)
constexpr double GravityMs2 = 9.81;
constexpr double CandelaToUE = 100.0 * 100.0; // UPointLightComponent::ComputeLightBrightness, inverse-squared Candelas

// ---- JSON --------------------------------------------------------------------------------------------------------
const FJsonValue* Field(const FJson& O, const FString& Path)
{
    if (!O) return nullptr;
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("."));
    const FJsonObject* Cur = O.Get();
    const FJsonValue* V = nullptr;
    for (int32 i = 0; i < Parts.Num(); ++i)
    {
        const TSharedPtr<FJsonValue> F = Cur ? Cur->TryGetField(Parts[i]) : nullptr;
        if (!F.IsValid() || F->IsNull()) return nullptr;
        V = F.Get(); // owned by the object, which outlives the call
        if (i + 1 < Parts.Num()) Cur = V->Type == EJson::Object ? V->AsObject().Get() : nullptr;
    }
    return V;
}
bool Has(const FJson& O, const FString& Path) { return Field(O, Path) != nullptr; }
double Num(const FJson& O, const FString& Path, double Default = 0)
{
    const FJsonValue* V = Field(O, Path);
    return V && V->Type == EJson::Number ? V->AsNumber() : Default;
}
bool Bool(const FJson& O, const FString& Path)
{
    const FJsonValue* V = Field(O, Path);
    return V && V->Type == EJson::Boolean && V->AsBool();
}
FString Str(const FJson& O, const FString& Path)
{
    const FJsonValue* V = Field(O, Path);
    return V && V->Type == EJson::String ? V->AsString() : FString();
}
FJson Obj(const FJson& O, const FString& Path)
{
    const FJsonValue* V = Field(O, Path);
    return V && V->Type == EJson::Object ? V->AsObject() : nullptr;
}
TArray<double> Nums(const FJson& O, const FString& Path)
{
    TArray<double> Out;
    if (const FJsonValue* V = Field(O, Path); V && V->Type == EJson::Array)
        for (const auto& E : V->AsArray()) Out.Add(E.IsValid() && E->Type == EJson::Number ? E->AsNumber() : 0.0);
    return Out;
}
/** A curve: [[t, v...], ...] sorted by t; empty when absent. */
TArray<TArray<double>> Curve(const FJson& O, const FString& Path)
{
    TArray<TArray<double>> Keys;
    if (const FJsonValue* V = Field(O, Path); V && V->Type == EJson::Array)
        for (const auto& K : V->AsArray())
        {
            TArray<double> Key;
            if (K.IsValid() && K->Type == EJson::Array)
                for (const auto& E : K->AsArray()) Key.Add(E.IsValid() && E->Type == EJson::Number ? E->AsNumber() : 0.0);
            if (Key.Num() >= 2) Keys.Add(MoveTemp(Key));
        }
    Keys.StableSort([](const TArray<double>& A, const TArray<double>& B) { return A[0] < B[0]; });
    return Keys;
}
/** Piecewise linear, clamped to the end keys (CryEngine splines hold their ends). Channel 0 is the first value. */
double Eval(const TArray<TArray<double>>& Keys, double T, int32 Channel = 0)
{
    if (Keys.IsEmpty()) return 1.0;
    auto At = [Channel](const TArray<double>& K) { return K.IsValidIndex(Channel + 1) ? K[Channel + 1] : K.Last(); };
    if (T <= Keys[0][0]) return At(Keys[0]);
    for (int32 i = 1; i < Keys.Num(); ++i)
        if (T <= Keys[i][0])
        {
            const double Span = Keys[i][0] - Keys[i - 1][0];
            const double A = Span > 0 ? (T - Keys[i - 1][0]) / Span : 1.0;
            return FMath::Lerp(At(Keys[i - 1]), At(Keys[i]), A);
        }
    return At(Keys.Last());
}
FJson Parse(const FString& Text)
{
    FJson O;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
    return O;
}
FString Join(const TArray<FString>& A) { return TEXT("[") + FString::Join(A, TEXT(",")) + TEXT("]"); }

/** game (x, y, z) -> UE (y, x, z), metres -> cm (FSCMeshImporter::Position / LightBasis). */
FVector3f Basis(const TArray<double>& V)
{
    auto C = [&V](int32 i) { return V.IsValidIndex(i) ? float(V[i]) : 0.f; };
    return FVector3f(C(1), C(0), C(2)) * float(Cm);
}
/** First number after `key=` in a raw attribute (e.g. RandomAngles "y=360"). */
TOptional<double> RawComponent(const FJson& E, const FString& Attr, const FString& Key)
{
    const FString Raw = Str(E, TEXT("raw.") + Attr);
    const int32 At = Raw.Find(Key + TEXT("="));
    if (At == INDEX_NONE) return {};
    return FCString::Atod(*Raw.Mid(At + Key.Len() + 1));
}
FString SafeSegment(FString S)
{
    for (TCHAR& C : S)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return S.IsEmpty() ? FString(TEXT("_")) : S;
}
FString SafePath(const FString& Path)
{
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("/"));
    for (FString& P : Parts) P = SafeSegment(P);
    return FString::Join(Parts, TEXT("/"));
}

// ---- distributions -----------------------------------------------------------------------------------------------
FRichCurve RichCurve(const TArray<TArray<double>>& Keys, int32 Channel, double Scale = 1.0)
{
    FRichCurve C;
    if (Keys.IsEmpty() || Keys[0][0] > 0) C.SetKeyInterpMode(C.AddKey(0.f, float(Eval(Keys, 0, Channel) * Scale)), RCIM_Linear);
    for (const auto& K : Keys) C.SetKeyInterpMode(C.AddKey(float(K[0]), float(Eval(Keys, K[0], Channel) * Scale)), RCIM_Linear);
    if (Keys.IsEmpty() || Keys.Last()[0] < 1) C.SetKeyInterpMode(C.AddKey(1.f, float(Eval(Keys, 1, Channel) * Scale)), RCIM_Linear);
    return C;
}
void SetRange(FNiagaraDistributionRangeFloat& D, double Min, double Max)
{
    if (FMath::IsNearlyEqual(Min, Max)) D.InitConstant(float(Max));
    else D.InitRange(float(FMath::Min(Min, Max)), float(FMath::Max(Min, Max)));
    D.ChannelConstantsAndRanges = FMath::IsNearlyEqual(Min, Max) ? TArray<float>{float(Max)} : TArray<float>{float(Min), float(Max)};
}
void SetUniformRange(FNiagaraDistributionRangeVector2& D, double Min, double Max)
{
    D.Mode = FMath::IsNearlyEqual(Min, Max) ? ENiagaraDistributionMode::UniformConstant : ENiagaraDistributionMode::UniformRange;
    D.ChannelConstantsAndRanges = D.Mode == ENiagaraDistributionMode::UniformConstant ? TArray<float>{float(Max)} : TArray<float>{float(Min), float(Max)};
    D.UpdateValuesFromDistribution();
}
void SetVec2Range(FNiagaraDistributionRangeVector2& D, const FVector2f& Min, const FVector2f& Max)
{
    D.Mode = Min.Equals(Max) ? ENiagaraDistributionMode::NonUniformConstant : ENiagaraDistributionMode::NonUniformRange;
    D.ChannelConstantsAndRanges = {Min.X, Min.Y};
    if (D.Mode == ENiagaraDistributionMode::NonUniformRange) D.ChannelConstantsAndRanges.Append({Max.X, Max.Y});
    D.UpdateValuesFromDistribution();
}
void SetVec3(FNiagaraDistributionRangeVector3& D, const FVector3f& V)
{
    D.Mode = ENiagaraDistributionMode::NonUniformConstant;
    D.ChannelConstantsAndRanges = {V.X, V.Y, V.Z};
    D.UpdateValuesFromDistribution();
}
void SetPosition(FNiagaraDistributionPosition& D, const FVector3f& V)
{
    D.InitConstant(V);
    D.Mode = ENiagaraDistributionMode::NonUniformConstant;
    D.ChannelConstantsAndRanges = {V.X, V.Y, V.Z};
    D.UpdateValuesFromDistribution();
}
void SetColorRange(FNiagaraDistributionColor& D, const FLinearColor& Min, const FLinearColor& Max)
{
    if (Min.Equals(Max, 0.f))
    {
        D.InitConstant(Max);
        D.Mode = ENiagaraDistributionMode::NonUniformConstant;
        D.ChannelConstantsAndRanges = {Max.R, Max.G, Max.B, Max.A};
    }
    else
    {
        // One random for all channels (CryEngine's Color Random scales brightness, not hue): UniformRange between two
        // colours. The editor's UniformRange stores scalars, so the two colours are written as the built values.
        D.Mode = ENiagaraDistributionMode::UniformRange;
        D.ChannelConstantsAndRanges = {Min.R, Min.G, Min.B, Min.A, Max.R, Max.G, Max.B, Max.A};
        D.Values = {Min, Max};
        D.ValuesTimeRange = FVector2f(0.f, 1.f);
        return;
    }
    D.UpdateValuesFromDistribution();
}
/** RGBA curves over particle age (missing channel curves are 1). */
void SetColorCurve(FNiagaraDistributionColor& D, const TArray<TArray<double>>& Rgb, const TArray<TArray<double>>& A)
{
    D.Mode = ENiagaraDistributionMode::NonUniformCurve;
    D.ChannelCurves.Reset();
    for (int32 c = 0; c < 3; ++c) D.ChannelCurves.Add(RichCurve(Rgb, c));
    D.ChannelCurves.Add(RichCurve(A, 0));
    D.UpdateValuesFromDistribution();
}

// ---- assets --------------------------------------------------------------------------------------------------------
struct FBuilder
{
    sc_handle* H = nullptr;
    double SceneUnitNits = 0;
    double LitSceneUnits = 0; // config: luminance (scene units) given to DiffuseLighting x colour of an unlit sprite
    TMap<FString, UTexture2D*> Textures;
    TMap<FString, UMaterialInterface*> Materials;
    int32 Errors = 0, Systems = 0;

    static UPackage* PackageFor(const FString& LongName)
    {
        UPackage* Package = CreatePackage(*LongName);
        if (FPackageName::DoesPackageExist(LongName)) Package->FullyLoad();
        return Package;
    }
    bool Save(UObject* Asset)
    {
        UPackage* Package = Asset->GetPackage();
        Package->MarkPackageDirty();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.Error = GError;
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        if (!UPackage::SavePackage(Package, Asset, *File, Args))
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR saving %s"), *File);
            ++Errors;
            return false;
        }
        FAssetRegistryModule::AssetCreated(Asset);
        return true;
    }

    /** An archive .dds as a persistent texture: split mips merged by the bridge (sc_p4k_read_dds), the top mip decoded to
     *  an editable BGRA8 source (sc_texels_decode_bgra8, as the WP-C1 cook does), sRGB from the DXGI format. */
    UTexture2D* Texture(const FString& ArchivePath)
    {
        if (UTexture2D** Found = Textures.Find(ArchivePath)) return *Found;
        UTexture2D*& Slot = Textures.Add(ArchivePath, nullptr);
        sc_blob Blob{};
        if (sc_p4k_read_dds(H, TCHAR_TO_UTF8(*ArchivePath), 0, &Blob) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR texture %s: %s"), *ArchivePath, UTF8_TO_TCHAR(sc_last_error(H)));
            ++Errors;
            return nullptr;
        }
        TArray<uint8> Dds(Blob.data, int32(Blob.len));
        sc_free_blob(&Blob);
        sc_dds_info Info{};
        if (sc_dds_prepare(Dds.GetData(), size_t(Dds.Num()), false, &Info) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR texture %s: %s"), *ArchivePath, UTF8_TO_TCHAR(sc_last_error(nullptr)));
            ++Errors;
            return nullptr;
        }
        TArray64<uint8> Bgra;
        Bgra.SetNumUninitialized(int64(Info.width) * Info.height * 4);
        if (sc_texels_decode_bgra8(Dds.GetData() + Info.data_offset, size_t(Dds.Num() - Info.data_offset), Info.dxgi_format, Info.width,
                                   Info.height, Bgra.GetData(), size_t(Bgra.Num())) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR texture %s decode: %s"), *ArchivePath, UTF8_TO_TCHAR(sc_last_error(nullptr)));
            ++Errors;
            return nullptr;
        }
        bool bSRGB = true;
        FSCTextureImporter::PixelFormatOf(Info.dxgi_format, bSRGB);
        const FString Name = SafeSegment(FPaths::GetBaseFilename(ArchivePath));
        const FString LongName = FString(Root) / TEXT("Textures") / SafePath(FPaths::GetPath(ArchivePath)) / Name;
        UPackage* Package = PackageFor(LongName);
        UTexture2D* Tex = FindObject<UTexture2D>(Package, *Name);
        if (!Tex) Tex = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
        Tex->PreEditChange(nullptr);
        Tex->Source.Init(int32(Info.width), int32(Info.height), 1, 1, TSF_BGRA8, Bgra.GetData());
        Tex->SRGB = bSRGB;
        Tex->CompressionSettings = TC_Default;
        Tex->LODGroup = TEXTUREGROUP_Effects;
        Tex->MipGenSettings = TMGS_FromTextureGroup;
        Tex->PostEditChange();
        if (!Save(Tex)) return nullptr;
        UE_LOG(LogSCFx, Display, TEXT("SC_FX_TEXTURE %s -> %s %ux%u dxgi=%u srgb=%d"), *ArchivePath, *Tex->GetPathName(), Info.width, Info.height,
               Info.dxgi_format, bSRGB ? 1 : 0);
        return Slot = Tex;
    }

    /** Constant instance of a particle master for one texture; EmissiveScale = the game's scene unit in cd/m2. */
    UMaterialInterface* Material(bool bAdditive, const FString& ArchivePath)
    {
        const FString Key = (bAdditive ? TEXT("A|") : TEXT("T|")) + ArchivePath;
        if (UMaterialInterface** Found = Materials.Find(Key)) return *Found;
        UMaterialInterface*& Slot = Materials.Add(Key, nullptr);
        UTexture2D* Tex = Texture(ArchivePath);
        UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, bAdditive ? AdditiveMaster : TranslucentMaster);
        if (!Tex || !Master)
        {
            if (!Master) UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR missing master %s (run ue/scripts/create_materials.py)"), bAdditive ? AdditiveMaster : TranslucentMaster);
            ++Errors;
            return nullptr;
        }
        const FString Name = FString::Printf(TEXT("MI_SC_Particle_%s_%s"), bAdditive ? TEXT("Additive") : TEXT("Translucent"), *Tex->GetName());
        UPackage* Package = PackageFor(FString(Root) / TEXT("Materials") / Name);
        UMaterialInstanceConstant* MI = FindObject<UMaterialInstanceConstant>(Package, *Name);
        if (!MI) MI = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MI->PreEditChange(nullptr);
        MI->SetParentEditorOnly(Master);
        MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("SpriteTexture")), Tex);
        MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveScale")), float(SceneUnitNits));
        MI->PostEditChange();
        if (!Save(MI)) return nullptr;
        return Slot = MI;
    }

    // ---- emitters ----------------------------------------------------------------------------------------------------
    struct FLog
    {
        TArray<FString> Mapped, Skipped, Approx;
    };
    struct FSpawn
    {
        bool bBurst = false;
        double AmountMin = 0, AmountMax = 0, RateMin = 0, RateMax = 0, Delay = 0, Duration = 1;
        ENiagaraLoopBehavior Loop = ENiagaraLoopBehavior::Infinite;
    };

    /** Spawn rules (header). Parent is the game parent emitter (null for the first). */
    static FSpawn Spawn(const FJson& E, const FJson& Parent, FLog& L)
    {
        FSpawn S;
        const double Count = Num(E, TEXT("spawn.count")) * Eval(Curve(E, TEXT("spawn.count_strength_curve")), 1.0);
        const double CountRandom = FMath::Clamp(Num(E, TEXT("spawn.count_random")), 0.0, 1.0);
        const double Life = FMath::Max(1e-3, Num(E, TEXT("lifetime.value"), 1) * Eval(Curve(E, TEXT("lifetime.strength_curve")), 1.0));
        double EmitterLife = Num(E, TEXT("spawn.emitter_lifetime"));
        S.Delay = Num(E, TEXT("spawn.delay"));
        double ParentCount = 1;
        const FString OnParent = Str(E, TEXT("spawn.spawn_on_parent"));
        if (!OnParent.IsEmpty() && Parent)
        {
            ParentCount = FMath::Max(1.0, Num(Parent, TEXT("spawn.count")));
            if (EmitterLife <= 0) EmitterLife = Num(Parent, TEXT("lifetime.value"), 1);
            S.Delay += Num(Parent, TEXT("spawn.delay"));
            L.Approx.Add(FString::Printf(TEXT("Spawn=%s:one emitter for %g parent particles at the parent origin"), *OnParent, ParentCount));
            if (Num(E, TEXT("inherit_velocity")) != 0) L.Skipped.Add(TEXT("inherit_velocity"));
        }
        const FString Mode = Str(E, TEXT("spawn.mode"));
        if (Mode == TEXT("burst"))
        {
            S.bBurst = true;
            S.AmountMax = FMath::Max(1.0, FMath::RoundToDouble(Count * ParentCount));
            S.AmountMin = FMath::Max(0.0, FMath::RoundToDouble(Count * (1 - CountRandom) * ParentCount));
            const double Pulse = Num(E, TEXT("spawn.pulse_period"));
            S.Loop = Pulse > 0 ? ENiagaraLoopBehavior::Infinite : ENiagaraLoopBehavior::Once;
            S.Duration = Pulse > 0 ? Pulse : S.Delay + 0.01;
            L.Mapped.Add(Pulse > 0 ? TEXT("spawn.burst+pulse") : TEXT("spawn.burst"));
        }
        else
        {
            double Alive = Count * ParentCount;
            if (Mode == TEXT("per_distance"))
            {
                Alive = FMath::Min(Num(E, TEXT("spawn.per_distance_per_m")) * BeamLengthM * ParentCount, Alive > 0 ? Alive : DBL_MAX);
                L.Approx.Add(TEXT("per_distance:EmitPerDistance x 1 m beam alive at once"));
            }
            const double Window = EmitterLife > 0 ? FMath::Min(Life, EmitterLife) : Life;
            S.RateMax = Alive / Window;
            S.RateMin = S.RateMax * (1 - CountRandom);
            S.Loop = EmitterLife > 0 ? ENiagaraLoopBehavior::Once : ENiagaraLoopBehavior::Infinite;
            S.Duration = EmitterLife > 0 ? EmitterLife : Life;
            L.Mapped.Add(Mode == TEXT("per_distance") ? TEXT("spawn.per_distance") : TEXT("spawn.continuous"));
        }
        if (S.Delay > 0) L.Mapped.Add(TEXT("spawn.delay"));
        return S;
    }

    template <typename T> static T* Module(UNiagaraStatelessEmitter* E)
    {
        return const_cast<T*>(E->GetModule<T>());
    }

    /** Configure one lightweight emitter from a game emitter. bLight: the light-only twin. Returns false when it ends up
     *  with nothing to draw. */
    bool Configure(UNiagaraStatelessEmitter* Em, const FJson& E, const FJson& Parent, bool bLight, FLog& L, bool& bInvisibleAtFull)
    {
        // Every template module off first; each rule below enables what it uses.
        for (UNiagaraStatelessModule* M : Em->GetModules())
            if (M && M->CanDisableModule()) M->SetIsModuleEnabled(false);
        Em->Modify();

        const bool bSpline = Has(E, TEXT("spline"));
        const FSpawn S = Spawn(E, Parent, L);
        FNiagaraEmitterStateData State = Em->GetEmitterState();
        State.InactiveResponse = ENiagaraEmitterInactiveResponse::Complete;
        State.LoopBehavior = S.Loop;
        State.LoopDurationMode = ENiagaraLoopDurationMode::Fixed;
        State.LoopDuration.InitConstant(float(FMath::Max(S.Duration, 0.01)));
        State.LoopDuration.ChannelConstantsAndRanges = {float(FMath::Max(S.Duration, 0.01))};
        State.bLoopDelayEnabled = !S.bBurst && S.Delay > 0;
        State.LoopDelay.InitConstant(float(S.Delay));
        State.LoopDelay.ChannelConstantsAndRanges = {float(S.Delay)};
        State.bDelayFirstLoopOnly = true;
        State.bEnableDistanceCulling = false;
        State.bEnableVisibilityCulling = false;
        Em->SetEmitterState(State);
        while (Em->GetNumSpawnInfos() > 0)
        {
            FGuid Id = Em->GetSpawnInfoByIndex(0)->SourceId;
            Em->RemoveSpawnInfoBySourceId(Id);
        }
        FNiagaraStatelessSpawnInfo& Info = Em->AddSpawnInfo();
        Info.bEnabled = true;
        if (S.bBurst)
        {
            Info.Type = ENiagaraStatelessSpawnInfoType::Burst;
            Info.SpawnTime = float(S.Delay);
            Info.Amount.Mode = S.AmountMin == S.AmountMax ? ENiagaraDistributionMode::UniformConstant : ENiagaraDistributionMode::UniformRange;
            Info.Amount.Min = int32(S.AmountMin);
            Info.Amount.Max = int32(S.AmountMax);
        }
        else
        {
            Info.Type = ENiagaraStatelessSpawnInfoType::Rate;
            SetRange(Info.Rate, S.RateMin, S.RateMax);
        }

        // Lifetime.
        auto* Init = Module<UNiagaraStatelessModule_InitializeParticle>(Em);
        const double Life = Num(E, TEXT("lifetime.value"), 1) * Eval(Curve(E, TEXT("lifetime.strength_curve")), 1.0);
        const double LifeRandom = FMath::Clamp(Num(E, TEXT("lifetime.random")), 0.0, 1.0);
        SetRange(Init->LifetimeDistribution, Life * (1 - LifeRandom), Life);
        Init->LifetimeDistribution.UpdateValuesFromDistribution();
        L.Mapped.Add(TEXT("lifetime"));
        if (Has(E, TEXT("lifetime.strength_curve")) || Has(E, TEXT("spawn.count_strength_curve"))) L.Approx.Add(TEXT("strength_curves@1"));

        // Colour and alpha (strength curves at 1).
        const TArray<double> Rgb0 = Nums(E, TEXT("color.rgb"));
        const auto ColorStrength = Curve(E, TEXT("color.strength_curve"));
        FLinearColor Rgb(1, 1, 1, 1);
        for (int32 c = 0; c < 3; ++c) Rgb.Component(c) = float((Rgb0.IsValidIndex(c) ? Rgb0[c] : 1.0) * (ColorStrength.IsEmpty() ? 1.0 : Eval(ColorStrength, 1.0, c)));
        const double ColorRandom = FMath::Clamp(Num(E, TEXT("color.random")), 0.0, 1.0);
        if (!ColorStrength.IsEmpty()) L.Approx.AddUnique(TEXT("strength_curves@1"));
        if (ColorRandom > 0) L.Mapped.Add(TEXT("color.random"));
        if (Bool(E, TEXT("color.random_hue"))) L.Skipped.Add(TEXT("color.random_hue"));

        // Shape: offsets (or the beam line) as a box.
        auto* Shape = Module<UNiagaraStatelessModule_ShapeLocation>(Em);
        Shape->SetIsModuleEnabled(true);
        Shape->ShapePrimitive = ENSM_ShapePrimitive::Box;
        Shape->bBoxSurfaceOnly = false;
        Shape->CoordinateSpace = ENiagaraCoordinateSpace::Simulation;
        const FVector3f Offset = Basis(Nums(E, TEXT("position_offset_m")));
        FVector3f Box = Basis(Nums(E, TEXT("random_offset_m"))).GetAbs() * 2.f;
        FVector3f Centre = Offset;
        if (bSpline)
        {
            const TArray<double> Amp = Nums(E, TEXT("spline.noise_amplitude"));
            Box.X = float(BeamLengthM * Cm);
            Box.Y = FMath::Max(Box.Y, float(2 * Cm * (Amp.IsValidIndex(0) ? Amp[0] : 0)));
            Box.Z = FMath::Max(Box.Z, float(2 * Cm * (Amp.IsValidIndex(1) ? Amp[1] : 0)));
            // PositionOffset moves where a guided spline starts; the spline still ends on the target, so a straight box
            // cannot carry it: the beam emitters share the axis.
            if (!Offset.IsNearlyZero()) L.Approx.Add(TEXT("spline:position_offset dropped (spline start only)"));
            Centre = FVector3f(float(BeamLengthM * Cm / 2), 0.f, 0.f);
            L.Approx.Add(TEXT("spline:box along +X 0..100cm, noise amplitude as box width"));
            L.Skipped.Add(TEXT("spline.noise_motion"));
            L.Skipped.Add(TEXT("spline.curvature"));
        }
        SetVec3(Shape->BoxSize, Box);
        SetPosition(Shape->TransformOffset, Centre);
        L.Mapped.Add(TEXT("position_offset_m"));
        L.Mapped.Add(TEXT("random_offset_m"));

        // Velocity: cone about +X.
        const double Speed = Num(E, TEXT("speed.value")) * Eval(Curve(E, TEXT("speed.strength_curve")), 1.0);
        const double SpeedRandom = FMath::Clamp(Num(E, TEXT("speed.random")), 0.0, 1.0);
        double BoundsReach = 0;
        if (Speed > 0 && !bSpline)
        {
            auto* Vel = Module<UNiagaraStatelessModule_AddVelocity>(Em);
            Vel->SetIsModuleEnabled(true);
            Vel->VelocityType = ENSM_VelocityType::InCone;
            SetRange(Vel->ConeVelocityDistribution, Speed * (1 - SpeedRandom) * Cm, Speed * Cm);
            Vel->ConeVelocityDistribution.UpdateValuesFromDistribution();
            Vel->ConeVelocityScale.InitConstant(1.f);
            Vel->ConeRotationType = ENSM_ConeRotationType::Direction;
            SetVec3(Vel->ConeDirection, FVector3f(1, 0, 0));
            const double EmitMax = Num(E, TEXT("emit_angle_deg.max")), EmitMin = Num(E, TEXT("emit_angle_deg.min"));
            const double Focus = Num(E, TEXT("focus_angle_deg.max"));
            Vel->ConeAngle = float(FMath::Clamp(2 * (EmitMax + Focus), 0.0, 360.0));
            Vel->InnerCone = float(FMath::Clamp(2 * EmitMin, 0.0, double(Vel->ConeAngle)));
            Vel->bSpeedFalloffFromConeAxisEnabled = false;
            Vel->CoordinateSpace = ENiagaraCoordinateSpace::Local;
            L.Mapped.Add(TEXT("speed"));
            if (Has(E, TEXT("emit_angle_deg"))) L.Mapped.Add(TEXT("emit_angle_deg"));
            if (Focus > 0) L.Approx.Add(TEXT("focus_angle:widens the cone"));
            BoundsReach += Speed * Life;
        }
        else if (Speed > 0)
            L.Skipped.Add(TEXT("speed(spline travel)"));
        const double Gravity = Num(E, TEXT("gravity_scale"));
        if (Gravity != 0)
        {
            auto* G = Module<UNiagaraStatelessModule_GravityForce>(Em);
            G->SetIsModuleEnabled(true);
            SetVec3(G->GravityDistribution, FVector3f(0, 0, float(-GravityMs2 * Cm * Gravity)));
            L.Mapped.Add(TEXT("gravity_scale"));
            BoundsReach += 0.5 * GravityMs2 * FMath::Abs(Gravity) * Life * Life;
        }
        const double DragValue = Has(E, TEXT("drag")) ? Num(E, TEXT("drag")) : Num(E, TEXT("air_resistance"));
        if (DragValue > 0)
        {
            auto* D = Module<UNiagaraStatelessModule_Drag>(Em);
            D->SetIsModuleEnabled(true);
            D->DragDistribution.InitConstant(float(DragValue));
            D->DragDistribution.ChannelConstantsAndRanges = {float(DragValue)};
            L.Mapped.Add(TEXT("drag"));
        }
        if (Has(E, TEXT("turbulence"))) L.Skipped.Add(TEXT("turbulence"));

        const double Size = Num(E, TEXT("size.value"), 1) * Eval(Curve(E, TEXT("size.strength_curve")), 1.0);
        if (Has(E, TEXT("size.strength_curve"))) L.Approx.AddUnique(TEXT("strength_curves@1"));
        const double Reach = (Offset.Size() / Cm) + (Box.Size() / Cm) + BoundsReach + 2 * Size + 0.5;
        if (FStructProperty* P = FindFProperty<FStructProperty>(UNiagaraStatelessEmitter::StaticClass(), TEXT("FixedBounds")))
            *P->ContainerPtrToValuePtr<FBox>(Em) = FBox(FVector(-Reach * Cm), FVector(Reach * Cm + (bSpline ? BeamLengthM * Cm : 0.0)));

        TArray<UNiagaraRendererProperties*> Old = Em->GetRenderers();
        for (UNiagaraRendererProperties* R : Old) Em->RemoveRenderer(R, FGuid());

        if (bLight)
        {
            const FJson Light = Obj(E, TEXT("light"));
            const double Intensity = Num(Light, TEXT("intensity"), 1) * Eval(Curve(Light, TEXT("strength_curve")), 1.0);
            const double IntensityRandom = FMath::Clamp(Num(Light, TEXT("random")), 0.0, 1.0);
            const double Candela = UE_DOUBLE_PI * SceneUnitNits * Intensity;
            const FLinearColor Max(Rgb.R * Candela * CandelaToUE, Rgb.G * Candela * CandelaToUE, Rgb.B * Candela * CandelaToUE, 1.f);
            FLinearColor Min = Max * float(1 - IntensityRandom);
            Min.A = 1.f;
            SetColorRange(Init->ColorDistribution, Min, Max);
            bInvisibleAtFull = Candela * FMath::Max3(Rgb.R, Rgb.G, Rgb.B) <= 0;
            const auto AgeCurve = Curve(Light, TEXT("age_curve"));
            if (!AgeCurve.IsEmpty())
            {
                auto* Scale = Module<UNiagaraStatelessModule_ScaleColor>(Em);
                Scale->SetIsModuleEnabled(true);
                TArray<TArray<double>> Rgb3;
                for (const auto& K : AgeCurve) Rgb3.Add({K[0], K[1], K[1], K[1]});
                SetColorCurve(Scale->ScaleDistribution, Rgb3, {});
            }
            auto* Attr = Module<UNiagaraStatelessModule_LightAttributes>(Em);
            Attr->SetIsModuleEnabled(true);
            Attr->bApplyRadius = true;
            const double Radius = Num(Light, TEXT("radius_m")) * Cm;
            const auto RadiusCurve = Curve(Light, TEXT("radius_age_curve"));
            if (RadiusCurve.IsEmpty()) Attr->Radius.InitConstant(float(Radius));
            else
            {
                Attr->Radius.Mode = ENiagaraDistributionMode::UniformCurve;
                Attr->Radius.ChannelCurves = {RichCurve(RadiusCurve, 0, Radius)};
                Attr->Radius.UpdateValuesFromDistribution();
            }
            auto* R = NewObject<UNiagaraLightRendererProperties>(Em, NAME_None, RF_Transactional);
            R->bUseInverseSquaredFalloff = true;
            R->bAffectsTranslucency = true;
            R->bAlphaScalesBrightness = false;
            R->RadiusScale = 1.f;
            Em->AddRenderer(R, FGuid());
            L.Mapped.Append({TEXT("light.intensity"), TEXT("light.radius_m"), TEXT("color.rgb")});
            if (!AgeCurve.IsEmpty()) L.Mapped.Add(TEXT("light.age_curve"));
            if (Bool(Light, TEXT("soft_bulb"))) L.Approx.Add(TEXT("light.soft_bulb:point light"));
            if (Bool(Light, TEXT("affects_this_area_only"))) L.Skipped.Add(TEXT("light.affects_this_area_only"));
            return true;
        }

        // Sprite: colour x luminance (scene units), alpha, size, curves, facing, flipbook, material.
        const double Emissive = Num(E, TEXT("emissive_lighting"));
        const double Luminance = Emissive + Num(E, TEXT("diffuse_lighting")) * LitSceneUnits;
        if (Emissive <= 0) L.Approx.Add(FString::Printf(TEXT("diffuse_lighting:unlit at %g scene units"), LitSceneUnits));
        const double Alpha = Num(E, TEXT("alpha.value"), 1) * Eval(Curve(E, TEXT("alpha.strength_curve")), 1.0);
        const double AlphaRandom = FMath::Clamp(Num(E, TEXT("alpha.random")), 0.0, 1.0);
        if (Has(E, TEXT("alpha.strength_curve"))) L.Approx.AddUnique(TEXT("strength_curves@1"));
        const FLinearColor Max(Rgb.R * Luminance, Rgb.G * Luminance, Rgb.B * Luminance, Alpha);
        const FLinearColor Min(Max.R * (1 - ColorRandom), Max.G * (1 - ColorRandom), Max.B * (1 - ColorRandom), Alpha * (1 - AlphaRandom));
        SetColorRange(Init->ColorDistribution, Min, Max);
        bInvisibleAtFull = Alpha <= 0 || FMath::Max3(Max.R, Max.G, Max.B) <= 0;
        L.Mapped.Append({TEXT("color"), TEXT("alpha"), TEXT("emissive_lighting")});

        const double SizeRandom = FMath::Clamp(Num(E, TEXT("size.random")), 0.0, 1.0);
        const double Aspect = Has(E, TEXT("aspect")) ? Num(E, TEXT("aspect"), 1) : 1.0;
        const double Diameter = 2 * Size * Cm;
        if (FMath::IsNearlyEqual(Aspect, 1.0)) SetUniformRange(Init->SpriteSizeDistribution, Diameter * (1 - SizeRandom), Diameter);
        else
        {
            SetVec2Range(Init->SpriteSizeDistribution, FVector2f(Diameter * Aspect, Diameter) * float(1 - SizeRandom), FVector2f(Diameter * Aspect, Diameter));
            L.Approx.Add(TEXT("aspect+random:per-axis range"));
        }
        L.Mapped.Add(TEXT("size"));
        if (TOptional<double> Angle = RawComponent(E, TEXT("RandomAngles"), TEXT("y")))
        {
            SetRange(Init->SpriteRotationDistribution, -*Angle, *Angle);
            Init->SpriteRotationDistribution.UpdateValuesFromDistribution();
            L.Mapped.Add(TEXT("RandomAngles.y"));
        }
        else Init->SpriteRotationDistribution.InitConstant(0.f);
        if (TOptional<double> Rate = RawComponent(E, TEXT("RandomRotationRate"), TEXT("y")))
        {
            auto* Rot = Module<UNiagaraStatelessModule_SpriteRotationRate>(Em);
            Rot->SetIsModuleEnabled(true);
            Rot->bUseRateScale = false;
            SetRange(Rot->RotationRateDistribution, -*Rate, *Rate);
            Rot->RotationRateDistribution.UpdateValuesFromDistribution();
            L.Mapped.Add(TEXT("RandomRotationRate.y"));
        }

        const auto SizeCurve = Curve(E, TEXT("size.age_curve"));
        if (!SizeCurve.IsEmpty())
        {
            auto* Scale = Module<UNiagaraStatelessModule_ScaleSpriteSize>(Em);
            Scale->SetIsModuleEnabled(true);
            Scale->ScaleDistribution.Mode = ENiagaraDistributionMode::UniformCurve;
            Scale->ScaleDistribution.ChannelCurves = {RichCurve(SizeCurve, 0)};
            Scale->ScaleDistribution.UpdateValuesFromDistribution();
            L.Mapped.Add(TEXT("size.age_curve"));
        }
        const auto ColorCurve = Curve(E, TEXT("color.age_curve"));
        const auto AlphaCurve = Curve(E, TEXT("alpha.age_curve"));
        if (!ColorCurve.IsEmpty() || !AlphaCurve.IsEmpty())
        {
            auto* Scale = Module<UNiagaraStatelessModule_ScaleColor>(Em);
            Scale->SetIsModuleEnabled(true);
            SetColorCurve(Scale->ScaleDistribution, ColorCurve, AlphaCurve);
            if (!ColorCurve.IsEmpty()) L.Mapped.Add(TEXT("color.age_curve"));
            if (!AlphaCurve.IsEmpty()) L.Mapped.Add(TEXT("alpha.age_curve"));
        }

        auto* R = NewObject<UNiagaraSpriteRendererProperties>(Em, NAME_None, RF_Transactional);
        const FString Blend = Str(E, TEXT("blend"));
        const bool bAdditive = Blend == TEXT("Additive");
        if (!bAdditive && Blend != TEXT("AlphaBased")) L.Approx.Add(TEXT("blend ") + Blend + TEXT(":translucent"));
        R->Material = Material(bAdditive, Str(E, TEXT("texture")));
        R->SortMode = bAdditive ? ENiagaraSortMode::None : ENiagaraSortMode::ViewDepth;
        L.Mapped.Add(TEXT("texture"));
        L.Mapped.Add(TEXT("blend"));
        const FString Facing = Str(E, TEXT("facing"));
        const bool bVelocity = Bool(E, TEXT("orient_to_velocity"));
        R->Alignment = bVelocity ? ENiagaraSpriteAlignment::VelocityAligned : ENiagaraSpriteAlignment::Unaligned;
        R->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
        if (bVelocity) L.Mapped.Add(TEXT("orient_to_velocity"));
        if (Facing == TEXT("Free") || Facing == TEXT("Horizontal"))
        {
            auto* F = Module<UNiagaraStatelessModule_SpriteFacingAndAlignment>(Em);
            F->SetIsModuleEnabled(true);
            F->bSpriteFacingEnabled = true;
            F->bSpriteAlignmentEnabled = false;
            SetVec3(F->SpriteFacing, Facing == TEXT("Free") ? FVector3f(1, 0, 0) : FVector3f(0, 0, 1));
            R->FacingMode = ENiagaraSpriteFacingMode::CustomFacingVector;
            L.Mapped.Add(TEXT("facing"));
        }
        else if (!Facing.IsEmpty() && Facing != TEXT("Camera"))
            L.Approx.Add(TEXT("facing ") + Facing + TEXT(":camera"));
        else
            L.Mapped.Add(TEXT("facing"));

        const TArray<double> Tiles = Nums(E, TEXT("tiles"));
        const FJson Anim = Obj(E, TEXT("anim"));
        if (Tiles.Num() == 2)
        {
            R->SubImageSize = FVector2D(Tiles[0], Tiles[1]);
            const int32 Frames = Anim ? int32(Num(Anim, TEXT("frames"), Tiles[0] * Tiles[1])) : int32(Tiles[0] * Tiles[1]);
            auto* Sub = Module<UNiagaraStatelessModule_SubUVAnimation>(Em);
            Sub->SetIsModuleEnabled(true);
            Sub->NumFrames = FMath::Max(1, Frames);
            Sub->bStartFrameRangeOverride_Enabled = false;
            Sub->bEndFrameRangeOverride_Enabled = false;
            R->bSubImageBlend = Anim && Bool(Anim, TEXT("blend"));
            const TArray<double> First = Anim ? Nums(Anim, TEXT("first_frame")) : TArray<double>();
            if (!Anim || Num(Anim, TEXT("rate")) <= 0)
            {
                // A fixed frame: the first-frame interval (1-based) when given, else the first frame.
                Sub->AnimationMode = ENSMSubUVAnimation_Mode::DirectSet;
                const int32 A = First.Num() == 2 ? FMath::Clamp(int32(First[0]) - 1, 0, Frames - 1) : 0;
                const int32 B = First.Num() == 2 ? FMath::Clamp(int32(First[1]) - 1, 0, Frames - 1) : 0;
                Sub->FrameIndex.Mode = A == B ? ENiagaraDistributionMode::UniformConstant : ENiagaraDistributionMode::UniformRange;
                Sub->FrameIndex.Min = FMath::Min(A, B);
                Sub->FrameIndex.Max = FMath::Max(A, B);
                L.Mapped.Add(TEXT("anim.static_frame"));
                if (First.Num() == 2) L.Approx.Add(TEXT("first_frame:1-based interval"));
            }
            else if (Bool(Anim, TEXT("lifetime_rate")))
            {
                const double Rate = Num(Anim, TEXT("rate"), 1);
                if (FMath::IsNearlyEqual(Rate, 1.0) || !Bool(Anim, TEXT("cycle"))) Sub->AnimationMode = ENSMSubUVAnimation_Mode::Linear;
                else
                {
                    Sub->AnimationMode = ENSMSubUVAnimation_Mode::InfiniteLoop;
                    Sub->LoopsPerSecond = float(Rate / FMath::Max(Life, 1e-3));
                }
                if (!FMath::IsNearlyEqual(Rate, 1.0) && !Bool(Anim, TEXT("cycle"))) L.Approx.Add(TEXT("anim.rate:once over life"));
                L.Mapped.Add(TEXT("anim"));
                if (First.Num() == 2) L.Skipped.Add(TEXT("anim.first_frame(random start)"));
            }
            else
            {
                const double Fps = Num(Anim, TEXT("fps"), Num(Anim, TEXT("rate")));
                Sub->AnimationMode = Bool(Anim, TEXT("cycle")) ? ENSMSubUVAnimation_Mode::InfiniteLoop : ENSMSubUVAnimation_Mode::Linear;
                Sub->LoopsPerSecond = float(Fps / FMath::Max(1, Frames));
                if (!Bool(Anim, TEXT("cycle"))) L.Approx.Add(TEXT("anim.fps once:over life"));
                L.Mapped.Add(TEXT("anim"));
            }
            L.Mapped.Add(TEXT("tiles"));
        }
        Em->AddRenderer(R, FGuid());
        return R->Material != nullptr;
    }

    /** Raw attributes the rules read (anything else of the emitter is reported as skipped). */
    static bool IsMappedRaw(const FString& K)
    {
        static const TSet<FString> Known = {
            TEXT("Enabled"), TEXT("Inheritance"), TEXT("ParticleSystem"), TEXT("Count"), TEXT("Continuous"), TEXT("ParticleLifeTime"),
            TEXT("EmitterLifeTime"), TEXT("PulsePeriod"), TEXT("SpawnDelay"), TEXT("EmitPerDistance"), TEXT("Size"), TEXT("Aspect"),
            TEXT("Alpha"), TEXT("Color"), TEXT("Speed"), TEXT("EmitAngle"), TEXT("FocusAngle"), TEXT("GravityScale"),
            TEXT("AirResistance"), TEXT("Drag"), TEXT("BlendType"), TEXT("Facing"), TEXT("OrientToVelocity"), TEXT("EmissiveLighting"),
            TEXT("DiffuseLighting"), TEXT("Texture"), TEXT("TextureAnimation"), TEXT("TextureTiling"), TEXT("LightSource"),
            TEXT("PositionOffset"), TEXT("RandomOffset"), TEXT("EmissionSize"), TEXT("RandomAngles"), TEXT("RandomRotationRate"),
            TEXT("Spawn"), TEXT("Comment"), TEXT("SplineGuided"), TEXT("InheritVelocity")};
        return Known.Contains(K);
    }

    /** One game effect (sc_particle_effect JSON) -> /Game/SC/FX/<library>/<effect>. */
    bool BuildEffect(const FString& Reference)
    {
        char* Text = nullptr;
        if (sc_particle_effect(H, TCHAR_TO_UTF8(*Reference), &Text) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR effect %s: %s"), *Reference, UTF8_TO_TCHAR(sc_last_error(H)));
            ++Errors;
            return false;
        }
        const FJson Effect = Parse(UTF8_TO_TCHAR(Text));
        sc_free_string(Text);
        if (!Effect)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR effect %s: JSON"), *Reference);
            ++Errors;
            return false;
        }
        const FString EffectName = Str(Effect, TEXT("effect"));
        const FString Name = SafeSegment(EffectName.Replace(TEXT("."), TEXT("_")));
        const FString LongName = FString(Root) / SafePath(Str(Effect, TEXT("library"))) / Name;

        UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, TemplatePath);
        if (!Template || Template->GetEmitterHandles().IsEmpty() || !Template->GetEmitterHandle(0).GetStatelessEmitter())
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR template %s has no lightweight emitter"), TemplatePath);
            ++Errors;
            return false;
        }
        UPackage* Package = PackageFor(LongName);
        if (UObject* Old = StaticFindObject(UNiagaraSystem::StaticClass(), Package, *Name))
        {
            Old->ClearFlags(RF_Public | RF_Standalone);
            Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        }
        // As UNiagaraSystemFactoryNew does for a template.
        UNiagaraSystem* System = Cast<UNiagaraSystem>(StaticDuplicateObject(Template, Package, *Name, RF_Public | RF_Standalone));
        System->TemplateAssetDescription = FText();
        System->Category = FText();
        const FNiagaraEmitterHandle TemplateHandle = System->GetEmitterHandle(0);

        const TArray<TSharedPtr<FJsonValue>> Emitters = Effect->GetArrayField(TEXT("emitters"));
        TSet<FName> Used;
        int32 Made = 0;
        auto Add = [&](const FString& Wanted, const FJson& E, const FJson& Parent, bool bLight, FLog& L) {
            FString Unique = SafeSegment(Wanted);
            for (int32 n = 2; Used.Contains(FName(*Unique)); ++n) Unique = FString::Printf(TEXT("%s_%d"), *SafeSegment(Wanted), n);
            Used.Add(FName(*Unique));
            System->DuplicateEmitterHandle(TemplateHandle, FName(*Unique));
            FNiagaraEmitterHandle& Handle = System->GetEmitterHandles().Last();
            bool bInvisible = false;
            const bool bOK = Configure(Handle.GetStatelessEmitter(), E, Parent, bLight, L, bInvisible);
            if (bInvisible)
            {
                Handle.SetIsEnabled(false, *System, false);
                L.Approx.Add(TEXT("disabled:invisible at strength 1"));
            }
            ++Made;
            return bOK;
        };
        for (int32 i = 0; i < Emitters.Num(); ++i)
        {
            const FJson E = Emitters[i]->AsObject();
            const int32 ParentIndex = int32(Num(E, TEXT("parent"), -1));
            const FJson Parent = Emitters.IsValidIndex(ParentIndex) ? Emitters[ParentIndex]->AsObject() : nullptr;
            const FString Path = Str(E, TEXT("path"));
            FString Local = Path.StartsWith(EffectName) ? Path.Mid(EffectName.Len()) : Path;
            Local.RemoveFromStart(TEXT("."));
            if (Local.IsEmpty()) Local = EffectName.Contains(TEXT(".")) ? EffectName.RightChop(EffectName.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1) : EffectName;
            Local = Local.Replace(TEXT("."), TEXT("_"));
            FLog L;
            TArray<FString> RawKeys;
            if (const FJson Raw = Obj(E, TEXT("raw")))
                for (const auto& Pair : Raw->Values) RawKeys.Add(FString(Pair.Key.ToView()));
            for (const FString& K : RawKeys)
                if (!IsMappedRaw(K)) L.Skipped.Add(K);
            for (const auto& K : E->GetArrayField(TEXT("inherited")))
                if (!IsMappedRaw(K->AsString())) L.Skipped.AddUnique(K->AsString() + TEXT("(inherited)"));
            TArray<FString> Out;
            if (!Bool(E, TEXT("enabled")))
                L.Skipped.Insert(TEXT("emitter(disabled in the library)"), 0);
            else
            {
                const bool bGeometry = !Str(E, TEXT("geometry")).IsEmpty();
                const bool bCustomMaterial = !Str(E, TEXT("material")).IsEmpty();
                if (bGeometry) L.Skipped.Insert(TEXT("geometry(mesh particles)"), 0);
                if (bCustomMaterial) L.Skipped.Insert(TEXT("material(custom shader)"), 0);
                const bool bSprite = !bGeometry && !bCustomMaterial && !Str(E, TEXT("texture")).IsEmpty() && Num(E, TEXT("alpha.value"), 1) > 0;
                if (!bSprite && !bGeometry && !bCustomMaterial)
                    L.Skipped.Insert(Str(E, TEXT("texture")).IsEmpty() ? TEXT("sprite(no texture)") : TEXT("sprite(alpha 0)"), 0);
                if (bSprite && Add(Local, E, Parent, false, L)) Out.Add(Local);
                const FJson Light = Obj(E, TEXT("light"));
                // FakeLight: the light only feeds the emitter's flare (not ported), it lights nothing.
                const bool bFakeLight = Str(E, TEXT("raw.FakeLight")).StartsWith(TEXT("true"), ESearchCase::IgnoreCase);
                if (Light && bFakeLight) L.Skipped.AddUnique(TEXT("light(FakeLight)"));
                if (Light && !bFakeLight && Num(Light, TEXT("intensity"), 1) > 0 && Num(Light, TEXT("radius_m")) > 0)
                {
                    FLog LightLog;
                    Add(Local + TEXT("_light"), E, Parent, true, LightLog);
                    Out.Add(Local + TEXT("_light"));
                    L.Mapped.Append(LightLog.Mapped);
                    for (const FString& A : LightLog.Approx) L.Approx.AddUnique(A);
                    for (const FString& S : LightLog.Skipped) L.Skipped.AddUnique(S);
                }
                if (Bool(E, TEXT("flare_enabled"))) L.Skipped.AddUnique(TEXT("flare"));
                if (!Str(E, TEXT("flow_texture")).IsEmpty()) L.Skipped.AddUnique(TEXT("flow_texture"));
            }
            TArray<FString> Mapped;
            for (const FString& M : L.Mapped) Mapped.AddUnique(M);
            UE_LOG(LogSCFx, Display, TEXT("SC_FX emitter=%s/%s -> %s mapped=%s skipped=%s approx=%s"), *Str(Effect, TEXT("library")), *Path,
                   Out.IsEmpty() ? TEXT("-") : *FString::Join(Out, TEXT("+")), *Join(Mapped), *Join(L.Skipped), *Join(L.Approx));
        }
        System->RemoveEmitterHandle(TemplateHandle);
        if (UNiagaraStatelessEmitter* Orphan = TemplateHandle.GetStatelessEmitter())
            Orphan->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

        // Strength input for the runtime (curves are baked at 1, see header).
        FNiagaraVariable Strength(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SC.Strength"));
        System->GetExposedParameters().AddParameter(Strength, true, true);
        System->GetExposedParameters().SetParameterValue(1.0f, Strength);

        System->RequestCompile(false);
        System->WaitForCompilationComplete(true, false);
        System->MarkPackageDirty();
        if (!Save(System)) return false;
        ++Systems;
        UE_LOG(LogSCFx, Display, TEXT("SC_FX_SYSTEM %s <- %s emitters=%d state=%s beam=+X 100cm (scale X to the beam length) user=SC.Strength(unbound, curves baked at 1)"),
               *System->GetPathName(), *Reference, System->GetEmitterHandles().Num(), System->GetSystemStateModeString());
        return Made > 0;
    }

    /** A MaterialEffects FXLib entry: build every particle effect it plays. */
    void BuildMaterialEffect(const FString& LibEffect)
    {
        char* Text = nullptr;
        if (sc_material_effect(H, TCHAR_TO_UTF8(*LibEffect), &Text) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR material effect %s: %s"), *LibEffect, UTF8_TO_TCHAR(sc_last_error(H)));
            ++Errors;
            return;
        }
        const FJson Entry = Parse(UTF8_TO_TCHAR(Text));
        sc_free_string(Text);
        const TArray<TSharedPtr<FJsonValue>>* Particles = nullptr;
        if (!Entry || !Entry->TryGetArrayField(TEXT("particles"), Particles) || Particles->IsEmpty())
        {
            UE_LOG(LogSCFx, Warning, TEXT("SC_FX material_effect=%s plays no particles"), *LibEffect);
            return;
        }
        for (const auto& P : *Particles)
        {
            const FString Reference = Str(P->AsObject(), TEXT("reference"));
            UE_LOG(LogSCFx, Display, TEXT("SC_FX material_effect=%s particle=%s random_group=%s"), *LibEffect, *Reference,
                   Has(P->AsObject(), TEXT("random_group")) ? *FString::Printf(TEXT("%g"), Num(P->AsObject(), TEXT("random_group"))) : TEXT("-"));
            BuildEffect(Reference);
        }
    }
};

TArray<FString> SplitList(const FString& S)
{
    TArray<FString> Out;
    S.ParseIntoArray(Out, TEXT(";"));
    for (FString& E : Out) E.TrimStartAndEndInline();
    Out.RemoveAll([](const FString& E) { return E.IsEmpty(); });
    return Out;
}
} // namespace

USCFxBuildCommandlet::USCFxBuildCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 USCFxBuildCommandlet::Main(const FString& Params)
{
    FModuleManager::Get().LoadModuleChecked(TEXT("NiagaraEditor")); // system compilation
    const FString ConfigPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("FX/effects.json"));
    FString ConfigText;
    FJson Config;
    if (FFileHelper::LoadFileToString(ConfigText, *ConfigPath)) Config = Parse(ConfigText);
    if (!Config)
    {
        UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR config %s missing or not JSON"), *ConfigPath);
        return 1;
    }
    TArray<FString> Effects, MaterialEffects;
    FString Arg;
    const bool bEffectsArg = FParse::Value(*Params, TEXT("Effects="), Arg, false);
    if (bEffectsArg) Effects = SplitList(Arg);
    const bool bMaterialArg = FParse::Value(*Params, TEXT("MaterialEffects="), Arg, false);
    if (bMaterialArg) MaterialEffects = SplitList(Arg);
    if (!bEffectsArg && !bMaterialArg)
    {
        for (const auto& V : Config->GetArrayField(TEXT("effects"))) Effects.Add(V->AsString());
        for (const auto& V : Config->GetArrayField(TEXT("material_effects"))) MaterialEffects.Add(V->AsString());
    }

    FBuilder B;
    B.SceneUnitNits = FSCStarEngine::Get().SceneUnitNits;
    B.LitSceneUnits = Num(Config, TEXT("unlit_diffuse_scene_units"), 0.0);
    if (sc_open(nullptr, nullptr, &B.H) != SC_STATUS_OK)
    {
        UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR sc_open: %s"), UTF8_TO_TCHAR(sc_last_error(nullptr)));
        return 1;
    }
    UE_LOG(LogSCFx, Display, TEXT("SC_FX build effects=%d material_effects=%d scene_unit_nits=%g unlit_diffuse_scene_units=%g"), Effects.Num(),
           MaterialEffects.Num(), B.SceneUnitNits, B.LitSceneUnits);
    for (const FString& E : Effects) B.BuildEffect(E);
    for (const FString& M : MaterialEffects) B.BuildMaterialEffect(M);
    sc_close(B.H);
    UE_LOG(LogSCFx, Display, TEXT("SC_FX_DONE systems=%d textures=%d materials=%d errors=%d"), B.Systems, B.Textures.Num(), B.Materials.Num(), B.Errors);
    return B.Errors == 0 ? 0 : 1;
}
