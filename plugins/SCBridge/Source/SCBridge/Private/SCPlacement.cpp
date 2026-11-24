// WP-O1-4c-3a: USCPlacementLibrary (SCPlacement.h), split from SCImporter.cpp.
#include "SCPlacement.h"
#include "SCEnvironment.h"
#include "SCImportLog.h"
#include "SCVehicle.h"
int32 USCPlacementLibrary::RestOnFloor(ASCVehicle* Vehicle, double FloorZ)
{
    if (!Vehicle) return 0;
    const int32 GearClips = Vehicle->RestOnLandingGear();
    FString LowestPart;
    const double Lowest = Vehicle->LowestVisibleZ(&LowestPart);
    UE_LOG(LogSCImport, Display, TEXT("VIEWER_REST gear_clips=%d lowest_z=%.1f part=%s"), GearClips, Lowest, *LowestPart);
    if (Lowest < TNumericLimits<double>::Max()) Vehicle->AddActorWorldOffset(FVector(0, 0, FloorZ - Lowest));
    return GearClips;
}
FVector USCPlacementLibrary::PresentEnvironment(ASCVehicle* Environment, const FString& State, double FloorZ)
{
    FSCEnvironment::SetState(Environment, State);
    const FVector Offset = FSCEnvironment::PlaceOnFloor(Environment, FloorZ, nullptr);
    if (Environment) Environment->BuildWaypoints(); // in the presented pose, while the rigs are alive (a cook saves them)
    return Offset;
}
