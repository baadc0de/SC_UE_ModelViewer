// SCLookLighting: look defaults at startup and the perimeter floor-wash experiment (LOOK-8).
// sc.Look.PerimeterFloorWash <scale>: classify the environment's spot/rect lights by where their emission lands (traced
//   against the scene, the shown ship ignored): >= 80 % of the weighted samples first hit upward-facing floor outside
//   the ship bay and <= 10 % inside it -> Scale x authored intensity (GPT Astra LOOK-8 round 3); ambiguous lights stay.
#include "SCLookLighting.h"
#include "SCHangarIndex.h"
#include "SCLook.h"
#include "SCVehicle.h"
#include "SCLightImporter.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLookLighting, Log, All);

namespace
{
TAutoConsoleVariable<int32> CVarNativeHDR(TEXT("sc.Look.NativeHDR"), 1,
    TEXT("Use the display's native ACES output transform on HDR player views. 0: expand the game's SDR shoulder."));

class FSCViewerDisplayOutput final : public FWorldSceneViewExtension
{
public:
    FSCViewerDisplayOutput(const FAutoRegister& Register, UWorld* World) : FWorldSceneViewExtension(Register, World) {}
    void SetupView(FSceneViewFamily& Family, FSceneView& View) override
    {
        SCLookLighting::ApplyDisplayOutput(Family, View, CVarNativeHDR.GetValueOnGameThread() != 0);
    }
};
}

TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe> SCLookLighting::InstallDisplayOutput(UWorld& World)
{
    return FSceneViewExtensions::NewExtension<FSCViewerDisplayOutput>(&World);
}

void SCLookLighting::ApplyDisplayOutput(const FSceneViewFamily& Family, FSceneView& View, bool bNativeHDR)
{
    // H2's fixed output devices are independent of the monitor, including when the viewport advertises HDR.
    if (!bNativeHDR || View.bIsSceneCapture || Family.SceneCaptureSource == SCS_FinalToneCurveHDR ||
        Family.SceneCaptureSource == SCS_FinalColorHDR || !Family.RenderTarget || !Family.RenderTarget->GetSceneHDREnabled() ||
        View.FinalPostProcessSettings.TonemappingMethod != ETonemappingMethod::HuePreservingShoulder) return;

    switch (Family.RenderTarget->GetDisplayOutputFormat())
    {
    case EDisplayOutputFormat::HDR_ACES_1000nit_ST2084:
    case EDisplayOutputFormat::HDR_ACES_2000nit_ST2084:
    case EDisplayOutputFormat::HDR_ACES_1000nit_ScRGB:
    case EDisplayOutputFormat::HDR_ACES_2000nit_ScRGB:
        // Standard ACES consumes the graded scene signal, the target's actual peak and its paper white.
        // The game's SDR shoulder saturates before UE can expand the signal for the display.
        View.FinalPostProcessSettings.TonemappingMethod = ETonemappingMethod::StandardACES;
        break;
    default:
        break;
    }
}

void SCLookLighting::MigrateLookSettings()
{
    USCViewerSettings* Settings = USCViewerSettings::Get();
    if (!Settings->MigrateLook()) return;
    Settings->Save();
    UE_LOG(LogSCLookLighting, Display, TEXT("LOOK settings migrated to look revision %d (star_bloom=%.2f)"), Settings->LookRevision,
           Settings->StarBloom);
}

void SCLookLighting::ApplyDefaults(UWorld* World)
{
    if (!World) return;
    USCViewerSettings* Settings = USCViewerSettings::Get();
    MigrateLookSettings(); // normally already done before the viewer volume was installed (ASCViewerGameMode::StartPlay)
    if (Settings->StarBloom > 0) SCLook::SetBloomStar(World, 4, 0.5f, Settings->StarBloom);
    int32 Volumes = 0;
    for (TActorIterator<APostProcessVolume> It(World); It; ++It)
    {
        if (!It->bUnbound) continue;
        FPostProcessSettings& S = It->Settings;
        if (Settings->StarBloom <= 0)
        {
            S.bOverride_BloomIntensity = true;
            S.BloomIntensity = Settings->BloomIntensity;
            S.bOverride_BloomThreshold = true;
            S.BloomThreshold = Settings->BloomThreshold;
        }
        S.bOverride_AutoExposureBias = true;
        S.AutoExposureBias = Settings->ExposureBias;
        ++Volumes;
    }
    UE_LOG(LogSCLookLighting, Display, TEXT("LOOK defaults star_bloom=%.2f bloom=%.2f threshold=%.2f exposure_bias=%.2f volumes=%d"),
           Settings->StarBloom, Settings->BloomIntensity, Settings->BloomThreshold, Settings->ExposureBias, Volumes);
}

