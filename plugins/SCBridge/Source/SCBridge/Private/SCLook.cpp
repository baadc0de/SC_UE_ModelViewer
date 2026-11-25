// Look tuning (2026-11-12, plan D15): runtime A/B of post-process settings on the viewer's unbound volume(s).
// sc.Look.PP <FPostProcessSettings field> <value>: sets the field and its bOverride_ flag (numbers and bools).
#include "SCLook.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"
#include "Scalability.h"
#include "RenderUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "SCVehicle.h"
#include "SCLights.h"
#include "SCPlayerBody.h"
#include "SCViewerPawn.h"
#include "SCViewerSubsystem.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Misc/Paths.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "SCViewerSettings.h"
#include "SCMaterialBuilder.h"
#include "SCViewerExposure.h"
#include "SCStarEngine.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "TextureResource.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "Containers/Ticker.h"
#include "AutoExposureConversion.h"

static FAutoConsoleCommandWithWorldAndArgs GSCLookPP(
    TEXT("sc.Look.PP"), TEXT("sc.Look.PP <PostProcessSettings field> <value>: set it (and its override) on every unbound post-process volume"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (Args.Num() < 2) return;
        UScriptStruct* Struct = FPostProcessSettings::StaticStruct();
        FProperty* Field = Struct->FindPropertyByName(FName(*Args[0]));
        FBoolProperty* Override = CastField<FBoolProperty>(Struct->FindPropertyByName(FName(*(TEXT("bOverride_") + Args[0]))));
        int32 Volumes = 0;
        for (TActorIterator<APostProcessVolume> It(World); It && Field; ++It)
        {
            if (!It->bUnbound) continue;
            void* Settings = &It->Settings;
            if (FNumericProperty* Number = CastField<FNumericProperty>(Field))
            {
                if (Number->IsFloatingPoint()) Number->SetFloatingPointPropertyValue(Number->ContainerPtrToValuePtr<void>(Settings), FCString::Atod(*Args[1]));
                else Number->SetIntPropertyValue(Number->ContainerPtrToValuePtr<void>(Settings), (int64)FCString::Atoi64(*Args[1]));
            }
            else if (FBoolProperty* Bool = CastField<FBoolProperty>(Field)) Bool->SetPropertyValue_InContainer(Settings, FCString::Atoi(*Args[1]) != 0);
            else if (FByteProperty* Byte = CastField<FByteProperty>(Field)) Byte->SetIntPropertyValue(Byte->ContainerPtrToValuePtr<void>(Settings), (int64)FCString::Atoi(*Args[1]));
            else if (FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Field); Object && Args[1] == TEXT("none"))
            {
                Object->SetObjectPropertyValue_InContainer(Settings, nullptr);
                if (Override) Override->SetPropertyValue_InContainer(Settings, false);
                ++Volumes;
                continue;
            }
            else if (FEnumProperty* Enum = CastField<FEnumProperty>(Field))
                Enum->GetUnderlyingProperty()->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Settings), (int64)FCString::Atoi(*Args[1]));
            else continue;
            if (Override) Override->SetPropertyValue_InContainer(Settings, true);
            ++Volumes;
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK pp %s=%s volumes=%d%s"), *Args[0], *Args[1], Volumes, Field ? TEXT("") : TEXT(" (no such field)"));
    }));

// sc.Look.Channels <0|1>: 0 = every ship light on all lighting channels (no WP-L1 room confinement), 1 = restore.
static TMap<TWeakObjectPtr<ULightComponent>, FLightingChannels> GSavedChannels;
static FAutoConsoleCommandWithWorldAndArgs GSCLookChannels(
    TEXT("sc.Look.Channels"), TEXT("sc.Look.Channels <0|1>: 0 = ship lights on every lighting channel (no room confinement), 1 = restore"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const bool bConfine = Args.IsValidIndex(0) && FCString::Atoi(*Args[0]) != 0;
        int32 Lights = 0;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
            for (ULightComponent* Light : It->Lights)
            {
                if (!Light) continue;
                if (!bConfine)
                {
                    if (!GSavedChannels.Contains(Light)) GSavedChannels.Add(Light, Light->LightingChannels);
                    Light->SetLightingChannels(true, true, true);
                }
                else if (const FLightingChannels* Saved = GSavedChannels.Find(Light))
                    Light->SetLightingChannels(Saved->bChannel0, Saved->bChannel1, Saved->bChannel2);
                ++Lights;
            }
        if (bConfine) GSavedChannels.Reset();
        UE_LOG(LogTemp, Display, TEXT("LOOK channels confine=%d lights=%d"), bConfine, Lights);
    }));

// sc.Look.WallLights <scale>: the environment's wall-facing fixtures (spot/rect lights aimed within 30 degrees of
// horizontal) at <scale> x their imported intensity; overhead keys (aimed down) and point lights are untouched.
static TMap<TWeakObjectPtr<ULightComponent>, float> GSavedWallIntensity;
static FAutoConsoleCommandWithWorldAndArgs GSCLookWallLights(
    TEXT("sc.Look.WallLights"), TEXT("sc.Look.WallLights <scale>: scale the environment's horizontally aimed spot/rect lights"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
        ASCVehicle* Env = Viewer ? Viewer->GetEnvironment() : nullptr;
        if (!Env || !Args.IsValidIndex(0)) return;
        const float Scale = FCString::Atof(*Args[0]);
        int32 Wall = 0, Other = 0;
        for (ULightComponent* Light : Env->Lights)
        {
            if (!Light || !(Light->IsA<USpotLightComponent>() || Light->IsA<URectLightComponent>())) continue;
            if (FMath::Abs(Light->GetForwardVector().Z) >= 0.5) { ++Other; continue; } // aimed down (or up): not a wall wash
            if (!FSCLights::SetIntensityScale(Env, Light, ESCLightIntensityScale::Wall, Scale))
            {
                const float Base = GSavedWallIntensity.FindOrAdd(Light, Light->Intensity);
                Light->SetIntensity(Base * Scale);
            }
            ++Wall;
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK wall_lights scale=%.2f wall=%d other_directional=%d"), Scale, Wall, Other);
    }));

// sc.Look.Gobo <name part> <0|1>: drop (0) or restore (1) the light function of every light whose gobo texture name
// contains <name part> (diagnostic: which projector texture produces a pattern).
static TMap<TWeakObjectPtr<ULightComponent>, TWeakObjectPtr<UMaterialInterface>> GSavedGobo;
static FAutoConsoleCommandWithWorldAndArgs GSCLookGobo(
    TEXT("sc.Look.Gobo"), TEXT("sc.Look.Gobo <texture name part> <0|1>: drop or restore matching light functions"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (Args.Num() < 2) return;
        const bool bOn = FCString::Atoi(*Args[1]) != 0;
        int32 Matched = 0;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
            for (ULightComponent* Light : It->Lights)
            {
                if (!Light) continue;
                if (bOn)
                {
                    if (const auto* Saved = GSavedGobo.Find(Light); Saved && Saved->IsValid())
                    {
                        Light->SetLightFunctionMaterial(Saved->Get());
                        GSavedGobo.Remove(Light);
                        ++Matched;
                    }
                    continue;
                }
                UTexture* Gobo = nullptr;
                const auto* MID = Cast<UMaterialInstanceDynamic>(Light->LightFunctionMaterial);
                if (!MID || !MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Gobo")), Gobo, true) || !Gobo ||
                    !Gobo->GetName().Contains(Args[0]))
                    continue;
                GSavedGobo.Add(Light, Light->LightFunctionMaterial);
                Light->SetLightFunctionMaterial(nullptr);
                ++Matched;
            }
        UE_LOG(LogTemp, Display, TEXT("LOOK gobo part=%s on=%d lights=%d"), *Args[0], bOn, Matched);
    }));

