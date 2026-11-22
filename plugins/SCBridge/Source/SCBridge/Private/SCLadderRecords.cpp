// Interior ladder package reader.
#include "SCLadderRecords.h"
#include "Dom/JsonObject.h"

namespace
{
const FJsonObject* Object(const FJsonObject& Parent, const TCHAR* Key)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    return Parent.TryGetObjectField(Key, Value) ? Value->Get() : nullptr;
}

bool Number(const FJsonObject& Parent, const TCHAR* Key, float& Out)
{
    double Value = 0;
    if (!Parent.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) ||
        Value < 0.0 || Value > TNumericLimits<float>::Max()) return false;
    Out = float(Value);
    return true;
}

bool ReadEnd(const FJsonObject& Definition, bool bTop, FSCLadderEnd& End)
{
    const FJsonObject* Radii = Object(Definition,
        bTop ? TEXT("autoMountableRadiusParametersAtTop") : TEXT("autoMountableRadiusParametersAtBottom"));
    float Walk = 0, Run = 0;
    if (!Radii || !Number(*Radii, TEXT("radiusWhenWalking"), Walk) ||
        !Number(*Radii, TEXT("radiusWhenRunning"), Run) || !Number(*Radii, TEXT("radiusOffset"), End.RadiusOffset) ||
        !Number(Definition, bTop ? TEXT("approachAngleAtTop") : TEXT("approachAngleAtBottom"), End.ApproachAngle) ||
        !Number(Definition, bTop ? TEXT("facingAngleAtTop") : TEXT("facingAngleAtBottom"), End.FacingAngle) ||
        !Number(Definition, bTop ? TEXT("remountDelayAtTop") : TEXT("remountDelayAtBottom"), End.RemountDelay)) return false;
    End.WalkRunRadius = FVector2D(Walk, Run);
    return true;
}

void ReadActions(const FJsonObject& Motion, TMap<FName, FSCLadderAction>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    if (!Motion.TryGetArrayField(TEXT("actions"), Records)) return;
    for (const auto& Value : *Records)
    {
        if (!Value || Value->Type != EJson::Object) continue;
        const auto Row = Value->AsObject();
        FString State, Name;
        const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
        if (!Row->TryGetStringField(TEXT("state"), State) || !Row->TryGetArrayField(TEXT("layers"), Layers) ||
            Layers->IsEmpty() || (*Layers)[0]->Type != EJson::Object) continue;
        const auto Layer = (*Layers)[0]->AsObject();
        FSCLadderAction Action;
        if (!Layer->TryGetStringField(TEXT("animation"), Name) || Name.IsEmpty() ||
            !Number(*Layer, TEXT("speed"), Action.Speed) || !Number(*Layer, TEXT("blend_in"), Action.Blend))
            continue;
        Action.Clip = FName(*Name);
        const TArray<TSharedPtr<FJsonValue>>* Procedures = nullptr;
        if (Row->TryGetArrayField(TEXT("procedurals"), Procedures))
            for (const auto& Procedure : *Procedures)
                if (Procedure && Procedure->Type == EJson::Object)
                {
                    const auto Fields = Procedure->AsObject();
                    FString Type;
                    if (Fields->TryGetStringField(TEXT("type"), Type) && Type == TEXT("PositionAdjust"))
                        Number(*Fields, TEXT("blend"), Action.PositionAdjust);
                }
        Out.Add(FName(*State), Action);
    }
}
}

TOptional<FSCLadderDefinition> FSCLadderDefinition::Read(const FJsonObject& Placement)
{
    const FJsonObject* Ladder = Object(Placement, TEXT("ladder"));
    if (!Ladder) return {};
    const FJsonObject* Definition = Object(*Ladder, TEXT("definition"));
    const FJsonObject* Motion = Object(*Ladder, TEXT("motion"));
    const FJsonObject* Config = Motion ? Object(*Motion, TEXT("config")) : nullptr;
    if (!Definition || !Config) return {};
    const FJsonObject* Animation = Object(*Config, TEXT("animationParams"));
    const FJsonObject* Movement = Object(*Config, TEXT("movementParams"));
    const TArray<TSharedPtr<FJsonValue>>* Offsets = nullptr;
    if (!Animation || !Movement || !Animation->TryGetArrayField(TEXT("animationOffsets"), Offsets) ||
        Offsets->IsEmpty() || (*Offsets)[0]->Type != EJson::Object) return {};
    const auto Walk = (*Offsets)[0]->AsObject();
    FSCLadderDefinition L;
    float Back = 0, Left = 0, Right = 0, Up = 0, Down = 0;
    if (!Number(*Definition, TEXT("height"), L.Height) || !ReadEnd(*Definition, false, L.Bottom) ||
        !ReadEnd(*Definition, true, L.Top) || !Number(*Animation, TEXT("playerHorizontalOffset"), L.HorizontalOffset) ||
        !Number(*Walk, TEXT("animationTravelDistance"), L.TravelInRungs) ||
        !Number(*Walk, TEXT("mountClimbDistanceFromBottom"), L.MountBottom) ||
        !Number(*Walk, TEXT("stopClimbDistanceFromBottom"), L.StopBottom) ||
        !Number(*Walk, TEXT("stopClimbDistanceFromTopBack"), Back) ||
        !Number(*Walk, TEXT("stopClimbDistanceFromTopLeft"), Left) ||
        !Number(*Walk, TEXT("stopClimbDistanceFromTopRight"), Right) ||
        !Number(*Movement, TEXT("movementSpeedUpwards"), Up) || !Number(*Movement, TEXT("movementSpeedDownwards"), Down) ||
        !Number(*Movement, TEXT("movementAcceleration"), L.Acceleration) ||
        !Number(*Movement, TEXT("movementInertiaDecayRate"), L.InertiaDecay) ||
        !Number(*Config, TEXT("horizontalViewLimit"), L.HorizontalView) ||
        !Number(*Config, TEXT("verticalUpViewLimit"), L.UpView) || L.Height <= 0.f || L.TravelInRungs <= 0.f ||
        Up <= 0.f || Down <= 0.f || L.Acceleration <= 0.f || L.InertiaDecay <= 0.f)
        return {};
    L.StopTop = FVector(Back, Left, Right);
    L.SpeedUpDown = FVector2D(Up, Down);
    bool bBack = false, bLeft = false, bRight = false;
    Definition->TryGetBoolField(TEXT("autoMountable"), L.bAutoMount);
    Definition->TryGetBoolField(TEXT("userIgnoreStaticCollisions"), L.bIgnoreStaticCollision);
    Definition->TryGetBoolField(TEXT("directionAtTopIncludesBack"), bBack);
    Definition->TryGetBoolField(TEXT("directionAtTopIncludesLeft"), bLeft);
    Definition->TryGetBoolField(TEXT("directionAtTopIncludesRight"), bRight);
    L.TopExits = FIntVector(bBack, bLeft, bRight);
    ReadActions(*Motion, L.Actions);
    for (const FName State : {FName(TEXT("mount_bottom")), FName(TEXT("mount_top")), FName(TEXT("climb")), FName(TEXT("dismount_bottom"))})
        if (!L.Actions.Contains(State)) return {};
    if (L.Actions.FindChecked(TEXT("climb")).Speed != 0.f) return {};
    return L;
}
