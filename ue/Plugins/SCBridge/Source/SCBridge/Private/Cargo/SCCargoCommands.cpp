// sc.Cargo.* console commands (routes, debugging) and the cargo debug drawing: lattices, slots, drone paths.
//   sc.Cargo.List | sc.Cargo.Preview <from> <to> <commodity> <scu> | sc.Cargo.Transfer <from> <to> <commodity> <scu>
//   sc.Cargo.Cancel <job> | sc.Cargo.Access <grids> open|close | sc.Cargo.Debug 0|1 | sc.Cargo.Origin corner|bottom|centre
//   sc.Cargo.View <grids> [m] [up m] [yaw] | sc.Cargo.ViewMoving [m] [up m] [yaw] | sc.Cargo.LoseDrone <id|busy>
//   sc.Cargo.Section <grids> [reach m] [step m]
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoRecords.h"
#include "SCViewerPawn.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

TArray<int32> USCCargoSubsystem::ResolveGrids(const FString& Selector, FName Owner) const
{
    TArray<int32> Out;
    if (!State) return Out;
    const FName Who = Owner.IsNone() ? State->Player : Owner;
    TArray<FString> Parts;
    Selector.ParseIntoArray(Parts, TEXT("+"), true);
    for (const FString& Part : Parts)
    {
        const bool bVehicle = Part.Equals(TEXT("ship"), ESearchCase::IgnoreCase) || Part.Equals(TEXT("vehicle"), ESearchCase::IgnoreCase);
        const bool bEnvironment = Part.Equals(TEXT("elevator"), ESearchCase::IgnoreCase) || Part.Equals(TEXT("environment"), ESearchCase::IgnoreCase) ||
                                  Part.Equals(TEXT("hangar"), ESearchCase::IgnoreCase);
        for (const auto& Pair : State->Grids)
        {
            const USCCargoGridComponent* G = Pair.Value.Get();
            if (!G) continue;
            const bool bMatch = Part.IsNumeric() ? FCString::Atoi(*Part) == G->GridId
                : bVehicle ? G->Kind == ESCCargoGridKind::Vehicle && (G->Owner.IsNone() || G->Owner == Who)
                : bEnvironment ? G->Kind == ESCCargoGridKind::Environment
                : G->GroupLabel.ToString().Contains(Part) || G->GridLabel.ToString().Contains(Part) || SCCargoRecords::PortKey(G->Record.PortPath).Contains(Part);
            if (bMatch) Out.AddUnique(G->GridId);
        }
    }
    Out.Sort();
    return Out;
}

FName USCCargoSubsystem::ResolveCommodity(const FString& Name) const
{
    if (!State) return NAME_None;
    for (const auto& Pair : State->Boxes)
        if (Pair.Value.Commodity.ToString().Equals(Name, ESearchCase::IgnoreCase) || Pair.Value.CommodityLabel.ToString().Equals(Name, ESearchCase::IgnoreCase))
            return Pair.Value.Commodity;
    return NAME_None;
}