// sc.Look.Display <game|ue>: the game's display curve as the engine's tone curve with its fidelity baseline (FSCDisplayCurve), or
// UE's tonemapper as the project and the map set it (Filmic by default, with UE's lens vignette).
static FAutoConsoleCommandWithWorldAndArgs GSCLookDisplay(
    TEXT("sc.Look.Display"), TEXT("sc.Look.Display <game|ue>: the game's display curve (engine tone curve) or UE's tonemapper"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const bool bGame = !(Args.IsValidIndex(0) && Args[0] == TEXT("ue"));
        const FSCDisplayCurve Display = FSCDisplayCurve::From(FSCStarEngine::Get());
        int32 Changed = 0;
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
            if (It->Tags.Contains(SCViewerExposure::VolumeTag))
            {
                Display.Apply(It->Settings, bGame);
                ++Changed;
            }
        UE_LOG(LogTemp, Display, TEXT("LOOK display=%s tone_curve=%s volumes=%d"), bGame ? TEXT("game") : TEXT("ue"),
               bGame && Display.bSupported ? TEXT("HuePreservingShoulder") : TEXT("project"), Changed);
    }));

// sc.Look.ExposureLog [seconds]: the first local player's eye adaptation as the renderer read it back (exposure scale, metered
// average luminance, pre-exposure): one LOOK_EXPOSURE line now, or with <seconds> one LOOK_EXPOSURE_TRACE line per frame for that long
// (diagnostics for adaptation runs; the readback trails the GPU by a few frames).
namespace
{
FSceneViewStateInterface* SCFirstViewState(UWorld* World)
{
    // The player's view state as its scene views get it (ULocalPlayer keeps ViewStates private).
    ULocalPlayer* Player = World ? World->GetFirstLocalPlayerFromController() : nullptr;
    FViewport* Viewport = Player && Player->ViewportClient ? Player->ViewportClient->Viewport : nullptr;
    FSceneViewInitOptions Options;
    return Viewport && Player->CalcSceneViewInitOptions(Options, Viewport) ? Options.SceneViewStateInterface : nullptr;
}
FString SCExposureFields(const FSceneViewStateInterface& State)
{
    // One coherent copy of the fork's readback (WP-E1 E1-5; the render thread publishes it under a lock): exposure, metered luminance
    // and the meter state (log2 reflected / incident meter, slow / fast stage of the two-stage adaptation) from the same frame. The view
    // state's pre-exposure is not in the trace (E1-5 review 2: a render-thread value read here unsynchronised, from another frame).
    const UE::AutoExposure::FEyeAdaptationReadback R = State.GetLastAutoExposureReadback();
    const FVector4f& Meter = R.MeterState;
    const auto Lane = [](float V) { return UE::AutoExposure::IsMeterStateValid(V) ? FString::Printf(TEXT("%.4f"), V) : FString(TEXT("none")); };
    // WP-E1 E1-9: the unclamped exposure scale the screens' DBA term reads (EyeAdaptationUnclamped), from the same readback.
    return FString::Printf(TEXT("exposure=%.6g log2_exposure=%.4f metered=%.6g log2_reflected=%s log2_incident=%s slow=%s fast=%s unclamped=%.6g"), R.Exposure,
                           R.Exposure > 0 ? FMath::Log2(R.Exposure) : 0.f, R.AverageSceneLuminance,
                           *Lane(Meter.X), *Lane(Meter.Y), *Lane(Meter.Z), *Lane(Meter.W), R.UnclampedExposure);
}
}
static FAutoConsoleCommandWithWorldAndArgs GSCLookExposureLog(
    TEXT("sc.Look.ExposureLog"), TEXT("sc.Look.ExposureLog [seconds]: log the eye adaptation readback now, or every frame for <seconds>"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const FSceneViewStateInterface* State = SCFirstViewState(World);
        if (!State) { UE_LOG(LogTemp, Warning, TEXT("LOOK_EXPOSURE no view state")); return; }
        UE_LOG(LogTemp, Display, TEXT("LOOK_EXPOSURE %s"), *SCExposureFields(*State));
        const double Seconds = Args.IsValidIndex(0) ? FCString::Atod(*Args[0]) : 0.0;
        if (Seconds <= 0) return;
        const double Start = FPlatformTime::Seconds();
        TWeakObjectPtr<UWorld> WeakWorld(World);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld, Start, Seconds](float) {
            const FSceneViewStateInterface* S = SCFirstViewState(WeakWorld.Get());
            const double T = FPlatformTime::Seconds() - Start;
            if (S) UE_LOG(LogTemp, Display, TEXT("LOOK_EXPOSURE_TRACE t=%.4f %s"), T, *SCExposureFields(*S));
            return S && T < Seconds;
        }));
    }));

// sc.Look.Exposure <game|ue> [<game|ue>]: the game's meter (r_exposureMeterMode: the clamped blend of the reflected and incident meters)
// and its pupil / pigment adaptation as the engine's two-stage adaptation (game), or UE's reflected meter and one-stage adaptation at
// the pupil's speed (ue, the mapping before WP-E1 E1-5). The first argument sets the meter, the second the adaptation (default: the
// first). The compensation curve, brightness bounds and percentiles stay the game's either way.
static FAutoConsoleCommandWithWorldAndArgs GSCLookExposure(
    TEXT("sc.Look.Exposure"), TEXT("sc.Look.Exposure <game|ue> [<game|ue>]: meter, then adaptation (default: same) the game's or UE's"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const bool bGameMeter = !(Args.IsValidIndex(0) && Args[0] == TEXT("ue"));
        const bool bGameAdaptation = Args.IsValidIndex(1) ? Args[1] != TEXT("ue") : bGameMeter;
        const FSCExposureSettings E = FSCExposureSettings::From(FSCStarEngine::Get(), FSCEngineExposure::FromEngine());
        int32 Changed = 0;
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
            if (It->Tags.Contains(SCViewerExposure::VolumeTag))
            {
                E.Apply(It->Settings, bGameMeter, bGameAdaptation);
                ++Changed;
            }
        UE_LOG(LogTemp, Display, TEXT("LOOK exposure meter=%s adaptation=%s volumes=%d"),
               bGameMeter ? *StaticEnum<EAutoExposureMeter>()->GetNameStringByValue(int64(E.Meter)) : TEXT("Reflected(ue)"),
               bGameAdaptation ? TEXT("TwoStage") : TEXT("Default(ue)"), Changed);
    }));

