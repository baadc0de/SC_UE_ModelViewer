// SCFxBuild: the game's particle effects (CryEngine ParticleLibrary, read through the FFI as sc_particle_effect JSON)
// rebuilt as Niagara systems of lightweight (stateless) emitters, saved under the gitignored /Game/SC/FX (WP-K4c).
//
//   UnrealEditor-Cmd.exe SCViewer.uproject -run=SCFxBuild [-Effects=<ref>;<ref>] [-MaterialEffects=<lib:effect>;...]
//                        [-Features=all|none|<f>,<f>,...] [-RefractionScale=<k> (verification aid; default 1)]
//                        [-LoopCapacity=<n> (verification aid: size loops for n spawn infos; default the engine's)]
//   UnrealEditor-Cmd.exe SCViewer.uproject -run=SCFxBuild -VerifyContract   (exit 1 when a built system under /Game/SC/FX is
//                        stale: no SC.FxContract or an older one than SCFxRules::FxContractVersion; the showcase package runs it)
//
// Without arguments the lists come from Config/FX/effects.json. An effect `<GUID>|<library>.<effect>` becomes
// /Game/SC/FX/<library>/<effect> (dots -> underscores; SCFxNames.h, the rule the runtime finds it by); a material effect (FXLib entry) builds the particle effects it
// plays. Each enabled game emitter with something to draw becomes one lightweight emitter duplicated from the engine's
// FountainLightweight template (sprite) plus, as the data asks: a light-only twin (a light shares no colour with its
// sprite), a distortion twin (Refraction), and one sprite per lens-flare element (FlareEnabled). Every rule below is
// per parameter; nothing is keyed on an effect or asset. -Features (strength, beam, spline, flares, soft, stretch,
// flow, distortion, extras, erosion) switches the WP-K4c fidelity rules off for before/after comparisons.
//
// Units and axes. Game values are metres / seconds; UE centimetres (x100). Game vectors map like the importer's
// (FSCMeshes::Position, LightBasis): game (x, y, z) -> UE (y, x, z). The emitter's forward, CryEngine's +Y (the
// focus / emit axis), is therefore the system's +X.
//
// Runtime contract (User parameters; the SC_FX_SYSTEM line lists the ones a system has):
//  SC.Strength          float 0..1, the effect's EmitterStrength input: the tractor beam's beamStrengthValues (stable,
//                       unstable, noTarget ...; the game blends between them with beamEffectBlendCoeff). Default 1.
//  SC.BeamEnd           vec3 cm, the beam's end (the target) in the component's space; the component sits at the beam
//                       start (the emitter helper). Default (100, 0, 0).
//  SC.BeamStartTangent  vec3 cm, the emitter's forward direction (component space) x the beam length. Default (100,0,0).
//  SC.BeamLength        float cm, |SC.BeamEnd|. Default 100.
//  SC.BeamLength2       vec2 cm, (SC.BeamLength, SC.BeamLength) (stateless expressions cannot widen a float). Default
//                       (100, 100).
// The effect's scale (e.g. a MaterialEffects entry's minscale..maxscale) is the component's scale: positions and speeds of
// the local-space emitters follow it, and so do sprite sizes (Niagara's Apply Owner Scale To Attributes module, its
// initial sprite size by the largest axis, enabled on every sprite-like emitter but guided beams). Everything else the
// systems evaluate themselves (stateless expressions of the parameters above; WP-E1 E1-6).
//
// Mapping (JSON field -> Niagara):
//  spawn: burst -> Burst spawn info (count x parent count for Spawn=FromParentBirth children; PulsePeriod -> loop);
//         continuous -> Rate = Count / min(lifetime, EmitterLifeTime) (CryEngine's Count is the number alive at once);
//         a count strength curve -> the rate's expression x the curve in SC.Strength (constant rates; random ones keep
//         strength 1);
//         per_distance (spline-guided) -> Rate = EmitPerDistance x SC.BeamLength / lifetime, so the beam holds
//         EmitPerDistance particles per metre at any length (the spawn rate's expression, which the fork evaluates per
//         loop: WP-E1 E1-6);
//         infinite loops re-read their rates per loop and last min(lifetime, 0.25 s) so strength and beam length follow,
//         stretched only where the loops alive at once (1 + ceil(lifetime / loop)) would not fit the spawn infos a
//         lightweight emitter draws (NiagaraStateless::GetMaxSpawnInfos(), the engine's effective limit
//         fx.Niagara.Stateless.MaxSpawnInfos, which the project's DefaultEngine.ini raises to 32 for the editor, the game and
//         this commandlet alike; SCFxRules.h); a pulse burst whose bursts
//         alive at once do not fit is reported (its period is the data's);
//         SpawnDelay -> delay.
//  lifetime (+Random) -> InitializeParticle Lifetime range [v(1-r), v].
//  size (+Random, Aspect) -> Sprite Size: CryEngine's Size is a radius -> 2 x 100 x size; age_curve -> ScaleSpriteSize.
//  color x (EmissiveLighting + DiffuseLighting x LitSceneUnits) -> particle colour in scene units; the material
//         instance's EmissiveScale is the game's scene unit (FSCStarEngine::SceneUnitNits, cd/m2). Color Random -> one
//         random for all channels; age curves (colour, alpha) -> ScaleColor.
//  EmitterStrength curves (colour, alpha, speed, count, light intensity) -> the curve itself, per channel, as a stateless
//         expression in User.SC.Strength: piecewise linear, held at its end keys, as a sum of clamped ramps (SCFxRules.h
//         ClampSum; the fork's clamp expression, WP-E1 E1-6), so it is exact at every strength (a polynomial through the
//         game's strengths overshot between them: WP-K4a round 2). Colour / alpha go to Dynamic Material Parameter 0 (the
//         masters multiply by it), speed to the cone velocity scale, light intensity to the light's diffuse/specular scale,
//         count to the spawn rate. Lifetime and size curves stay baked at strength 1 (lifetime and random size ranges).
//  speed (+Random) + EmitAngle / FocusAngle -> AddVelocity cone about +X (Niagara's cone angle is the full angle).
//  gravity_scale -> GravityForce (0, 0, -981 x scale) cm/s2, world space; Drag / AirResistance -> Drag.
//  position_offset_m -> ShapeLocation offset; random_offset_m (EmissionSize) -> box of 2 x the offset.
//  spline (SplineGuided): particles start at the emitter and travel to SC.BeamEnd over their lifetime on the quadratic
//         Bezier whose control point is C = 1/2 (curvature x SC.BeamStartTangent + (1 - curvature) x SC.BeamEnd): the
//         beam leaves along the emitter's forward and bends to the target (straight and uniform when they agree):
//         velocity (C x 2 / T) + constant acceleration (2 (E - 2C) / T^2). SplineNoise -> curl noise of the noise
//         amplitude over the spatial wavelength. Their PositionOffset (the spline start) is dropped. All guided emitters
//         of an effect ride one spline (the weapon's beam): the effect's first guided emitter gives the curvature.
//  facing Camera -> face camera (OrientToVelocity: velocity aligned); Free -> custom facing along +X.
//  VelocityStretch -> velocity aligned + ScaleSpriteSizeBySpeed: length x (1 + speed x stretch / size); a guided
//         beam's sprites (speed = SC.BeamLength / lifetime) get up to + SC.BeamLength x stretch / lifetime of length, on a
//         tent over age (2 min(age, 1 - age)) so a centred streak never leaves the beam.
//  tiles / anim -> renderer SubImageSize + SubUVAnimation (lifetime rate: linear over life; fps: loops; AnimRate 0:
//         a random fixed frame in the first-frame interval).
//  texture -> imported once to /Game/SC/FX/Textures/<archive path>; material instance per (master, texture, material
//         parameters) under /Game/SC/FX/Materials: blend Additive -> M_SC_Particle_Additive, else _Translucent.
//         SoftParticle(V2) -> SoftFade (FadeThickness, else half the sprite radius); RandomFlipU/V -> FlipU/V; FlowTexture x
//         FlowMapIntensity -> flow-map frame blending; Refraction (Strength) -> a twin on M_SC_Particle_Distortion
//         (NormalTexture when given).
//  light -> LightAttributes radius (+ age curve) and a light renderer; colour = particle colour x pi x SceneUnitNits x
//         Intensity cd (FSCStarEngine::PointCandela's far field, no bulb) x 100^2 (UE's internal cd -> cm units).
//         FakeLight lights only feed the flare and light nothing.
//  flare (Flare = <library>.<item>, Data/Libs/Flares/<library>.xml LensFlareLibrary): each Glow element -> a camera
//         facing procedural glow (1 - r)^Gamma, each Ghost -> a sprite of its texture; colour = the emitter colour x
//         Tint, luminance = root x element Brightness x the light's intensity (Affectedbylightcolor), age = the light's
//         age curve; size = 2 x emitter Size x root Size x element Size x Transformation.Scale metres x flare_world_scale (config; CryEngine sizes
//         flares on screen; the emitter's size stands in for the screen scale).
//  AlphaErosion (own or inherited; base x ParticleAge curve) -> Dynamic Material Parameter 1 X: the masters cut texture
//         alpha below it and rescale the rest (low-alpha skirts dissolve; without it emissive sprites clip to their
//         alpha >= 1/255 silhouette).
//  RandomAngles y / RandomRotationRate y (raw) -> sprite rotation / rotation rate ranges; CameraMaxDistance -> the
//         emitter's max distance culling.
// Every raw attribute not mapped is listed in `skipped`, every approximation in `approx`.
#include "SCFxBuildCommandlet.h"
#include "SCBridgeClient.h" // scbridge.h
#include "SCFxNames.h"
#include "SCFxRules.h"
#include "SCFxContract.h"
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
#include "Stateless/NiagaraStatelessSpawnInfoCapacity.h" // NiagaraStateless::MaxGpuSpawnInfos (the fork's public constant)
#include "Stateless/Expressions/NiagaraStatelessExpressionFloat.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionVec2.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionVec3.h"
#include "Stateless/Modules/NiagaraStatelessModule_ApplyOwnerScaleToAttributes.h"
#include "Stateless/Modules/NiagaraStatelessModule_AccelerationForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_AddVelocity.h"
#include "Stateless/Modules/NiagaraStatelessModule_CurlNoiseForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_Drag.h"
#include "Stateless/Modules/NiagaraStatelessModule_DynamicMaterialParameters.h"
#include "Stateless/Modules/NiagaraStatelessModule_GravityForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_InitializeParticle.h"
#include "Stateless/Modules/NiagaraStatelessModule_LightAttributes.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleColor.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSize.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSizeBySpeed.h"
#include "Stateless/Modules/NiagaraStatelessModule_ShapeLocation.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteFacingAndAlignment.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteRotationRate.h"
#include "Stateless/Modules/NiagaraStatelessModule_SubUVAnimation.h"
#include "Stateless/NiagaraStatelessEmitter.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "XmlFile.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCFx, Log, All);

