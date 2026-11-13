// FSCStarEngine: fixture loading (no fallbacks) and the light / glow / exposure rules of the game's renderer.
#include "SCStarEngine.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/LogMacros.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString FSCStarEngine::FixturePath()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SCBridge"));
    return Plugin ? Plugin->GetBaseDir() / TEXT("Resources/StarEngineRender.json") : FString();
}

bool FSCStarEngine::Load(const FString& Path, FSCStarEngine& Out, FString& Error)
{
    FString Text;
    TSharedPtr<FJsonObject> Root;
    if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root)
    { Error = TEXT("cannot read ") + Path; return false; }
    if (Root->GetStringField(TEXT("schema")) != TEXT("star_engine_render_v1"))
    { Error = TEXT("unexpected schema in ") + Path; return false; }
    TArray<FString> Missing, Invalid;
    const auto Value = [&](const TCHAR* Section, const TCHAR* Name) {
        const TSharedPtr<FJsonObject>* Group = nullptr;
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        double Number = 0;
        if (!Root->TryGetObjectField(Section, Group) || !(*Group)->TryGetObjectField(Name, Entry) ||
            !(*Entry)->TryGetNumberField(TEXT("value"), Number))
            Missing.Add(FString(Section) + TEXT(".") + Name);
        else if (!FMath::IsFinite(Number))
            Invalid.Add(FString(Section) + TEXT(".") + Name + TEXT(" is not finite"));
        return Number;
    };
    Out = FSCStarEngine();
    Out.SceneUnitNits = Value(TEXT("shader_constants"), TEXT("HDR_LUMINANCE_SCALE"));
    Out.LuminanceToEV = Value(TEXT("shader_constants"), TEXT("LOG2_LUMINANCE_TO_EV"));
    Out.GlowSquareFactor = Value(TEXT("shader_constants"), TEXT("sqrGlowFactor"));
    Out.MiddleGrey = Value(TEXT("shader_constants"), TEXT("middleGray"));
    Out.MaxDisplayNitsLDR = Value(TEXT("shader_constants"), TEXT("MaxDisplayNitsLDR"));
    Out.MeterLogMin = Value(TEXT("shader_constants"), TEXT("LOG_MIN"));
    Out.MeterLogMax = Value(TEXT("shader_constants"), TEXT("LOG_MAX"));
    Out.GlassDensityScale = Value(TEXT("shader_constants"), TEXT("glassColourDensityScale"));
    Out.GlassColourFloor = Value(TEXT("shader_constants"), TEXT("glassColourFloor"));
    Out.GlassVertexHeightBias = Value(TEXT("shader_constants"), TEXT("glassVertexHeightBias"));
    Out.GlassVertexHeightScale = Value(TEXT("shader_constants"), TEXT("glassVertexHeightScale"));
    Out.GlassHeightClip = Value(TEXT("shader_constants"), TEXT("glassHeightClip"));
    Out.GlassTintStrengthDefault = Value(TEXT("shader_constants"), TEXT("glassTintStrengthDefault"));
    Out.GlassTilingDefault = Value(TEXT("shader_constants"), TEXT("glassTilingDefault"));
    Out.GlassVStretchDefault = Value(TEXT("shader_constants"), TEXT("glassVStretchDefault"));
    Out.GlassThinFilmThicknessDefault = Value(TEXT("shader_constants"), TEXT("glassThinFilmThicknessDefault"));
    Out.DirtDetailScale = Value(TEXT("shader_constants"), TEXT("detailScale"));
    {
        const TSharedPtr<FJsonObject>* Group = nullptr;
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Root->TryGetObjectField(TEXT("shader_textures"), Group) || !(*Group)->TryGetObjectField(TEXT("WearDirtFrostDetailMap"), Entry) ||
            !(*Entry)->TryGetStringField(TEXT("value"), Out.WearDirtFrostDetailMap) || Out.WearDirtFrostDetailMap.IsEmpty())
            Missing.Add(TEXT("shader_textures.WearDirtFrostDetailMap"));
    }
    Out.MultiplierMin = Value(TEXT("exe_cvars"), TEXT("r_ExposureMultiplierMin"));
    Out.MultiplierMax = Value(TEXT("exe_cvars"), TEXT("r_ExposureMultiplierMax"));
    Out.AdaptTimeDark = Value(TEXT("exe_cvars"), TEXT("r_ExposureAdaptTimeDark"));
    Out.AdaptTimeBright = Value(TEXT("exe_cvars"), TEXT("r_ExposureAdaptTimeBright"));
    Out.AdaptTimePupil = Value(TEXT("exe_cvars"), TEXT("r_ExposureAdaptTimePupil"));
    Out.AdaptRangePupil = Value(TEXT("exe_cvars"), TEXT("r_ExposureAdaptRangePupil"));
    Out.DiscardRatioDark = Value(TEXT("exe_cvars"), TEXT("r_ExposureDiscardRatioDark"));
    Out.DiscardRatioBright = Value(TEXT("exe_cvars"), TEXT("r_ExposureDiscardRatioBright"));
    Out.TonemappingCurve = int32(Value(TEXT("exe_cvars"), TEXT("r_HDRTonemappingCurve")));
    Out.TonemappingCurveToe = Value(TEXT("exe_cvars"), TEXT("r_HDRTonemappingCurveToe"));
    // The compensation anchors carry their scene EV in the cvar name (r_ExposureCompensationEV<n>).
    static const FString AnchorPrefix(TEXT("r_ExposureCompensationEV"));
    const TSharedPtr<FJsonObject>* Cvars = nullptr;
    if (Root->TryGetObjectField(TEXT("exe_cvars"), Cvars))
        for (const auto& Pair : (*Cvars)->Values)
        {
            const FString Name(Pair.Key);
            const FString Suffix = Name.RightChop(AnchorPrefix.Len());
            if (Name.StartsWith(AnchorPrefix) && Suffix.IsNumeric())
                Out.Compensation.Add(FVector2D(FCString::Atod(*Suffix), Value(TEXT("exe_cvars"), *Name)));
        }
    Out.Compensation.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X; });
    if (Out.Compensation.Num() < 2) Missing.Add(TEXT("exe_cvars.") + AnchorPrefix + TEXT("<n> (two or more)"));
    if (!Missing.IsEmpty())
    { Error = FString::Printf(TEXT("%s is incomplete: %s"), *Path, *FString::Join(Missing, TEXT(", "))); return false; }
    // The domain the rules below divide, take logarithms and bisect in. A fixture outside it is refused, not repaired.
    const auto Require = [&Invalid](bool bHolds, const TCHAR* Rule) { if (!bHolds) Invalid.Add(Rule); };
    Require(Out.SceneUnitNits > 0, TEXT("HDR_LUMINANCE_SCALE > 0"));
    Require(Out.GlowSquareFactor > 0, TEXT("sqrGlowFactor > 0"));
    Require(Out.MiddleGrey > 0, TEXT("middleGray > 0"));
    Require(Out.MaxDisplayNitsLDR > 0, TEXT("MaxDisplayNitsLDR > 0"));
    Require(Out.MeterLogMin < Out.MeterLogMax, TEXT("LOG_MIN < LOG_MAX"));
    Require(Out.GlassDensityScale > 0, TEXT("glassColourDensityScale > 0"));
    Require(Out.GlassColourFloor > 0 && Out.GlassColourFloor <= 1, TEXT("0 < glassColourFloor <= 1"));
    Require(Out.GlassVertexHeightScale > 0, TEXT("glassVertexHeightScale > 0"));
    Require(Out.GlassHeightClip >= 0, TEXT("glassHeightClip >= 0"));
    Require(Out.GlassTintStrengthDefault >= 0 && Out.GlassTintStrengthDefault <= 1, TEXT("0 <= glassTintStrengthDefault <= 1"));
    Require(Out.GlassTilingDefault > 0, TEXT("glassTilingDefault > 0"));
    Require(Out.GlassVStretchDefault > 0, TEXT("glassVStretchDefault > 0"));
    Require(Out.GlassThinFilmThicknessDefault >= 0, TEXT("glassThinFilmThicknessDefault >= 0"));
    Require(Out.MultiplierMin > 0 && Out.MultiplierMin < Out.MultiplierMax, TEXT("0 < r_ExposureMultiplierMin < r_ExposureMultiplierMax"));
    Require(Out.AdaptTimeDark > 0 && Out.AdaptTimeBright > 0 && Out.AdaptTimePupil > 0, TEXT("r_ExposureAdaptTime* > 0 (half-lives)"));
    Require(Out.AdaptRangePupil >= 0, TEXT("r_ExposureAdaptRangePupil >= 0"));
    Require(Out.DiscardRatioDark >= 0 && Out.DiscardRatioBright >= 0 && Out.DiscardRatioDark + Out.DiscardRatioBright < 1,
            TEXT("r_ExposureDiscardRatioDark/Bright >= 0 and sum < 1"));
    for (int32 I = 1; I < Out.Compensation.Num(); ++I)
    {
        const FVector2D& A = Out.Compensation[I - 1];
        const FVector2D& B = Out.Compensation[I];
        // The multiplier falls as the scene brightens only while the compensation gains less than a stop per stop
        // (LogMultiplierAt); EVAtMultiplier inverts it on that premise.
        if (B.X <= A.X)
            Invalid.Add(FString::Printf(TEXT("%s<n>: two anchors at EV %g"), *AnchorPrefix, A.X));
        else if ((B.Y - A.Y) / (B.X - A.X) >= 1)
            Invalid.Add(FString::Printf(TEXT("%s<n>: slope %g >= 1 between EV %g and %g"), *AnchorPrefix, (B.Y - A.Y) / (B.X - A.X), A.X, B.X));
    }
    if (!Invalid.IsEmpty())
    { Error = FString::Printf(TEXT("%s is out of the rules' domain: %s"), *Path, *FString::Join(Invalid, TEXT("; "))); return false; }
    return true;
}