// sc.Look.SSDO <game|off>: the game's screen-space directional occlusion (fixture radius, strength and amounts on the engine fork's SSDO,
// WP-E1 E1-7; it replaces Lumen's short-range AO term by term) or none (the look, D15). Also the setting later volumes are installed with.
static FAutoConsoleCommandWithWorldAndArgs GSCLookSSDO(
    TEXT("sc.Look.SSDO"), TEXT("sc.Look.SSDO <game|off>: the game's screen-space directional occlusion on the viewer volume, or none (the look)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const bool bGame = Args.IsValidIndex(0) && (Args[0] == TEXT("game") || Args[0] == TEXT("1"));
        USCViewerSettings::Get()->bStarEngineSSDO = bGame;
        const FSCSSDOSettings S = FSCSSDOSettings::From(FSCStarEngine::Get());
        int32 Changed = 0;
        // E1-7 review: a console dispatch without a world (TActorIterator asserts on null) only changes the setting future volumes take.
        if (World)
            for (TActorIterator<APostProcessVolume> It(World); It; ++It)
                if (It->Tags.Contains(SCViewerExposure::VolumeTag))
                {
                    S.Apply(It->Settings, bGame);
                    ++Changed;
                }
        UE_LOG(LogTemp, Display, TEXT("LOOK ssdo=%s supported=%d radius=%.1f strength=%.2f direct=%.2f ambient=%.2f reflection=%.2f volumes=%d"),
               bGame && S.bSupported ? TEXT("game") : TEXT("off"), S.bSupported, S.Radius, S.Strength, S.DirectAmount, S.AmbientAmount,
               S.ReflectionAmount, Changed);
    }));

// sc.Look.BloomStar <points> <length> [intensity]: FFT (convolution) bloom with a generated kernel, a soft core plus
// <points> diffraction streaks of <length> (fraction of the kernel radius); 0 points returns to the standard bloom.
static TStrongObjectPtr<UTexture2D> GBloomKernel;
void SCLook::SetBloomStar(UWorld* World, int32 Points, float Length, float Intensity)
{
    if (!World) return;
    if (Points > 0)
    {
        constexpr int32 N = 512;
        TArray<FFloat16Color> Pixels;
        Pixels.SetNumUninitialized(N * N);
        const int32 Streaks = Points % 2 ? Points * 2 : Points; // an odd star still yields symmetric spikes
        for (int32 Y = 0; Y < N; ++Y)
            for (int32 X = 0; X < N; ++X)
            {
                const float U = (X + 0.5f) / N * 2.f - 1.f, V = (Y + 0.5f) / N * 2.f - 1.f, R = FMath::Sqrt(U * U + V * V);
                float K = FMath::Exp(-R * R / 0.00002f) * 40.f + FMath::Exp(-R * R / 0.01f) * 0.01f; // sharp peak (the image) + faint glow
                for (int32 I = 0; I < Streaks; ++I)
                {
                    const float A = PI * I / (Streaks / 2.f) * 0.5f + PI / 4.f; // diagonal by default
                    const float Along = U * FMath::Cos(A) + V * FMath::Sin(A), Across = -U * FMath::Sin(A) + V * FMath::Cos(A);
                    if (Along > 0) K += 0.05f * FMath::Exp(-Across * Across / 0.000004f) * FMath::Exp(-Along / FMath::Max(0.02f, Length * 0.25f));
                }
                Pixels[Y * N + X] = FFloat16Color(FLinearColor(K, K, K, 1));
            }
        GBloomKernel.Reset(UTexture2D::CreateTransient(N, N, PF_FloatRGBA));
        void* Data = GBloomKernel->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FFloat16Color));
        GBloomKernel->GetPlatformData()->Mips[0].BulkData.Unlock();
        GBloomKernel->SRGB = false;
        GBloomKernel->UpdateResource();
    }
    int32 Volumes = 0;
    for (TActorIterator<APostProcessVolume> It(World); It; ++It)
    {
        if (!It->bUnbound) continue;
        FPostProcessSettings& S = It->Settings;
        S.bOverride_BloomMethod = Points > 0;
        S.BloomMethod = Points > 0 ? BM_FFT : BM_SOG;
        S.bOverride_BloomConvolutionTexture = Points > 0;
        S.BloomConvolutionTexture = Points > 0 ? GBloomKernel.Get() : nullptr;
        S.bOverride_BloomIntensity = Points > 0;
        S.BloomIntensity = Intensity;
        ++Volumes;
    }
    UE_LOG(LogTemp, Display, TEXT("LOOK bloom_star points=%d length=%.2f intensity=%.2f volumes=%d"), Points, Length, Intensity, Volumes);
}
static FAutoConsoleCommandWithWorldAndArgs GSCLookBloomStar(
    TEXT("sc.Look.BloomStar"), TEXT("sc.Look.BloomStar <points (0 = standard bloom)> <streak length 0..1> [intensity]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SCLook::SetBloomStar(World, Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) : 0, Args.IsValidIndex(1) ? FCString::Atof(*Args[1]) : 0.5f,
                             Args.IsValidIndex(2) ? FCString::Atof(*Args[2]) : 0.05f);
    }));