namespace
{
using FJson = TSharedPtr<FJsonObject>;
using FCurve = TArray<TArray<double>>;
const TCHAR* const TemplatePath = TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight");
const TCHAR* const AdditiveMaster = TEXT("/SCBridge/Materials/M_SC_Particle_Additive.M_SC_Particle_Additive");
const TCHAR* const TranslucentMaster = TEXT("/SCBridge/Materials/M_SC_Particle_Translucent.M_SC_Particle_Translucent");
const TCHAR* const DistortionMaster = TEXT("/SCBridge/Materials/M_SC_Particle_Distortion.M_SC_Particle_Distortion");
const TCHAR* const Root = SCFxNames::Root();
const TCHAR* const FlareDir = TEXT("Data/Libs/Flares/");
constexpr double Cm = 100.0;
constexpr double GravityMs2 = 9.81;
constexpr double CandelaToUE = 100.0 * 100.0; // UPointLightComponent::ComputeLightBrightness, inverse-squared Candelas
// The runtime contract (header).
const FName UserStrength(TEXT("User.SC.Strength"));
const FName UserBeamEnd(TEXT("User.SC.BeamEnd"));
const FName UserBeamTangent(TEXT("User.SC.BeamStartTangent"));
const FName UserBeamLength(TEXT("User.SC.BeamLength"));
const FName UserBeamLength2(TEXT("User.SC.BeamLength2"));
constexpr double DefaultBeamCm = 100.0;
// The spawn infos a lightweight emitter draws on the GPU: the loops (and bursts) of a looping emitter whose particles may
// live at once must fit them (SCFxRules::LoopDuration). The engine's effective limit (WP-E1 E1-6 review:
// fx.Niagara.Stateless.MaxSpawnInfos, UE's 8 by default, 32 in the project's DefaultEngine.ini, which this commandlet reads as
// the game does), so the guard follows what the project ships with.
int32 SpawnInfoCapacity() { return int32(NiagaraStateless::GetMaxSpawnInfos()); }
const FName UserFxContract(TEXT("User.SC.FxContract"));
// Infinite loops re-read bound and expression rates once per loop: this short, so strength and beam length follow.
constexpr double FollowLoopSeconds = 0.25;

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
FCurve Curve(const FJson& O, const FString& Path)
{
    FCurve Keys;
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
double Eval(const FCurve& Keys, double T, int32 Channel = 0) { return SCFxRules::EvalKeys(Keys, T, Channel); } // the tested rule
FJson Parse(const FString& Text)
{
    FJson O;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
    return O;
}
FString Join(const TArray<FString>& A) { return TEXT("[") + FString::Join(A, TEXT(",")) + TEXT("]"); }

/** game (x, y, z) -> UE (y, x, z), metres -> cm (FSCMeshes::Position / LightBasis). */
FVector3f Basis(const TArray<double>& V)
{
    auto C = [&V](int32 i) { return V.IsValidIndex(i) ? float(V[i]) : 0.f; };
    return FVector3f(C(1), C(0), C(2)) * float(Cm);
}
/** A raw attribute as written (empty when absent). */
FString Raw(const FJson& E, const FString& Attr) { return Str(E, TEXT("raw.") + Attr); }
/** The base value of a raw attribute ("true,..." -> true). */
bool RawFlag(const FJson& E, const FString& Attr) { return Raw(E, Attr).TrimStart().StartsWith(TEXT("true"), ESearchCase::IgnoreCase); }
/** First number after `key=` in a raw attribute (e.g. RandomAngles "y=360", SoftParticleV2 "FadeThickness=2.5"). */
TOptional<double> RawComponent(const FJson& E, const FString& Attr, const FString& Key)
{
    const FString Text = Raw(E, Attr);
    int32 At = INDEX_NONE;
    for (int32 From = 0; (At = Text.Find(Key + TEXT("="), ESearchCase::IgnoreCase, ESearchDir::FromStart, From)) != INDEX_NONE; From = At + 1)
        if (At == 0 || !FChar::IsAlnum(Text[At - 1])) break; // a whole key, not the tail of a longer one
    if (At == INDEX_NONE) return {};
    return FCString::Atod(*Text.Mid(At + Key.Len() + 1));
}
/** The leading number of a raw attribute ("0.078067,Random=1" -> 0.078067). */
TOptional<double> RawNumber(const FJson& E, const FString& Attr)
{
    const FString Text = Raw(E, Attr).TrimStart();
    if (Text.IsEmpty() || !(FChar::IsDigit(Text[0]) || Text[0] == TEXT('-') || Text[0] == TEXT('.'))) return {};
    return FCString::Atod(*Text);
}
/** A CryEngine curve modifier inside a raw attribute, e.g. "0.5,ParticleAge=(t=0.1,v=0.016;t=1,v=1,flags=4)": keys
 *  [t, v] sorted by t; a key's missing t or v is 0 (the particles.rs rule). */
FCurve RawCurve(const FString& Text, const FString& Modifier)
{
    FCurve Keys;
    const int32 At = Text.Find(Modifier + TEXT("=("));
    if (At == INDEX_NONE) return Keys;
    const FString Rest = Text.Mid(At + Modifier.Len() + 2);
    const int32 End = Rest.Find(TEXT(")"));
    TArray<FString> Parts;
    (End == INDEX_NONE ? Rest : Rest.Left(End)).ParseIntoArray(Parts, TEXT(";"), false);
    for (const FString& P : Parts)
    {
        double T = 0, V = 0;
        TArray<FString> Fields;
        P.ParseIntoArray(Fields, TEXT(","));
        for (const FString& Fd : Fields)
        {
            FString K, Val;
            if (!Fd.Split(TEXT("="), &K, &Val)) continue;
            K.TrimStartAndEndInline();
            if (K == TEXT("t")) T = FCString::Atod(*Val);
            else if (K == TEXT("v")) V = FCString::Atod(*Val);
        }
        Keys.Add({T, V});
    }
    Keys.StableSort([](const TArray<double>& A, const TArray<double>& B) { return A[0] < B[0]; });
    return Keys;
}
// The naming rule the runtime shares (SCFxNames.h).
using SCFxNames::SafePath;
using SCFxNames::SafeSegment;

// ---- distributions -----------------------------------------------------------------------------------------------
FRichCurve RichCurve(const FCurve& Keys, int32 Channel, double Scale = 1.0)
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
void SetColorCurve(FNiagaraDistributionColor& D, const FCurve& Rgb, const FCurve& A)
{
    D.Mode = ENiagaraDistributionMode::NonUniformCurve;
    D.ChannelCurves.Reset();
    for (int32 c = 0; c < 3; ++c) D.ChannelCurves.Add(RichCurve(Rgb, c));
    D.ChannelCurves.Add(RichCurve(A, 0));
    D.UpdateValuesFromDistribution();
}

// ---- stateless expressions (per-instance values from User parameters) --------------------------------------------
// Typed: the fork exports Niagara's stateless expression structs (WP-E1 E1-6; UE 5.9 did not, and they were made through
// reflection).
template <typename T> FInstancedStruct Op(FInstancedStruct A, FInstancedStruct B)
{
    T E;
    E.A = MoveTemp(A);
    E.B = MoveTemp(B);
    return FInstancedStruct::Make(E);
}
FInstancedStruct FConst(double V)
{
    FNiagaraStatelessExpressionFloatConstant E;
    E.A = float(V);
    return FInstancedStruct::Make(E);
}
FInstancedStruct FBind(FName N)
{
    FNiagaraStatelessExpressionFloatBinding E;
    E.A = N;
    return FInstancedStruct::Make(E);
}
FInstancedStruct FMul(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionFloatMultiply>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct FAdd(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionFloatAdd>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct FClamp(FInstancedStruct A, double Min, double Max)
{
    FNiagaraStatelessExpressionFloatClamp E;
    E.A = MoveTemp(A);
    E.Min = FConst(Min);
    E.Max = FConst(Max);
    return FInstancedStruct::Make(E);
}
FInstancedStruct VConst(const FVector3f& V)
{
    FNiagaraStatelessExpressionVec3Constant E;
    E.A = V;
    return FInstancedStruct::Make(E);
}
FInstancedStruct VBind(FName N)
{
    FNiagaraStatelessExpressionVec3Binding E;
    E.A = N;
    return FInstancedStruct::Make(E);
}
FInstancedStruct VMul(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionVec3Multiply>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct VAdd(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionVec3Add>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct VSub(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionVec3Subtract>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct V2Const(const FVector2f& V)
{
    FNiagaraStatelessExpressionVec2Constant E;
    E.A = V;
    return FInstancedStruct::Make(E);
}
FInstancedStruct V2Bind(FName N)
{
    FNiagaraStatelessExpressionVec2Binding E;
    E.A = N;
    return FInstancedStruct::Make(E);
}
FInstancedStruct V2Mul(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionVec2Multiply>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct V2Add(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionVec2Add>(MoveTemp(A), MoveTemp(B)); }
FInstancedStruct VScale(FInstancedStruct A, double K) { return VMul(MoveTemp(A), VConst(FVector3f(float(K)))); }

FInstancedStruct FGreater(FInstancedStruct A, FInstancedStruct B) { return Op<FNiagaraStatelessExpressionFloatGreaterThan>(MoveTemp(A), MoveTemp(B)); }
} // namespace

/** A piecewise-linear curve of a bound float, held at its end keys: Base + sum Rise x clamp((x - From) x Scale, 0, 1), a step's term
 *  the comparison x > From (review fixes 3: strict at every float above the key, 0 included). X is the curve's input leaf. */
FInstancedStruct SCFxPiecewiseLinearExpression(const TArray<FVector2D>& Keys, const FInstancedStruct& X)
{
    const SCFxRules::FClampSum Sum = SCFxRules::ClampSum(Keys);
    FInstancedStruct E = FConst(Sum.Base);
    for (const SCFxRules::FClampTerm& T : Sum.Terms)
    {
        FInstancedStruct Term = T.bStep ? FGreater(X, FConst(T.From)) : FClamp(FMul(FAdd(X, FConst(-T.From)), FConst(T.Scale)), 0.0, 1.0);
        E = FAdd(MoveTemp(E), FMul(MoveTemp(Term), FConst(T.Rise)));
    }
    return E;
}

namespace
{
FInstancedStruct PiecewiseLinear(const TArray<FVector2D>& Keys, FName X) { return SCFxPiecewiseLinearExpression(Keys, FBind(X)); }

/** A quantity driven by an EmitterStrength curve: its value at strength 1 and, when it changes over the game's
 *  strengths, the curve as an expression in SC.Strength (PiecewiseLinear). */
struct FStrength
{
    double At1 = 1;
    bool bVaries = false;
    bool bAllZero = false;
    FInstancedStruct Expr;
};

// ---- assets --------------------------------------------------------------------------------------------------------
struct FFeatures
{
    bool Strength = true, Beam = true, Spline = true, Flares = true, Soft = true, Stretch = true, Flow = true, Distortion = true,
         Extras = true, Erosion = true;
    void Parse(const FString& Spec)
    {
        if (Spec.IsEmpty() || Spec == TEXT("all")) return;
        TArray<FString> On;
        Spec.ParseIntoArray(On, TEXT(","));
        auto Is = [&On](const TCHAR* N) { return On.Contains(N); };
        Strength = Is(TEXT("strength")), Beam = Is(TEXT("beam")), Spline = Is(TEXT("spline")), Flares = Is(TEXT("flares"));
        Soft = Is(TEXT("soft")), Stretch = Is(TEXT("stretch")), Flow = Is(TEXT("flow")), Distortion = Is(TEXT("distortion"));
        Extras = Is(TEXT("extras")), Erosion = Is(TEXT("erosion"));
    }
    FString ToString() const
    {
        TArray<FString> On;
        auto Add = [&On](bool b, const TCHAR* N) { if (b) On.Add(N); };
        Add(Strength, TEXT("strength")), Add(Beam, TEXT("beam")), Add(Spline, TEXT("spline")), Add(Flares, TEXT("flares"));
        Add(Soft, TEXT("soft")), Add(Stretch, TEXT("stretch")), Add(Flow, TEXT("flow")), Add(Distortion, TEXT("distortion"));
        Add(Extras, TEXT("extras")), Add(Erosion, TEXT("erosion"));
        return On.IsEmpty() ? FString(TEXT("none")) : FString::Join(On, TEXT(","));
    }
};

enum class EKind : uint8 { Sprite, Light, Distortion, Flare };
enum class ETexRole : uint8 { Color, Flow, Normal };

/** Material instance parameters (header): one instance per distinct set. */
struct FMat
{
    enum class EMaster : uint8 { Additive, Translucent, Distortion } Master = EMaster::Additive;
    FString Texture, Flow, Normal;
    FVector2f Frames = FVector2f(1, 1);
    bool bFlipU = false, bFlipV = false;
    float FlowStrength = 0, GlowGamma = 0, SoftFade = 0, Refraction = 0;
    FString Key() const
    {
        return FString::Printf(TEXT("%d|%s|%s|%s|%g,%g|%d%d|%g|%g|%g|%g"), int32(Master), *Texture, *Flow, *Normal, Frames.X, Frames.Y,
                               bFlipU, bFlipV, FlowStrength, GlowGamma, SoftFade, Refraction);
    }
};

/** One lens-flare element (Data/Libs/Flares LensFlareLibrary FlareItem) as the builder ports it. */
struct FFlareElement
{
    FString Name;
    bool bGlow = true;
    double Gamma = 1, Brightness = 1, Size = 1;
    FVector2D Scale = FVector2D(1, 1);
    FLinearColor Tint = FLinearColor::White;
    FString Texture; // archive path (Ghost elements)
};

struct FBuilder
{
    sc_handle* H = nullptr;
    double SceneUnitNits = 0;
    double LitSceneUnits = 0;   // config: luminance (scene units) given to DiffuseLighting x colour of an unlit sprite
    double FlareWorldScale = 1; // config flare_world_scale: the viewer's stand-in for CryEngine's screen-space flare size
    TArray<double> Knots = {1}; // the game's strengths (config "strength" record): where a curve is checked for variation
    double BeamRangeM = 0;      // the strength record's beam range: the beam emitters' bounds
    TArray<TSharedPtr<FJsonValue>> CurrentEmitters; // the effect being built
    TOptional<double> GuideCurvature;               // its beam spline's curvature (first guided emitter)
    FString GuideName;
    double RefractionScale = 1; // -RefractionScale=<k>: a verification aid that exaggerates refraction (default 1)
    int32 LoopCapacity = SpawnInfoCapacity(); // -LoopCapacity=<n>: a verification aid, loops sized for another engine's capacity
    FFeatures F;
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

    /** The strength record (config "strength": {record, type}): the distinct values in 0..1 of its numeric fields that
     *  name strengths (blend coefficients and scales excluded), and its range field for the beam bounds. */
    void LoadStrengths(const FJson& Config)
    {
        const FString Record = Str(Config, TEXT("strength.record")), Type = Str(Config, TEXT("strength.type"));
        if (Record.IsEmpty()) return;
        char* Text = nullptr;
        if (sc_datacore_record(H, TCHAR_TO_UTF8(*Record), &Text) != SC_STATUS_OK)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR strength record %s: %s"), *Record, UTF8_TO_TCHAR(sc_last_error(H)));
            ++Errors;
            return;
        }
        const TSharedPtr<FJsonValue> Value = [&] {
            TSharedPtr<FJsonValue> V;
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(UTF8_TO_TCHAR(Text)), V);
            return V;
        }();
        sc_free_string(Text);
        FJson Found, Owner;
        TFunction<void(const TSharedPtr<FJsonValue>&, const FJson&)> Walk = [&](const TSharedPtr<FJsonValue>& V, const FJson& Parent) {
            if (Found || !V.IsValid()) return;
            if (V->Type == EJson::Object)
            {
                const FJson O = V->AsObject();
                if (Str(O, TEXT("__type")) == Type)
                {
                    Found = O, Owner = Parent;
                    return;
                }
                for (const auto& Pair : O->Values) Walk(Pair.Value, O);
            }
            else if (V->Type == EJson::Array)
                for (const auto& E : V->AsArray()) Walk(E, Parent);
        };
        Walk(Value, nullptr);
        if (!Found)
        {
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR strength record %s has no %s"), *Record, *Type);
            ++Errors;
            return;
        }
        TArray<double> Values;
        TArray<FString> Names;
        for (const auto& Pair : Found->Values)
        {
            const FString Key(Pair.Key.ToView());
            if (Pair.Value->Type != EJson::Number || Key.Contains(TEXT("Blend")) || Key.Contains(TEXT("Scale"))) continue;
            const double V = Pair.Value->AsNumber();
            if (V < 0 || V > 1) continue;
            Names.Add(FString::Printf(TEXT("%s=%g"), *Key, V));
            bool bSeen = false;
            for (double K : Values) bSeen |= FMath::IsNearlyEqual(K, V, 1e-4);
            if (!bSeen) Values.Add(V);
        }
        Values.Sort();
        if (!Values.IsEmpty()) Knots = Values;
        const FString RangeField = Str(Config, TEXT("strength.range_field"));
        if (Owner && !RangeField.IsEmpty()) BeamRangeM = Num(Owner, RangeField);
        TArray<FString> K;
        for (double V : Knots) K.Add(FString::Printf(TEXT("%g"), V));
        UE_LOG(LogSCFx, Display, TEXT("SC_FX strength record=%s %s fields=%s knots=%s blend=%g range_m=%g"), *Record, *Type, *Join(Names),
               *Join(K), Num(Found, TEXT("beamEffectBlendCoeff")), BeamRangeM);
    }

    /** A curve's channel (x Scale) over the game's strengths (header). */
    FStrength Strength(const FCurve& Curve, int32 Channel = 0, double Scale = 1.0) const
    {
        FStrength S;
        S.At1 = Eval(Curve, 1.0, Channel) * Scale;
        // Classified over the configured strengths and every authored key in [0, 1] (E1-6 review: a curve peaking between the
        // knots is neither constant nor invisible), both sides of a step included (review fixes 3: each key's own value).
        const TArray<double> Y = SCFxRules::ClassificationValues(Curve, Knots, Channel, Scale);
        S.bAllZero = !Y.ContainsByPredicate([](double V) { return V > 0; });
        for (double V : Y) S.bVaries |= !FMath::IsNearlyEqual(V, Y[0], 1e-6);
        if (!F.Strength || Curve.IsEmpty() || !S.bVaries)
        {
            S.bVaries = false;
            S.bAllZero = S.At1 <= 0;
            return S;
        }
        // The curve itself (piecewise linear over strength, held at its end keys), evaluated per instance by the engine.
        // Each key's own value of the channel (as Eval reads it, so a step of two keys at one strength stays a step).
        TArray<FVector2D> Keys;
        for (const TArray<double>& K : Curve) Keys.Add(FVector2D(K[0], (K.IsValidIndex(Channel + 1) ? K[Channel + 1] : K.Last()) * Scale));
        S.Expr = PiecewiseLinear(Keys, UserStrength);
        return S;
    }
    static void Apply(FNiagaraDistributionFloat& D, const FStrength& S)
    {
        if (S.bVaries)
        {
            D.Mode = ENiagaraDistributionMode::Expression;
            D.ParameterExpression = S.Expr;
        }
        else D.InitConstant(float(S.At1));
    }
    static void Apply(FNiagaraDistributionRangeFloat& D, const FStrength& S)
    {
        if (S.bVaries)
        {
            D.Mode = ENiagaraDistributionMode::Expression;
            D.ParameterExpression = S.Expr;
        }
        else SetRange(D, S.At1, S.At1);
    }

    /** An archive .dds as a persistent texture: split mips merged by the bridge (sc_p4k_read_dds), the top mip decoded to
     *  an editable BGRA8 source (sc_texels_decode_bgra8, as the WP-C1 cook does). Colour textures take sRGB from the DXGI
     *  format; flow vectors and normals are data, always linear. */
    UTexture2D* Texture(const FString& ArchivePath, ETexRole Role = ETexRole::Color)
    {
        const FString Key = FString::Printf(TEXT("%d|%s"), int32(Role), *ArchivePath);
        if (UTexture2D** Found = Textures.Find(Key)) return *Found;
        UTexture2D*& Slot = Textures.Add(Key, nullptr);
        sc_blob Blob{};
        if (sc_p4k_read_dds(H, TCHAR_TO_UTF8(*ArchivePath), 0, &Blob) != SC_STATUS_OK)
        {
            const FString Why = UTF8_TO_TCHAR(sc_last_error(H));
            if (UTexture2D* Plain = UncompressedTexture(ArchivePath, Role)) return Slot = Plain;
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR texture %s: %s"), *ArchivePath, *Why);
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
        return Slot = SaveTexture(ArchivePath, Role, int32(Info.width), int32(Info.height), Bgra, bSRGB, FString::Printf(TEXT("dxgi=%u"), Info.dxgi_format));
    }

    /** An editable BGRA8 texture asset under /Game/SC/FX/Textures/<archive path>. */
    UTexture2D* SaveTexture(const FString& ArchivePath, ETexRole Role, int32 W, int32 Hgt, const TArray64<uint8>& Bgra, bool bSRGB, const FString& Format)
    {
        if (Role != ETexRole::Color) bSRGB = false;
        const FString Name = SafeSegment(FPaths::GetBaseFilename(ArchivePath)) + (Role == ETexRole::Color ? TEXT("") : Role == ETexRole::Flow ? TEXT("_flow") : TEXT("_normal"));
        const FString LongName = FString(Root) / TEXT("Textures") / SafePath(FPaths::GetPath(ArchivePath)) / Name;
        UPackage* Package = PackageFor(LongName);
        UTexture2D* Tex = FindObject<UTexture2D>(Package, *Name);
        if (!Tex) Tex = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
        Tex->PreEditChange(nullptr);
        Tex->Source.Init(W, Hgt, 1, 1, TSF_BGRA8, Bgra.GetData());
        Tex->SRGB = bSRGB;
        Tex->CompressionSettings = Role == ETexRole::Normal ? TC_Normalmap : Role == ETexRole::Flow ? TC_VectorDisplacementmap : TC_Default;
        Tex->LODGroup = TEXTUREGROUP_Effects;
        Tex->MipGenSettings = TMGS_FromTextureGroup;
        Tex->PostEditChange();
        if (!Save(Tex)) return nullptr;
        UE_LOG(LogSCFx, Display, TEXT("SC_FX_TEXTURE %s -> %s %dx%d %s srgb=%d"), *ArchivePath, *Tex->GetPathName(), W, Hgt, *Format, bSRGB ? 1 : 0);
        return Tex;
    }

    /** A legacy uncompressed 32-bit RGB(A) DDS (DDPF_RGB with channel masks, e.g. the flow-vector maps), which the
     *  package DDS writer does not take: its top level read from the raw archive file (sc_p4k_read). */
    UTexture2D* UncompressedTexture(const FString& ArchivePath, ETexRole Role)
    {
        sc_blob Blob{};
        if (sc_p4k_read(H, TCHAR_TO_UTF8(*ArchivePath), &Blob) != SC_STATUS_OK) return nullptr;
        TArray<uint8> Raw(Blob.data, int32(Blob.len));
        sc_free_blob(&Blob);
        auto U32 = [&Raw](int32 At) { return At + 4 <= Raw.Num() ? uint32(Raw[At]) | uint32(Raw[At + 1]) << 8 | uint32(Raw[At + 2]) << 16 | uint32(Raw[At + 3]) << 24 : 0u; };
        constexpr uint32 DdpfRgb = 0x40, DdpfFourCC = 0x4;
        if (Raw.Num() < 128 || FMemory::Memcmp(Raw.GetData(), "DDS ", 4) != 0) return nullptr;
        const int32 Hgt = int32(U32(12)), W = int32(U32(16));
        const uint32 Flags = U32(80), Bits = U32(88);
        if (!(Flags & DdpfRgb) || (Flags & DdpfFourCC) || Bits != 32 || int64(W) * Hgt * 4 + 128 > Raw.Num()) return nullptr;
        const uint32 Mask[4] = {U32(92), U32(96), U32(100), U32(104)}; // R G B A
        auto Channel = [](uint32 Px, uint32 M) -> uint8 {
            if (!M) return 255;
            const uint32 Shift = FMath::CountTrailingZeros(M);
            return uint8(((Px & M) >> Shift) * 255 / (M >> Shift));
        };
        TArray64<uint8> Bgra;
        Bgra.SetNumUninitialized(int64(W) * Hgt * 4);
        for (int64 i = 0; i < int64(W) * Hgt; ++i)
        {
            const uint32 Px = U32(int32(128 + i * 4));
            Bgra[i * 4 + 0] = Channel(Px, Mask[2]);
            Bgra[i * 4 + 1] = Channel(Px, Mask[1]);
            Bgra[i * 4 + 2] = Channel(Px, Mask[0]);
            Bgra[i * 4 + 3] = Channel(Px, Mask[3]);
        }
        return SaveTexture(ArchivePath, Role, W, Hgt, Bgra, false, TEXT("uncompressed-rgb32"));
    }

    /** Constant instance of a particle master for one parameter set; EmissiveScale = the game's scene unit in cd/m2. */
    UMaterialInterface* Material(const FMat& M)
    {
        const FString Key = M.Key();
        if (UMaterialInterface** Found = Materials.Find(Key)) return *Found;
        UMaterialInterface*& Slot = Materials.Add(Key, nullptr);
        const TCHAR* MasterPath = M.Master == FMat::EMaster::Additive ? AdditiveMaster : M.Master == FMat::EMaster::Translucent ? TranslucentMaster : DistortionMaster;
        UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, MasterPath);
        UTexture2D* Tex = M.Texture.IsEmpty() ? nullptr : Texture(M.Texture);
        UTexture2D* Flow = M.Flow.IsEmpty() ? nullptr : Texture(M.Flow, ETexRole::Flow);
        UTexture2D* Normal = M.Normal.IsEmpty() ? nullptr : Texture(M.Normal, ETexRole::Normal);
        if (!Master || (!M.Texture.IsEmpty() && !Tex))
        {
            if (!Master) UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR missing master %s (run viewer/scripts/create_materials.py)"), MasterPath);
            ++Errors;
            return nullptr;
        }
        const TCHAR* Kind = M.Master == FMat::EMaster::Additive ? TEXT("Additive") : M.Master == FMat::EMaster::Translucent ? TEXT("Translucent") : TEXT("Distortion");
        const FString Name = FString::Printf(TEXT("MI_SC_Particle_%s_%s_%s"), Kind, Tex ? *Tex->GetName() : TEXT("Glow"), *FMD5::HashAnsiString(*Key).Left(8));
        UPackage* Package = PackageFor(FString(Root) / TEXT("Materials") / Name);
        UMaterialInstanceConstant* MI = FindObject<UMaterialInstanceConstant>(Package, *Name);
        if (!MI) MI = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MI->PreEditChange(nullptr);
        MI->SetParentEditorOnly(Master);
        MI->ClearParameterValuesEditorOnly();
        if (Tex) MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("SpriteTexture")), Tex);
        if (Flow) MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FlowTexture")), Flow);
        if (Normal) MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("NormalTexture")), Normal);
        auto Scalar = [MI](const TCHAR* N, double V) { MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(N), float(V)); };
        Scalar(TEXT("EmissiveScale"), SceneUnitNits);
        Scalar(TEXT("FramesU"), M.Frames.X);
        Scalar(TEXT("FramesV"), M.Frames.Y);
        Scalar(TEXT("FlipU"), M.bFlipU ? 1 : 0);
        Scalar(TEXT("FlipV"), M.bFlipV ? 1 : 0);
        Scalar(TEXT("FlowStrength"), Flow ? M.FlowStrength : 0);
        Scalar(TEXT("GlowGamma"), M.GlowGamma);
        Scalar(TEXT("SoftFade"), M.SoftFade);
        Scalar(TEXT("RefractionStrength"), M.Refraction);
        Scalar(TEXT("UseNormalMap"), Normal ? 1 : 0);
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
        bool bBurst = false, bBeam = false;
        double AmountMin = 0, AmountMax = 0, RateMin = 0, RateMax = 0, Delay = 0, Duration = 1, PerCm = 0;
        ENiagaraLoopBehavior Loop = ENiagaraLoopBehavior::Infinite;
    };

    /** Spawn rules (header). Parent is the game parent emitter (null for the first). */
    FSpawn Spawn(const FJson& E, const FJson& Parent, bool bBeam, FLog& L) const
    {
        FSpawn S;
        const double Count = Num(E, TEXT("spawn.count")) * (F.Strength ? 1.0 : Eval(Curve(E, TEXT("spawn.count_strength_curve")), 1.0));
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
            // A pulse's bursts alive at once are spawn infos too; the period is the data's, so an overflow is reported.
            if (Pulse > 0 && SCFxRules::LiveBursts(Life, Pulse) > LoopCapacity)
                L.Approx.Add(FString::Printf(TEXT("pulse %.3gs: %d bursts alive > %d spawn infos (the newest are not drawn)"), Pulse, SCFxRules::LiveBursts(Life, Pulse), LoopCapacity));
            return S;
        }
        double Alive = Count * ParentCount;
        if (Mode == TEXT("per_distance"))
        {
            const double PerM = Num(E, TEXT("spawn.per_distance_per_m")) * ParentCount;
            if (bBeam && F.Beam)
            {
                // Per metre of beam at any length: the rate follows SC.BeamLength (Spawn builds the expression).
                S.bBeam = true;
                S.PerCm = PerM / Cm / Life;
                L.Mapped.Add(TEXT("spawn.per_distance(SC.BeamLength)"));
                if (Count > 0) L.Skipped.Add(FString::Printf(TEXT("Count %g (cap; EmitPerDistance holds at any length)"), Count));
            }
            else
            {
                Alive = FMath::Min(PerM, Alive > 0 ? Alive : DBL_MAX);
                L.Approx.Add(TEXT("per_distance:EmitPerDistance x 1 m alive at once"));
            }
        }
        const double Window = EmitterLife > 0 ? FMath::Min(Life, EmitterLife) : Life;
        S.RateMax = Alive / Window;
        S.RateMin = S.RateMax * (1 - CountRandom);
        S.Loop = EmitterLife > 0 ? ENiagaraLoopBehavior::Once : ENiagaraLoopBehavior::Infinite;
        // Infinite loops re-read bound / expression rates once per loop: keep them short so strength and length follow.
        S.Duration = EmitterLife > 0 ? EmitterLife : (F.Strength || F.Beam ? FMath::Min(Life, FollowLoopSeconds) : Life);
        // Every loop whose particles still live is a spawn info of its own on the GPU, and a lightweight emitter draws at
        // most NiagaraStateless::MaxGpuSpawnInfos of them (UE 5.9: 8, the fork: 32; the rest are dropped: "overflowed
        // spawn infos", flicker): the loop is stretched only where 1 + ceil(lifetime / loop) would not fit.
        if (S.Loop == ENiagaraLoopBehavior::Infinite)
        {
            const double Wanted = S.Duration;
            S.Duration = SCFxRules::LoopDuration(Wanted, Life, LoopCapacity);
            if (S.Duration > Wanted)
                L.Approx.Add(FString::Printf(TEXT("loop %.3gs -> %.3gs (lifetime %.3gs, %d spawn infos)"), Wanted, S.Duration, Life, LoopCapacity));
            // Review fixes 3 (Astra A8): with one spawn info no loop fits (consecutive intervals overlap while particles live); the
            // wanted loop is kept and reported (the engine draws the newest that fit).
            if (!SCFxRules::LoopsFit(Life, S.Duration, LoopCapacity))
                L.Approx.Add(FString::Printf(TEXT("loop %.3gs: %d live loops > %d spawn info(s), cannot fit (the newest are not drawn)"), S.Duration, SCFxRules::LiveLoops(Life, S.Duration), LoopCapacity));
            L.Mapped.Add(FString::Printf(TEXT("loop=%.3gs(live_loops=%d/%d)"), S.Duration, SCFxRules::LiveLoops(Life, S.Duration), LoopCapacity));
        }
        if (Mode != TEXT("per_distance")) L.Mapped.Add(TEXT("spawn.continuous"));
        if (S.Delay > 0) L.Mapped.Add(TEXT("spawn.delay"));
        return S;
    }

