// WP-P5: assembling an interior's lifts and interaction network at import (SCImportJob calls BuildInteriorLifts once per
// interior, after its placements exist and before instancing merges repeated parts).
#pragma once
#include "CoreMinimal.h"
#include "SCPackageReader.h"

class ASCVehicle;
class USceneComponent;
class USCRigComponent;

/** What the importer placed for one interior entity id. */
struct FSCLiftBuildPart
{
    USceneComponent* Anchor = nullptr;
    USCRigComponent* Rig = nullptr;
};

namespace SCVehicleLifts
{
/** Register the interior's interaction links and build its lifts: a moving anchor per carriage with the carriage and its
 *  riders re-parented under it, their doors, stop frames and names, the panels that operate it. */
void BuildInteriorLifts(ASCVehicle* Vehicle, int32 InteriorIndex, const FSCInterior& Interior, USceneComponent* Container, bool bHelper,
                        const TMap<FString, FSCLiftBuildPart>& Parts);
/** The entity ids that are lift carriages (an entity that is a lift is not a door pad, DOORS-1). */
TSet<FString> CarriageIds(const FSCInterior& Interior);
/** WP-P5 ground stop: the first blocking surface under lift LiftIndex's carriage on its way down from where it stands to stop
 *  To (a sweep of the box of its colliders against world collision; the vehicle and pawns excluded). OutTravel: metres along
 *  the leg; OutBy: what was hit and where the carriage's bottom rests. False: nothing in the leg, or not a leg downward. */
bool GroundContact(const ASCVehicle* Vehicle, int32 LiftIndex, int32 To, double& OutTravel, FString& OutBy);
}