int64 SCLook::RayTracingSegments(UWorld* World, int32* OutPrimitives)
{
    int64 Segments = 0;
    int32 Primitives = 0;
    if (World)
        for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
        {
            const UPrimitiveComponent* P = *It;
            if (P->GetWorld() != World || !P->IsRegistered() || !P->bVisibleInRayTracing) continue;
            // Hidden primitives stay in the ray-tracing scene only when they feed indirect lighting (Lumen proxies).
            if ((!P->IsVisible() || P->bHiddenInGame) && !P->bAffectIndirectLightingWhileHidden) continue;
            int32 Sections = 0;
            if (const auto* Mesh = Cast<UStaticMeshComponent>(P))
            {
                const UStaticMesh* Asset = Mesh->GetStaticMesh();
                const FStaticMeshRenderData* Data = Asset ? Asset->GetRenderData() : nullptr;
                Sections = Data && !Data->LODResources.IsEmpty() ? Data->LODResources[0].Sections.Num() : 0;
            }
            else if (const auto* Skinned = Cast<USkinnedMeshComponent>(P))
            {
                const FSkeletalMeshRenderData* Data = Skinned->GetSkinnedAsset() ? Skinned->GetSkinnedAsset()->GetResourceForRendering() : nullptr;
                Sections = Data && !Data->LODRenderData.IsEmpty() ? Data->LODRenderData[0].RenderSections.Num() : 0;
            }
            if (Sections == 0) continue;
            Segments += Sections;
            ++Primitives;
        }
    if (OutPrimitives) *OutPrimitives = Primitives;
    return Segments;
}
void SCLook::ApplyMaxQuality()
{
    Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
    Levels.GlobalIlluminationQuality = 4; // Cinematic
    Levels.ReflectionQuality = 4;
    Scalability::SetQualityLevels(Levels);
    IConsoleManager& Console = IConsoleManager::Get();
    auto Set = [&Console](const TCHAR* Name, int32 Value) {
        if (IConsoleVariable* Var = Console.FindConsoleVariable(Name)) Var->Set(Value, ECVF_SetByCode);
    };
    Set(TEXT("r.Lumen.HardwareRayTracing.LightingMode"), 1);           // hit lighting for GI and reflections
    Set(TEXT("r.Lumen.HardwareRayTracing.HitLighting.ShadowMode"), 2); // area shadows at hits
    auto Get = [&Console](const TCHAR* Name) {
        const IConsoleVariable* Var = Console.FindConsoleVariable(Name);
        return Var ? Var->GetString() : FString(TEXT("-"));
    };
    const FString Report = FString::Printf(
        TEXT("LOOK max_quality gi=%s reflections=%s lumen_hwrt=%s lighting_mode=%s hit_shadow_mode=%s probe_downsample=%s nanite=%s dlss=%s screen_percentage=%s aa=%s"),
        *Get(TEXT("sg.GlobalIlluminationQuality")), *Get(TEXT("sg.ReflectionQuality")), *Get(TEXT("r.Lumen.HardwareRayTracing")),
        *Get(TEXT("r.Lumen.HardwareRayTracing.LightingMode")), *Get(TEXT("r.Lumen.HardwareRayTracing.HitLighting.ShadowMode")),
        *Get(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor")), *Get(TEXT("r.Nanite")), *Get(TEXT("r.NGX.DLSS.Enable")),
        *Get(TEXT("r.ScreenPercentage")), *Get(TEXT("r.AntiAliasingMethod")));
    UE_LOG(LogTemp, Display, TEXT("%s"), *Report);
    // Shipping keeps no log: -SCQualityReport=<file> writes the line for checks of a packaged build.
    FString ReportFile;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCQualityReport="), ReportFile)) FFileHelper::SaveStringToFile(Report + TEXT("\n"), *ReportFile);
}

void SCLook::ApplyBasicLighting()
{
    if (IsRayTracingEnabled()) return;
    IConsoleManager& Console = IConsoleManager::Get();
    auto Set = [&Console](const TCHAR* Name, float Value) {
        if (IConsoleVariable* Var = Console.FindConsoleVariable(Name)) Var->Set(Value, ECVF_SetByCode);
    };
    // Every light keeps its virtual shadow map; up to 32 lights per pixel get filtered shadows (the engine's maximum; 16
    // overflowed in dense hangar views and left the lights beyond them unshadowed there).
    Set(TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel"), 32);
    // Dynamic resolution holds 60 fps on the mid-range GPUs this path serves, down to half resolution (TSR upsamples); a
    // GPU that holds 60 fps renders at full resolution.
    Set(TEXT("r.DynamicRes.OperationMode"), 2);
    Set(TEXT("r.DynamicRes.MinScreenPercentage"), 50);
    Set(TEXT("r.DynamicRes.MaxScreenPercentage"), 100);
    Set(TEXT("r.DynamicRes.FrameTimeBudget"), 1000.f / 60.f);
    UE_LOG(LogTemp, Display, TEXT("LOOK basic_lighting vsm_lights_per_pixel=32 dynamic_res=60fps min_screen_percentage=50"));
}

static int32 GShortRangeAOOverride = -1; // sc.Look.ShortRangeAO: -1 auto (budget), 0 screen space, 1 traced
void SCLook::UpdateShortRangeAO(UWorld* World)
{
    int32 Primitives = 0;
    const int64 Segments = RayTracingSegments(World, &Primitives);
    const int32 Budget = USCViewerSettings::Get()->TracedShortRangeAOMaxSegments;
    const bool bTraced = GShortRangeAOOverride >= 0 ? GShortRangeAOOverride != 0 : (Budget < 0 || Segments <= Budget);
    if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing")))
        Var->Set(bTraced ? 1 : 0, ECVF_SetByCode);
    UE_LOG(LogTemp, Display, TEXT("LOOK short_range_ao=%s rt_segments=%lld rt_primitives=%d budget=%d override=%d"), bTraced ? TEXT("traced") : TEXT("screen"),
           Segments, Primitives, Budget, GShortRangeAOOverride);
}
static FAutoConsoleCommandWithWorldAndArgs GSCLookShortRangeAO(
    TEXT("sc.Look.ShortRangeAO"), TEXT("sc.Look.ShortRangeAO [auto|0|1]: short-range AO by ray-tracing budget, screen space, or traced"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (Args.IsValidIndex(0)) GShortRangeAOOverride = Args[0] == TEXT("auto") ? -1 : FCString::Atoi(*Args[0]) != 0;
        SCLook::UpdateShortRangeAO(World);
    }));

// sc.Look.Focus <f-stop> [sensor width mm]: cinematic depth of field focused on what the screen centre sees (a trace
// along the view); f-stop 0 turns depth of field off.
static FAutoConsoleCommandWithWorldAndArgs GSCLookFocus(
    TEXT("sc.Look.Focus"), TEXT("sc.Look.Focus <f-stop (0 = off)> [sensor width mm]: depth of field focused at the screen centre"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!PC || !PC->PlayerCameraManager || !Args.IsValidIndex(0)) return;
        const float FStop = FCString::Atof(*Args[0]);
        const float Sensor = Args.IsValidIndex(1) ? FCString::Atof(*Args[1]) : 36.f;
        // From 30 cm ahead of the camera (clear of a first-person capsule); the player's own body counts in third person.
        const FVector Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
        const FVector From = PC->PlayerCameraManager->GetCameraLocation() + Dir * 30.0;
        const FVector To = From + Dir * 100000.0;
        FHitResult Hit;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(SCLookFocus), true);
        const double Distance = World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Query) ? Hit.Distance : 5000.0;
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
        {
            if (!It->bUnbound) continue;
            FPostProcessSettings& S = It->Settings;
            S.bOverride_DepthOfFieldFstop = S.bOverride_DepthOfFieldFocalDistance = S.bOverride_DepthOfFieldSensorWidth = FStop > 0;
            S.bOverride_DepthOfFieldMinFstop = FStop > 0;
            S.DepthOfFieldFstop = FStop;
            S.DepthOfFieldMinFstop = FMath::Min(FStop, 1.2f);
            S.DepthOfFieldFocalDistance = FStop > 0 ? float(Distance) : 0.f;
            S.DepthOfFieldSensorWidth = Sensor;
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK focus fstop=%.2f sensor=%.0f distance=%.0f hit=%s"), FStop, Sensor, Distance, *GetNameSafe(Hit.GetActor()));
    }));