    template <typename T> static T* Module(UNiagaraStatelessEmitter* E) { return const_cast<T*>(E->GetModule<T>()); }

    /** Configure one lightweight emitter from a game emitter (Kind: the sprite, its light, distortion or flare twin).
     *  Returns false when it ends up with nothing to draw; bInvisible when it draws nothing at any game strength. */
    bool Configure(UNiagaraStatelessEmitter* Em, const FJson& E, const FJson& Parent, EKind Kind, const FFlareElement* Flare, FLog& L,
                   bool& bInvisible)
    {
        // Every template module off first; each rule below enables what it uses.
        for (UNiagaraStatelessModule* M : Em->GetModules())
            if (M && M->CanDisableModule()) M->SetIsModuleEnabled(false);
        Em->Modify();

        const bool bSpline = Has(E, TEXT("spline"));
        const bool bBeam = bSpline && F.Spline;
        const FSpawn S = Spawn(E, Parent, bSpline, L);
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
        State.bEnableVisibilityCulling = false;
        const double CameraMax = RawNumber(E, TEXT("CameraMaxDistance")).Get(0);
        State.bEnableDistanceCulling = F.Extras && CameraMax > 0;
        State.bMaxDistanceEnabled = State.bEnableDistanceCulling;
        State.MaxDistance = float(CameraMax * Cm);
        if (State.bEnableDistanceCulling) L.Mapped.AddUnique(TEXT("CameraMaxDistance"));
        Em->SetEmitterState(State);
        while (Em->GetNumSpawnInfos() > 0)
        {
            FGuid Id = Em->GetSpawnInfoByIndex(0)->SourceId;
            Em->RemoveSpawnInfoBySourceId(Id);
        }
        FNiagaraStatelessSpawnInfo& Info = Em->AddSpawnInfo();
        Info.bEnabled = true;
        const FStrength CountStrength = Strength(Curve(E, TEXT("spawn.count_strength_curve")));
        if (S.bBurst)
        {
            Info.Type = ENiagaraStatelessSpawnInfoType::Burst;
            Info.SpawnTime = float(S.Delay);
            const double K = CountStrength.At1;
            Info.Amount.Mode = S.AmountMin == S.AmountMax ? ENiagaraDistributionMode::UniformConstant : ENiagaraDistributionMode::UniformRange;
            Info.Amount.Min = int32(FMath::RoundToDouble(S.AmountMin * K));
            Info.Amount.Max = int32(FMath::RoundToDouble(S.AmountMax * K));
            if (CountStrength.bVaries) L.Approx.AddUnique(TEXT("count strength@1 (burst)"));
        }
        else
        {
            Info.Type = ENiagaraStatelessSpawnInfoType::Rate;
            // Spawn rates are expressions where they follow a parameter (the fork evaluates a spawn-info expression per
            // loop, built against the emitter's bindings: WP-E1 E1-6; UE 5.9 evaluated it unbuilt): a beam's rate is its
            // particles per cm per second x SC.BeamLength, a constant rate with a count strength curve the rate x the
            // curve. An expression has no random range, so a random rate keeps the curve at strength 1.
            if (S.bBeam)
            {
                Info.Rate.Mode = ENiagaraDistributionMode::Expression;
                Info.Rate.ParameterExpression = FMul(FConst(S.PerCm), FBind(UserBeamLength));
                L.Mapped.AddUnique(TEXT("SC.BeamLength"));
                if (CountStrength.bVaries) L.Approx.AddUnique(TEXT("count.strength_curve@1 (beam)"));
            }
            else if (CountStrength.bVaries && FMath::IsNearlyEqual(S.RateMin, S.RateMax))
            {
                Info.Rate.Mode = ENiagaraDistributionMode::Expression;
                Info.Rate.ParameterExpression = FMul(FConst(S.RateMax), CountStrength.Expr);
                L.Mapped.AddUnique(TEXT("count.strength_curve(SC.Strength)"));
            }
            else
            {
                if (CountStrength.bVaries) L.Approx.AddUnique(TEXT("count.strength_curve@1 (random rate)"));
                SetRange(Info.Rate, S.RateMin * CountStrength.At1, S.RateMax * CountStrength.At1);
            }
        }

        // Lifetime (strength curve baked at 1).
        auto* Init = Module<UNiagaraStatelessModule_InitializeParticle>(Em);
        const double Life = Num(E, TEXT("lifetime.value"), 1) * Eval(Curve(E, TEXT("lifetime.strength_curve")), 1.0);
        const double LifeRandom = FMath::Clamp(Num(E, TEXT("lifetime.random")), 0.0, 1.0);
        SetRange(Init->LifetimeDistribution, Life * (1 - LifeRandom), Life);
        Init->LifetimeDistribution.UpdateValuesFromDistribution();
        L.Mapped.Add(TEXT("lifetime"));
        if (Has(E, TEXT("lifetime.strength_curve"))) L.Approx.AddUnique(TEXT("lifetime.strength_curve@1"));

        // Shape: offsets as a box; a guided beam starts at the emitter.
        auto* Shape = Module<UNiagaraStatelessModule_ShapeLocation>(Em);
        Shape->SetIsModuleEnabled(true);
        Shape->ShapePrimitive = ENSM_ShapePrimitive::Box;
        Shape->bBoxSurfaceOnly = false;
        Shape->CoordinateSpace = ENiagaraCoordinateSpace::Simulation;
        const FVector3f Offset = Basis(Nums(E, TEXT("position_offset_m")));
        FVector3f Box = Basis(Nums(E, TEXT("random_offset_m"))).GetAbs() * 2.f;
        FVector3f Centre = Offset;
        const TArray<double> Amp = Nums(E, TEXT("spline.noise_amplitude_xyz"));
        const double AmpCm = Cm * (Amp.IsEmpty() ? 0.0 : FMath::Max(Amp));
        if (bSpline)
        {
            if (!Offset.IsNearlyZero()) L.Approx.Add(TEXT("spline:position_offset dropped (spline start only)"));
            Centre = FVector3f::ZeroVector;
            if (!bBeam)
            {
                // Without the guided path: a straight box along +X 0..100 cm (scale the component to the beam).
                Box.X = float(DefaultBeamCm);
                Box.Y = FMath::Max(Box.Y, float(2 * Cm * (Amp.IsValidIndex(0) ? Amp[0] : 0)));
                Box.Z = FMath::Max(Box.Z, float(2 * Cm * (Amp.IsValidIndex(1) ? Amp[1] : 0)));
                Centre.X = float(DefaultBeamCm / 2);
                L.Approx.Add(TEXT("spline:box along +X 0..100cm, noise amplitude as box width"));
            }
        }
        SetVec3(Shape->BoxSize, Box);
        SetPosition(Shape->TransformOffset, Centre);
        L.Mapped.Add(TEXT("position_offset_m"));
        L.Mapped.Add(TEXT("random_offset_m"));

        // Motion: the guided beam path, or a cone about +X with forces.
        const FStrength SpeedStrength = Strength(Curve(E, TEXT("speed.strength_curve")));
        const double Speed = Num(E, TEXT("speed.value"));
        const double SpeedRandom = FMath::Clamp(Num(E, TEXT("speed.random")), 0.0, 1.0);
        double BoundsReach = 0, TypicalSpeedCm = Speed * SpeedStrength.At1 * Cm;
        if (bBeam)
        {
            const double T = FMath::Max(Life, 1e-3);
            // Every guided emitter of an effect rides the one beam spline the weapon evaluates (emitter -> target); the
            // effect's first guided emitter defines its curvature, so all layers coincide end to end.
            const double Own = FMath::Clamp(Num(E, TEXT("spline.curvature")), 0.0, 1.0);
            const double Curv = GuideCurvature.Get(Own);
            if (!FMath::IsNearlyEqual(Own, Curv))
                L.Approx.AddUnique(FString::Printf(TEXT("spline:curvature %g -> %g (the effect's beam spline, %s)"), Own, Curv, *GuideName));
            auto* Vel = Module<UNiagaraStatelessModule_AddVelocity>(Em);
            Vel->SetIsModuleEnabled(true);
            Vel->VelocityType = ENSM_VelocityType::Linear;
            Vel->CoordinateSpace = ENiagaraCoordinateSpace::Local;
            Vel->LinearVelocityDistribution.Mode = ENiagaraDistributionMode::Expression;
            Vel->LinearVelocityDistribution.ParameterExpression =
                VAdd(VScale(VBind(UserBeamTangent), Curv / T), VScale(VBind(UserBeamEnd), (1 - Curv) / T));
            Vel->LinearVelocityScale.InitConstant(1.f);
            auto* Acc = Module<UNiagaraStatelessModule_AccelerationForce>(Em);
            Acc->SetIsModuleEnabled(true);
            Acc->CoordinateSpace = ENiagaraCoordinateSpace::Local;
            Acc->AccelerationDistribution.Mode = ENiagaraDistributionMode::Expression;
            Acc->AccelerationDistribution.ParameterExpression = VScale(VSub(VBind(UserBeamEnd), VBind(UserBeamTangent)), 2 * Curv / (T * T));
            L.Mapped.Add(TEXT("spline.curvature"));
            L.Mapped.Add(TEXT("spline(travel start->SC.BeamEnd over lifetime)"));
            L.Skipped.Add(FString::Printf(TEXT("speed %g m/s (travel is beam length / lifetime)"), Speed));
            if (Bool(E, TEXT("spline.noise_enabled")) && AmpCm > 0)
            {
                // Curl noise of the noise amplitude: the stateless field moves a particle ~ strength x lifetime (cm) along
                // its path; its frequency is one over the shortest spatial wavelength (SplineNoise SpatialWavelength, m).
                auto* Noise = Module<UNiagaraStatelessModule_CurlNoiseForce>(Em);
                Noise->SetIsModuleEnabled(true);
                Noise->NoiseStrength = float(AmpCm / T);
                double Wave = 0;
                const FString SplineRaw = Str(E, TEXT("spline.raw"));
                const int32 At = SplineRaw.Find(TEXT("SpatialWavelength=("));
                if (At != INDEX_NONE)
                {
                    const FString Inner = SplineRaw.Mid(At + 19, SplineRaw.Mid(At + 19).Find(TEXT(")")));
                    TArray<FString> Parts;
                    Inner.ParseIntoArray(Parts, TEXT(","));
                    for (const FString& P : Parts)
                    {
                        const double W = FCString::Atod(*P.Mid(P.Find(TEXT("=")) + 1));
                        if (W > 0) Wave = Wave > 0 ? FMath::Min(Wave, W) : W;
                    }
                }
                // UE's frequency normalisation (fx.NiagaraStateless.SolveVelocities.Noise.FrequencyNormalize = 50 per cm).
                Noise->NoiseFrequency = float(Wave > 0 ? 50.0 / (Wave * Cm) : 1.0);
                Noise->bBiasNoiseFieldEnabled = false;
                L.Mapped.Add(TEXT("spline.noise(amplitude, wavelength)"));
                L.Approx.Add(TEXT("spline.noise:static curl field (NoiseVel scroll, noise textures, damping, noise strength curve skipped)"));
            }
            TypicalSpeedCm = (BeamRangeM > 0 ? BeamRangeM * Cm : DefaultBeamCm) / T;
        }
        else if (Speed > 0 && !bSpline)
        {
            auto* Vel = Module<UNiagaraStatelessModule_AddVelocity>(Em);
            Vel->SetIsModuleEnabled(true);
            Vel->VelocityType = ENSM_VelocityType::InCone;
            double SpeedMax = Speed;
            // Inherited parent velocity (Spawn=FromParentBirth): the parent's speed x InheritVelocity widens the speed range.
            const double Inherit = Num(E, TEXT("inherit_velocity"));
            if (F.Extras && Inherit != 0 && Parent && !Str(E, TEXT("spawn.spawn_on_parent")).IsEmpty())
            {
                SpeedMax += FMath::Abs(Inherit) * Num(Parent, TEXT("speed.value"));
                L.Approx.Add(TEXT("inherit_velocity:parent speed added to the speed range"));
            }
            else if (Inherit != 0)
                L.Skipped.Add(TEXT("inherit_velocity"));
            SetRange(Vel->ConeVelocityDistribution, Speed * (1 - SpeedRandom) * Cm, SpeedMax * Cm);
            Vel->ConeVelocityDistribution.UpdateValuesFromDistribution();
            Apply(Vel->ConeVelocityScale, SpeedStrength);
            if (SpeedStrength.bVaries) L.Mapped.Add(TEXT("speed.strength_curve"));
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
            BoundsReach += SpeedMax * SpeedStrength.At1 * Life;
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
        if (Has(E, TEXT("size.strength_curve"))) L.Approx.AddUnique(TEXT("size.strength_curve@1"));
        const double Reach = (Offset.Size() / Cm) + (Box.Size() / Cm) + BoundsReach + 2 * Size + 0.5;
        const double BeamReach = bBeam ? FMath::Max(BeamRangeM, DefaultBeamCm / Cm) : (bSpline ? DefaultBeamCm / Cm : 0.0);
        if (FStructProperty* P = FindFProperty<FStructProperty>(UNiagaraStatelessEmitter::StaticClass(), TEXT("FixedBounds")))
            *P->ContainerPtrToValuePtr<FBox>(Em) = FBox(FVector(-(Reach + BeamReach) * Cm), FVector((Reach + BeamReach) * Cm));

        TArray<UNiagaraRendererProperties*> Old = Em->GetRenderers();
        for (UNiagaraRendererProperties* R : Old) Em->RemoveRenderer(R, FGuid());

        // Colour: the emitter's colour without its strength curve (Dynamic Material Parameter 0 carries that).
        const TArray<double> Rgb0 = Nums(E, TEXT("color.rgb"));
        const FCurve ColorStrength = Curve(E, TEXT("color.strength_curve"));
        FStrength ColorS[3];
        FLinearColor Rgb(1, 1, 1, 1);
        for (int32 c = 0; c < 3; ++c)
        {
            ColorS[c] = Strength(ColorStrength, c);
            Rgb.Component(c) = float(Rgb0.IsValidIndex(c) ? Rgb0[c] : 1.0);
        }
        const bool bColorVaries = ColorS[0].bVaries || ColorS[1].bVaries || ColorS[2].bVaries;
        const double ColorRandom = FMath::Clamp(Num(E, TEXT("color.random")), 0.0, 1.0);
        if (ColorRandom > 0) L.Mapped.AddUnique(TEXT("color.random"));
        if (Bool(E, TEXT("color.random_hue"))) L.Skipped.AddUnique(TEXT("color.random_hue"));

        if (Kind == EKind::Light)
        {
            const FJson Light = Obj(E, TEXT("light"));
            const double IntensityRandom = FMath::Clamp(Num(Light, TEXT("random")), 0.0, 1.0);
            const double Candela = UE_DOUBLE_PI * SceneUnitNits * Num(Light, TEXT("intensity"), 1);
            // Hue at the reference strength (1, or where the colour is brightest when it is black at 1: review fixes 3, Terra T4);
            // intensity x colour luminance relative to it over the strengths -> the light's diffuse/specular scale.
            const SCFxRules::FLightColourReference Reference = SCFxRules::LightColourReference(ColorStrength, Rgb, Knots);
            FLinearColor Max = Reference.Rgb;
            Max.A = 1.f;
            const double LumRef = Reference.Luminance;
            if (LumRef > 0) Max *= float(Candela * CandelaToUE);
            Max.A = 1.f;
            FLinearColor Min = Max * float(1 - IntensityRandom);
            Min.A = 1.f;
            SetColorRange(Init->ColorDistribution, Min, Max);
            // E1-6 review: the light's intensity curve and its colour's luminance (relative to strength 1) are two exact piecewise-linear
            // curves in SC.Strength (the colour's at its own keys: a weighted sum of the channels), multiplied; resampling their product
            // at the knots lost the authored breaks and was wrong between them. Review fixes 3: the luminance keys from each colour key's
            // own channels (a step stays a step), relative to the reference.
            const FStrength IntensityS = Strength(Curve(Light, TEXT("strength_curve")));
            FCurve ColorLuminance;
            for (const FVector2D& K : SCFxRules::RelativeLuminanceKeys(ColorStrength, Rgb, LumRef)) ColorLuminance.Add({K.X, K.Y});
            const FStrength ColorLumS = Strength(ColorLuminance);
            FStrength LightS;
            LightS.At1 = IntensityS.At1 * ColorLumS.At1;
            LightS.bVaries = IntensityS.bVaries || ColorLumS.bVaries;
            LightS.bAllZero = IntensityS.bAllZero || ColorLumS.bAllZero;
            if (LightS.bVaries)
                LightS.Expr = FMul(IntensityS.bVaries ? IntensityS.Expr : FConst(IntensityS.At1), ColorLumS.bVaries ? ColorLumS.Expr : FConst(ColorLumS.At1));
            bInvisible = Candela <= 0 || LumRef <= 0 && !LightS.bVaries;
            const FCurve AgeCurve = Curve(Light, TEXT("age_curve"));
            if (!AgeCurve.IsEmpty())
            {
                auto* ScaleColor = Module<UNiagaraStatelessModule_ScaleColor>(Em);
                ScaleColor->SetIsModuleEnabled(true);
                FCurve Rgb3;
                for (const auto& K : AgeCurve) Rgb3.Add({K[0], K[1], K[1], K[1]});
                SetColorCurve(ScaleColor->ScaleDistribution, Rgb3, {});
            }
            auto* Attr = Module<UNiagaraStatelessModule_LightAttributes>(Em);
            Attr->SetIsModuleEnabled(true);
            Attr->bApplyRadius = true;
            const double Radius = Num(Light, TEXT("radius_m")) * Cm;
            const FCurve RadiusCurve = Curve(Light, TEXT("radius_age_curve"));
            if (RadiusCurve.IsEmpty()) Attr->Radius.InitConstant(float(Radius));
            else
            {
                Attr->Radius.Mode = ENiagaraDistributionMode::UniformCurve;
                Attr->Radius.ChannelCurves = {RichCurve(RadiusCurve, 0, Radius)};
                Attr->Radius.UpdateValuesFromDistribution();
            }
            Attr->bApplyDiffuseScale = Attr->bApplySpecularScale = LightS.bVaries || !FMath::IsNearlyEqual(LightS.At1, 1.0);
            Apply(Attr->DiffuseScale, LightS);
            Apply(Attr->SpecularScale, LightS);
            if (LightS.bVaries) L.Mapped.Add(TEXT("light.strength_curve(SC.Strength)"));
            if (bColorVaries) L.Approx.AddUnique(FString::Printf(TEXT("light hue at strength %g"), Reference.Strength));
            auto* R = NewObject<UNiagaraLightRendererProperties>(Em, NAME_None, RF_Transactional);
            R->bUseInverseSquaredFalloff = true;
            R->bAffectsTranslucency = true;
            R->bAlphaScalesBrightness = false;
            R->RadiusScale = 1.f;
            Em->AddRenderer(R, FGuid());
            L.Mapped.Append({TEXT("light.intensity"), TEXT("light.radius_m"), TEXT("color.rgb")});
            if (!AgeCurve.IsEmpty()) L.Mapped.Add(TEXT("light.age_curve"));
            if (IntensityRandom > 0) L.Mapped.Add(TEXT("light.random(per light)"));
            if (Bool(Light, TEXT("soft_bulb"))) L.Approx.AddUnique(TEXT("light.soft_bulb:point light"));
            if (Bool(Light, TEXT("affects_this_area_only"))) L.Skipped.AddUnique(TEXT("light.affects_this_area_only"));
            return true;
        }

        // Sprite-like twins: colour x luminance (scene units), alpha, size, curves, facing, flipbook, material.
        const double Emissive = Num(E, TEXT("emissive_lighting"));
        double Luminance = Emissive + Num(E, TEXT("diffuse_lighting")) * LitSceneUnits;
        if (Emissive <= 0 && Kind == EKind::Sprite) L.Approx.AddUnique(FString::Printf(TEXT("diffuse_lighting:unlit at %g scene units"), LitSceneUnits));
        double Alpha = Num(E, TEXT("alpha.value"), 1);
        const double AlphaRandom = FMath::Clamp(Num(E, TEXT("alpha.random")), 0.0, 1.0);
        FCurve AlphaStrengthCurve = Curve(E, TEXT("alpha.strength_curve"));
        FCurve AlphaAge = Curve(E, TEXT("alpha.age_curve"));
        FCurve ColorAge = Curve(E, TEXT("color.age_curve"));
        FLinearColor Tint = Rgb;
        double SizeX = 2 * Size * Cm, SizeY = SizeX;
        if (Kind == EKind::Flare)
        {
            // Lens-flare element (header): luminance = root x element brightness x light intensity; the light's age curve.
            const FJson Light = Obj(E, TEXT("light"));
            Luminance = Flare->Brightness * Num(Light, TEXT("intensity"), 1);
            Tint = FLinearColor(Rgb.R * Flare->Tint.R, Rgb.G * Flare->Tint.G, Rgb.B * Flare->Tint.B);
            Alpha = 1;
            AlphaStrengthCurve = Curve(Light, TEXT("strength_curve"));
            AlphaAge = {};
            ColorAge.Reset();
            for (const auto& K : Curve(Light, TEXT("age_curve"))) ColorAge.Add({K[0], K[1], K[1], K[1]});
            SizeX = 2 * Size * Flare->Size * Flare->Scale.X * Cm * FlareWorldScale;
            SizeY = 2 * Size * Flare->Size * Flare->Scale.Y * Cm * FlareWorldScale;
            L.Approx.AddUnique(TEXT("flare:world-space sprites sized by the emitter (CryEngine sizes on screen); occlusion by depth"));
        }
        const FStrength AlphaS = Strength(AlphaStrengthCurve);
        const FLinearColor Max(Tint.R * Luminance, Tint.G * Luminance, Tint.B * Luminance, Alpha);
        const FLinearColor Min(Max.R * (1 - ColorRandom), Max.G * (1 - ColorRandom), Max.B * (1 - ColorRandom), Alpha * (1 - AlphaRandom));
        SetColorRange(Init->ColorDistribution, Min, Max);
        bool bAnyChannel = false;
        for (int32 c = 0; c < 3; ++c) bAnyChannel |= Max.Component(c) > 0 && !ColorS[c].bAllZero;
        bInvisible = Alpha <= 0 || AlphaS.bAllZero || !bAnyChannel;
        L.Mapped.Append({TEXT("color"), TEXT("alpha"), TEXT("emissive_lighting")});
        if (!F.Strength && (bColorVaries || AlphaS.bVaries)) L.Approx.AddUnique(TEXT("strength_curves@1"));

        // Dynamic Material Parameter 0: the colour / alpha strength scales (1 when the curves are absent).
        auto* Dyn = Module<UNiagaraStatelessModule_DynamicMaterialParameters>(Em);
        Dyn->SetIsModuleEnabled(true);
        Dyn->bParameter0Enabled = true;
        Dyn->bParameter1Enabled = Dyn->bParameter2Enabled = Dyn->bParameter3Enabled = false;
        // AlphaErosion (own or inherited): Dynamic Material Parameter 1 X = the erosion threshold over the particle's
        // age (base x ParticleAge curve); the masters cut texture alpha below it and rescale the rest.
        const FString ErosionRaw = F.Erosion && Kind != EKind::Flare ? EffectiveRaw(E, TEXT("AlphaErosion")) : FString();
        if (!ErosionRaw.IsEmpty())
        {
            const double Base = FCString::Atod(*ErosionRaw);
            const FCurve Age = RawCurve(ErosionRaw, TEXT("ParticleAge"));
            Dyn->bParameter1Enabled = true;
            FNiagaraStatelessDynamicParameterSet& P1 = Dyn->Parameter1;
            P1.bXChannelEnabled = true;
            P1.bYChannelEnabled = P1.bZChannelEnabled = P1.bWChannelEnabled = false;
            if (Age.IsEmpty()) P1.XChannelDistribution.InitConstant(float(Base));
            else
            {
                P1.XChannelDistribution.Mode = ENiagaraDistributionMode::UniformCurve;
                P1.XChannelDistribution.ChannelCurves = {RichCurve(Age, 0, Base)};
                P1.XChannelDistribution.UpdateValuesFromDistribution();
            }
            L.Mapped.AddUnique(TEXT("AlphaErosion"));
        }
        FNiagaraStatelessDynamicParameterSet& P0 = Dyn->Parameter0;
        P0.bXChannelEnabled = P0.bYChannelEnabled = P0.bZChannelEnabled = P0.bWChannelEnabled = true;
        Apply(P0.XChannelDistribution, ColorS[0]);
        Apply(P0.YChannelDistribution, ColorS[1]);
        Apply(P0.ZChannelDistribution, ColorS[2]);
        Apply(P0.WChannelDistribution, AlphaS);
        if (bColorVaries) L.Mapped.Add(TEXT("color.strength_curve(SC.Strength)"));
        if (AlphaS.bVaries) L.Mapped.Add(TEXT("alpha.strength_curve(SC.Strength)"));

        const double SizeRandom = FMath::Clamp(Num(E, TEXT("size.random")), 0.0, 1.0);
        const double Aspect = Has(E, TEXT("aspect")) ? Num(E, TEXT("aspect"), 1) : 1.0;
        SizeX *= Aspect;
        if (FMath::IsNearlyEqual(SizeX, SizeY)) SetUniformRange(Init->SpriteSizeDistribution, SizeY * (1 - SizeRandom), SizeY);
        else SetVec2Range(Init->SpriteSizeDistribution, FVector2f(SizeX, SizeY) * float(1 - SizeRandom), FVector2f(SizeX, SizeY));
        L.Mapped.Add(TEXT("size"));
        if (TOptional<double> Angle = RawComponent(E, TEXT("RandomAngles"), TEXT("y")); Angle && Kind != EKind::Flare)
        {
            SetRange(Init->SpriteRotationDistribution, -*Angle, *Angle);
            Init->SpriteRotationDistribution.UpdateValuesFromDistribution();
            L.Mapped.AddUnique(TEXT("RandomAngles.y"));
        }
        else Init->SpriteRotationDistribution.InitConstant(0.f);
        if (TOptional<double> Rate = RawComponent(E, TEXT("RandomRotationRate"), TEXT("y")); Rate && Kind != EKind::Flare)
        {
            auto* Rot = Module<UNiagaraStatelessModule_SpriteRotationRate>(Em);
            Rot->SetIsModuleEnabled(true);
            Rot->bUseRateScale = false;
            SetRange(Rot->RotationRateDistribution, -*Rate, *Rate);
            Rot->RotationRateDistribution.UpdateValuesFromDistribution();
            L.Mapped.AddUnique(TEXT("RandomRotationRate.y"));
        }

        const FCurve SizeCurve = Kind == EKind::Flare ? FCurve() : Curve(E, TEXT("size.age_curve"));
        if (!SizeCurve.IsEmpty())
        {
            auto* Scale = Module<UNiagaraStatelessModule_ScaleSpriteSize>(Em);
            Scale->SetIsModuleEnabled(true);
            Scale->ScaleDistribution.Mode = ENiagaraDistributionMode::UniformCurve;
            Scale->ScaleDistribution.ChannelCurves = {RichCurve(SizeCurve, 0)};
            Scale->ScaleDistribution.UpdateValuesFromDistribution();
            L.Mapped.AddUnique(TEXT("size.age_curve"));
        }
        // The effect's scale (CryEngine scales an emitter's sizes with its spawn scale, e.g. a MaterialEffects entry's
        // minscale..maxscale) is the component's: it moves positions and speeds of a local-space emitter, and Niagara's
        // Apply Owner Scale To Attributes module (UE 5.9's opt-in; off, a sprite's size ignores the component's scale)
        // scales the initial sprite size by the component's largest axis. Guided beams are never scaled (their length is
        // SC.BeamLength).
        if (Kind != EKind::Light && !bBeam)
        {
            auto* OwnerScale = Module<UNiagaraStatelessModule_ApplyOwnerScaleToAttributes>(Em);
            OwnerScale->SetIsModuleEnabled(true);
            OwnerScale->SystemScaleData = FNiagaraStatelessSystemScaleBuildData();
            OwnerScale->SystemScaleData.bScaleInitialSpriteSize = true;
            OwnerScale->SystemScaleData.bScaleInitialSpriteSizeBySingleAxis = false;
            OwnerScale->SystemScaleData.ScaleInitialSpriteSizeScaleAmount = 1.f;
            L.Mapped.AddUnique(TEXT("scale(component: initial sprite size)"));
        }
        if (!ColorAge.IsEmpty() || !AlphaAge.IsEmpty())
        {
            auto* Scale = Module<UNiagaraStatelessModule_ScaleColor>(Em);
            Scale->SetIsModuleEnabled(true);
            SetColorCurve(Scale->ScaleDistribution, ColorAge, AlphaAge);
            if (!ColorAge.IsEmpty()) L.Mapped.AddUnique(Kind == EKind::Flare ? TEXT("light.age_curve(flare)") : TEXT("color.age_curve"));
            if (!AlphaAge.IsEmpty()) L.Mapped.AddUnique(TEXT("alpha.age_curve"));
        }

        // Material.
        auto* R = NewObject<UNiagaraSpriteRendererProperties>(Em, NAME_None, RF_Transactional);
        const FString Blend = Str(E, TEXT("blend"));
        FMat M;
        M.Master = Kind == EKind::Distortion ? FMat::EMaster::Distortion
                   : (Kind == EKind::Flare || Blend == TEXT("Additive")) ? FMat::EMaster::Additive
                                                                         : FMat::EMaster::Translucent;
        if (Kind == EKind::Sprite && Blend != TEXT("Additive") && Blend != TEXT("AlphaBased")) L.Approx.Add(TEXT("blend ") + Blend + TEXT(":translucent"));
        const TArray<double> Tiles = Kind == EKind::Flare ? TArray<double>() : Nums(E, TEXT("tiles"));
        if (Tiles.Num() == 2) M.Frames = FVector2f(float(Tiles[0]), float(Tiles[1]));
        if (Kind == EKind::Flare)
        {
            M.Texture = Flare->bGlow ? FString() : Flare->Texture;
            M.GlowGamma = Flare->bGlow ? float(FMath::Max(Flare->Gamma, 0.01)) : 0.f;
        }
        else
        {
            M.Texture = Str(E, TEXT("texture"));
            if (F.Extras)
            {
                M.bFlipU = RawFlag(E, TEXT("RandomFlipU"));
                M.bFlipV = RawFlag(E, TEXT("RandomFlipV"));
                if (M.bFlipU || M.bFlipV) L.Mapped.AddUnique(TEXT("RandomFlipU/V"));
            }
            const FString Flow = Str(E, TEXT("flow_texture"));
            if (!Flow.IsEmpty() && F.Flow)
            {
                M.Flow = Flow;
                M.FlowStrength = float(RawNumber(E, TEXT("FlowMapIntensity")).Get(0));
                L.Mapped.AddUnique(TEXT("flow_texture x FlowMapIntensity"));
            }
            else if (!Flow.IsEmpty())
                L.Skipped.AddUnique(TEXT("flow_texture"));
        }
        const FString SoftRaw = Raw(E, TEXT("SoftParticleV2")).IsEmpty() ? Raw(E, TEXT("SoftParticle")) : Raw(E, TEXT("SoftParticleV2"));
        if (F.Soft && SoftRaw.TrimStart().StartsWith(TEXT("true"), ESearchCase::IgnoreCase))
        {
            const TOptional<double> Thickness = RawComponent(E, Raw(E, TEXT("SoftParticleV2")).IsEmpty() ? TEXT("SoftParticle") : TEXT("SoftParticleV2"), TEXT("FadeThickness"));
            M.SoftFade = float(Thickness ? *Thickness * Cm : 0.5 * Size * Cm); // fallback: half the radius (Astra, item5-1)
            L.Mapped.AddUnique(Thickness ? TEXT("SoftParticle.FadeThickness") : TEXT("SoftParticle"));
            if (!Thickness) L.Approx.AddUnique(TEXT("soft fade = half the sprite radius (no FadeThickness)"));
        }
        if (Kind == EKind::Distortion)
        {
            M.Refraction = float(RawComponent(E, TEXT("Refraction"), TEXT("Strength")).Get(0) * RefractionScale);
            M.Normal = Str(E, TEXT("normal_texture"));
            L.Mapped.AddUnique(M.Normal.IsEmpty() ? TEXT("Refraction(alpha lens)") : TEXT("Refraction+NormalTexture"));
        }
        R->Material = Material(M);
        R->SortMode = M.Master == FMat::EMaster::Translucent ? ENiagaraSortMode::ViewDepth : ENiagaraSortMode::None;
        L.Mapped.AddUnique(TEXT("texture"));
        L.Mapped.AddUnique(TEXT("blend"));

        // Facing and stretch.
        const FString Facing = Kind == EKind::Flare ? FString(TEXT("Camera")) : Str(E, TEXT("facing"));
        const double Stretch = Kind == EKind::Flare ? 0.0 : RawNumber(E, TEXT("VelocityStretch")).Get(0);
        const bool bStretch = F.Stretch && Stretch > 0 && (TypicalSpeedCm > 0);
        const bool bVelocity = Bool(E, TEXT("orient_to_velocity")) || bStretch;
        R->Alignment = bVelocity ? ENiagaraSpriteAlignment::VelocityAligned : ENiagaraSpriteAlignment::Unaligned;
        R->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
        if (Bool(E, TEXT("orient_to_velocity"))) L.Mapped.AddUnique(TEXT("orient_to_velocity"));
        if (bStretch && bBeam)
        {
            // A guided beam's particles travel at SC.BeamLength / lifetime: the stretch speed x VelocityStretch adds
            // BeamLength x stretch / lifetime to the sprite's length (Y of a velocity-aligned sprite), centred on the
            // particle like CryEngine's default stretch. UE's ScaleSpriteSizeBySpeed follows speed^2 against a fixed
            // threshold, so the beam binds the length instead (SC.BeamLength2 = (length, length); size random dropped).
            Init->SpriteSizeDistribution.Mode = ENiagaraDistributionMode::Expression;
            Init->SpriteSizeDistribution.ParameterExpression =
                V2Add(V2Const(FVector2f(float(SizeX), float(SizeY))),
                      V2Mul(V2Bind(UserBeamLength2), V2Const(FVector2f(0.f, float(Stretch / FMath::Max(Life, 1e-3))))));
            // Centred like CryEngine stretch, but no streak may leave the beam: the length follows a tent over age,
            // 2 min(age, 1 - age) x the full stretch, so a streak centred at age x length spans at most [0, length] and
            // every layer starts at the emitter and ends at SC.BeamEnd. Width keeps the size curve.
            auto* Scale = Module<UNiagaraStatelessModule_ScaleSpriteSize>(Em);
            Scale->SetIsModuleEnabled(true);
            Scale->ScaleDistribution.Mode = ENiagaraDistributionMode::NonUniformCurve;
            Scale->ScaleDistribution.ChannelCurves = {RichCurve(SizeCurve, 0), RichCurve(FCurve{{0.0, 0.0}, {0.5, 1.0}, {1.0, 0.0}}, 0)};
            Scale->ScaleDistribution.UpdateValuesFromDistribution();
            L.Mapped.AddUnique(FString::Printf(TEXT("VelocityStretch(+%.2f x SC.BeamLength, centred, tent over age: stays inside the beam)"), Stretch / FMath::Max(Life, 1e-3)));
            L.Approx.AddUnique(TEXT("stretch: length 2 min(age, 1 - age) x full so it stays between emitter and target"));
            if (SizeRandom > 0) L.Approx.AddUnique(TEXT("size.random dropped (stretched beam)"));
            L.Skipped.AddUnique(TEXT("VelocityStretch.ReachFullStretchTimeMult"));
        }
        else if (bStretch)
        {
            // CryEngine stretches a sprite by speed x VelocityStretch (s) along its velocity: scale Y (the velocity axis
            // of a velocity-aligned sprite) to 1 + v x stretch / size at the emitter's typical speed. UE's factor follows
            // speed^2 up to that speed, so slower particles stretch less than in the game.
            auto* ByS = Module<UNiagaraStatelessModule_ScaleSpriteSizeBySpeed>(Em);
            ByS->SetIsModuleEnabled(true);
            SetRange(ByS->VelocityThreshold, TypicalSpeedCm, TypicalSpeedCm);
            SetVec2Range(ByS->MinScaleFactor, FVector2f(1, 1), FVector2f(1, 1));
            const float Factor = float(1.0 + TypicalSpeedCm * Stretch / FMath::Max(SizeY, 1.0));
            SetVec2Range(ByS->MaxScaleFactor, FVector2f(1, Factor), FVector2f(1, Factor));
            ByS->bSampleScaleFactorByCurve = false;
            L.Mapped.AddUnique(FString::Printf(TEXT("VelocityStretch(x%.2f at %.0f cm/s)"), Factor, TypicalSpeedCm));
            L.Approx.AddUnique(TEXT("stretch follows speed^2 below the typical speed; stretch Random skipped"));
        }
        else if (Stretch > 0)
            L.Skipped.AddUnique(TEXT("VelocityStretch"));
        if (Facing == TEXT("Free") || Facing == TEXT("Horizontal"))
        {
            auto* Fa = Module<UNiagaraStatelessModule_SpriteFacingAndAlignment>(Em);
            Fa->SetIsModuleEnabled(true);
            Fa->bSpriteFacingEnabled = true;
            Fa->bSpriteAlignmentEnabled = false;
            SetVec3(Fa->SpriteFacing, Facing == TEXT("Free") ? FVector3f(1, 0, 0) : FVector3f(0, 0, 1));
            R->FacingMode = ENiagaraSpriteFacingMode::CustomFacingVector;
            L.Mapped.AddUnique(TEXT("facing"));
        }
        else if (!Facing.IsEmpty() && Facing != TEXT("Camera"))
            L.Approx.AddUnique(TEXT("facing ") + Facing + TEXT(":camera"));
        else
            L.Mapped.AddUnique(TEXT("facing"));

        // Flipbook.
        const FJson Anim = Kind == EKind::Flare ? nullptr : Obj(E, TEXT("anim"));
        if (Tiles.Num() == 2)
        {
            R->SubImageSize = FVector2D(Tiles[0], Tiles[1]);
            const int32 Frames = Anim ? int32(Num(Anim, TEXT("frames"), Tiles[0] * Tiles[1])) : int32(Tiles[0] * Tiles[1]);
            auto* Sub = Module<UNiagaraStatelessModule_SubUVAnimation>(Em);
            Sub->SetIsModuleEnabled(true);
            Sub->NumFrames = FMath::Max(1, Frames);
            Sub->bStartFrameRangeOverride_Enabled = false;
            Sub->bEndFrameRangeOverride_Enabled = false;
            R->bSubImageBlend = (Anim && Bool(Anim, TEXT("blend"))) || !M.Flow.IsEmpty();
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
                L.Mapped.AddUnique(TEXT("anim.static_frame"));
                if (First.Num() == 2) L.Approx.AddUnique(TEXT("first_frame:1-based interval"));
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
                if (!FMath::IsNearlyEqual(Rate, 1.0) && !Bool(Anim, TEXT("cycle"))) L.Approx.AddUnique(TEXT("anim.rate:once over life"));
                L.Mapped.AddUnique(TEXT("anim"));
                if (First.Num() == 2) L.Skipped.AddUnique(TEXT("anim.first_frame(random start: the stateless SubUV module plays from frame 0)"));
            }
            else
            {
                const double Fps = Num(Anim, TEXT("fps"), Num(Anim, TEXT("rate")));
                Sub->AnimationMode = Bool(Anim, TEXT("cycle")) ? ENSMSubUVAnimation_Mode::InfiniteLoop : ENSMSubUVAnimation_Mode::Linear;
                Sub->LoopsPerSecond = float(Fps / FMath::Max(1, Frames));
                if (!Bool(Anim, TEXT("cycle"))) L.Approx.AddUnique(TEXT("anim.fps once:over life"));
                L.Mapped.AddUnique(TEXT("anim"));
            }
            L.Mapped.AddUnique(TEXT("tiles"));
        }
        Em->AddRenderer(R, FGuid());
        return R->Material != nullptr;
    }

