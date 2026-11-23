// WP-O1-4b-2: a prepared player body's parsed kit (FSCPlayerKit) and body clips (FSCPlayerClipMap), saved with the body's
// actor in its prepared level, so a run attaches the body without the archive and without parsing anything again. The plain
// structs stay plain (no reflection): this object writes and reads them field by field (SCPlayerKitData.cpp).
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SCPlayerAnimator.h"
#include "SCPlayerKitData.generated.h"

UCLASS()
class SCBRIDGE_API USCPlayerKitData : public UObject
{
    GENERATED_BODY()
public:
    /** Layout of what Serialize writes: a field added to the kit or clip structs is written in SCPlayerKitData.cpp and bumps
     *  this; data of another layout loads as none, read to its end and skipped (the body is then unavailable, and the prep
     *  step's format refuses the level before that). */
    static constexpr int32 Version = 1;
    /** Both set, or neither (nothing prepared, or data of another layout). */
    TSharedPtr<const FSCPlayerKit> Kit;
    TSharedPtr<const FSCPlayerClipMap> Clips;
    bool HasData() const { return Kit.IsValid() && Clips.IsValid(); }
    virtual void Serialize(FArchive& Ar) override;
    /** The payload Serialize carries as one byte array (names as text). */
    static void WritePayload(const FSCPlayerKit& Kit, const FSCPlayerClipMap& Clips, TArray<uint8>& Out);
    /** Parse a payload of Layout: false (both reset) for another layout, or bytes that are not exactly one payload. */
    static bool ReadPayload(int32 Layout, const TArray<uint8>& Bytes, TSharedPtr<const FSCPlayerKit>& OutKit, TSharedPtr<const FSCPlayerClipMap>& OutClips);
};

// Field by field, in declaration order (SCPlayerKit.h, SCPlayerAnimator.h).
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCItemPort& Port);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCFireAction& Action);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCWeaponFragment::FLayer& Layer);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCWeaponFragment& Fragment);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCPlayerWeapon& Weapon);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCMotionLayer& Layer);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCMotionFragment& Fragment);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCMotionState& State);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCBlendDimension& Dimension);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCBlendExample& Example);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCBlendSelector& Selector);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCBlendSpace& Space);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCFlashlight& Light);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCPlayerKit::FPart& Part);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCPlayerKit& Kit);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCPlayerClip::FTrack& Track);
SCBRIDGE_API FArchive& operator<<(FArchive& Ar, FSCPlayerClip& Clip);
