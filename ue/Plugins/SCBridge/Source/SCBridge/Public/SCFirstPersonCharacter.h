// Collision-aware first-person exploration, shared by Slate, native input and headless verification.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SCFirstPersonCharacter.generated.h"
class ASCVehicle;
class UCameraComponent;
class USpotLightComponent;
UCLASS()
class SCBRIDGE_API ASCFirstPersonCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    ASCFirstPersonCharacter();
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USpotLightComponent> Flashlight;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<ASCVehicle> Vehicle;
    UPROPERTY(BlueprintReadOnly) bool bNoclip = false;
    UPROPERTY(BlueprintReadOnly) bool bStuck = false;
    UPROPERTY(BlueprintReadOnly) bool bTeleportMenuOpen = false;
    UPROPERTY(BlueprintReadOnly) int32 RecoveryCount = 0;
    UPROPERTY(BlueprintReadOnly) int32 CapsuleHits = 0;
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool Explore(ASCVehicle* InVehicle);
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool TeleportToSpawn(int32 Index);
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool TeleportToFloor(FVector Floor);
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool Interact();
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") void ToggleNoclip();
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") void SetMoveInput(FVector Input);
    UFUNCTION(BlueprintPure, Category = "SC|Exploration") FString HUDText() const;
    void Look(FVector2D Pixels);
    bool HandleKey(FKey Key, bool bDown);
    void ClearInput();
    /** Find a walkable surface and unoccupied standing capsule around a semantic spawn hint. */
    bool ResolveSpawn(FVector Hint, FVector& CapsuleCentre) const;
private:
    FVector Move = FVector::ZeroVector, LastPosition = FVector::ZeroVector;
    FTransform LastSpawn;
    TSet<FKey> Held;
    float BlockedSeconds = 0;
    void SetRunning(bool bRun);
    UFUNCTION() void CapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                               FVector NormalImpulse, const FHitResult& Hit);
};
