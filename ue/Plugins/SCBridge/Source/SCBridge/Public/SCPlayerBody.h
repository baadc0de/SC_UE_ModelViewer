// WP-P2b: the real player body on ASCFirstPersonCharacter. Takes an imported player-kit actor (rigs for every
// skinned part, items on their port joints), drives the leader rig with FSCPlayerAnimator, copies the pose to the
// follower parts by joint name, moves weapons between stow and hand ports, places the camera at the player's
// camera joint (first person) or behind the body (third person) and hides head parts in first person.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SCPlayerAnimator.h"
#include "SCImporter.h"
#include "SCPlayerBody.generated.h"

class ASCVehicle;
class ASCFirstPersonCharacter;
class USCRigComponent;
class UPrimitiveComponent;

UCLASS()
class SCBRIDGE_API USCPlayerBodyComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    USCPlayerBodyComponent();
    /** Rig of the kit's root part (the full body skeleton); null if the actor is not a player kit. */
    static USCRigComponent* FindLeader(const ASCVehicle* Kit);
    /** Import options of a player kit: unpublished, no collision, every skin a rig, no clip catalogue. */
    static FSCImportOptions KitImportOptions(UWorld* World, const FString& ScenePath);
    /** Leader bone name -> index, for routing clip sidecars (game thread; the map is then worker-safe). */
    static TMap<FString, int32> BoneMap(const USCRigComponent* Leader);
    bool Attach(ASCVehicle* Kit, TSharedPtr<const FSCPlayerKit> Data, TSharedPtr<const FSCPlayerClipMap> Clips, FString& Error);
    /** Detach and hide the kit (the subsystem keeps it for the next first-person session). */
    void Detach();
    bool HasBody() const { return Kit != nullptr; }
    ASCVehicle* GetKit() const { return Kit; }
    USCRigComponent* GetLeader() const { return Leader; }

    /** Advance animation, weapons and camera by DeltaSeconds (the component tick; tests call it directly). */
    void Step(float DeltaSeconds);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

    // Weapons: slots are the stowed weapons with playable select/deselect, in key order.
    int32 NumSlots() const { return Slots.Num(); }
    const FSCPlayerWeapon* SlotWeapon(int32 Slot) const;
    /** Select slot (0-based), or holster with INDEX_NONE; the active slot holsters. False when ignored. */
    bool RequestSlot(int32 Slot);
    ESCWeaponState GetWeaponState() const { return Weapons.GetState(); }
    int32 GetActiveSlot() const { return Weapons.GetActive(); }
    /** Joint the slot's weapon is attached to now (hand port joint while selecting/armed/deselecting). */
    FName WeaponParentBone(int32 Slot) const;

    // View
    void SetThirdPerson(bool bOn, float OrbitYawDegrees = 0, float OrbitDistance = -1);
    bool IsThirdPerson() const { return bThirdPerson; }
    bool IsHiddenInFirstPerson(const UPrimitiveComponent* Part) const { return HeadParts.Contains(Part); }
    int32 NumHeadParts() const { return HeadParts.Num(); }
    const FSCPlayerAnimator& GetAnimator() const { return Animator; }
    FString HUDLine() const;
    FString Describe() const;
    /** First-person camera near plane (cm): small enough for arms and weapon in front of the eye (viewer constant). */
    static constexpr float NearClip = 2.f;

private:
    ASCFirstPersonCharacter* Character() const;
    void PlaceWeapon(int32 WeaponIndex, bool bInHand);
    /** A bone port's frame: the same-named helper on the port's owner item when that item is placed (a backpack
     *  overrides the body's stow joints), else the body joint itself. */
    void ResolvePortFrame(const FSCItemPort& Port, USceneComponent*& Parent, FName& Socket, FTransform& Frame) const;
    void BeginSelect(int32 Slot);
    void BeginDeselect(int32 Then);
    void UpdateProfile();
    void ApplyView();
    FSCAnimatorInput MakeInput(float DeltaSeconds) const;

    UPROPERTY() TObjectPtr<ASCVehicle> Kit;
    UPROPERTY() TObjectPtr<USCRigComponent> Leader;
    UPROPERTY() TArray<TObjectPtr<USCRigComponent>> Followers;
    /** Followers with joints of their own: their joint -> leader joint (INDEX_NONE keeps the bind local). */
    TMap<TObjectPtr<USCRigComponent>, TArray<int32>> CopyMaps;
    UPROPERTY() TArray<TObjectPtr<UPrimitiveComponent>> HeadParts;
    TSharedPtr<const FSCPlayerKit> Data;
    TSharedPtr<const FSCPlayerClipMap> ClipStore;
    FSCPlayerAnimator Animator;
    FSCWeaponStateMachine Weapons;
    TArray<int32> Slots; // indices into Data->Weapons
    TMap<int32, FName> WeaponAttach; // weapon index -> port joint/helper it sits on now
    FString ProfileWeapon = TEXT("<none>");
    bool bThirdPerson = false;
    float OrbitYaw = 0, OrbitDistance = 250; // third-person presentation (cm), not game data
    FVector Eye = FVector::ZeroVector; // capsule space
    int32 CameraBone = INDEX_NONE;
    float LastDelta = 0;
};
