// WP-M1: door runtime state, access, and the data-derived room classification.
#include "SCVehicle.h"
#include "Cargo/SCCargoSubsystem.h"
#include "Cargo/SCCargoContainer.h"
#include "Engine/OverlapResult.h"
#include "SCFirstPersonCharacter.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/World.h"

namespace
{
FBox DoorAperture(const ASCVehicle& Ship, const FSCDoorInteraction& Door)
{
    FBox Bounds(ForceInit);
    TArray<USceneComponent*> Parts;
    Door.Rig->GetChildrenComponents(true, Parts);
    const FTransform& ShipFrame = Ship.GetActorTransform();
    const bool bSharedRig = Ship.Doors.ContainsByPredicate([&](const FSCDoorInteraction& Other) {
        return Other.Rig == Door.Rig && &Other != &Door;
    });
    for (const USceneComponent* Part : Parts)
        if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Part);
            Primitive && Primitive->IsCollisionEnabled())
        {
            const USceneComponent* Attachment = Part;
            const USceneComponent* OwnerRig = Part->GetAttachParent();
            while (OwnerRig && !OwnerRig->IsA<USCRigComponent>())
            {
                Attachment = OwnerRig;
                OwnerRig = OwnerRig->GetAttachParent();
            }
            if (OwnerRig == Door.Rig && (!bSharedRig || Attachment->GetAttachSocketName() == Door.Joint))
                Bounds += Primitive->CalcBounds(Primitive->GetComponentTransform().GetRelativeTransform(ShipFrame)).GetBox();
        }
    if (!Bounds.IsValid && !bSharedRig && Door.Rig->IsCollisionEnabled())
        Bounds = Door.Rig->CalcBounds(Door.Rig->GetComponentTransform().GetRelativeTransform(ShipFrame)).GetBox();
    return Bounds;
}

bool PawnInDoor(const ASCVehicle& Ship, const FBox& Aperture)
{
    if (!Aperture.IsValid) return false;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(&Ship);
    const ASCFirstPersonCharacter* Player = Viewer ? Viewer->GetFirstPerson() : nullptr;
    const UCapsuleComponent* Capsule = Player ? Player->GetCapsuleComponent() : nullptr;
    if (!Capsule) return false;
    const FVector Position = Ship.GetActorTransform().InverseTransformPosition(Capsule->GetComponentLocation());
    const double Radius = Capsule->GetScaledCapsuleRadius();
    const double HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    return Aperture.ExpandBy(FVector(Radius, Radius, HalfHeight)).IsInsideOrOn(Position);
}

bool DoorwayOccupied(const ASCVehicle& Ship, const FBox& Aperture)
{
    if (PawnInDoor(Ship, Aperture)) return true;
    const UWorld* World = Ship.GetWorld();
    if (!World || !Aperture.IsValid) return false;
    const FTransform& ShipFrame = Ship.GetActorTransform();
    const FVector Half = Aperture.GetExtent() * ShipFrame.GetScale3D().GetAbs();
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSecureDoorway), false, &Ship);
    TArray<FOverlapResult> Hits;
    World->OverlapMultiByObjectType(Hits, ShipFrame.TransformPosition(Aperture.GetCenter()), ShipFrame.GetRotation(),
        Objects, FCollisionShape::MakeBox(Half), Params);
    return Hits.ContainsByPredicate([](const FOverlapResult& Hit) { return Cast<ASCCargoContainer>(Hit.GetActor()) != nullptr; });
}
}

void ASCVehicle::RememberDoorPanelLabel(const USCRigComponent* Rig, const FSCUiBinding& Binding)
{
    if (Rig && !Binding.DoorLabel.IsEmpty() && !DoorPanelLabels.Contains(Rig)) DoorPanelLabels.Add(Rig, Binding.DoorLabel);
}

