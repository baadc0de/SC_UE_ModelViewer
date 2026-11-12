// Room-volume parsing, containment and the per-room light confinement on three lighting channels
// (see SCLightAreas.h, WP-U3b §12, WP-L1).
#include "SCLightAreas.h"
#include "SCVehicle.h"
#include "SCImporter.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "MeshDescription.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/UObjectIterator.h"
#include "Algo/Count.h"
namespace
{
// Trimesh containment, the exporter's room_containment.rs: parity of the triangles a +Z ray crosses.
// Signed area of the directed edge A -> B about the origin and whether the origin is to its left; false for an
// edge with no extent. An origin exactly on the edge's line is placed as if it sat an infinitesimal (e, e^2)
// further along +X, +Y. The area is evaluated in one vertex order whichever way the edge is traversed, so the
// two triangles sharing an edge always get opposite sides.
bool EdgeSide(const FVector& A, const FVector& B, double& Area, bool& bLeft)
{
    const bool bReversed = B.X < A.X || (B.X == A.X && B.Y < A.Y);
    const FVector& First = bReversed ? B : A;
    const FVector& Second = bReversed ? A : B;
    const double Forward = First.X * Second.Y - First.Y * Second.X;
    Area = bReversed ? -Forward : Forward;
    if (Area != 0) { bLeft = Area > 0; return true; }
    if (A.Y != B.Y) { bLeft = A.Y > B.Y; return true; }
    if (A.X != B.X) { bLeft = B.X > A.X; return true; }
    return false;
}
// Whether the +Z ray from the origin crosses triangle ABC: the origin is on the same side of all three edges
// seen along the ray, and the surface lies ahead. No edge ever answers "on the edge", so a ray through an edge
// or a vertex of a closed soup counts once per side, and a face seen edge-on is never crossed.
bool UpwardRayCrosses(const FVector& A, const FVector& B, const FVector& C)
{
    double AB, BC, CA;
    bool bAB, bBC, bCA;
    if (!EdgeSide(A, B, AB, bAB) || !EdgeSide(B, C, BC, bBC) || !EdgeSide(C, A, CA, bCA) || bAB != bBC || bBC != bCA) return false;
    const double Area = AB + BC + CA; // each edge weights the opposite corner
    return Area != 0 && (BC * A.Z + CA * B.Z + AB * C.Z) / Area > 0;
}
bool Vector3(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!Value || !Value->TryGetArray(A) || A->Num() != 3) return false;
    double V[3];
    for (int32 I = 0; I < 3; ++I)
        if (!(*A)[I]->TryGetNumber(V[I]) || !FMath::IsFinite(V[I])) return false;
    Out = FVector(V[0], V[1], V[2]);
    return true;
}
bool Vectors(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, TArray<FVector>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(Key, A)) return false;
    Out.Reset(A->Num());
    for (const auto& V : *A)
        if (!Vector3(V, Out.AddDefaulted_GetRef())) return false;
    return true;
}
}

bool FSCRoomVolume::Parse(const FString& Json, FSCRoomVolume& Out, FString& Error)
{
    Out = FSCRoomVolume();
    TSharedPtr<FJsonObject> J;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), J) || !J) { Error = TEXT("room volume is not a JSON object"); return false; }
    J->TryGetStringField(TEXT("room"), Out.Room);
    J->TryGetStringField(TEXT("room_name"), Out.RoomName);
    J->TryGetStringField(TEXT("source"), Out.Source);
    // Column-major 4x4 (segment -> container). UE matrices use row vectors, so column C is row C.
    const TArray<TSharedPtr<FJsonValue>>* Columns;
    FMatrix ToContainer = FMatrix::Identity;
    if (!J->TryGetArrayField(TEXT("transform"), Columns) || Columns->Num() != 4) { Error = TEXT("room volume transform is not 4x4"); return false; }
    for (int32 C = 0; C < 4; ++C)
    {
        const TArray<TSharedPtr<FJsonValue>>* Column;
        if (!(*Columns)[C]->TryGetArray(Column) || Column->Num() != 4) { Error = TEXT("room volume transform is not 4x4"); return false; }
        for (int32 R = 0; R < 4; ++R)
        {
            double V;
            if (!(*Column)[R]->TryGetNumber(V) || !FMath::IsFinite(V)) { Error = TEXT("room volume transform is not numeric"); return false; }
            ToContainer.M[C][R] = V;
        }
    }
    if (FMath::IsNearlyZero(ToContainer.Determinant(), 1e-12)) { Error = TEXT("room volume transform is singular"); return false; }
    Out.FromContainer = ToContainer.Inverse();
    FString Kind;
    J->TryGetStringField(TEXT("kind"), Kind);
    if (Kind == TEXT("box"))
    {
        Out.Shape = EShape::Box;
        if (!Vector3(J->TryGetField(TEXT("box_min")), Out.Min) || !Vector3(J->TryGetField(TEXT("box_max")), Out.Max))
        { Error = TEXT("room volume box needs box_min and box_max"); return false; }
    }
    else if (Kind == TEXT("prism"))
    {
        Out.Shape = EShape::Prism;
        if (!Vectors(J, TEXT("points"), Out.Points) || Out.Points.Num() < 3) { Error = TEXT("room volume prism needs a polygon"); return false; }
        if (!J->TryGetNumberField(TEXT("height"), Out.Height) || !FMath::IsFinite(Out.Height)) { Error = TEXT("room volume prism needs a height"); return false; }
    }
    else if (Kind == TEXT("trimesh"))
    {
        Out.Shape = EShape::Trimesh;
        if (!Vectors(J, TEXT("vertices"), Out.Points) || Out.Points.IsEmpty() || Out.Points.Num() % 3 != 0)
        { Error = TEXT("room volume trimesh needs three vertices per triangle"); return false; }
    }
    else { Error = TEXT("unsupported room volume kind '") + Kind + TEXT("'"); return false; }
    return true;
}

