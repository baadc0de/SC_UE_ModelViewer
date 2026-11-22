// WP-I1 I1-3: operating-panel screen faces follow their owner's state (ASCVehicle::ScreenFaces).
// A door pad's face shows the exporter's render of the pad's DoorOpenState for its door (the pad's state machine mirrors
// the door in the game): opening / closing while the door's fragment plays, opened / closed at rest. States the viewer
// does not model (lock, power, override) are shown with a pinned state (sc.Screens.States).
// WP-I1-4: under the crosshair a face shows the exporter's render of that state with the cursor on the button aimed at
// (`hover_images[]`: the game's hover look); AimPanelButton finds the button, SetPanelHover shows it.
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Components/MeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "Engine/World.h"
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
                    return !P.bTargeted && ClipCatalog->Clips.IsValidIndex(P.Clip) &&
                           ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); });
                }));
                Key = DoorOpenStateKey(IsRigOpen(Rig), bMoving);
                if (IsDoorLocked(DoorForRig(Rig)))
                {
                    const FString Lock = TEXT("DoorLockState.DoorLocked");
                    const FString Combined = Lock + TEXT("+") + Key;
                    Key = Face.States.Contains(Combined) ? Combined : Lock;
                }
            }
        }
        const TWeakObjectPtr<UMaterialInterface>* State = Face.States.Find(Key);
        UMaterialInterface* Material = State && State->IsValid() ? State->Get() : Face.Default.Get();
        FString Shown = State && State->IsValid() ? Key : FString();
        // The cursor on a button: that state's render with the button hovered, when the export has it.
        if (const TWeakObjectPtr<UMaterialInterface>* Hover = Face.Hovered.IsEmpty() ? nullptr : Face.Hovers.Find(FSCUiHoverImage::FaceKey(Face.Hovered, Shown));
            Hover && Hover->IsValid())
        {
            Material = Hover->Get();
            Shown += TEXT("|hover:") + Face.Hovered;
        }
        if (!Material || (Shown == Face.Shown && C->GetMaterial(Face.Slot) == Material)) continue;
        C->SetMaterial(Face.Slot, Material);
        Face.Shown = Shown;
        ++Swapped;
        UE_LOG(LogSCPanelScreens, Display, TEXT("PANEL_SCREEN face=%d component=%s slot=%d state=%s"), Index, *C->GetName(), Face.Slot,
               Shown.IsEmpty() ? TEXT("default") : *Shown);
    }
    return Swapped;
}

bool ASCVehicle::AimPanelButton(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32& OutAim, FString& OutNode) const
{
    OutAim = INDEX_NONE;
    OutNode.Reset();
    const FVector Dir = Direction.GetSafeNormal();
    double Best = UE_BIG_NUMBER;
    FVector BestPoint;
    FVector2f BestUV;
    for (int32 I = 0; I < PanelAims.Num(); ++I)
    {
        const FSCPanelAim& A = PanelAims[I];
        const UMeshComponent* C = A.Component.Get();
        if (!C || !C->IsVisible()) continue;
        // A panel is reached from as far as its UI is operated when that is farther (FindInteraction's rule).
        const double AimReach = FMath::Max<double>(Reach, A.Reach);
        const FTransform T = A.Frame();
        const FVector LocalEye = T.InverseTransformPosition(Eye);
        const FVector LocalRay = T.InverseTransformPosition(Eye + Dir * AimReach) - LocalEye;
        const double LocalReach = LocalRay.Size();
        double Distance;
        FVector2f UV;
        if (LocalReach <= 0 || !A.Geometry.Raycast(LocalEye, LocalRay / LocalReach, LocalReach, Distance, UV)) continue;
        const FVector Point = T.TransformPosition(LocalEye + LocalRay / LocalReach * Distance);
        const double World = FVector::Dist(Eye, Point);
        if (World >= FMath::Min(Best, AimReach)) continue;
        Best = World;
        BestPoint = Point;
        BestUV = UV;
        OutAim = I;
    }
    if (OutAim == INDEX_NONE) return false;
    // As AimFloorPanel: nothing that answers a trace in front of the face (it stops half a centimetre short of it).
    if (UWorld* World = GetWorld())
    {
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCPanelHoverAim), true, Ignore);
        if (World->LineTraceSingleByChannel(Hit, Eye, BestPoint - Dir * 0.5, ECC_Visibility, Params))
        {
            OutAim = INDEX_NONE;
            return false;
        }
    }
    const FSCPanelAim& A = PanelAims[OutAim];
    const FVector2D P = FVector2D(BestUV.X, BestUV.Y) - A.UVBase;
    const FSCUiButton* Under = A.Buttons.FindByPredicate([&](const FSCUiButton& B) { return B.RectPx.IsInsideOrOn(P); });
    OutNode = Under ? Under->Node : A.Fallback;
    return true;
}

int32 ASCVehicle::SetPanelHover(int32 Aim, const FString& Node)
{
    bool bChanged = false;
    for (int32 I = 0; I < PanelAims.Num(); ++I)
    {
        if (!ScreenFaces.IsValidIndex(PanelAims[I].Face)) continue;
        FString& Hovered = ScreenFaces[PanelAims[I].Face].Hovered;
        const FString Want = I == Aim ? Node : FString();
        if (Hovered == Want) continue;
        UE_LOG(LogSCPanelScreens, Display, TEXT("PANEL_HOVER face=%d node=%s"), PanelAims[I].Face, Want.IsEmpty() ? TEXT("-") : *Want);
        Hovered = Want;
        bChanged = true;
    }
    return bChanged ? RefreshPanelScreens() : 0;
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
    // A face's content does not follow the paint (as a bound panel image): a paint change must not put the face's
    // authored instance back over it (a blank screen face records a paint slot at import).
    PaintSlots.RemoveAll([&](const FSCPaintSlot& P) { return P.Component.Get() == C && P.Slot == F.Slot; });
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