void ASCVehicle::BuildShipDoors()
{
    ShipDoors.Reset(Doors.Num());
    TArray<FString> Labels;
    for (const FSCDoorInteraction& Door : Doors)
    {
        FShipDoor& Data = ShipDoors.AddDefaulted_GetRef();
        FString Label = Door.DisplayName;
        if (Door.Rig)
        {
            const FSCDoorRecord* Record = DoorRecords.Find(Door.Rig.Get());
            Data.bLocked = Record && Record->Door.bIsLocked.Get(false);
            Data.Aperture = DoorAperture(*this, Door);
            const FSCSceneInstance* Source = Manifest.Children.FindByPredicate([&](const FSCSceneInstance& Item) {
                return Item.EntityName == Door.Rig->OwnerEntity;
            });
            if (Source)
            {
                Data.bRamp = Source->ItemType.Equals(TEXT("Ramp"), ESearchCase::IgnoreCase) ||
                    Source->ItemSubType.Equals(TEXT("Ramp"), ESearchCase::IgnoreCase);
                if (!Source->DisplayName.IsEmpty()) Label = Source->DisplayName;
                else if (!Source->PortPath.IsEmpty())
                {
                    Label = Source->PortPath.Last();
                    Label.RemoveFromStart(TEXT("hardpoint_"), ESearchCase::IgnoreCase);
                    Label.ReplaceInline(TEXT("_"), TEXT(" "));
                    if (!Label.IsEmpty()) Label[0] = FChar::ToUpper(Label[0]);
                }
            }
            if (Data.Aperture.IsValid && !LightAreas.IsEmpty())
            {
                const FVector Extent = Data.Aperture.GetExtent();
                int32 Axis = Extent.Y < Extent.X ? 1 : 0;
                if (Extent.Z < Extent[Axis]) Axis = 2;
                const FVector Centre = Data.Aperture.GetCenter();
                // Probe each side of the opening. Start close to its faces and allow a gap between authored room volumes.
                for (double Gap : {10.0, 25.0, 50.0})
                {
                    FVector Offset(ForceInit);
                    Offset[Axis] = Extent[Axis] + Gap;
                    const FString A = LightRoomAt(GetActorTransform().TransformPosition(Centre + Offset));
                    const FString B = LightRoomAt(GetActorTransform().TransformPosition(Centre - Offset));
                    const ESCShipDoorGroup Estimate = FSCShipDoorPolicy::Classify(true, A, B);
                    if (Estimate != ESCShipDoorGroup::Other)
                    {
                        Data.Group = Estimate;
                        Data.GroupSource = TEXT("room_volumes");
                        if (Estimate == ESCShipDoorGroup::Interior) break; // a room on either side is stronger evidence
                    }
                }
            }
            if (const FString* PanelLabel = DoorPanelLabels.Find(Door.Rig.Get())) Label = *PanelLabel;
        }
        Labels.Add(MoveTemp(Label));
    }
    SCHangarWaypoints::NumberDuplicates(Labels);
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id) ShipDoors[Id].Label = MoveTemp(Labels[Id]);
    RefreshPanelScreens();
}

FString ASCVehicle::ShipDoorLabel(int32 Index) const
{
    if (ShipDoors.IsValidIndex(Index) && !ShipDoors[Index].Label.IsEmpty()) return ShipDoors[Index].Label;
    return Doors.IsValidIndex(Index) ? Doors[Index].DisplayName : FString();
}

bool ASCVehicle::IsDoorLocked(int32 Index) const
{
    return ShipDoors.IsValidIndex(Index) && ShipDoors[Index].bLocked;
}

bool ASCVehicle::SetDoorLocked(int32 Index, bool bLocked)
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig || LiftDrivenRigs.Contains(Doors[Index].Rig.Get())) return false;
    if (!ShipDoors.IsValidIndex(Index)) ShipDoors.SetNum(Doors.Num());
    FShipDoor& Door = ShipDoors[Index];
    Door.bSecurePending = false;
    if (Door.bLocked == bLocked) return true;
    Door.bLocked = bLocked;
    RefreshPanelScreens();
    return true;
}

FName ASCVehicle::ShipLinkPlayer() const
{
    if (const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this)) return ShipLink->GetPlayer();
    return TEXT("you");
}

FString ASCVehicle::DoorOrderRefusal(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse) const
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig) return TEXT("No such door");
    const FShipDoor* Info = ShipDoors.IsValidIndex(Index) ? &ShipDoors[Index] : nullptr;
    if (LiftDrivenRigs.Contains(Doors[Index].Rig.Get())) return TEXT("Lift-controlled door");
    const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this);
    const ESCShipRole ActorRole = ShipLink ? ShipLink->RoleOf(Actor, this) : ESCShipRole::None;
    const bool bLocalInside = LocalUse.IsSet() && IsInsideLightArea(LocalUse.GetValue());
    if (Info && Info->bCargoRelock)
    {
        if (Action == ESCShipDoorAction::Open && ActorRole != ESCShipRole::Owner) return TEXT("Locked");
        if (Action == ESCShipDoorAction::Lock || Action == ESCShipDoorAction::Unlock) return TEXT("Door in use by cargo");
    }
    if (Action == ESCShipDoorAction::Close)
        if (const auto* Cargo = USCCargoSubsystem::Get(this); Cargo && Cargo->IsAccessDoorBusy(this, Index))
            return TEXT("Door in use by cargo");
    if (Info && Info->bSecurePending && Action == ESCShipDoorAction::Open) return TEXT("Securing");
    return FSCShipDoorPolicy::Refusal(ActorRole, Action, IsDoorLocked(Index),
        Info ? Info->Group : ESCShipDoorGroup::Other, bLocalInside);
}

