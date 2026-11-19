// WP-I1 I1-3: operating-panel screen faces follow their owner's state (ASCVehicle::ScreenFaces).
// A door pad's face shows the exporter's render of the pad's DoorOpenState for its door (the pad's state machine mirrors
// the door in the game): opening / closing while the door's fragment plays, opened / closed at rest. States the viewer
// does not model (lock, power, override) are shown with a pinned state (sc.Screens.States).
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Components/MeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCPanelScreens, Log, All);

FString ASCVehicle::DoorOpenStateKey(bool bOpen, bool bMoving)
{
    // The state names of the pads' `SInteractionStateMachineParams` DoorOpenState (the game's vocabulary, which the
    // exporter's `state_images[]` carry as is).
    if (bMoving) return bOpen ? TEXT("DoorOpenState.DoorOpening") : TEXT("DoorOpenState.DoorClosing");
    return bOpen ? TEXT("DoorOpenState.DoorOpened") : TEXT("DoorOpenState.DoorClosed");
}

int32 ASCVehicle::RefreshPanelScreens()
{
    int32 Swapped = 0;
    for (int32 Index = 0; Index < ScreenFaces.Num(); ++Index)
    {
        FSCScreenFace& Face = ScreenFaces[Index];
        UMeshComponent* C = Face.Component.Get();
        if (!C || Face.Custom.IsValid()) continue; // a face given other content keeps it
        FString Key = Face.Pinned;
        if (Key.IsEmpty())
        {
            const TWeakObjectPtr<USCRigComponent>* Door = DoorControls.Find(Face.Panel);
            const USCRigComponent* Rig = Door ? Door->Get() : nullptr;
            if (!Rig) Key = FString(); // a panel without a door shows its default (a released pin included)
            else
            {
                // Moving: a clip plays on the rig, targeted at it or untargeted (PlayFragment) and binding its mesh.
                const bool bMoving = IsRigPlaying(Rig) || (ClipCatalog && Playback.ContainsByPredicate([&](const FSCClipPlayback& P) {
                    return !P.TargetRig.IsValid() && ClipCatalog->Clips.IsValidIndex(P.Clip) &&
                           ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); });
                }));
                Key = DoorOpenStateKey(IsRigOpen(Rig), bMoving);
            }
        }
        const TWeakObjectPtr<UMaterialInterface>* State = Face.States.Find(Key);
        UMaterialInterface* Material = State && State->IsValid() ? State->Get() : Face.Default.Get();
        const FString Shown = State && State->IsValid() ? Key : FString();
        if (!Material || (Shown == Face.Shown && C->GetMaterial(Face.Slot) == Material)) continue;
        C->SetMaterial(Face.Slot, Material);
        Face.Shown = Shown;
        ++Swapped;
        UE_LOG(LogSCPanelScreens, Display, TEXT("PANEL_SCREEN face=%d component=%s slot=%d state=%s"), Index, *C->GetName(), Face.Slot,
               Shown.IsEmpty() ? TEXT("default") : *Shown);
    }
    return Swapped;
}

bool ASCVehicle::SetScreenFaceMaterial(int32 Face, UMaterialInterface* Material)
{
    if (!ScreenFaces.IsValidIndex(Face)) return false;
    FSCScreenFace& F = ScreenFaces[Face];
    UMeshComponent* C = F.Component.Get();
    if (!C) return false;
    F.Custom = Material;
    if (!Material)
    {
        // Back to the default, then to what the face's state shows.
        if (UMaterialInterface* Default = F.Default.Get()) C->SetMaterial(F.Slot, Default);
        F.Shown.Reset();
        RefreshPanelScreens();
        return true;
    }
    C->SetMaterial(F.Slot, Material);
    return true;
}

// sc.Screens.States [face|all <StateType.State | follow>] (WP-I1 I1-3 verification): with no arguments, list the
// panel faces with state images (SCREEN_STATE: index, component, slot, default image, its states, the state shown, the
// door followed); with a face index (or "all") and a state key, hold that state on the face(s) whatever the door does
// (the lock / power / override states the viewer does not model), "follow" to follow the door again.
static FAutoConsoleCommandWithWorldAndArgs GSCScreensStates(
    TEXT("sc.Screens.States"), TEXT("sc.Screens.States [face|all <StateType.State|follow>]: list or pin panel screen states"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        int32 Base = 0;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            ASCVehicle* V = *It;
            for (int32 I = 0; I < V->ScreenFaces.Num(); ++I)
            {
                FSCScreenFace& F = V->ScreenFaces[I];
                const int32 Index = Base + I;
                if (Args.Num() >= 2 && (Args[0] == TEXT("all") || FCString::Atoi(*Args[0]) == Index))
                    F.Pinned = Args[1] == TEXT("follow") ? FString() : Args[1];
                else if (Args.IsEmpty())
                {
                    TArray<FString> Keys;
                    F.States.GetKeys(Keys);
                    Keys.Sort();
                    const TWeakObjectPtr<USCRigComponent>* Door = V->DoorControls.Find(F.Panel);
                    UE_LOG(LogSCPanelScreens, Display, TEXT("SCREEN_STATE index=%d component=%s slot=%d image=%s states=%s shown=%s pinned=%s door=%s door_index=%d"),
                           Index, *GetNameSafe(F.Component.Get()), F.Slot, *FPaths::GetBaseFilename(F.Image), *FString::Join(Keys, TEXT(",")),
                           F.Shown.IsEmpty() ? TEXT("default") : *F.Shown, F.Pinned.IsEmpty() ? TEXT("-") : *F.Pinned,
                           Door && Door->IsValid() ? *(*Door)->OwnerEntity : TEXT("-"), Door ? V->DoorForRig(Door->Get()) : INDEX_NONE);
                }
            }
            Base += V->ScreenFaces.Num();
            if (Args.Num() >= 2)
            {
                const int32 Swapped = V->RefreshPanelScreens();
                UE_LOG(LogSCPanelScreens, Display, TEXT("SCREEN_STATES_PIN swapped=%d"), Swapped);
            }
        }
        if (Args.IsEmpty()) UE_LOG(LogSCPanelScreens, Display, TEXT("SCREEN_STATES count=%d"), Base);
    }));