// sc.Look.Probe: the part at the screen centre, its material per slot and every parameter value (diagnostics).
static FAutoConsoleCommandWithWorldAndArgs GSCLookProbe(
    TEXT("sc.Look.Probe"), TEXT("sc.Look.Probe: log the part at the screen centre and its material parameters"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!PC || !PC->PlayerCameraManager) return;
        const FVector Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
        const FVector From = PC->PlayerCameraManager->GetCameraLocation();
        FHitResult Hit;
        if (!World->LineTraceSingleByChannel(Hit, From, From + Dir * 100000.0, ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(SCLookProbe), true)))
            return;
        UPrimitiveComponent* C = Hit.GetComponent();
        const auto* SM = Cast<UStaticMeshComponent>(C);
        UE_LOG(LogTemp, Display, TEXT("PROBE at=%s comp=%s mesh=%s"), *Hit.ImpactPoint.ToCompactString(), *GetNameSafe(C),
               SM ? *GetNameSafe(SM->GetStaticMesh()) : TEXT("-"));
        for (int32 Slot = 0; C && Slot < C->GetNumMaterials(); ++Slot)
        {
            UMaterialInterface* M = C->GetMaterial(Slot);
            const auto* MI = Cast<UMaterialInstance>(M);
            UE_LOG(LogTemp, Display, TEXT("PROBE slot=%d material=%s parent=%s"), Slot, *GetNameSafe(M), MI ? *GetNameSafe(MI->Parent) : TEXT("-"));
            if (!MI) continue;
            for (const FScalarParameterValue& P : MI->ScalarParameterValues)
                UE_LOG(LogTemp, Display, TEXT("PROBE   s %s=%.4f"), *P.ParameterInfo.Name.ToString(), P.ParameterValue);
            for (const FVectorParameterValue& P : MI->VectorParameterValues)
                UE_LOG(LogTemp, Display, TEXT("PROBE   v %s=%s"), *P.ParameterInfo.Name.ToString(), *P.ParameterValue.ToString());
            for (const FTextureParameterValue& P : MI->TextureParameterValues)
                UE_LOG(LogTemp, Display, TEXT("PROBE   t %s=%s srgb=%d"), *P.ParameterInfo.Name.ToString(), *GetNameSafe(P.ParameterValue),
                       P.ParameterValue ? int32(P.ParameterValue->SRGB) : -1);
        }
    }));

// sc.Look.Sky <intensity>: every sky light's intensity, then a fresh capture.
static FAutoConsoleCommandWithWorldAndArgs GSCLookSky(
    TEXT("sc.Look.Sky"), TEXT("sc.Look.Sky <intensity>: set every sky light's intensity and recapture"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        int32 Count = 0;
        for (TObjectIterator<USkyLightComponent> It; It; ++It)
            if (It->GetWorld() == World)
            {
                if (Args.IsValidIndex(0)) It->SetIntensity(FCString::Atof(*Args[0]));
                It->RecaptureSky();
                ++Count;
            }
        UE_LOG(LogTemp, Display, TEXT("LOOK sky intensity=%s lights=%d"), Args.IsValidIndex(0) ? *Args[0] : TEXT("-"), Count);
    }));

static bool IsGlassMaterial(const UMaterialInterface* M) { return M && GetNameSafe(M->GetMaterial()).Contains(TEXT("Glass")); }
/** The look's glass reflectance also reaches the screens' glass face (M_SC_Screen, WP-I1). */
static bool TakesGlassLook(const UMaterialInterface* M) { return IsGlassMaterial(M) || FSCMaterialBuilder::IsScreenMaster(M); }

// sc.Look.HideGlass <0|1> [<mesh name part>]: hide the vehicle's components drawn with a glass master material, or only
// those whose mesh name contains the part (A/B through-glass views).
static FAutoConsoleCommandWithWorldAndArgs GSCLookHideGlass(
    TEXT("sc.Look.HideGlass"), TEXT("sc.Look.HideGlass <0|1>: hide components drawn with a glass material"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const bool bHide = !Args.IsValidIndex(0) || FCString::Atoi(*Args[0]) != 0;
        int32 Hidden = 0;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            TArray<UStaticMeshComponent*> Components;
            It->GetComponents(Components);
            for (UStaticMeshComponent* C : Components)
            {
                bool bGlass = false;
                for (int32 I = 0; I < C->GetNumMaterials() && !bGlass; ++I)
                    bGlass = IsGlassMaterial(C->GetMaterial(I));
                if (!bGlass || (Args.IsValidIndex(1) && !GetNameSafe(C->GetStaticMesh()).Contains(Args[1]))) continue;
                C->SetHiddenInGame(bHide);
                ++Hidden;
            }
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK glass hidden=%d components=%d"), bHide, Hidden);
    }));

// sc.Look.GlassRay: the vehicle components drawn with a glass material whose bounds the screen-centre ray crosses
// (glass has no collision, so sc.Look.Probe cannot hit it), nearest first, with each glass slot's parameters.
static FAutoConsoleCommandWithWorldAndArgs GSCLookGlassRay(
    TEXT("sc.Look.GlassRay"), TEXT("sc.Look.GlassRay: log the glass parts on the screen-centre ray and their glass parameters"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!PC || !PC->PlayerCameraManager) return;
        const FVector Dir = PC->PlayerCameraManager->GetCameraRotation().Vector();
        const FVector From = PC->PlayerCameraManager->GetCameraLocation();
        TArray<TPair<double, UStaticMeshComponent*>> Hits;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            TArray<UStaticMeshComponent*> Components;
            It->GetComponents(Components);
            for (UStaticMeshComponent* C : Components)
            {
                bool bGlass = false;
                for (int32 I = 0; I < C->GetNumMaterials() && !bGlass; ++I) bGlass = IsGlassMaterial(C->GetMaterial(I));
                FVector HitPoint, HitNormal; float Time;
                const FBox Box = C->Bounds.GetBox();
                if (bGlass && C->IsVisible() && FMath::LineExtentBoxIntersection(Box, From, From + Dir * 100000.0, FVector::ZeroVector, HitPoint, HitNormal, Time))
                    Hits.Add({Box.IsInside(From) ? 0.0 : FVector::Dist(From, HitPoint), C});
            }
        }
        Hits.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
        for (const auto& [Distance, C] : Hits)
        {
            UE_LOG(LogTemp, Display, TEXT("GLASSRAY entry=%.0f comp=%s mesh=%s bounds=%s"), Distance, *GetNameSafe(C),
                   *GetNameSafe(C->GetStaticMesh()), *C->Bounds.GetBox().ToString());
            for (int32 Slot = 0; Slot < C->GetNumMaterials(); ++Slot)
            {
                const auto* MI = Cast<UMaterialInstance>(C->GetMaterial(Slot));
                if (!MI || !IsGlassMaterial(MI)) continue;
                UE_LOG(LogTemp, Display, TEXT("GLASSRAY   slot=%d material=%s"), Slot, *GetNameSafe(MI));
                for (const FScalarParameterValue& P : MI->ScalarParameterValues)
                    UE_LOG(LogTemp, Display, TEXT("GLASSRAY     s %s=%.5f"), *P.ParameterInfo.Name.ToString(), P.ParameterValue);
                for (const FVectorParameterValue& P : MI->VectorParameterValues)
                    UE_LOG(LogTemp, Display, TEXT("GLASSRAY     v %s=%s"), *P.ParameterInfo.Name.ToString(), *P.ParameterValue.ToString());
                for (const FTextureParameterValue& P : MI->TextureParameterValues)
                    UE_LOG(LogTemp, Display, TEXT("GLASSRAY     t %s=%s"), *P.ParameterInfo.Name.ToString(), *GetNameSafe(P.ParameterValue));
            }
        }
        UE_LOG(LogTemp, Display, TEXT("GLASSRAY parts=%d"), Hits.Num());
    }));