TArray<FString> USCCargoSubsystem::Describe() const
{
    TArray<FString> Lines;
    if (!State) return Lines;
    const UEnum* Access = StaticEnum<ESCCargoGridAccess>();
    const UEnum* BoxStates = StaticEnum<ESCCargoBoxState>();
    const UEnum* DroneStates = StaticEnum<ESCCargoDroneState>();
    const UEnum* JobStates = StaticEnum<ESCCargoJobState>();
    TArray<int32> Ids;
    State->Grids.GetKeys(Ids);
    Ids.Sort();
    for (const int32 Id : Ids)
    {
        FSCCargoGridView V;
        if (!GetGrid(Id, V)) continue;
        FString Boxes;
        for (const FSCCargoBoxView& B : V.Boxes) Boxes += FString::Printf(TEXT(" %d:%dSCU@%s:%s"), B.BoxId, B.SCU, *B.Origin.ToString(), *B.OwnerLabel.ToString());
        Lines.Add(FString::Printf(TEXT("GRID id=%d owner=\"%s\" group=\"%s\" label=\"%s\" cells=%s max_item_m=%s used=%d reserved=%d capacity=%d access=%s blocked=%d boxes=[%s ]"),
                                  Id, *V.OwnerLabel.ToString(), *V.GroupLabel.ToString(), *V.GridLabel.ToString(), *V.Cells.ToString(), *V.MaxItemSizeM.ToString(), V.UsedSCU, V.ReservedSCU,
                                  V.CapacitySCU, *Access->GetNameStringByValue(int64(V.Access)), V.BlockedCells.Num(), *Boxes));
    }
    for (const FSCCargoCommodityView& C : GetCommodities({}))
    {
        FString Sizes;
        for (const int32 S : C.BoxSizesSCU) Sizes += FString::Printf(TEXT(" %d"), S);
        Lines.Add(FString::Printf(TEXT("COMMODITY key=%s label=\"%s\" total=%d sizes=[%s ]"), *C.Commodity.ToString(), *C.Label.ToString(), C.TotalSCU, *Sizes));
    }
    for (const auto& Pair : State->Boxes)
        if (Pair.Value.GridId == INDEX_NONE || Pair.Value.State != ESCCargoBoxState::Stored)
            Lines.Add(FString::Printf(TEXT("BOX id=%d scu=%d state=%s job=%d grid=%d"), Pair.Key, Pair.Value.Occupancy.StandardCargoUnits,
                                      *BoxStates->GetNameStringByValue(int64(Pair.Value.State)), Pair.Value.JobId, Pair.Value.GridId));
    for (const FSCCargoDroneView& D : GetDrones())
        Lines.Add(FString::Printf(TEXT("DRONE id=%d owner=\"%s\" state=%s job=%d carrying=%d at=%s"), D.DroneId, *D.OwnerLabel.ToString(), *DroneStates->GetNameStringByValue(int64(D.State)), D.JobId,
                                  D.CarryingBoxId, *D.WorldLocation.ToCompactString()));
    for (const FSCCargoJobView& J : GetJobs())
        Lines.Add(FString::Printf(TEXT("JOB id=%d owner=\"%s\" state=%s moves=%d/%d scu=%d/%d progress=%.2f eta_s=%.0f summary=\"%s\"%s"), J.JobId, *J.OwnerLabel.ToString(),
                                  *JobStates->GetNameStringByValue(int64(J.State)), J.MovesDone, J.Moves, J.DoneSCU, J.TotalSCU, J.Progress, J.EtaSeconds,
                                  *J.Summary.ToString(), J.FailureReason.IsEmpty() ? TEXT("") : *(TEXT(" reason=\"") + J.FailureReason.ToString() + TEXT("\""))));
    return Lines;
}

void USCCargoSubsystem::DebugDraw() const
{
    UWorld* World = GetWorld();
    for (const auto& Pair : State->Grids)
        if (const USCCargoGridComponent* G = Pair.Value.Get()) G->DebugDraw(0.f, true);
    for (const FSCCargoJob& J : State->Jobs)
        for (const FSCCargoMove& M : J.Moves)
        {
            if (M.Step == FSCCargoMove::EStep::Done || M.Step == FSCCargoMove::EStep::Failed) continue;
            // The load's route (orange) and the drones' own flights (blue).
            const TArray<FVector>& Carry = M.Carry.Points;
            for (int32 I = 1; I < Carry.Num(); ++I) DrawDebugLine(World, Carry[I - 1], Carry[I], FColor(255, 170, 40), false, 0.f, 0, 3.f);
            for (const FSCCargoFlight& F : M.DroneFlights)
                for (int32 I = 1; I < F.Points.Num(); ++I) DrawDebugLine(World, F.Points[I - 1], F.Points[I], FColor(90, 200, 255), false, 0.f, 0, 1.5f);
        }
}

// ------------------------------------------------------------------------------------------------------------ console

