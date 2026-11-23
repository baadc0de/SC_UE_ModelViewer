// Hangar waypoints: which environment records are places a player walks to (by their DataCore components), their labels,
// ASCVehicle::BuildWaypoints, and the teleport menu's sections.
#include "SCHangarWaypoints.h"
#include "SCPackageReader.h"
#include "SCVehicle.h"
#include "SCEnvironment.h"
#include "Internationalization/Regex.h"

const TCHAR* SCHangarWaypoints::SourceName(ESCWaypointSource Source)
{
    switch (Source)
    {
    case ESCWaypointSource::TransitGateway: return TEXT("transit_gateway");
    case ESCWaypointSource::Panel: return TEXT("panel");
    case ESCWaypointSource::Inventory: return TEXT("inventory");
    case ESCWaypointSource::ShipPad: return TEXT("ship_pad");
    }
    return TEXT("-");
}

FString SCHangarWaypoints::CleanLabel(const FString& Name)
{
    FString Label = Name;
    // The editor's instance counter ("-029") and a mesh LOD suffix ("_LOD0") name a copy, not the place.
    for (const TCHAR* Pattern : {TEXT("-\\d+$"), TEXT("_LOD\\d+$")})
    {
        const FRegexPattern Suffix(Pattern, ERegexPatternFlags::CaseInsensitive);
        FRegexMatcher Match(Suffix, Label);
        if (Match.FindNext()) Label.LeftInline(Match.GetMatchBeginning());
    }
    return Label.Replace(TEXT("_"), TEXT(" ")).TrimStartAndEnd();
}

void SCHangarWaypoints::NumberDuplicates(TArray<FString>& Labels)
{
    TMap<FString, int32> Count, Seen;
    for (const FString& L : Labels) ++Count.FindOrAdd(L);
    for (FString& L : Labels)
        if (Count[L] > 1)
        {
            const int32 N = ++Seen.FindOrAdd(L);
            L = FString::Printf(TEXT("%s #%d"), *L, N);
        }
}

namespace
{
/** A placement's authored entity name (the reader keeps the mesh path in EntityName for geometry; several kiosk classes
 *  share one mesh). */
const FString& PlacementName(const FSCSceneInstance& P) { return P.SocEntityName.IsEmpty() ? P.EntityName : P.SocEntityName; }
}

TArray<FSCWaypointCandidate> SCHangarWaypoints::Select(const FSCSceneManifest& Manifest)
{
    TArray<FSCWaypointCandidate> GatewayCandidates, Panels, Inventory;
    for (int32 I = 0; I < Manifest.Interiors.Num(); ++I)
    {
        const FSCInterior& Interior = Manifest.Interiors[I];
        // A transit gateway is where the transit system (the personnel elevator) lets the player in: the hangar's arrival.
        int32 Gateways = 0;
        for (int32 E = 0; E < Interior.Entities.Num(); ++E)
            if (Interior.Entities[E].Kind == TEXT("transit_gateway"))
            {
                GatewayCandidates.Add({ESCWaypointSource::TransitGateway, I, E, CleanLabel(Interior.Entities[E].EntityName)});
                ++Gateways;
            }
        // A panel the player operates (kiosks, transit panels). A transit panel is an operated UI whose owner is not an
        // attachable item; a container holding one transit gateway and one transit panel is a gateway with its call panel,
        // and the gateway stands for it. With more of either, which panel serves which gateway is not in the data: all stay.
        TArray<int32> Operated, Transit;
        for (int32 P = 0; P < Interior.Placements.Num(); ++P)
        {
            const TArray<FSCUiBinding>& Bindings = Interior.Placements[P].UiBindings;
            if (!Bindings.ContainsByPredicate([](const FSCUiBinding& B) { return B.IsOperatingPanel(); })) continue;
            Operated.Add(P);
            if (!Bindings.ContainsByPredicate([](const FSCUiBinding& B) { return B.IsOperatingPanel() && !B.SourceItemType.IsEmpty(); })) Transit.Add(P);
        }
        const int32 CallPanel = Gateways == 1 && Transit.Num() == 1 ? Transit[0] : INDEX_NONE;
        for (int32 P : Operated)
            if (P != CallPanel) Panels.Add({ESCWaypointSource::Panel, I, P, CleanLabel(PlacementName(Interior.Placements[P]))});
    }
    // Inventory containers of the environment's interiors: item banks, the freight elevator's cargo grid.
    for (int32 C = 0; C < Manifest.InventoryContainers.Num(); ++C)
        if (const FSCInventoryContainerRecord& R = Manifest.InventoryContainers[C]; R.InteriorIndex != INDEX_NONE)
            Inventory.Add({ESCWaypointSource::Inventory, R.InteriorIndex, C, CleanLabel(R.EntityClass)});
    TArray<FSCWaypointCandidate> Out = MoveTemp(GatewayCandidates);
    Out.Append(Panels);
    Out.Append(Inventory);
    return Out;
}