// sc.Look.GlassParam <name> <x> [<y> <z>] [@<instance name part>]: set a scalar (one value) or vector (three) parameter
// on every glass material instance of the vehicles, or on those whose name contains the part (A/B without a rebuild).
static FAutoConsoleCommandWithWorldAndArgs GSCLookGlassParam(
    TEXT("sc.Look.GlassParam"), TEXT("sc.Look.GlassParam <name> <x> [<y> <z>] [@<instance name part> | @kit]: set a glass material parameter on glass instances (@kit: only the first-person player kit's glass)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& InArgs, UWorld* World) {
        TArray<FString> Args = InArgs;
        const FString Filter = Args.Num() && Args.Last().StartsWith(TEXT("@")) ? Args.Pop().Mid(1) : FString();
        if (Args.Num() < 2) return;
        // @kit: only the first-person player kit's glass (weapon optics, visor; the Kit refraction profile's actors).
        const bool bKitOnly = Filter.Equals(TEXT("kit"), ESearchCase::IgnoreCase);
        const TArray<const AActor*> Kits = bKitOnly ? SCLook::PlayerKits(World) : TArray<const AActor*>();
        const FString NameFilter = bKitOnly ? FString() : Filter;
        TSet<UMaterialInstanceDynamic*> Done;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            if (bKitOnly && !Kits.Contains(*It)) continue;
            // Every primitive: static meshes and the skeletal parts of a player kit (weapon optics, visor).
            TArray<UPrimitiveComponent*> Components;
            It->GetComponents(Components);
            for (UPrimitiveComponent* C : Components)
                for (int32 I = 0; I < C->GetNumMaterials(); ++I)
                {
                    auto* MID = Cast<UMaterialInstanceDynamic>(C->GetMaterial(I));
                    if (!MID || !IsGlassMaterial(MID) || Done.Contains(MID) || !MID->GetName().Contains(NameFilter)) continue;
                    Done.Add(MID);
                    if (Args.Num() >= 4)
                        MID->SetVectorParameterValue(FName(*Args[0]), FLinearColor(FCString::Atof(*Args[1]), FCString::Atof(*Args[2]), FCString::Atof(*Args[3])));
                    else
                        MID->SetScalarParameterValue(FName(*Args[0]), FCString::Atof(*Args[1]));
                }
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK glass_param %s filter=%s instances=%d"), *FString::Join(Args, TEXT(" ")), *Filter, Done.Num());
    }));

// sc.Look.GlassReflectance [<scale> [<floor>]]: the look's glass reflectance (USCViewerSettings::GlassReflectanceScale /
// GlassReflectanceFloor: max(F0 x scale, floor where F0 > 0), clamped to 1 in M_SC_Glass), for this session: every glass instance
// the vehicles and environments built takes it, and so does every one built later. Constant copies of a cooked ship or hangar keep
// the values they were cooked with (counted, not changed). No arguments: logs the current values.
static FAutoConsoleCommandWithWorldAndArgs GSCLookGlassReflectance(
    TEXT("sc.Look.GlassReflectance"), TEXT("sc.Look.GlassReflectance [<scale> [<floor>]]: glass F0 = max(F0 x scale, floor) (1 0 = the game's reflectance) on every glass instance"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSettings* Settings = USCViewerSettings::Get();
        if (Args.IsValidIndex(0)) Settings->GlassReflectanceScale = FMath::Max(0.f, FCString::Atof(*Args[0]));
        if (Args.IsValidIndex(1)) Settings->GlassReflectanceFloor = FMath::Clamp(FCString::Atof(*Args[1]), 0.f, 1.f);
        int32 Set = 0, Constant = 0;
        TSet<const UMaterialInterface*> Seen;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            for (UMaterialInstanceDynamic* MID : It->Materials)
                if (MID && TakesGlassLook(MID) && !Seen.Contains(MID))
                {
                    Seen.Add(MID);
                    FSCMaterialBuilder::ApplyGlassLook(MID);
                    ++Set;
                }
            TArray<UPrimitiveComponent*> Components;
            It->GetComponents(Components);
            for (const UPrimitiveComponent* C : Components)
                for (int32 I = 0; I < C->GetNumMaterials(); ++I)
                {
                    const UMaterialInterface* M = C->GetMaterial(I);
                    if (!TakesGlassLook(M) || Seen.Contains(M)) continue;
                    Seen.Add(M);
                    if (!Cast<UMaterialInstanceDynamic>(M)) ++Constant;
                }
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK glass_reflectance scale=%.3f floor=%.4f instances=%d constant=%d"), Settings->GlassReflectanceScale,
               Settings->GlassReflectanceFloor, Set, Constant);
    }));

const SCLook::FGlassRefractionProfile& SCLook::GlassRefractionProfile(EGlassProfile Profile)
{
    switch (Profile)
    {
    case EGlassProfile::Canopy: return GlassProfileCanopy;
    case EGlassProfile::Kit: return GlassProfileKit;
    default: return GlassProfileDefault;
    }
}

SCLook::EGlassProfile SCLook::SelectGlassProfile(const UMaterialInterface* Glass, bool bOnPlayerKit)
{
    if (bOnPlayerKit) return EGlassProfile::Kit;
    // The game's F0 as authored (the reflectance floor is applied in the material, not on the parameter). An instance
    // without the parameter has the master's 0.04: not canopy.
    FLinearColor F0(0.04f, 0.04f, 0.04f);
    if (Glass) Glass->GetVectorParameterValue(FMaterialParameterInfo(TEXT("SpecularColor")), F0);
    return F0.R < 1e-6f ? EGlassProfile::Canopy : EGlassProfile::Default;
}

void SCLook::ApplyGlassProfile(UMaterialInstanceDynamic* MID, EGlassProfile Profile)
{
    static const FName Strength(TEXT("GlassRefractionStrength")), Dispersion(TEXT("GlassDispersion"));
    const FGlassRefractionProfile& P = GlassRefractionProfile(Profile);
    MID->SetScalarParameterValue(Strength, P.Strength);
    MID->SetScalarParameterValue(Dispersion, P.Dispersion);
}

TArray<const AActor*> SCLook::PlayerKits(UWorld* World)
{
    TArray<const AActor*> Kits;
    if (!World) return Kits;
    for (TObjectIterator<USCPlayerBodyComponent> It; It; ++It)
        if (It->GetWorld() == World && It->GetKit()) Kits.AddUnique(It->GetKit());
    return Kits;
}

int32 SCLook::SetGlassRefraction(UWorld* World, bool bOn)
{
    return SetGlassRefraction(World, bOn, PlayerKits(World));
}

