// sc.Lift.* console commands (WP-P5): list, call, request, doors, send an interaction along the exported links, where.
#include "SCViewerAutomation.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "SCLift.h"
#include "SCFirstPersonCharacter.h"
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
                   *FString::Join(Choices, TEXT(", ")), Anchor.IsValid() ? *V->GetActorTransform().InverseTransformPosition(Anchor->GetComponentLocation()).ToCompactString() : TEXT("-"));
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
            for (int32 I = 0; V && I < V->Lifts.Num(); ++I) UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_WHERE %s"), *V->Lifts[I]->Describe());
            const ASCFirstPersonCharacter* C = Weak.IsValid() ? Weak->GetFirstPerson() : nullptr;
            const UPrimitiveComponent* Base = C ? Cast<UPrimitiveComponent>(C->GetMovementBaseObject()) : nullptr;
            UE_LOG(LogSCLiftCmd, Display, TEXT("LIFT_WHERE player base=%s lift=%d pos=%s"), Base ? *Base->GetName() : TEXT("-"),
                   V && Base ? V->LiftCarrying(Base) : INDEX_NONE, C ? *C->GetVehicleRelativeLocation().ToCompactString() : TEXT("-"));
        });
}