namespace
{
USCCargoSubsystem* Cargo(UWorld* World)
{
    USCCargoSubsystem* S = USCCargoSubsystem::Get(World);
    if (!S) UE_LOG(LogSCCargo, Warning, TEXT("CARGO_CMD no cargo subsystem in this world"));
    return S;
}
// A camera boom: the view pulled in towards the target until nothing stands between them (cargo and drones aside), so a
// box in an alcove or the elevator shaft is seen from inside rather than from behind a wall.
FVector Boom(UWorld* World, const FVector& Target, const FVector& At)
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoView), /*bTraceComplex=*/true);
    for (TActorIterator<ASCCargoContainer> It(World); It; ++It) Params.AddIgnoredActor(*It);
    for (TActorIterator<ASCCargoDrone> It(World); It; ++It) Params.AddIgnoredActor(*It);
    if (const APlayerController* PC = World->GetFirstPlayerController(); PC && PC->GetPawn()) Params.AddIgnoredActor(PC->GetPawn());
    FHitResult Hit;
    // A target inside something (a grid's centre in a wall) gives no boom to pull in: the view stays where asked.
    if (!World->SweepSingleByChannel(Hit, Target, At, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(25.f), Params) || Hit.bStartPenetrating)
        return At;
    return Hit.Location + (Target - At).GetSafeNormal() * 30.0;
}

bool Request(USCCargoSubsystem* S, const TArray<FString>& InArgs, FSCCargoTransferRequest& Out)
{
    // --owner=<id>: the requester (simulating another owner in a multi-owner hangar); the player by default.
    TArray<FString> Args;
    for (const FString& A : InArgs)
    {
        if (A.StartsWith(TEXT("--owner="))) Out.Requester = FName(*A.RightChop(8));
        else Args.Add(A);
    }
    if (Args.Num() < 4)
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_CMD usage: <from grids> <to grids> <commodity> <scu> [--owner=<id>] (grids: id, ship, elevator, label text; join with +)"));
        return false;
    }
    Out.FromGrids = S->ResolveGrids(Args[0], Out.Requester);
    Out.ToGrids = S->ResolveGrids(Args[1], Out.Requester);
    Out.Commodity = S->ResolveCommodity(Args[2]);
    Out.AmountSCU = FCString::Atoi(*Args[3]);
    return true;
}
void LogPreview(const FSCCargoTransferPreview& P, const FString& Tag)
{
    FString Stops;
    for (const int32 S : P.AchievableSCU) Stops += FString::Printf(TEXT(" %d"), S);
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_PREVIEW %s valid=%d requested=%d planned=%d moves=%d refusals=%d eta_s=%.0f achievable=[%s ]"), *Tag, P.bValid, P.RequestedSCU,
           P.PlannedSCU, P.Moves.Num(), P.Refusals.Num(), P.EstimatedSeconds, *Stops);
    for (const FSCCargoMovePlan& M : P.Moves)
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PREVIEW_MOVE box=%d scu=%d from=%d to=%d origin=%s cells=%s rotated=%d drones=%d"), M.BoxId, M.SCU, M.FromGrid,
               M.ToGrid, *M.Origin.ToString(), *M.Cells.ToString(), M.bRotated, M.DronesNeeded);
    for (const FSCCargoRefusal& R : P.Refusals)
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PREVIEW_REFUSAL box=%d scu=%d reason=\"%s\""), R.BoxId, R.SCU, *R.Reason.ToString());
}

