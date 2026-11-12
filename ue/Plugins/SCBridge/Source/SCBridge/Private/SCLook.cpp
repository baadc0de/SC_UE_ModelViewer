// Look tuning (2026-11-12, plan D15): runtime A/B of post-process settings on the viewer's unbound volume(s).
// sc.Look.PP <FPostProcessSettings field> <value>: sets the field and its bOverride_ flag (numbers and bools).
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"
#include "SCVehicle.h"
#include "SCViewerPawn.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"

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

// sc.Look.HideGlass <0|1>: hide the vehicle's components drawn with a glass master material (A/B through-glass views).
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
                    if (const UMaterialInterface* M = C->GetMaterial(I)) bGlass = GetNameSafe(M->GetMaterial()).Contains(TEXT("Glass"));
                if (!bGlass) continue;
                C->SetHiddenInGame(bHide);
                ++Hidden;
            }
        }
        UE_LOG(LogTemp, Display, TEXT("LOOK glass hidden=%d components=%d"), bHide, Hidden);
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