    /** Raw attributes the rules read (anything else of the emitter is reported as skipped). */
    bool IsMappedRaw(const FString& K) const
    {
        static const TSet<FString> Known = {
            TEXT("Enabled"), TEXT("Inheritance"), TEXT("ParticleSystem"), TEXT("Count"), TEXT("Continuous"), TEXT("ParticleLifeTime"),
            TEXT("EmitterLifeTime"), TEXT("PulsePeriod"), TEXT("SpawnDelay"), TEXT("EmitPerDistance"), TEXT("Size"), TEXT("Aspect"),
            TEXT("Alpha"), TEXT("Color"), TEXT("Speed"), TEXT("EmitAngle"), TEXT("FocusAngle"), TEXT("GravityScale"),
            TEXT("AirResistance"), TEXT("Drag"), TEXT("BlendType"), TEXT("Facing"), TEXT("OrientToVelocity"), TEXT("EmissiveLighting"),
            TEXT("DiffuseLighting"), TEXT("Texture"), TEXT("TextureAnimation"), TEXT("TextureTiling"), TEXT("LightSource"),
            TEXT("PositionOffset"), TEXT("RandomOffset"), TEXT("EmissionSize"), TEXT("RandomAngles"), TEXT("RandomRotationRate"),
            TEXT("Spawn"), TEXT("Comment"), TEXT("SplineGuided"), TEXT("InheritVelocity")};
        if (Known.Contains(K)) return true;
        if (F.Extras && (K == TEXT("CameraMaxDistance") || K == TEXT("RandomFlipU") || K == TEXT("RandomFlipV"))) return true;
        if (F.Erosion && K == TEXT("AlphaErosion")) return true;
        if (F.Flares && (K == TEXT("Flare") || K == TEXT("FlareEnabled") || K == TEXT("FakeLight"))) return true;
        if (F.Soft && (K == TEXT("SoftParticleV2") || K == TEXT("SoftParticle"))) return true;
        if (F.Stretch && K == TEXT("VelocityStretch")) return true;
        if (F.Flow && (K == TEXT("FlowTexture") || K == TEXT("FlowMapIntensity"))) return true;
        if (F.Distortion && (K == TEXT("Refraction") || K == TEXT("NormalTexture"))) return true;
        return false;
    }