bool FSCRoomVolume::Contains(const FVector& ContainerPointM) const
{
    const FVector P = FromContainer.TransformPosition(ContainerPointM);
    if (P.ContainsNaN()) return false;
    switch (Shape)
    {
    case EShape::Box:
        return P.X >= Min.X && P.X <= Max.X && P.Y >= Min.Y && P.Y <= Max.Y && P.Z >= Min.Z && P.Z <= Max.Z;
    case EShape::Prism:
    {
        double Base = TNumericLimits<double>::Max();
        for (const FVector& Q : Points) Base = FMath::Min(Base, Q.Z);
        if (P.Z < Base || P.Z > Base + Height) return false;
        bool bInside = false; // even-odd rule on the xy polygon
        for (int32 I = 0; I < Points.Num(); ++I)
        {
            const FVector& A = Points[I];
            const FVector& B = Points[(I + 1) % Points.Num()];
            if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X) bInside = !bInside;
        }
        return bInside;
    }
    case EShape::Trimesh:
    {
        int32 Crossings = 0; // an odd count is inside a closed soup
        for (int32 I = 0; I + 2 < Points.Num(); I += 3)
            Crossings += UpwardRayCrosses(Points[I] - P, Points[I + 1] - P, Points[I + 2] - P);
        return Crossings % 2 == 1;
    }
    }
    return false;
}

double FSCRoomVolume::BoundsDistance(const FVector& ContainerPointM) const
{
    const FVector P = FromContainer.TransformPosition(ContainerPointM);
    if (P.ContainsNaN()) return TNumericLimits<double>::Max();
    FBox Bounds(ForceInit);
    if (Shape == EShape::Box)
        Bounds = FBox(Min, Max);
    else
    {
        for (const FVector& Q : Points) Bounds += Q;
        if (Shape == EShape::Prism) Bounds.Max.Z = Bounds.Min.Z + Height; // the polygon is the base, extruded along +z
    }
    return FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(P));
}

bool FSCLightArea::Contains(const FVector& World) const
{
    const USceneComponent* A = Anchor.Get();
    if (!A) return false;
    const FVector Point = AnchorToContainer.TransformPosition(A->GetComponentTransform().InverseTransformPosition(World));
    for (const FSCRoomVolume& Volume : Volumes)
        if (Volume.Contains(Point)) return true;
    return false;
}

FString FSCLightArea::RoomAt(const FVector& World) const
{
    const USceneComponent* A = Anchor.Get();
    if (!A) return FString();
    const FVector Point = AnchorToContainer.TransformPosition(A->GetComponentTransform().InverseTransformPosition(World));
    for (const FSCRoomVolume& Volume : Volumes)
        if (Volume.Contains(Point)) return Volume.Room;
    return FString();
}

double FSCLightArea::RoomDistance(const FVector& World, const FString& Room) const
{
    double Best = TNumericLimits<double>::Max();
    const USceneComponent* A = Anchor.Get();
    if (!A) return Best;
    const FVector Point = AnchorToContainer.TransformPosition(A->GetComponentTransform().InverseTransformPosition(World));
    for (const FSCRoomVolume& Volume : Volumes)
        if (Volume.Room == Room) Best = FMath::Min(Best, Volume.BoundsDistance(Point));
    return Best;
}

