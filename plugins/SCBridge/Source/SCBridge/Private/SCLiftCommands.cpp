// sc.Lift.* console commands (WP-P5): list, call, request, doors, send an interaction along the exported links, where;
// the floor panels (list, and aiming the first-person view at an option or a point of a panel's face).
#include "SCViewerAutomation.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "SCLift.h"
#include "SCFirstPersonCharacter.h"
#include "Components/MeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "TextureResource.h"
#include "Misc/Paths.h"
#include "Components/SceneComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLiftCmd, Log, All);

namespace
{
FSCLift* LiftArg(ASCVehicle* V, const TArray<FString>& Args, int32 Index = 0)
{
    const int32 Id = Args.IsValidIndex(Index) && Args[Index].IsNumeric() ? FCString::Atoi(*Args[Index]) : INDEX_NONE;
    if (!V || !V->Lifts.IsValidIndex(Id))
    {
        UE_LOG(LogSCLiftCmd, Warning, TEXT("LIFT_CMD ok=0 reason=no_such_lift arg=%s lifts=%d"), Args.IsValidIndex(Index) ? *Args[Index] : TEXT("-"),
               V ? V->Lifts.Num() : 0);
        return nullptr;
    }
    return V->Lifts[Id].Get();
}
}

void FSCViewerAutomation::RegisterLiftCommands()
{
    TWeakObjectPtr<USCViewerSubsystem> Weak = Owner;
    auto Add = [&](const TCHAR* Name, const TCHAR* Help, TFunction<void(ASCVehicle*, const TArray<FString>&)> Body) {
        Commands.Add(MakeUnique<FAutoConsoleCommand>(Name, Help, FConsoleCommandWithArgsDelegate::CreateLambda([Weak, Body](const TArray<FString>& Args) {
            ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
            Body(V, Args);
            if (V) V->UpdateTickEnabled();
        })));
    };
    Add(TEXT("sc.Lift.List"), TEXT("sc.Lift.List: log every lift (LIFT_LIST) and its stops (LIFT_STOP)"), [](ASCVehicle* V, const TArray<FString>&) {
        for (int32 I = 0; V && I < V->Lifts.Num(); ++I)
        {
            const FSCLift& L = *V->Lifts[I];
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_LIST id=%d entity=%s class=%s stops=%d at=%d state=%s gate=%d gateways=%d riders=%d"), I, *L.EntityId,
                   *L.ClassName, L.Stops.Num(), L.CurrentStop, LexToString(L.State), L.Gate.Num(), L.GatewayDoors.Num(), L.Riders.Num());
            for (const FSCLiftStop& S : L.Stops)
            {
                TArray<FString> Doors;
                for (const FSCLiftDoor& D : S.LandingDoors) Doors.Add(D.EntityId);
                UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_STOP lift=%d index=%d node=%s z_cm=%.1f s_m=%.3f name=\"%s\" doors=%s"), I, S.Index, *S.NodeId, S.ZCm,
                       S.S, *S.Name, Doors.IsEmpty() ? TEXT("-") : *FString::Join(Doors, TEXT(",")));
            }
        }
        if (!V) return;
        for (const auto& [Anchor, Panel] : V->LiftPanels)
        {
            TArray<FString> Choices;
            for (const FSCPointInteraction& C : Panel.Choices) Choices.Add(FString::Printf(TEXT("%s=%s"), *C.Name.ToString(), *C.DisplayName));
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_PANEL entity=%s label=\"%s\" choices=\"%s\" at=%s"), *Panel.EntityId, *Panel.Label,
                   *FString::Join(Choices, TEXT(", ")), Anchor ? *V->GetActorTransform().InverseTransformPosition(Anchor->GetComponentLocation()).ToCompactString() : TEXT("-"));
        }
    });
    Add(TEXT("sc.Lift.Call"), TEXT("sc.Lift.Call <lift> <stop>: as a call button at that stop (Ascent/DescentRequest on its node)"),
        [](ASCVehicle* V, const TArray<FString>& Args) {
            FSCLift* L = LiftArg(V, Args);
            if (!L || !Args.IsValidIndex(1)) return;
            const int32 Stop = FCString::Atoi(*Args[1]);
            const bool bBelow = L->Stops.IsValidIndex(Stop) && L->Stops[Stop].S < L->S;
            const bool OK = L->ReceiveNode(Stop, bBelow ? TEXT("DescentRequest") : TEXT("AscentRequest"), TEXT("console"), TEXT("sc.Lift.Call"));
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_CMD call lift=%d stop=%d ok=%d"), L->Id, Stop, OK);
        });
    Add(TEXT("sc.Lift.Request"), TEXT("sc.Lift.Request <lift> <stop>: as the floor panel (From Elevator call <stop>)"),
        [](ASCVehicle* V, const TArray<FString>& Args) {
            FSCLift* L = LiftArg(V, Args);
            if (!L || !Args.IsValidIndex(1)) return;
            const bool OK = L->ReceiveCarriage(FName(*(TEXT("From Elevator call ") + Args[1])), TEXT("console"), TEXT("sc.Lift.Request"));
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_CMD request lift=%d stop=%s ok=%d"), L->Id, *Args[1], OK);
        });
    Add(TEXT("sc.Lift.Doors"), TEXT("sc.Lift.Doors <lift> open|close: OpenInnerDoor + the stop's DoorOpen / CloseAllDoors + DoorClose"),
        [](ASCVehicle* V, const TArray<FString>& Args) {
            FSCLift* L = LiftArg(V, Args);
            if (!L || !Args.IsValidIndex(1)) return;
            const bool bOpen = Args[1].Equals(TEXT("open"), ESearchCase::IgnoreCase);
            const bool OK = bOpen ? L->OpenDoors() : L->CloseDoors();
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_CMD doors lift=%d open=%d ok=%d state=%s"), L->Id, bOpen, OK, LexToString(L->State));
        });
    Add(TEXT("sc.Lift.Send"), TEXT("sc.Lift.Send <entity_id> <interaction ...>: fire an entity's interaction along its exported links (the press path)"),
        [](ASCVehicle* V, const TArray<FString>& Args) {
            if (!V || Args.Num() < 2) return;
            TArray<FString> Slot(Args);
            Slot.RemoveAt(0);
            const int32 Handled = V->SendInteraction(Args[0], FName(*FString::Join(Slot, TEXT(" "))));
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_CMD send entity=%s interaction=\"%s\" handled=%d"), *Args[0], *FString::Join(Slot, TEXT(" ")), Handled);
        });
    Add(TEXT("sc.Lift.Where"), TEXT("sc.Lift.Where: log every lift's position and state (LIFT_WHERE) and the player's movement base"),
        [Weak](ASCVehicle* V, const TArray<FString>&) {
            for (int32 I = 0; V && I < V->Lifts.Num(); ++I)
            {
                // Where the carriage itself is (vehicle frame, cm): it rides the lift's anchor when attached.
                const USceneComponent* Carriage = V->Lifts[I]->CarriageAnchor.Get();
                UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_WHERE %s carriage=%s"), *V->Lifts[I]->Describe(),
                       Carriage ? *V->GetActorTransform().InverseTransformPosition(Carriage->GetComponentLocation()).ToCompactString() : TEXT("-"));
            }
            const ASCFirstPersonCharacter* C = Weak.IsValid() ? Weak->GetFirstPerson() : nullptr;
            const UPrimitiveComponent* Base = C ? Cast<UPrimitiveComponent>(C->GetMovementBaseObject()) : nullptr;
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_WHERE player base=%s lift=%d pos=%s"), Base ? *Base->GetName() : TEXT("-"),
                   V && Base ? V->LiftCarrying(Base) : INDEX_NONE, C ? *C->GetVehicleRelativeLocation().ToCompactString() : TEXT("-"));
        });
    Add(TEXT("sc.Lift.Panels"), TEXT("sc.Lift.Panels: log the floor panels (FLOOR_PANEL) and their rows (FLOOR_PANEL_ROW, content rects)"),
        [](ASCVehicle* V, const TArray<FString>&) {
            for (int32 I = 0; V && I < V->FloorPanels.Num(); ++I)
            {
                const FSCFloorPanel& P = V->FloorPanels[I];
                // Where the face is (vehicle frame, cm) and where it faces: a scripted view stands in front of it.
                const FVector Centre = V->GetActorTransform().InverseTransformPosition(P.Frame().TransformPosition(P.Geometry.PointAt(FVector2D(0.5, 0.5))));
                const FVector Facing = V->GetActorTransform().InverseTransformVectorNoScale(P.Frame().TransformVectorNoScale(P.Geometry.Normal));
                UE_LOG(LogSCLiftCmd, Display, TEXT("FLOOR_PANEL_AT index=%d centre=%.1f,%.1f,%.1f normal=%.3f,%.3f,%.3f"), I, Centre.X, Centre.Y, Centre.Z, Facing.X,
                       Facing.Y, Facing.Z);
                UE_LOG(LogSCLiftCmd, Display, TEXT("FLOOR_PANEL index=%d kind=%s entity=%s lift=%d face=%d size_cm=%.1fx%.1f colours=%s %s image=%s"), I,
                       P.Kind == ESCPanelKind::Buttons ? TEXT("buttons") : TEXT("floor_select"), *P.EntityId, P.Lift, P.Face, P.Geometry.Width, P.Geometry.Height,
                       P.Colours.bFromData ? TEXT("data") : TEXT("white-on-black"), *P.Shown.Describe(), P.Shown.Image.IsEmpty() ? TEXT("-") : *FPaths::GetBaseFilename(P.Shown.Image));
                // What the face draws: its material and the render target's pixels at each row's centre and the status bar
                // (linear), to tell a drawing fault from a material one.
                const UMeshComponent* Face = P.Component.Get();
                const UMaterialInterface* M = Face ? Face->GetMaterial(P.Slot) : nullptr;
                UTexture* Image = nullptr;
                if (M) M->GetTextureParameterValue(FName(TEXT("ScreenImage")), Image);
                FString Pixels;
                if (UTextureRenderTarget2D* RT = P.Target.Get(); RT && RT->GameThread_GetRenderTargetResource())
                {
                    TArray<FLinearColor> Data;
                    if (RT->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Data) && Data.Num() == RT->SizeX * RT->SizeY)
                    {
                        auto At = [&](const FVector2D& C) {
                            const FLinearColor L = Data[FMath::Clamp(int32(C.Y * RT->SizeY), 0, RT->SizeY - 1) * RT->SizeX + FMath::Clamp(int32(C.X * RT->SizeX), 0, RT->SizeX - 1)];
                            return FString::Printf(TEXT("(%.3f,%.3f,%.3f,%.2f)"), L.R, L.G, L.B, L.A);
                        };
                        for (const FSCFloorPanelOption& R : P.Layout.Options) Pixels += At(R.Rect.GetCenter() + FVector2D(R.Rect.GetSize().X * 0.4, 0)) + TEXT(" ");
                        Pixels += TEXT("status=") + At(P.Layout.Status.GetCenter() + FVector2D(0.3, 0)) + TEXT(" field=") + At(FVector2D(0.02, 0.5));
                    }
                }
                UE_LOG(LogSCLiftCmd, Display, TEXT("FLOOR_PANEL_FACE panel=%d component=%s slot=%d material=%s parent=%s image=%s target=%s pixels=%s"), I,
                       *GetNameSafe(Face), P.Slot, *GetNameSafe(M), M ? *GetNameSafe(M->GetMaterial()) : TEXT("-"), *GetNameSafe(Image),
                       *GetNameSafe(P.Target.Get()), Pixels.IsEmpty() ? TEXT("-") : *Pixels);
                for (int32 O = 0; O < P.Layout.Options.Num(); ++O)
                {
                    const FSCFloorPanelOption& R = P.Layout.Options[O];
                    UE_LOG(LogSCLiftCmd, Display, TEXT("FLOOR_PANEL_ROW panel=%d option=%d stop=%d label=\"%s\" interaction=\"%s\" rect=%.3f,%.3f..%.3f,%.3f"), I, O,
                           R.Stop, *R.Label, *R.Interaction.ToString(), R.Rect.Min.X, R.Rect.Min.Y, R.Rect.Max.X, R.Rect.Max.Y);
                }
            }
        });
    Add(TEXT("sc.Lift.PanelAim"),
        TEXT("sc.Lift.PanelAim <panel> <option> | <panel> <x> <y>: hold the first-person view on an option's centre, or on content point (x, y) of "
             "the panel's face (0..1, y down; outside 0..1 is off the face), until mouse look or sc.FP.Look"),
        [Weak](ASCVehicle* V, const TArray<FString>& Args) {
            ASCFirstPersonCharacter* C = Weak.IsValid() ? Weak->GetFirstPerson() : nullptr;
            const int32 Panel = Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) : INDEX_NONE;
            if (!V || !C || !V->FloorPanels.IsValidIndex(Panel) || Args.Num() < 2)
            {
                UE_LOG(LogSCLiftCmd, Warning, TEXT("LIFT_CMD panel_aim ok=0 reason=args_or_no_panel"));
                return;
            }
            const FSCFloorPanel& P = V->FloorPanels[Panel];
            FVector2D Content;
            if (Args.Num() >= 3) Content = FVector2D(FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
            else if (P.Layout.Options.IsValidIndex(FCString::Atoi(*Args[1]))) Content = P.ContentOf(P.Layout.Options[FCString::Atoi(*Args[1])].Rect.GetCenter());
            else return;
            const UMeshComponent* Face = P.Component.Get();
            if (!Face) return;
            const FVector Point = P.Frame().TransformPosition(P.Geometry.PointAt(Content));
            C->HoldViewOn(Face, Face->GetComponentTransform().InverseTransformPosition(Point)); // held until mouse look or sc.FP.Look
            const FRotator View = C->GetVehicleRelativeView();
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_CMD panel_aim panel=%d content=%.3f,%.3f yaw=%.2f pitch=%.2f dist_cm=%.1f (held)"), Panel, Content.X,
                   Content.Y, View.Yaw, View.Pitch, FVector::Dist(Point, C->InteractionOrigin()));
        });
}