    /** A raw attribute as the emitter effectively has it: its own, else (Inheritance=Parent, listed in `inherited`) its
     *  parent's, up the chain of the current effect. */
    FString EffectiveRaw(const FJson& E, const FString& Attr) const
    {
        FJson Cur = E;
        for (int32 Guard = 0; Cur && Guard < 32; ++Guard)
        {
            const FString Own = Raw(Cur, Attr);
            if (!Own.IsEmpty()) return Own;
            bool bInherited = false;
            for (const auto& K : Cur->GetArrayField(TEXT("inherited"))) bInherited |= K->AsString() == Attr;
            const int32 P = int32(Num(Cur, TEXT("parent"), -1));
            if (!bInherited || !CurrentEmitters.IsValidIndex(P)) return FString();
            Cur = CurrentEmitters[P]->AsObject();
        }
        return FString();
    }

    /** The lens-flare elements of `Flare` (<library>.<item>) from Data/Libs/Flares/<library>.xml. */
    TArray<FFlareElement> FlareElements(const FString& Flare, FLog& L)
    {
        TArray<FFlareElement> Out;
        FString Library, Item;
        if (!Flare.Split(TEXT("."), &Library, &Item)) return Out;
        char* Text = nullptr;
        const FString Path = FlareDir + Library + TEXT(".xml");
        if (sc_p4k_read_xml(H, TCHAR_TO_UTF8(*Path), &Text) != SC_STATUS_OK)
        {
            L.Skipped.AddUnique(FString::Printf(TEXT("flare(%s: %s)"), *Path, UTF8_TO_TCHAR(sc_last_error(H))));
            return Out;
        }
        FXmlFile Xml(UTF8_TO_TCHAR(Text), EConstructMethod::ConstructFromBuffer);
        sc_free_string(Text);
        const FXmlNode* RootNode = Xml.GetRootNode();
        const FXmlNode* RootItem = nullptr;
        for (const FXmlNode* N : RootNode ? RootNode->GetChildrenNodes() : TArray<FXmlNode*>())
            if (N->GetTag() == TEXT("FlareItem") && N->GetAttribute(TEXT("Name")).Equals(Item, ESearchCase::IgnoreCase)) RootItem = N;
        if (!RootItem)
        {
            L.Skipped.AddUnique(FString::Printf(TEXT("flare(%s has no %s)"), *Path, *Item));
            return Out;
        }
        auto Params = [](const FXmlNode* N, const TCHAR* Tag) -> const FXmlNode* {
            const FXmlNode* P = N->FindChildNode(TEXT("Params"));
            return P ? P->FindChildNode(Tag) : nullptr;
        };
        auto Attr = [](const FXmlNode* N, const TCHAR* A, double Default) {
            const FString V = N ? N->GetAttribute(A) : FString();
            return V.IsEmpty() ? Default : FCString::Atod(*V);
        };
        auto Pair = [](const FXmlNode* N, const TCHAR* A, FVector2D Default) {
            const FString V = N ? N->GetAttribute(A) : FString();
            FString X, Y;
            return V.Split(TEXT(","), &X, &Y) ? FVector2D(FCString::Atod(*X), FCString::Atod(*Y)) : Default;
        };
        const FXmlNode* RootCommon = Params(RootItem, TEXT("Common"));
        const double RootSize = Attr(RootCommon, TEXT("Size"), 1), RootBrightness = Attr(RootCommon, TEXT("Brightness"), 1);
        for (const FXmlNode* N : RootItem->GetChildrenNodes())
        {
            if (N->GetTag() != TEXT("FlareItem") || N->GetAttribute(TEXT("Enable")) == TEXT("0")) continue;
            const FString Type = N->GetAttribute(TEXT("Type"));
            const FXmlNode* Common = Params(N, TEXT("Common"));
            FFlareElement El;
            El.Name = N->GetAttribute(TEXT("Name"));
            El.Size = RootSize * Attr(Common, TEXT("Size"), 1);
            El.Brightness = RootBrightness * Attr(Common, TEXT("Brightness"), 1);
            El.Scale = Pair(Params(N, TEXT("Transformation")), TEXT("Scale"), FVector2D(1, 1));
            const FString Tint = Common ? Common->GetAttribute(TEXT("Tint")) : FString();
            TArray<FString> T;
            Tint.ParseIntoArray(T, TEXT(","));
            if (T.Num() == 3) El.Tint = FLinearColor(FCString::Atof(*T[0]), FCString::Atof(*T[1]), FCString::Atof(*T[2]));
            if (Type == TEXT("Glow"))
                El.Gamma = Attr(Params(N, TEXT("Glow")), TEXT("Gamma"), 1);
            else if (const FXmlNode* Ghost = Params(N, TEXT("LensGhost")))
            {
                // The archive stores the authored .tif/.dds path as a .dds under Data/.
                FString Tex = Ghost->GetAttribute(TEXT("Texture")).Replace(TEXT("\\"), TEXT("/"));
                Tex = (Tex.StartsWith(TEXT("Data/"), ESearchCase::IgnoreCase) ? Tex : TEXT("Data/") + Tex);
                El.Texture = FPaths::GetBaseFilename(Tex, false) + TEXT(".dds");
                El.bGlow = false;
            }
            else
            {
                L.Skipped.AddUnique(TEXT("flare element type ") + Type);
                continue;
            }
            Out.Add(El);
        }
        L.Skipped.AddUnique(TEXT("flare occlusion fade, sensor variation, dynamics trigger"));
        return Out;
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
        const FString LongName = SCFxNames::PackageFor(Str(Effect, TEXT("library")), EffectName); // the runtime finds it by the same rule
        const FString Name = FPaths::GetBaseFilename(LongName);

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
        CurrentEmitters = Emitters;
        GuideCurvature.Reset();
        GuideName.Reset();
        for (const auto& V : Emitters)
            if (const FJson E = V->AsObject(); Bool(E, TEXT("enabled")) && Has(E, TEXT("spline")))
            {
                GuideCurvature = FMath::Clamp(Num(E, TEXT("spline.curvature")), 0.0, 1.0);
                GuideName = Str(E, TEXT("path"));
                break;
            }
        TSet<FName> Used;
        int32 Made = 0;
        bool bUsesStrength = false, bUsesBeam = false;
        auto Add = [&](const FString& Wanted, const FJson& E, const FJson& Parent, EKind Kind, const FFlareElement* Flare, FLog& L) {
            FString Unique = SafeSegment(Wanted);
            for (int32 n = 2; Used.Contains(FName(*Unique)); ++n) Unique = FString::Printf(TEXT("%s_%d"), *SafeSegment(Wanted), n);
            Used.Add(FName(*Unique));
            System->DuplicateEmitterHandle(TemplateHandle, FName(*Unique));
            FNiagaraEmitterHandle& Handle = System->GetEmitterHandles().Last();
            bool bInvisible = false;
            FLog Local;
            const bool bOK = Configure(Handle.GetStatelessEmitter(), E, Parent, Kind, Flare, Local, bInvisible);
            if (bInvisible)
            {
                Handle.SetIsEnabled(false, *System, false);
                Local.Approx.Add(TEXT("disabled:invisible at every game strength"));
            }
            for (const FString& M : Local.Mapped)
            {
                L.Mapped.AddUnique(M);
                bUsesStrength |= M.Contains(TEXT("SC.Strength"));
                bUsesBeam |= M.Contains(TEXT("SC.BeamLength")) || M.Contains(TEXT("SC.BeamEnd"));
            }
            for (const FString& A : Local.Approx) L.Approx.AddUnique(A);
            for (const FString& S : Local.Skipped) L.Skipped.AddUnique(S);
            ++Made;
            return bOK ? Unique : FString();
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
            if (const FJson RawObj = Obj(E, TEXT("raw")))
                for (const auto& Pair : RawObj->Values) RawKeys.Add(FString(Pair.Key.ToView()));
            for (const FString& K : RawKeys)
                if (!IsMappedRaw(K)) L.Skipped.Add(K);
            for (const auto& K : E->GetArrayField(TEXT("inherited")))
                if (!IsMappedRaw(K->AsString())) L.Skipped.AddUnique(K->AsString() + TEXT("(inherited)"));
            TArray<FString> Out;
            auto Emit = [&Out](const FString& N) { if (!N.IsEmpty()) Out.Add(N); };
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
                if (bSprite) Emit(Add(Local, E, Parent, EKind::Sprite, nullptr, L));
                // Refraction twin (Refraction Strength > 0).
                if (bSprite && RawComponent(E, TEXT("Refraction"), TEXT("Strength")).Get(0) > 0)
                {
                    if (F.Distortion) Emit(Add(Local + TEXT("_refraction"), E, Parent, EKind::Distortion, nullptr, L));
                    else L.Skipped.AddUnique(TEXT("Refraction"));
                }
                const FJson Light = Obj(E, TEXT("light"));
                const bool bFakeLight = RawFlag(E, TEXT("FakeLight"));
                if (Light && bFakeLight) L.Mapped.AddUnique(TEXT("FakeLight(no light: flare only)"));
                if (Light && !bFakeLight && Num(Light, TEXT("intensity"), 1) > 0 && Num(Light, TEXT("radius_m")) > 0)
                    Emit(Add(Local + TEXT("_light"), E, Parent, EKind::Light, nullptr, L));
                const FString Flare = Str(E, TEXT("flare"));
                if (Bool(E, TEXT("flare_enabled")) && !Flare.IsEmpty())
                {
                    if (F.Flares)
                    {
                        for (const FFlareElement& El : FlareElements(Flare, L))
                            Emit(Add(Local + TEXT("_flare_") + El.Name.RightChop(El.Name.Find(TEXT(".")) + 1), E, Parent, EKind::Flare, &El, L));
                        L.Mapped.AddUnique(TEXT("flare(") + Flare + TEXT(")"));
                    }
                    else L.Skipped.AddUnique(TEXT("flare"));
                }
            }
            UE_LOG(LogSCFx, Display, TEXT("SC_FX emitter=%s/%s -> %s mapped=%s skipped=%s approx=%s"), *Str(Effect, TEXT("library")), *Path,
                   Out.IsEmpty() ? TEXT("-") : *FString::Join(Out, TEXT("+")), *Join(L.Mapped), *Join(L.Skipped), *Join(L.Approx));
        }
        System->RemoveEmitterHandle(TemplateHandle);
        if (UNiagaraStatelessEmitter* Orphan = TemplateHandle.GetStatelessEmitter())
            Orphan->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