TArray<FVector3f> FSCLightAreas::Sample(const FMeshDescription& Description)
{
    TArray<FVector3f> Out;
    const int32 Count = Description.Vertices().Num();
    if (Count == 0) return Out;
    const auto Positions = FStaticMeshConstAttributes(Description).GetVertexPositions();
    const int32 Wanted = FMath::Min(Count, MaxSamples);
    Out.Reserve(Wanted);
    int32 Index = 0;
    for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
    {
        // Sample K is vertex floor(K * Count / Wanted): an even stride over the vertex order.
        if (Out.Num() < Wanted && Index == int32(int64(Out.Num()) * Count / Wanted)) Out.Add(Positions[Vertex]);
        ++Index;
    }
    return Out;
}

void FSCLightAreas::Register(ASCVehicle* Vehicle, USceneComponent* Anchor, const TArray<FString>& VolumeJson, bool bHelperFrame,
                             TArray<FString>& Warnings)
{
    if (!Vehicle || !Anchor || VolumeJson.IsEmpty()) return;
    FSCLightArea Area;
    Area.Anchor = Anchor;
    // The same container -> anchor mapping the light importer applies to light positions (metres -> cm).
    const FVector X = FSCLightImporter::ContainerBasis(FVector(1, 0, 0), bHelperFrame) * 100;
    const FVector Y = FSCLightImporter::ContainerBasis(FVector(0, 1, 0), bHelperFrame) * 100;
    const FVector Z = FSCLightImporter::ContainerBasis(FVector(0, 0, 1), bHelperFrame) * 100;
    Area.AnchorToContainer = FMatrix(FPlane(X, 0), FPlane(Y, 0), FPlane(Z, 0), FPlane(0, 0, 0, 1)).Inverse();
    for (const FString& Text : VolumeJson)
    {
        FString Error;
        FSCRoomVolume Volume;
        if (FSCRoomVolume::Parse(Text, Volume, Error))
            Area.Volumes.Add(MoveTemp(Volume));
        else
            Warnings.Add(Anchor->GetName() + TEXT(": ") + Error);
    }
    if (!Area.Volumes.IsEmpty()) Vehicle->LightAreas.Add(MoveTemp(Area));
}

TMap<FString, int32> FSCLightAreas::ColorRooms(const TArray<FString>& Rooms, const TArray<TPair<FString, FString>>& Reach,
                                               int32& Conflicts)
{
    TArray<FString> Order; // unique rooms in the given order
    for (const FString& Room : Rooms) Order.AddUnique(Room);
    const int32 N = Order.Num();
    // Reaching lights per unordered room pair (index pair), from one entry per light.
    TMap<TPair<int32, int32>, int32> Weight;
    for (const auto& [A, B] : Reach)
    {
        const int32 IA = Order.IndexOfByKey(A), IB = Order.IndexOfByKey(B);
        if (IA == INDEX_NONE || IB == INDEX_NONE || IA == IB) continue;
        ++Weight.FindOrAdd({FMath::Min(IA, IB), FMath::Max(IA, IB)});
    }
    TArray<int32> Pick; // per room: 0 = channel 1, 1 = channel 2
    Pick.Init(0, N);
    auto Cost = [&](const TArray<int32>& Bits) {
        int64 Leak = 0;
        for (const auto& [Pair, W] : Weight)
            if (Bits[Pair.Key] == Bits[Pair.Value]) Leak += W;
        return Leak;
    };
    if (N <= MaxExactRooms)
    {
        // Every split with the first room on channel 1 (flipping all channels changes nothing): least leak, then the
        // most balanced split, then the first such split in counting order.
        int64 BestLeak = TNumericLimits<int64>::Max();
        int32 BestImbalance = TNumericLimits<int32>::Max();
        TArray<int32> Bits;
        Bits.Init(0, N);
        for (int64 Mask = 0; Mask < (int64(1) << FMath::Max(0, N - 1)); ++Mask)
        {
            int32 Ones = 0;
            for (int32 I = 1; I < N; ++I) Ones += Bits[I] = (Mask >> (I - 1)) & 1;
            const int64 Leak = Cost(Bits);
            const int32 Imbalance = FMath::Abs(N - 2 * Ones);
            if (Leak < BestLeak || (Leak == BestLeak && Imbalance < BestImbalance))
            {
                BestLeak = Leak;
                BestImbalance = Imbalance;
                Pick = Bits;
            }
        }
    }
    else
    {
        // Greedy: each room takes the channel its coloured neighbours reach it with least, ties to the less used one.
        int32 Used[2] = {0, 0};
        for (int32 I = 0; I < N; ++I)
        {
            int64 Near[2] = {0, 0};
            for (const auto& [Pair, W] : Weight)
                if (const int32 Other = Pair.Key == I ? Pair.Value : Pair.Value == I ? Pair.Key : INDEX_NONE; Other != INDEX_NONE && Other < I)
                    Near[Pick[Other]] += W;
            Pick[I] = Near[0] != Near[1] ? int32(Near[1] < Near[0]) : int32(Used[1] < Used[0]);
            ++Used[Pick[I]];
        }
    }
    TMap<FString, int32> Channel;
    for (int32 I = 0; I < N; ++I) Channel.Add(Order[I], 1 + Pick[I]);
    Conflicts = Algo::CountIf(Weight, [&](const auto& Entry) { return Pick[Entry.Key.Key] == Pick[Entry.Key.Value]; });
    return Channel;
}

