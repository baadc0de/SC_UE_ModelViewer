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
#include "SCVehicle.h"
#include "SCViewerPawn.h"
#include "SCViewerSubsystem.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "SCViewerSettings.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "TextureResource.h"

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
            const float Base = GSavedWallIntensity.FindOrAdd(Light, Light->Intensity);
            Light->SetIntensity(Base * Scale);
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

// sc.Look.Display <game|ue>: the game's display curve (the M_SC_DisplayMapping blendable) or UE's filmic tonemapper.
static FAutoConsoleCommandWithWorldAndArgs GSCLookDisplay(
    TEXT("sc.Look.Display"), TEXT("sc.Look.Display <game|ue>: the game's ported display curve or UE's filmic tonemapper"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        const float Weight = Args.IsValidIndex(0) && Args[0] == TEXT("ue") ? 0.f : 1.f;
        int32 Changed = 0;
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
            for (FWeightedBlendable& B : It->Settings.WeightedBlendables.Array)
                if (const auto* MID = Cast<UMaterialInstanceDynamic>(B.Object); MID && MID->Parent && MID->Parent->GetName() == TEXT("M_SC_DisplayMapping"))
                {
                    B.Weight = Weight;
                    ++Changed;
                }
        UE_LOG(LogTemp, Display, TEXT("LOOK display=%s blendables=%d"), Weight > 0 ? TEXT("game") : TEXT("ue"), Changed);
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
    TEXT("sc.Look.GlassParam"), TEXT("sc.Look.GlassParam <name> <x> [<y> <z>] [@<instance name part>]: set a glass material parameter on glass instances"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& InArgs, UWorld* World) {
        TArray<FString> Args = InArgs;
        const FString Filter = Args.Num() && Args.Last().StartsWith(TEXT("@")) ? Args.Pop().Mid(1) : FString();
        if (Args.Num() < 2) return;
        TSet<UMaterialInstanceDynamic*> Done;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            TArray<UStaticMeshComponent*> Components;
            It->GetComponents(Components);
            for (UStaticMeshComponent* C : Components)
                for (int32 I = 0; I < C->GetNumMaterials(); ++I)
                {
                    auto* MID = Cast<UMaterialInstanceDynamic>(C->GetMaterial(I));
                    if (!MID || !IsGlassMaterial(MID) || Done.Contains(MID) || !MID->GetName().Contains(Filter)) continue;
                    Done.Add(MID);
                    if (Args.Num() >= 4)
                        MID->SetVectorParameterValue(FName(*Args[0]), FLinearColor(FCString::Atof(*Args[1]), FCString::Atof(*Args[2]), FCString::Atof(*Args[3])));
                    else
                        MID->SetScalarParameterValue(FName(*Args[0]), FCString::Atof(*Args[1]));
                }
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK glass_param %s filter=%s instances=%d"), *FString::Join(Args, TEXT(" ")), *Filter, Done.Num());
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