        // The runtime contract (header): the parameters this system reads, with their defaults.
        TArray<FString> Contract;
        FNiagaraUserRedirectionParameterStore& User = System->GetExposedParameters();
        auto AddFloat = [&](FName N, float V) {
            FNiagaraVariable Var(FNiagaraTypeDefinition::GetFloatDef(), N);
            User.AddParameter(Var, true, true);
            User.SetParameterValue(V, Var);
            Contract.Add(FString::Printf(TEXT("%s=%g"), *N.ToString(), V));
        };
        auto AddVec = [&](FName N, const FVector3f& V) {
            FNiagaraVariable Var(FNiagaraTypeDefinition::GetVec3Def(), N);
            User.AddParameter(Var, true, true);
            User.SetParameterValue(V, Var);
            Contract.Add(FString::Printf(TEXT("%s=(%g,%g,%g)"), *N.ToString(), V.X, V.Y, V.Z));
        };
        AddFloat(UserStrength, 1.f);
        {
            // E1-6 review: the generated contract's version (SCFxRules::FxContractVersion), which the runtime and the package check.
            FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), UserFxContract);
            User.AddParameter(Var, true, true);
            User.SetParameterValue(SCFxRules::FxContractVersion, Var);
            Contract.Add(FString::Printf(TEXT("%s=%d"), *UserFxContract.ToString(), SCFxRules::FxContractVersion));
        }
        if (bUsesBeam)
        {
            AddVec(UserBeamEnd, FVector3f(float(DefaultBeamCm), 0, 0));
            AddVec(UserBeamTangent, FVector3f(float(DefaultBeamCm), 0, 0));
            AddFloat(UserBeamLength, float(DefaultBeamCm));
            FNiagaraVariable Var(FNiagaraTypeDefinition::GetVec2Def(), UserBeamLength2);
            User.AddParameter(Var, true, true);
            User.SetParameterValue(FVector2f(float(DefaultBeamCm)), Var);
            Contract.Add(FString::Printf(TEXT("%s=(%g,%g)"), *UserBeamLength2.ToString(), DefaultBeamCm, DefaultBeamCm));
        }

        System->RequestCompile(false);
        System->WaitForCompilationComplete(true, false);
        System->MarkPackageDirty();
        if (!Save(System)) return false;
        ++Systems;
        UE_LOG(LogSCFx, Display, TEXT("SC_FX_SYSTEM %s <- %s emitters=%d state=%s user=%s strength=%s features=%s"), *System->GetPathName(),
               *Reference, System->GetEmitterHandles().Num(), System->GetSystemStateModeString(), *Join(Contract),
               bUsesStrength ? TEXT("bound") : TEXT("unused"), *F.ToString());
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