FAutoConsoleCommandWithWorldAndArgs GSCCargoList(TEXT("sc.Cargo.List"), TEXT("sc.Cargo.List: grids, commodities, moving boxes, drones and jobs"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (USCCargoSubsystem* S = Cargo(World))
            for (const FString& Line : S->Describe()) UE_LOG(LogSCCargo, Display, TEXT("CARGO_LIST %s"), *Line);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoPreview(TEXT("sc.Cargo.Preview"), TEXT("sc.Cargo.Preview <from> <to> <commodity> <scu>: plan only, log moves and refusals"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        FSCCargoTransferRequest R;
        if (USCCargoSubsystem* S = Cargo(World); S && Request(S, Args, R)) LogPreview(S->Preview(R), FString::Join(Args, TEXT(" ")));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoTransfer(TEXT("sc.Cargo.Transfer"), TEXT("sc.Cargo.Transfer <from> <to> <commodity> <scu>: plan and start a transfer job"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        FSCCargoTransferRequest R;
        USCCargoSubsystem* S = Cargo(World);
        if (!S || !Request(S, Args, R)) return;
        LogPreview(S->Preview(R), FString::Join(Args, TEXT(" ")));
        FText Error;
        const int32 Job = S->Submit(R, Error);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_TRANSFER job=%d%s"), Job, Job == INDEX_NONE ? *(TEXT(" error=\"") + Error.ToString() + TEXT("\"")) : TEXT(""));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoCancel(TEXT("sc.Cargo.Cancel"), TEXT("sc.Cargo.Cancel <job id>: cancel a transfer (boxes in the air finish their move)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCCargoSubsystem* S = Cargo(World); S && Args.Num() > 0)
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_CANCEL job=%s ok=%d"), *Args[0], S->Cancel(FCString::Atoi(*Args[0])));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoAccess(TEXT("sc.Cargo.Access"), TEXT("sc.Cargo.Access <grids> open|close: extend or retract grids (refused while loaded)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCCargoSubsystem* S = Cargo(World);
        if (!S || Args.Num() < 2) return;
        const bool bOpen = Args[1].Equals(TEXT("open"), ESearchCase::IgnoreCase) || Args[1].Equals(TEXT("extend"), ESearchCase::IgnoreCase);
        for (const int32 Id : S->ResolveGrids(Args[0]))
        {
            FText Error;
            const bool bOK = S->RequestGridAccess(Id, bOpen, Error);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_ACCESS grid=%d request=%s ok=%d%s"), Id, bOpen ? TEXT("open") : TEXT("close"), bOK,
                   Error.IsEmpty() ? TEXT("") : *(TEXT(" reason=\"") + Error.ToString() + TEXT("\"")));
        }
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoViewMoving(TEXT("sc.Cargo.ViewMoving"),
    TEXT("sc.Cargo.ViewMoving [distance m=14] [height m=6] [yaw deg=30]: put the camera looking at the first box in the air (or a flying drone)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCCargoSubsystem* S = Cargo(World);
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!S || !PC || !PC->GetPawn()) return;
        FVector Target = FVector::ZeroVector;
        bool bFound = false;
        for (const FSCCargoBoxView& B : S->GetMovingBoxes()) { Target = B.WorldCenter; bFound = true; break; }
        if (!bFound)
            for (const FSCCargoDroneView& D : S->GetDrones())
                if (D.State != ESCCargoDroneState::Docked) { Target = D.WorldLocation; bFound = true; break; }
        if (!bFound) { UE_LOG(LogSCCargo, Display, TEXT("CARGO_VIEW nothing in the air")); return; }
        const double Distance = (Args.IsValidIndex(0) ? FCString::Atod(*Args[0]) : 14.0) * 100.0;
        const double Height = (Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : 6.0) * 100.0;
        const double Yaw = Args.IsValidIndex(2) ? FCString::Atod(*Args[2]) : 30.0;
        const FVector At = Boom(World, Target, Target - FRotator(0, Yaw, 0).Vector() * Distance + FVector(0, 0, Height));
        const FRotator View = (Target - At).Rotation();
        if (ASCViewerPawn* Pawn = Cast<ASCViewerPawn>(PC->GetPawn())) Pawn->SetView(At, View);
        else
        {
            PC->GetPawn()->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
            PC->SetControlRotation(View);
        }
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_VIEW moving target=%s at=%s"), *Target.ToCompactString(), *At.ToCompactString());
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoLoseDrone(TEXT("sc.Cargo.LoseDrone"),
    TEXT("sc.Cargo.LoseDrone <drone id | busy>: destroy a drone (verification: a team losing a member mid-move)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (!World || Args.IsEmpty()) return;
        const bool bBusy = Args[0].Equals(TEXT("busy"), ESearchCase::IgnoreCase);
        for (TActorIterator<ASCCargoDrone> It(World); It; ++It)
            if (bBusy ? It->MoveId != INDEX_NONE && It->CarryingBoxId != INDEX_NONE : It->DroneId == FCString::Atoi(*Args[0]))
            {
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_LOSE_DRONE drone=%d move=%d box=%d"), It->DroneId, It->MoveId, It->CarryingBoxId);
                It->Destroy();
                return;
            }
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_LOSE_DRONE none matches %s"), *Args[0]);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoSection(TEXT("sc.Cargo.Section"),
    TEXT("sc.Cargo.Section <grids> [reach m=30] [step m=1]: log sections of the collision around the grids (CARGO_SECTION)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCCargoSubsystem* S = Cargo(World);
        if (!S || Args.IsEmpty()) return;
        for (const int32 Id : S->ResolveGrids(Args[0]))
            S->LogSections(Id, Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : 30.0, Args.IsValidIndex(2) ? FCString::Atod(*Args[2]) : 1.0);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoDebug(TEXT("sc.Cargo.Debug"), TEXT("sc.Cargo.Debug 0|1: draw lattices, slots and drone paths"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCCargoSubsystem* S = Cargo(World)) S->SetDebugDraw(!Args.IsValidIndex(0) || FCString::Atoi(*Args[0]) != 0);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoOrigin(TEXT("sc.Cargo.Origin"), TEXT("sc.Cargo.Origin corner|bottom|centre: grid origin convention (verification)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*) {
        const FString Rule = Args.IsValidIndex(0) ? Args[0].ToLower() : FString();
        USCCargoGridComponent::OriginRule = Rule == TEXT("bottom") ? ESCCargoGridOrigin::BottomCentre
                                          : Rule == TEXT("centre") || Rule == TEXT("center") ? ESCCargoGridOrigin::Centre
                                          : Rule == TEXT("corner")                          ? ESCCargoGridOrigin::MinCorner
                                                                                            : ESCCargoGridOrigin::BottomCentre;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_ORIGIN rule=%d (0 corner, 1 bottom centre, 2 centre; placed boxes keep their pose)"), int32(USCCargoGridComponent::OriginRule));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCCargoView(TEXT("sc.Cargo.View"),
    TEXT("sc.Cargo.View <grids> [distance m=20] [height m=8] [yaw deg=0] [free]: put the camera looking at the grids (route shots); free: no boom"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCCargoSubsystem* S = Cargo(World);
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!S || !PC || !PC->GetPawn() || Args.IsEmpty()) return;
        FBox Box(ForceInit);
        for (const int32 Id : S->ResolveGrids(Args[0]))
            if (FSCCargoGridView V; S->GetGrid(Id, V)) Box += V.WorldBounds;
        if (!Box.IsValid) { UE_LOG(LogSCCargo, Warning, TEXT("CARGO_VIEW no grid matches %s"), *Args[0]); return; }
        const double Distance = (Args.IsValidIndex(1) ? FCString::Atod(*Args[1]) : 20.0) * 100.0;
        const double Height = (Args.IsValidIndex(2) ? FCString::Atod(*Args[2]) : 8.0) * 100.0;
        const double Yaw = Args.IsValidIndex(3) ? FCString::Atod(*Args[3]) : 0.0;
        const FVector Target = Box.GetCenter();
        const FVector Wanted = Target - FRotator(0, Yaw, 0).Vector() * Distance + FVector(0, 0, Height);
        const FVector At = Args.IsValidIndex(4) && Args[4].Equals(TEXT("free"), ESearchCase::IgnoreCase) ? Wanted : Boom(World, Target, Wanted); // "free": no boom (an outside view of an interior hold)
        const FRotator View = (Target - At).Rotation();
        if (ASCViewerPawn* Pawn = Cast<ASCViewerPawn>(PC->GetPawn())) Pawn->SetView(At, View);
        else
        {
            PC->GetPawn()->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
            PC->SetControlRotation(View);
        }
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_VIEW grids=%s at=%s view=%s"), *Args[0], *At.ToCompactString(), *View.ToCompactString());
    }));
} // namespace