int32 ASCVehicle::BuildWaypoints()
{
    FBox Bay(ForceInit);
    FString BayName;
    const bool bBay = FSCEnvironment::FindShipBay(this, Bay, &BayName);
    // A cooked environment has no import sources: it keeps the waypoints its cook saved; with its rigs kept (WP-O1-4b) only
    // the ship-pad entry follows the elevator.
    if (WaypointSources.IsEmpty())
    {
        const FName ShipPad = SCHangarWaypoints::SourceName(ESCWaypointSource::ShipPad);
        if (bBay)
        {
            FSCSpawnPoint* Pad = Waypoints.FindByPredicate([&](const FSCSpawnPoint& P) { return P.Source == ShipPad; });
            if (!Pad)
            {
                // The ship pad is the only waypoint the data has (nothing was saved for it): it is the bay's.
                Pad = &Waypoints.AddDefaulted_GetRef();
                Pad->Kind = FSCSpawnKind::Hangar;
                Pad->Source = ShipPad;
                Pad->DisplayName = SCHangarWaypoints::CleanLabel(BayName);
            }
            Pad->Transform = FTransform(GetActorTransform().InverseTransformPosition(FVector(Bay.GetCenter().X, Bay.GetCenter().Y, Bay.Max.Z)));
            ++WaypointRevision;
        }
        UE_LOG(LogTemp, Display, TEXT("HANGAR_WAYPOINTS kept=%d (no sources: a cooked environment, or no waypoints in the data)"), Waypoints.Num());
        return Waypoints.Num();
    }
    TArray<FSCSpawnPoint> Points;
    TArray<FString> Labels;
    const FTransform Actor = GetActorTransform();
    auto Add = [&](ESCWaypointSource Source, const FString& Label, const FVector& World) {
        FSCSpawnPoint& P = Points.AddDefaulted_GetRef();
        P.Kind = FSCSpawnKind::Hangar;
        P.Source = SCHangarWaypoints::SourceName(Source);
        P.Transform = FTransform(Actor.InverseTransformPosition(World));
        Labels.Add(Label);
    };
    for (const FSCWaypointSource& S : WaypointSources)
        if (const USceneComponent* Anchor = S.Anchor.Get()) Add(S.Source, S.Label, Anchor->GetComponentTransform().TransformPosition(S.Local));
    if (bBay) Add(ESCWaypointSource::ShipPad, SCHangarWaypoints::CleanLabel(BayName), FVector(Bay.GetCenter().X, Bay.GetCenter().Y, Bay.Max.Z));
    SCHangarWaypoints::NumberDuplicates(Labels);
    for (int32 I = 0; I < Points.Num(); ++I)
    {
        Points[I].DisplayName = Labels[I];
        UE_LOG(LogTemp, Display, TEXT("HANGAR_WAYPOINT index=%d source=%s pos=%s label=\"%s\""), I, *Points[I].Source.ToString(),
               *Points[I].Transform.GetLocation().ToCompactString(), *Labels[I]);
    }
    Waypoints = MoveTemp(Points);
    ++WaypointRevision;
    UE_LOG(LogTemp, Display, TEXT("HANGAR_WAYPOINTS count=%d sources=%d ship_pad=%d"), Waypoints.Num(), WaypointSources.Num(), bBay);
    return Waypoints.Num();
}

TArray<FSCTeleportRow> SCTeleportMenu::Rows(int32 ShipCount, int32 HangarCount)
{
    TArray<FSCTeleportRow> Out;
    auto Section = [&](FSCTeleportRow::EType Type, int32 Count) {
        if (Count <= 0) return;
        Out.Add({FSCTeleportRow::EType::Heading, int32(Type)}); // a heading's Index names its section
        for (int32 I = 0; I < Count; ++I) Out.Add({Type, I});
    };
    Section(FSCTeleportRow::EType::Ship, ShipCount);
    Section(FSCTeleportRow::EType::Hangar, HangarCount);
    return Out;
}

int32 SCTeleportMenu::Step(const TArray<FSCTeleportRow>& Rows, int32 Current, int32 Delta)
{
    TArray<int32> Destinations;
    for (int32 I = 0; I < Rows.Num(); ++I)
        if (Rows[I].IsDestination()) Destinations.Add(I);
    if (Destinations.IsEmpty()) return INDEX_NONE;
    const int32 At = Destinations.IndexOfByKey(Current);
    if (At == INDEX_NONE) return Destinations[0];
    const int32 N = Destinations.Num();
    return Destinations[((At + Delta) % N + N) % N];
}