int32 SCLook::SetGlassRefraction(UWorld* World, bool bOn, TConstArrayView<const AActor*> Kits, FGlassProfileCounts* OutCounts)
{
    USCViewerSettings::Get()->bGlassRefraction = bOn;
    int32 Set = 0, Converted = 0;
    FGlassProfileCounts Total, Actor;
    bool bKit = false;
    TSet<const UMaterialInterface*> Seen;
    auto Apply = [&](UMaterialInstanceDynamic* MID) {
        if (Seen.Contains(MID)) return;
        Seen.Add(MID);
        MID->SetScalarParameterValue(FSCMaterialBuilder::GlassRefractionLookParam, bOn ? 1.f : 0.f);
        ++Set;
        if (!bOn) return; // off: the profile values are inert (the offset is zero)
        const EGlassProfile Profile = SelectGlassProfile(MID, bKit);
        ApplyGlassProfile(MID, Profile);
        int32& N = Profile == EGlassProfile::Canopy ? Actor.Canopy : Profile == EGlassProfile::Kit ? Actor.Kit : Actor.Default;
        ++N;
    };
    if (World) // a console call without a world only records the setting
    for (TActorIterator<ASCVehicle> It(World); It; ++It)
    {
        bKit = Kits.Contains(*It);
        Actor = FGlassProfileCounts();
        for (UMaterialInstanceDynamic* MID : It->Materials)
            if (FSCMaterialBuilder::IsGlassMaster(MID)) Apply(MID);
        // A cooked ship or hangar carries constant copies of its glass instances (the master default: on, strength 1). Each gets one
        // dynamic instance over it (shared by every slot of the actor that used it), on or off: on, so it takes its profile; off (the
        // diagnostic), so it does not keep refracting at the master default.
        TMap<UMaterialInterface*, UMaterialInstanceDynamic*> Dynamic;
        TArray<UPrimitiveComponent*> Components;
        It->GetComponents(Components);
        for (UPrimitiveComponent* C : Components)
            for (int32 I = 0; I < C->GetNumMaterials(); ++I)
            {
                UMaterialInterface* M = C->GetMaterial(I);
                if (!FSCMaterialBuilder::IsGlassMaster(M)) continue;
                if (auto* MID = Cast<UMaterialInstanceDynamic>(M)) { Apply(MID); continue; }
                UMaterialInstanceDynamic*& Over = Dynamic.FindOrAdd(M);
                if (!Over)
                {
                    Over = UMaterialInstanceDynamic::Create(M, *It);
                    Over->SetFlags(RF_Transient);
                    ++Converted;
                }
                C->SetMaterial(I, Over);
                Apply(Over);
            }
        if (bOn && Actor.Canopy + Actor.Kit + Actor.Default > 0)
            UE_LOG(LogTemp, Display, TEXT("LOOK glass_refraction_actor actor=%s canopy=%d kit=%d default=%d"), *It->GetName(), Actor.Canopy,
                   Actor.Kit, Actor.Default);
        Total.Canopy += Actor.Canopy;
        Total.Kit += Actor.Kit;
        Total.Default += Actor.Default;
    }
    UE_LOG(LogTemp, Display, TEXT("LOOK glass_refraction on=%d instances=%d converted=%d canopy=%d kit=%d default=%d"), bOn, Set, Converted,
           Total.Canopy, Total.Kit, Total.Default);
    if (OutCounts) *OutCounts = Total;
    return Set;
}

void SCLook::RefreshGlassRefraction(UWorld* World)
{
    SetGlassRefraction(World, USCViewerSettings::Get()->bGlassRefraction);
}

// sc.Look.GlassRefraction [0|1]: the glass refraction look (UE's distortion pass moves the scene behind every M_SC_Glass instance;
// WP-U3b.md §14.10), on in the viewer's look; 0 switches it off for this session (diagnostics). No argument: logs the state.
static FAutoConsoleCommandWithWorldAndArgs GSCLookGlassRefraction(
    TEXT("sc.Look.GlassRefraction"), TEXT("sc.Look.GlassRefraction [0|1]: glass refracts what is behind it (on in the look; 0: off for this session, diagnostics; no argument: log the state)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (Args.IsValidIndex(0)) SCLook::SetGlassRefraction(World, FCString::Atoi(*Args[0]) != 0);
        else UE_LOG(LogTemp, Display, TEXT("LOOK glass_refraction on=%d"), USCViewerSettings::Get()->bGlassRefraction);
    }));