void FSCLightAreas::SetChannel(UPrimitiveComponent* Component, int32 Channel)
{
    if (Component) Component->SetLightingChannels(Channel == 0, Channel == 1, Channel == 2);
}

void FSCLightAreas::Apply(ASCVehicle* Vehicle)
{
    if (!Vehicle) return;
    int32 Volumes = 0;
    TArray<FString> Rooms;
    TMap<FString, FString> RoomNames; // GUID -> authored label, for the log only
    for (const FSCLightArea& Area : Vehicle->LightAreas)
        for (const FSCRoomVolume& Volume : Area.Volumes)
        {
            ++Volumes;
            Rooms.AddUnique(Volume.Room);
            RoomNames.FindOrAdd(Volume.Room, Volume.RoomName);
        }
    if (Volumes == 0)
    {
        UE_LOG(LogSCImport, Display, TEXT("LIGHT_AREAS volumes=0"));
        return;
    }
    // Lights: the room each sits in, and which other rooms an area-only light's attenuation sphere reaches.
    TArray<TPair<FString, FString>> Reach;
    TMap<TPair<FString, FString>, TPair<int32, double>> ReachDetail; // lights per pair, deepest radius overlap (m)
    for (FSCImportedLightState& State : Vehicle->LightStates)
    {
        const ULightComponent* Light = State.Component;
        if (!Light) continue;
        const FVector At = Light->GetComponentLocation();
        State.AreaRoom = Vehicle->LightRoomAt(At);
        State.bInsideArea = !State.AreaRoom.IsEmpty();
        if (!State.bAreaOnly || !State.bInsideArea) continue;
        for (const FString& Other : Rooms)
        {
            if (Other == State.AreaRoom) continue;
            double Distance = TNumericLimits<double>::Max();
            for (const FSCLightArea& Area : Vehicle->LightAreas) Distance = FMath::Min(Distance, Area.RoomDistance(At, Other));
            if (Distance > State.RadiusM) continue;
            Reach.Add({State.AreaRoom, Other}); // one entry per light: the colouring weighs pairs by reaching lights
            auto& Detail = ReachDetail.FindOrAdd({State.AreaRoom, Other}, {0, 0.});
            ++Detail.Key;
            Detail.Value = FMath::Max(Detail.Value, State.RadiusM - Distance);
        }
    }
    int32 Conflicts = 0;
    Vehicle->LightRoomChannels = ColorRooms(Rooms, Reach, Conflicts);
    // Area-only lights take their room's channel (0 outside); everything else is the game's AffectsEverything.
    int32 LightsInside = 0, LightsOutside = 0, LightsEverywhere = 0;
    for (FSCImportedLightState& State : Vehicle->LightStates)
    {
        ULightComponent* Light = State.Component;
        if (!Light) continue;
        if (!State.bAreaOnly)
        {
            Light->SetLightingChannels(true, true, true);
            ++LightsEverywhere;
        }
        else
        {
            const int32 Channel = State.bInsideArea ? Vehicle->LightRoomChannels.FindRef(State.AreaRoom) : 0;
            Light->SetLightingChannels(Channel == 0, Channel == 1, Channel == 2);
            ++(State.bInsideArea ? LightsInside : LightsOutside);
        }
        UE_LOG(LogSCImport, Verbose, TEXT("LIGHT_AREA_LIGHT name=%s owner=%s area_only=%d room=%s"), *State.Name, *State.OwnerEntity,
               State.bAreaOnly, *RoomNames.FindRef(State.AreaRoom));
    }
    // Surfaces: the game gates per pixel; a component is inside when most of its sampled vertices are inside a room
    // (WP-U3b vote) and then takes the room most of those are in (ties: room order). Samples landing in another
    // channel than their component's are counted: the residual of per-component channels against per-pixel volumes.
    int32 MeshesInside = 0, MeshesOutside = 0, SamplesTotal = 0, SamplesElsewhere = 0;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(Vehicle);
    for (UPrimitiveComponent* Component : Primitives)
    {
        const UObject* Asset = nullptr;
        if (const auto* Static = Cast<UStaticMeshComponent>(Component)) Asset = Static->GetStaticMesh();
        else if (const auto* Skinned = Cast<USkinnedMeshComponent>(Component)) Asset = Skinned->GetSkinnedAsset();
        const TArray<FVector3f>* Samples = Asset ? Vehicle->MeshSamples.Find(Asset) : nullptr;
        TArray<FString> SampleRooms;
        if (Samples && !Samples->IsEmpty())
        {
            const FTransform& ToWorld = Component->GetComponentTransform();
            for (const FVector3f& Sample : *Samples) SampleRooms.Add(Vehicle->LightRoomAt(ToWorld.TransformPosition(FVector(Sample))));
        }
        else
            SampleRooms.Add(Vehicle->LightRoomAt(Component->Bounds.Origin));
        TMap<FString, int32> Votes;
        int32 Inside = 0;
        for (const FString& Room : SampleRooms)
            if (!Room.IsEmpty()) { ++Inside; ++Votes.FindOrAdd(Room); }
        FString Room;
        if (2 * Inside > SampleRooms.Num())
            for (const FString& Candidate : Rooms)
                if (Votes.FindRef(Candidate) > Votes.FindRef(Room)) Room = Candidate;
        const int32 Channel = Room.IsEmpty() ? 0 : Vehicle->LightRoomChannels.FindRef(Room);
        SetChannel(Component, Channel);
        ++(Room.IsEmpty() ? MeshesOutside : MeshesInside);
        SamplesTotal += SampleRooms.Num();
        for (const FString& Sampled : SampleRooms)
            SamplesElsewhere += (Sampled.IsEmpty() ? 0 : Vehicle->LightRoomChannels.FindRef(Sampled)) != Channel;
        UE_LOG(LogSCImport, Verbose, TEXT("LIGHT_AREA_MESH component=%s asset=%s inside=%d/%d room=%s channel=%d"), *Component->GetName(),
               Component->ComponentTags.Num() ? *Component->ComponentTags[0].ToString() : *GetNameSafe(Asset), Inside, SampleRooms.Num(),
               *RoomNames.FindRef(Room), Channel);
    }
    // Lights that belong to no vehicle (hangar panels, sun, the player's torch) are not area lights: every channel.
    int32 Environment = 0;
    for (TObjectIterator<ULightComponent> It; It; ++It)
    {
        ULightComponent* Light = *It;
        if (Light->GetWorld() != Vehicle->GetWorld() || Cast<ASCVehicle>(Light->GetOwner()) ||
            (Light->LightingChannels.bChannel1 && Light->LightingChannels.bChannel2))
            continue;
        Light->SetLightingChannels(Light->LightingChannels.bChannel0, true, true);
        ++Environment;
    }
    FString Channels;
    for (const FString& Room : Rooms) Channels += FString::Printf(TEXT("%s%s:%d"), Channels.IsEmpty() ? TEXT("") : TEXT(","), *RoomNames.FindRef(Room),
                                                                  Vehicle->LightRoomChannels.FindRef(Room));
    FString Pairs;
    for (const auto& [Key, Detail] : ReachDetail)
    {
        const auto& [A, B] = Key;
        Pairs += FString::Printf(TEXT("%s%s>%s(%d,%.2fm%s)"), Pairs.IsEmpty() ? TEXT("") : TEXT(","), *RoomNames.FindRef(A), *RoomNames.FindRef(B),
                                 Detail.Key, Detail.Value,
                                 Vehicle->LightRoomChannels.FindRef(A) == Vehicle->LightRoomChannels.FindRef(B) ? TEXT(",shared") : TEXT(""));
    }
    UE_LOG(LogSCImport, Display,
           TEXT("LIGHT_AREAS volumes=%d rooms=%d room_channels=%s reach=%s conflicts=%d lights_inside=%d lights_outside=%d "
                "lights_everywhere=%d meshes_inside=%d meshes_outside=%d samples_other_channel=%d/%d environment_lights=%d"),
           Volumes, Rooms.Num(), *Channels, Pairs.IsEmpty() ? TEXT("-") : *Pairs, Conflicts, LightsInside, LightsOutside, LightsEverywhere,
           MeshesInside, MeshesOutside, SamplesElsewhere, SamplesTotal, Environment);
}