const FSCStarEngine& FSCStarEngine::Get()
{
    static const FSCStarEngine Loaded = [] {
        FSCStarEngine Value;
        FString Error;
        if (!Load(FixturePath(), Value, Error)) UE_LOG(LogTemp, Fatal, TEXT("Star Engine render fixture: %s"), *Error);
        return Value;
    }();
    return Loaded;
}

double FSCStarEngine::PointCandela(double Intensity, double BulbRadiusM, double AttenuationTweak) const
{
    // Normalised intensity I * (1 + tweak / bulb)^2 times the far field (bulb / (tweak * d))^2 of the attenuation.
    if (AttenuationTweak <= 0) return 0; // no falloff term: not a far-field light
    return UE_DOUBLE_PI * SceneUnitNits * Intensity * FMath::Square(1.0 + BulbRadiusM / AttenuationTweak);
}

double FSCStarEngine::PlanarCandela(double Intensity, double WidthM, double HeightM) const
{
    return UE_DOUBLE_PI * SceneUnitNits * Intensity * WidthM * HeightM;
}

double FSCStarEngine::CompensationAt(double MeteredEV) const
{
    int32 Segment = 0; // the segment [Segment, Segment + 1] holding MeteredEV; the end segments extend outwards
    while (Segment + 2 < Compensation.Num() && MeteredEV >= Compensation[Segment + 1].X) ++Segment;
    const FVector2D& A = Compensation[Segment];
    const FVector2D& B = Compensation[Segment + 1];
    return A.Y + (B.Y - A.Y) * (MeteredEV - A.X) / (B.X - A.X);
}

double FSCStarEngine::LogMultiplierAt(double MeteredEV) const
{
    // exposure = middleGray / exp2(logLumTarget), logLumTarget = logLumMetered - compensation (scene units).
    const double LogLumMetered = MeteredEV - LuminanceToEV - FMath::Log2(SceneUnitNits);
    return FMath::Log2(MiddleGrey) - (LogLumMetered - CompensationAt(MeteredEV));
}

double FSCStarEngine::EVAtMultiplier(double Multiplier) const
{
    // Monotonic while every compensation slope is below one stop per stop; bisect.
    const double Target = FMath::Log2(Multiplier);
    double Low = -64, High = 64;
    for (int32 Step = 0; Step < 64; ++Step)
    {
        const double Mid = 0.5 * (Low + High);
        (LogMultiplierAt(Mid) > Target ? Low : High) = Mid;
    }
    return 0.5 * (Low + High);
}