namespace
{
TMap<TWeakObjectPtr<ULightComponent>, float> GFloorWashOriginal;
TSet<TWeakObjectPtr<ULightComponent>> GScaledFloorWash;

/** Unit directions of the light's emission in its own frame (+X forward), with weights: a spot fills its outer cone
 *  (falloff to the edge), a rect light its cosine-weighted hemisphere. */
void EmissionSamples(const ULightComponent* Light, TArray<TPair<FVector, double>>& Out)
{
    constexpr int32 Rings = 8, PerRing = 16;
    const auto* Spot = Cast<USpotLightComponent>(Light);
    // The bounding cone: the outer cone, or the cone through a square frustum projector's corners (WP-E1 E1-3).
    const double MaxAngle = Spot ? FMath::Clamp(double(Spot->GetHalfConeAngle()), FMath::DegreesToRadians(1.0), FMath::DegreesToRadians(89.0))
                                 : FMath::DegreesToRadians(85.0);
    Out.Add({FVector::ForwardVector, 1.0});
    for (int32 R = 1; R <= Rings; ++R)
    {
        const double Theta = MaxAngle * R / Rings;
        const double Weight = Spot ? FMath::Max(0.0, 1.0 - double(R - 1) / Rings) : FMath::Cos(Theta);
        for (int32 K = 0; K < PerRing; ++K)
        {
            const double Phi = 2 * PI * (K + 0.5 * (R % 2)) / PerRing;
            Out.Add({FVector(FMath::Cos(Theta), FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi)),
                     Weight * FMath::Sin(Theta)}); // solid-angle weight of the ring
        }
    }
}
}

int32 SCLookLighting::SetPerimeterFloorWash(UWorld* World, float Scale)
{
    for (const auto& [Light, Intensity] : GFloorWashOriginal)
        if (Light.IsValid()) Light->SetIntensity(Intensity);
    GFloorWashOriginal.Reset();
    for (const auto& WeakLight : GScaledFloorWash)
        if (ULightComponent* Light = WeakLight.Get())
            FSCLightImporter::SetIntensityScale(Cast<ASCVehicle>(Light->GetOwner()), Light, ESCLightIntensityScale::FloorWash, 1.f);
    GScaledFloorWash.Reset();
    USCViewerSubsystem* Viewer = World ? USCViewerSubsystem::Get(World) : nullptr;
    ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
    if (!Environment || FMath::IsNearlyEqual(Scale, 1.f)) { UE_LOG(LogSCLookLighting, Display, TEXT("LOOK floor_wash scale=1 lights=0")); return 0; }
    // The ship bay: the cooked hangar's measured elevator (centred under the world origin by placement), else none.
    const FString Folder = Viewer->GetEnvironmentRequest().Replace(TEXT("\\"), TEXT("/"));
    const USCHangarIndex* Index = USCHangarIndex::Load();
    const FSCHangarCapacity* Bay = Index ? Index->Find(FPaths::GetCleanFilename(FPaths::GetPath(Folder)), FPaths::GetBaseFilename(Folder)) : nullptr;
    if (!Bay) { UE_LOG(LogSCLookLighting, Warning, TEXT("LOOK floor_wash: the environment's ship bay is not measured (cooked hangars only)")); return 0; }
    const FVector2D Half = Bay->BayCm / 2;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(SCFloorWash), true);
    if (ASCVehicle* Ship = Viewer->GetVehicle()) Query.AddIgnoredActor(Ship);
    int32 Lights = 0, Considered = 0;
    TArray<TPair<FVector, double>> Samples;
    for (ULightComponent* Light : Environment->Lights)
    {
        if (!Light || !Light->IsVisible() || !(Light->IsA<USpotLightComponent>() || Light->IsA<URectLightComponent>())) continue;
        ++Considered;
        Samples.Reset();
        EmissionSamples(Light, Samples);
        const FTransform& T = Light->GetComponentTransform();
        const double Reach = FMath::Max(100.0, double(Cast<ULocalLightComponent>(Light)->AttenuationRadius));
        double Total = 0, FloorOutside = 0, Inside = 0;
        for (const auto& [Dir, Weight] : Samples)
        {
            const FVector From = T.GetLocation();
            const FVector To = From + T.TransformVectorNoScale(Dir) * Reach;
            FHitResult Hit;
            Total += Weight;
            if (!World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Query)) continue;
            const bool bInBay = FMath::Abs(Hit.ImpactPoint.X) <= Half.X && FMath::Abs(Hit.ImpactPoint.Y) <= Half.Y;
            if (bInBay) Inside += Weight;
            else if (Hit.ImpactNormal.Z > 0.9) FloorOutside += Weight;
        }
        if (Total <= 0 || FloorOutside / Total < 0.8 || Inside / Total > 0.1) continue;
        if (FSCLightImporter::SetIntensityScale(Environment, Light, ESCLightIntensityScale::FloorWash, Scale))
            GScaledFloorWash.Add(Light);
        else
        {
            GFloorWashOriginal.Add(Light, Light->Intensity);
            Light->SetIntensity(Light->Intensity * Scale);
        }
        ++Lights;
    }
    UE_LOG(LogSCLookLighting, Display, TEXT("LOOK floor_wash scale=%.2f lights=%d considered=%d bay_m=%.0fx%.0f"), Scale, Lights, Considered,
           Bay->BayCm.X / 100, Bay->BayCm.Y / 100);
    return Lights;
}

static FAutoConsoleCommandWithWorldAndArgs GSCLookPerimeterFloorWash(
    TEXT("sc.Look.PerimeterFloorWash"), TEXT("sc.Look.PerimeterFloorWash <scale>: scale environment lights that wash the floor outside the ship bay (1 restores)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SCLookLighting::SetPerimeterFloorWash(World, Args.IsValidIndex(0) ? FCString::Atof(*Args[0]) : 1.f);
    }));