// sc.Screens [index [distance cm]] (WP-I1 verification): every screen face of the loaded vehicles and environments
// (sections drawn with a screen master, instanced parts included; geometry=unavailable when the cook dropped the CPU copy), logged as SCREEN_FACE with the image it shows ("dark" =
// none), its mesh, centre and facing; with an index, the current pawn is put `distance` (default 45 cm) in front of
// that face, looking at it (use the fly camera: a first-person capsule is pushed out of geometry).
static FAutoConsoleCommandWithWorldAndArgs GSCScreens(
    TEXT("sc.Screens"), TEXT("sc.Screens [index [distance cm]]: list the screen faces (M_SC_Screen), or view one from the front"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        struct FFace { FString Label; FVector Centre = FVector::ZeroVector, Normal = FVector::ZeroVector; bool bGeometry = false; };
        TArray<FFace> Faces;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            TArray<UStaticMeshComponent*> Components;
            It->GetComponents(Components);
            Components.Sort([](const UStaticMeshComponent& A, const UStaticMeshComponent& B) { return A.GetName() < B.GetName(); });
            for (UStaticMeshComponent* C : Components)
            {
                const UStaticMesh* Mesh = C->GetStaticMesh();
                const FStaticMeshRenderData* Render = Mesh ? Mesh->GetRenderData() : nullptr;
                if (!Mesh || !C->IsVisible() || !Render || Render->LODResources.IsEmpty()) continue;
                // A resident LOD whose CPU copy is still there (a cooked, streamed mesh may have dropped it, or not
                // streamed the high LODs in): positions, tangents with UV0 and the indices, all checked before use. The
                // LOD is held by a reference while it is read: stream-out first moves CurrentFirstLODIdx past the LODs it
                // releases, then discards their CPU data only once no external reference remains (StaticMeshUpdate).
                int32 LODIndex = INDEX_NONE;
                TRefCountPtr<const FStaticMeshLODResources> Held;
                for (int32 L = FMath::Clamp(int32(Render->CurrentFirstLODIdx), 0, Render->LODResources.Num() - 1); L < Render->LODResources.Num(); ++L)
                {
                    const FStaticMeshLODResources& R = Render->LODResources[L];
                    TRefCountPtr<const FStaticMeshLODResources> Ref(&R);
                    if (L < int32(Render->CurrentFirstLODIdx)) continue; // streamed out before the reference was taken
                    if (R.VertexBuffers.PositionVertexBuffer.GetVertexData() && R.VertexBuffers.PositionVertexBuffer.GetNumVertices() > 0 &&
                        R.VertexBuffers.StaticMeshVertexBuffer.GetTangentData() && R.VertexBuffers.StaticMeshVertexBuffer.GetTexCoordData() &&
                        R.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0 && R.IndexBuffer.GetArrayView().Num() > 0)
                    {
                        LODIndex = L;
                        Held = MoveTemp(Ref);
                        break;
                    }
                }
                const int32 SectionsLOD = LODIndex == INDEX_NONE ? FMath::Clamp(int32(Render->CurrentFirstLODIdx), 0, Render->LODResources.Num() - 1) : LODIndex;
                const FStaticMeshLODResources& LOD = Render->LODResources[SectionsLOD];
                const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
                const FStaticMeshVertexBuffer& Tangents = LOD.VertexBuffers.StaticMeshVertexBuffer;
                const FIndexArrayView Indices = LODIndex == INDEX_NONE ? FIndexArrayView() : LOD.IndexBuffer.GetArrayView();
                // Indices must address both the position and the tangent / UV buffers.
                const uint32 NumVertices = LODIndex == INDEX_NONE ? 0 : FMath::Min(Positions.GetNumVertices(), Tangents.GetNumVertices());
                TArray<FTransform> Instances;
                if (const auto* ISM = Cast<UInstancedStaticMeshComponent>(C))
                    for (int32 I = 0; I < ISM->GetInstanceCount(); ++I) ISM->GetInstanceTransform(I, Instances.AddDefaulted_GetRef(), true);
                else Instances.Add(C->GetComponentTransform());
                for (const FStaticMeshSection& Section : LOD.Sections)
                {
                    const UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
                    if (!FSCMaterialBuilder::IsScreenMaster(Material) || !Section.NumTriangles) continue;
                    UTexture* Image = nullptr;
                    Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FSCMaterialBuilder::ScreenImageParam), Image);
                    const FString* Source = Image ? It->SourcePaths.Find(Image) : nullptr;
                    const UTexture2D* Image2D = Cast<UTexture2D>(Image);
                    const FString Shown = Source ? FString::Printf(TEXT("%s size=%dx%d mips=%d resident=%d texture=%s"), *FPaths::GetBaseFilename(*Source),
                        Image2D ? Image2D->GetSizeX() : 0, Image2D ? Image2D->GetSizeY() : 0, Image2D ? Image2D->GetNumMips() : 0,
                        Image2D ? Image2D->GetNumResidentMips() : 0, *GetNameSafe(Image)) : FString(TEXT("dark"));
                    const FString Where = FString::Printf(TEXT("image=%s mesh=%s component=%s master=%s lod=%d"), *Shown, *GetNameSafe(Mesh), *C->GetName(),
                                                          *GetNameSafe(Material->GetMaterial()), SectionsLOD);
                    // The section's index range and every index it holds must lie inside the CPU copy.
                    bool bValid = LODIndex != INDEX_NONE && uint64(Section.FirstIndex) + uint64(Section.NumTriangles) * 3 <= uint64(Indices.Num());
                    for (uint32 K = 0; bValid && K < Section.NumTriangles * 3; ++K) bValid = Indices[Section.FirstIndex + K] < NumVertices;
                    if (!bValid)
                    {
                        Faces.Add({Where + TEXT(" geometry=unavailable")});
                        continue;
                    }
                    FBox2f UV(ForceInit);
                    for (uint32 K = 0; K < Section.NumTriangles * 3; ++K)
                        UV += Tangents.GetVertexUV(Indices[Section.FirstIndex + K], 0);
                    for (int32 I = 0; I < Instances.Num(); ++I)
                    {
                        FVector Centre = FVector::ZeroVector, Normal = FVector::ZeroVector;
                        double Area = 0;
                        for (uint32 T = 0; T < Section.NumTriangles; ++T)
                        {
                            FVector P[3];
                            for (int32 K = 0; K < 3; ++K)
                                P[K] = Instances[I].TransformPosition(FVector(Positions.VertexPosition(Indices[Section.FirstIndex + T * 3 + K])));
                            const FVector N = FVector::CrossProduct(P[1] - P[0], P[2] - P[0]);
                            const double A = 0.5 * N.Size();
                            Area += A;
                            Centre += (P[0] + P[1] + P[2]) / 3 * A;
                            FVector V = FVector::ZeroVector; // the authored vertex normals give the visible side
                            for (int32 K = 0; K < 3; ++K)
                                V += Instances[I].TransformVectorNoScale(FVector(FVector3f(Tangents.VertexTangentZ(Indices[Section.FirstIndex + T * 3 + K]))));
                            Normal += V.GetSafeNormal() * A;
                        }
                        if (Area <= 0 || Normal.IsNearlyZero()) continue;
                        Faces.Add({FString::Printf(TEXT("%s instance=%d uv0=%.3f,%.3f..%.3f,%.3f"), *Where, I, UV.Min.X, UV.Min.Y, UV.Max.X, UV.Max.Y),
                                   Centre / Area, Normal.GetSafeNormal(), true});
                    }
                }
            }
        }
        if (Args.IsEmpty())
        {
            for (int32 I = 0; I < Faces.Num(); ++I)
                UE_LOG(LogTemp, Display, TEXT("SCREEN_FACE index=%d %s centre=%s normal=%s"), I, *Faces[I].Label, *Faces[I].Centre.ToCompactString(),
                       *Faces[I].Normal.ToCompactString());
            UE_LOG(LogTemp, Display, TEXT("SCREEN_FACES count=%d"), Faces.Num());
            return;
        }
        const int32 Index = FCString::Atoi(*Args[0]);
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!Faces.IsValidIndex(Index) || !Faces[Index].bGeometry || !PC || !PC->GetPawn()) { UE_LOG(LogTemp, Warning, TEXT("SCREEN_VIEW ok=0 index=%d count=%d"), Index, Faces.Num()); return; }
        const FFace& F = Faces[Index];
        const FVector At = F.Centre + F.Normal * (Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : 45.0);
        const FRotator View = (-F.Normal).Rotation();
        if (ASCViewerPawn* Viewer = Cast<ASCViewerPawn>(PC->GetPawn())) Viewer->SetView(At, View);
        else { PC->GetPawn()->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics); PC->SetControlRotation(View); }
        // What the view ray meets first on its way to the face (an occluder in front of the screen shows here).
        FHitResult Hit;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(SCScreens), true, PC->GetPawn());
        Q.bReturnFaceIndex = true;
        const bool bHit = World->LineTraceSingleByChannel(Hit, At, F.Centre - F.Normal * 2.0, ECC_Visibility, Q);
        const UPrimitiveComponent* P = bHit ? Hit.GetComponent() : nullptr;
        const auto* SM = Cast<UStaticMeshComponent>(P);
        int32 HitSection = 0;
        const UMaterialInterface* HitMaterial = P && Hit.FaceIndex >= 0 ? P->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, HitSection) : nullptr;
        UE_LOG(LogTemp, Display, TEXT("SCREEN_VIEW ok=1 index=%d %s at=%s view=%s first_hit=%s mesh=%s dist=%.1f material=%s"), Index, *F.Label,
               *At.ToCompactString(), *View.ToCompactString(), P ? *P->GetName() : TEXT("-"), SM ? *GetNameSafe(SM->GetStaticMesh()) : TEXT("-"),
               bHit ? Hit.Distance : -1.0, *GetNameSafe(HitMaterial ? HitMaterial->GetMaterial() : nullptr));
    }));

// sc.Look.Camera <x> <y> <z> <yaw> <pitch>: put the current pawn (the fly camera in fly mode) at a world position and
// view direction (look reviews of environments).
static FAutoConsoleCommandWithWorldAndArgs GSCLookCamera(
    TEXT("sc.Look.Camera"), TEXT("sc.Look.Camera <x> <y> <z> <yaw> <pitch>: place the current pawn and view (world cm, degrees)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!PC || !PC->GetPawn() || Args.Num() < 5) return;
        const FVector At(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
        const FRotator View(FCString::Atod(*Args[4]), FCString::Atod(*Args[3]), 0);
        if (ASCViewerPawn* Viewer = Cast<ASCViewerPawn>(PC->GetPawn()))
            Viewer->SetView(At, View); // the viewer pawn keeps its own view rotation, not the controller's
        else
        {
            PC->GetPawn()->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
            PC->SetControlRotation(View);
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK camera at=%s view=%s pawn=%s"), *At.ToCompactString(), *View.ToCompactString(), *PC->GetPawn()->GetClass()->GetName());
    }));