// -VerifyContract: every built system under /Game/SC/FX carries the current generated contract (E1-6 review). Systems built before it
// load with stale behaviour (the runtime no longer writes SC.SV / SC.BR / SC.Scale2), so the showcase package refuses them.
static int32 VerifyContract()
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    TArray<FAssetData> Assets;
    Registry.GetAssetsByPath(TEXT("/Game/SC/FX"), Assets, true);
    int32 Systems = 0, Stale = 0, Failed = 0;
    for (const FAssetData& Asset : Assets)
    {
        if (Asset.AssetClassPath != UNiagaraSystem::StaticClass()->GetClassPathName()) continue;
        ++Systems;
        const UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset.GetAsset());
        if (!System)
        {
            // Review fixes 3 (Astra A9): a system the registry lists but that does not load is not verified: an error, not a skip.
            ++Failed;
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_STALE %s does not load: cannot verify its contract"), *Asset.GetObjectPathString());
            continue;
        }
        const int32 Version = SCFxContract::Of(*System);
        if (Version < SCFxRules::FxContractVersion)
        {
            ++Stale;
            UE_LOG(LogSCFx, Error, TEXT("SC_FX_STALE %s contract=%d (current %d): rebuild with -run=SCFxBuild"), *System->GetPathName(), Version, SCFxRules::FxContractVersion);
        }
    }
    UE_LOG(LogSCFx, Display, TEXT("SC_FX_CONTRACT systems=%d stale=%d load_failures=%d version=%d"), Systems, Stale, Failed, SCFxRules::FxContractVersion);
    return Stale == 0 && Failed == 0 ? 0 : 1;
}