FString ASCVehicle::OrderShipDoor(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse)
{
    const FString Refusal = DoorOrderRefusal(Index, Action, Actor, LocalUse);
    if (!Refusal.IsEmpty()) return Refusal;
    if (Action == ESCShipDoorAction::Lock || Action == ESCShipDoorAction::Unlock)
        return SetDoorLocked(Index, Action == ESCShipDoorAction::Lock) ? FString() : FString(TEXT("Lock unavailable"));
    const bool bOpen = Action == ESCShipDoorAction::Open;
    if (IsDoorOpen(Index) == bOpen) return {};
    return PlayDoor(Index, bOpen, 0.f, 0, Actor, LocalUse) ? FString() : FString(TEXT("Door cannot move"));
}

TArray<FSCShipDoorResult> ASCVehicle::SecureShip(FName Actor)
{
    TArray<FSCShipDoorResult> Results;
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id)
    {
        FShipDoor& Door = ShipDoors[Id];
        if (Door.Group != ESCShipDoorGroup::Exterior || LiftDrivenRigs.Contains(Doors[Id].Rig.Get())) continue;
        FSCShipDoorResult& Result = Results.AddDefaulted_GetRef();
        Result.Door = Id;
        Result.Refusal = DoorOrderRefusal(Id, ESCShipDoorAction::Lock, Actor);
        if (!Result.Refusal.IsEmpty()) continue;
        if (IsDoorOpen(Id) && DoorwayOccupied(*this, Door.Aperture))
        {
            Result.Refusal = TEXT("Doorway occupied");
            continue;
        }
        Result.Refusal = OrderShipDoor(Id, ESCShipDoorAction::Close, Actor);
        if (!Result.Refusal.IsEmpty()) continue;
        if (IsDoorPlaying(Id)) Door.bSecurePending = Result.bPending = true;
        else SetDoorLocked(Id, true);
    }
    return Results;
}

void ASCVehicle::FinishSecureOrders()
{
    for (int32 Index = 0; Index < ShipDoors.Num(); ++Index)
        if (ShipDoors[Index].bSecurePending && !IsDoorPlaying(Index))
        {
            ShipDoors[Index].bSecurePending = false;
            if (!IsDoorOpen(Index)) SetDoorLocked(Index, true);
        }
}

void ASCVehicle::RestoreCargoDoorLocks()
{
    const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(this);
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id)
    {
        FShipDoor& Door = ShipDoors[Id];
        if (!Door.bCargoRelock || (Cargo && Cargo->IsAccessDoorBusy(this, Id))) continue;
        if (IsDoorOpen(Id))
        {
            if (!DoorwayOccupied(*this, Door.Aperture)) PlayDoor(Id, false);
            continue;
        }
        if (IsDoorPlaying(Id)) continue;
        Door.bCargoRelock = false;
        SetDoorLocked(Id, true);
    }
}

bool ASCVehicle::AimDoorPanelLock(const USceneComponent* Panel, const FVector& Eye, const FVector& Direction, double Reach, bool bLocked) const
{
    for (const FSCFloorPanel& Face : DoorPanelLocks)
    {
        if (Face.Anchor.Get() != Panel || !Face.Component.IsValid()) continue;
        const FTransform Frame = Face.Frame();
        const FVector LocalEye = Frame.InverseTransformPosition(Eye);
        const FVector Dir = Frame.InverseTransformVectorNoScale(Direction.GetSafeNormal());
        double Distance = 0;
        FVector2f UV;
        if (!Face.Geometry.Raycast(LocalEye, Dir, Reach, Distance, UV)) continue;
        const int32 Option = Face.Layout.OptionAt(FVector2D(UV.X, UV.Y) - Face.UVBase);
        if (Face.Layout.Options.IsValidIndex(Option))
        {
            const FName Correct = bLocked ? TEXT("UnlockDoor") : TEXT("LockDoor");
            const FName Hit = Face.Layout.Options[Option].Interaction;
            if (Hit == Correct) return true;
            // The package has clickable rectangles for the default render. In the locked image the other
            // state-machine action occupies the same lock control when the panel authors UnlockDoor.
            if (bLocked && Hit == FName(TEXT("LockDoor")) && Face.StateTypes.ContainsByPredicate([](const FSCUiStateType& Type) {
                return Type.Name == TEXT("DoorLockState") && Type.Next(TEXT("UnlockDoor"), TEXT("DoorLocked")) != nullptr;
            })) return true;
        }
    }
    return false;
}