int32 USCFxBuildCommandlet::Main(const FString& Params)
{
    if (FParse::Param(*Params, TEXT("VerifyContract"))) return VerifyContract();
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
    FString FeatureSpec;
    FParse::Value(*Params, TEXT("Features="), FeatureSpec, false);
    B.F.Parse(FeatureSpec);
    FParse::Value(*Params, TEXT("RefractionScale="), B.RefractionScale);
    FParse::Value(*Params, TEXT("LoopCapacity="), B.LoopCapacity);
    B.LoopCapacity = FMath::Max(B.LoopCapacity, 1); // review fixes 3 (A8): the engine accepts 1; a loop that cannot fit is reported
    B.SceneUnitNits = FSCStarEngine::Get().SceneUnitNits;
    B.LitSceneUnits = Num(Config, TEXT("unlit_diffuse_scene_units"), 0.0);
    B.FlareWorldScale = Num(Config, TEXT("flare_world_scale"), 1.0);
    if (sc_open(nullptr, nullptr, &B.H) != SC_STATUS_OK)
    {
        UE_LOG(LogSCFx, Error, TEXT("SC_FX_ERROR sc_open: %s"), UTF8_TO_TCHAR(sc_last_error(nullptr)));
        return 1;
    }
    B.LoadStrengths(Config);
    UE_LOG(LogSCFx, Display, TEXT("SC_FX build effects=%d material_effects=%d scene_unit_nits=%g unlit_diffuse_scene_units=%g features=%s spawn_infos=%d loop_capacity=%d"),
           Effects.Num(), MaterialEffects.Num(), B.SceneUnitNits, B.LitSceneUnits, *B.F.ToString(), SpawnInfoCapacity(), B.LoopCapacity);
    for (const FString& E : Effects) B.BuildEffect(E);
    for (const FString& M : MaterialEffects) B.BuildMaterialEffect(M);
    sc_close(B.H);
    UE_LOG(LogSCFx, Display, TEXT("SC_FX_DONE systems=%d textures=%d materials=%d errors=%d"), B.Systems, B.Textures.Num(), B.Materials.Num(), B.Errors);
    return B.Errors == 0 ? 0 : 1;
}
