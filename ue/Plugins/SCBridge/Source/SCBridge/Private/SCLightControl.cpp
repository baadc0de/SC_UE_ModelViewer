// C2 fixes: light groups and the light-control panels that switch them (ControlPanel_Screen_LightControl).
//
// Data chain (LIVE 4.10.2, cache format 39): a panel's row (`ui_bindings[].buttons[]`: the canvas node's onLeftClick names
// the panel interaction, LightsOn / LightsLow / LightsAlarm) -> the panel's Sendable interaction and its LinkingTag
// (`sendable_interactions[]`, /Interaction/LightGroup/SetDefault / SetAuxiliary / SetEmergency) -> the light groups the
// panel drives (its EntityLinks name the group, or an entity the group links too: the chipset both name) -> the group's
// interaction with that tag and the light state it ends in (`light_group.interactions[]`: Default / Auxiliary / Emergency)
// -> that state on every light of the group (`attachment.group_entity_id`; the light record's `<state>State` block).
// The panel's own state machine takes the interaction (`state_types[].transitions`: LightSwitchState) and follows the
// group (`state_types[].linked`: LightState from the group's state tag); its face shows the render of that state.
#include "SCVehicle.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "SCClipCatalog.h"
#include "Components/LightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLightControl, Log, All);

namespace
{
/** The light record's block of a light group state: EntityComponentLight's state fields are the state's name in lower
 *  camel case with "State" (Default -> defaultState, Emergency -> emergencyState): the game's own vocabulary on both sides. */
FName LightStateKey(const FString& LightState)
{
    if (LightState.IsEmpty()) return NAME_None;
    return FName(*(LightState.Left(1).ToLower() + LightState.Mid(1) + TEXT("State")));
}
/** "/Interaction/LightGroup/SetEmergency" -> "LightGroup": the tag family a linking tag belongs to. */
FString TagFamily(const FString& Tag)
{
    TArray<FString> Parts;
    Tag.ParseIntoArray(Parts, TEXT("/"));
    return Parts.Num() >= 2 ? Parts[1] : FString();
}
}

TArray<int32> ASCVehicle::LightGroupsLinkedBy(int32 Interior, const TArray<FString>& Links) const
{
    TArray<int32> Out;
    for (int32 G = 0; G < LightGroups.Num(); ++G)
    {
        const FSCLightGroup& Group = LightGroups[G];
        if (Group.Interior != Interior) continue; // entity ids are unique within one container
        if (Links.Contains(Group.EntityId) || Group.Links.ContainsByPredicate([&](const FString& L) { return Links.Contains(L); })) Out.Add(G);
    }
    return Out;
}

int32 ASCVehicle::DeliverLightGroupTag(int32 Group, const FString& LinkingTag, FString* OutState)
{
    if (OutState) OutState->Reset();
    if (!LightGroups.IsValidIndex(Group) || LinkingTag.IsEmpty()) return 0;
    FSCLightGroup& G = LightGroups[Group];
    const FSCLightGroupInteraction* I = G.Interactions.FindByPredicate([&](const FSCLightGroupInteraction& X) { return X.LinkingTag == LinkingTag; });
    if (!I)
    {
        UE_LOG(LogSCLightControl, Display, TEXT("LIGHT_GROUP_STATE group=%s tag=%s handled=0 (the group receives no such tag)"), *G.Name, *LinkingTag);
        return 0;
    }
    if (OutState) *OutState = I->LightState;
    const FName Key = LightStateKey(I->LightState);
    int32 Changed = 0, Unauthored = 0, Members = 0;
    double Candela = 0;
    for (int32 L = 0; L < LightStates.Num(); ++L)
    {
        if (LightStates[L].GroupEntityId != G.EntityId || LightStates[L].Interior != G.Interior) continue; // ids are per container
        ++Members;
        if (FSCLightImporter::ApplyState(this, L, Key)) ++Changed;
        else ++Unauthored; // a state the light does not author (DisableOverride: the group keeps what it shows)
        if (const ULightComponent* C = LightStates[L].Component) Candela += C->Intensity;
    }
    G.LightState = I->LightState;
    UE_LOG(LogSCLightControl, Display, TEXT("LIGHT_GROUP_STATE group=%s entity=%s tag=%s interaction=\"%s\" state=%s key=%s lights=%d changed=%d unauthored=%d candela_sum=%.1f"),
           *G.Name, *G.EntityId, *LinkingTag, *I->Name.ToString(), *I->LightState, *Key.ToString(), Members, Changed, Unauthored, Candela);
    // Every panel driving the group follows it: a linked state tag of the group's family naming its new state
    // (/State/LightGroup/Emergency -> LightState.LightsAlarm). A reading of the tag tree: the group's own state tags are not
    // exported, the panels' `linked` tags are.
    const FString Linked = FString::Printf(TEXT("/State/%s/%s"), *TagFamily(LinkingTag), *I->LightState);
    for (FSCFloorPanel& P : FloorPanels)
        if (P.Kind == ESCPanelKind::Buttons && LightGroupsLinkedBy(P.Interior, P.Links).Contains(Group))
            for (const FSCUiStateType& T : P.StateTypes)
                for (const TPair<FString, FString>& L : T.Linked)
                    if (L.Key == Linked) P.UiState.Add(T.Name, L.Value);
    RefreshFloorPanels();
    return Changed;
}

int32 ASCVehicle::PressButtonPanel(int32 Panel, int32 Option)
{
    if (!FloorPanels.IsValidIndex(Panel) || !FloorPanels[Panel].Layout.Options.IsValidIndex(Option)) return 0;
    FSCFloorPanel& P = FloorPanels[Panel];
    const FName Interaction = P.Layout.Options[Option].Interaction;
    // 1. The panel's own state machine takes its interaction (LightsAlarm -> LightSwitchState.LightSwitchAlarm).
    bool bOwnState = false;
    for (const FSCUiStateType& T : P.StateTypes)
        if (const FString* S = T.Next(Interaction, P.UiState.FindRef(T.Name))) { P.UiState.Add(T.Name, *S); bOwnState = true; }
    // 2. Its linking tag goes to the light groups it drives.
    const FSCSendableInteraction* Send = P.Sendables.FindByPredicate([&](const FSCSendableInteraction& S) { return S.Name == Interaction; });
    const FString Tag = Send ? Send->LinkingTag : FString();
    int32 Changed = 0;
    TArray<FString> Groups, States;
    for (const int32 G : LightGroupsLinkedBy(P.Interior, P.Links))
    {
        FString State;
        Changed += DeliverLightGroupTag(G, Tag, &State);
        Groups.Add(LightGroups[G].Name);
        if (!State.IsEmpty()) States.AddUnique(State);
    }
    // 3. The panel (and every other panel of the group) follows the group: DeliverLightGroupTag.
    TArray<FString> Shown;
    for (const auto& [Type, State] : P.UiState) Shown.Add(Type + TEXT(".") + State);
    Shown.Sort();
    RefreshFloorPanels();
    UE_LOG(LogSCLightControl, Display, TEXT("LIGHT_PANEL_PRESS panel=%d entity=%s option=%d interaction=\"%s\" tag=%s groups=%s group_state=%s lights_changed=%d panel_state=%s image=%s"),
           Panel, *P.EntityId, Option, *Interaction.ToString(), Tag.IsEmpty() ? TEXT("-") : *Tag, Groups.IsEmpty() ? TEXT("-") : *FString::Join(Groups, TEXT(",")),
           States.IsEmpty() ? TEXT("-") : *FString::Join(States, TEXT(",")), Changed, *FString::Join(Shown, TEXT(",")), *FPaths::GetBaseFilename(P.ImageForState()));
    return Changed > 0 ? Changed : int32(bOwnState);
}

// sc.Doors.Pose [door index]: each door's (or one door's) driven joint in the vehicle frame (cm), its recorded state and the
// clip playing on it with its frame (DOOR_POSE): polled through a toggle, a snap shows as a jump of the joint.
static FAutoConsoleCommandWithWorldAndArgs GSCDoorsPose(
    TEXT("sc.Doors.Pose"), TEXT("sc.Doors.Pose [door index]: log doors' joint positions, state and playing clip (DOOR_POSE)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            ASCVehicle* V = *It;
            for (int32 D = 0; D < V->Doors.Num(); ++D)
            {
                if (Args.Num() >= 1 && FCString::Atoi(*Args[0]) != D) continue;
                const FSCDoorInteraction& Door = V->Doors[D];
                if (!Door.Rig) continue;
                const FVector Joint = V->GetActorTransform().InverseTransformPosition(Door.Rig->GetSocketLocation(Door.Joint));
                // A point 3 m out along every axis of the joint's frame: it moves when the joint only turns (a hinge).
                const FVector Tip = V->GetActorTransform().InverseTransformPosition(Door.Rig->GetSocketTransform(Door.Joint).TransformPosition(FVector(300)));
                const FSCClipPlayback* P = V->Playback.FindByPredicate([&](const FSCClipPlayback& X) { return X.TargetRig.Get() == Door.Rig; });
                UE_LOG(LogSCLightControl, Display, TEXT("DOOR_POSE index=%d owner=%s rig=%s joint=%s pos=%.2f,%.2f,%.2f tip=%.2f,%.2f,%.2f open=%d starts_open=%d reverse_close=%d clip=%s frame=%.2f speed=%.2f t=%.3f"),
                       D, *Door.Rig->OwnerEntity, *Door.Rig->GetName(), *Door.Joint.ToString(), Joint.X, Joint.Y, Joint.Z, Tip.X, Tip.Y, Tip.Z, V->IsDoorOpen(D), Door.bStartsOpen, Door.bReverseClose,
                       P && V->ClipCatalog ? *V->ClipCatalog->Clips[P->Clip].Id.ToString() : TEXT("-"), P ? P->Frame : -1.0, P ? P->Speed : 0.f,
                       World->GetTimeSeconds());
            }
        }
    }));

// sc.Doors.Trace <door index|-1>: log that door every vehicle tick while a clip plays on its rig (DOOR_TICK: recorded state,
// clip, playhead before and after, joint tip) and each audio event its clips cross (DOOR_AUDIO_EVENT); -1 stops.
static FAutoConsoleCommandWithWorldAndArgs GSCDoorsTrace(
    TEXT("sc.Doors.Trace"), TEXT("sc.Doors.Trace <door index|-1>: log a door every tick while it moves (DOOR_TICK, DOOR_AUDIO_EVENT)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            It->TraceDoor = Args.Num() >= 1 && It->Doors.IsValidIndex(FCString::Atoi(*Args[0])) ? FCString::Atoi(*Args[0]) : INDEX_NONE;
            UE_LOG(LogSCLightControl, Display, TEXT("DOOR_TRACE vehicle=%s door=%d"), *It->GetName(), It->TraceDoor);
        }
    }));

// sc.Doors.Sensors: each proximity sensor (DOOR_SENSOR index centre half exit_delay doors), its box in the vehicle frame (cm).
static FAutoConsoleCommandWithWorldAndArgs GSCDoorsSensors(
    TEXT("sc.Doors.Sensors"), TEXT("sc.Doors.Sensors: log the door proximity sensors (DOOR_SENSOR: box in the vehicle frame, exit delay, doors)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            ASCVehicle* V = *It;
            for (int32 S = 0; S < V->DoorSensors.Num(); ++S)
            {
                const FSCDoorSensor& Sensor = V->DoorSensors[S];
                const USceneComponent* Anchor = Sensor.Anchor.Get();
                if (!Anchor) continue;
                const FTransform ToVehicle = Anchor->GetComponentTransform().GetRelativeTransform(V->GetActorTransform());
                const FBox Box = Sensor.Box.TransformBy(ToVehicle);
                TArray<FString> Doors;
                for (const auto& Rig : Sensor.Doors) Doors.Add(FString::FromInt(V->DoorForRig(Rig.Get())));
                UE_LOG(LogSCLightControl, Display, TEXT("DOOR_SENSOR index=%d centre=%.1f,%.1f,%.1f half=%.1f,%.1f,%.1f exit_delay=%.2f doors=%s"), S,
                       Box.GetCenter().X, Box.GetCenter().Y, Box.GetCenter().Z, Box.GetExtent().X, Box.GetExtent().Y, Box.GetExtent().Z, Sensor.ExitDelay,
                       *FString::Join(Doors, TEXT(",")));
            }
        }
    }));

// sc.Lights.Groups [group state]: list the light groups (LIGHT_GROUP index name entity interior state lights candela_sum) or
// deliver a light state's tag to one (as a panel row would; the group's own interactions decide what it ends in).
static FAutoConsoleCommandWithWorldAndArgs GSCLightsGroups(
    TEXT("sc.Lights.Groups"), TEXT("sc.Lights.Groups [<group index|name> <Default|Auxiliary|Emergency|...>]: list light groups or set one's state"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            ASCVehicle* V = *It;
            for (int32 G = 0; G < V->LightGroups.Num(); ++G)
            {
                FSCLightGroup& Group = V->LightGroups[G];
                if (Args.Num() >= 2 && (Args[0] == FString::FromInt(G) || Args[0] == Group.Name))
                    if (const FSCLightGroupInteraction* I = Group.Interactions.FindByPredicate([&](const FSCLightGroupInteraction& X) { return X.LightState == Args[1]; }))
                        V->DeliverLightGroupTag(G, I->LinkingTag);
                int32 Lights = 0;
                double Candela = 0;
                for (const FSCImportedLightState& L : V->LightStates)
                    if (L.GroupEntityId == Group.EntityId && L.Interior == Group.Interior)
                    {
                        ++Lights;
                        if (L.Component) Candela += L.Component->Intensity;
                    }
                UE_LOG(LogSCLightControl, Display, TEXT("LIGHT_GROUP index=%d name=%s entity=%s interior=%d state=%s lights=%d candela_sum=%.1f"), G, *Group.Name,
                       *Group.EntityId, Group.Interior, Group.LightState.IsEmpty() ? TEXT("imported") : *Group.LightState, Lights, Candela);
            }
        }
    }));

#if WITH_DEV_AUTOMATION_TESTS
// Inspect the active preset output over time. An optional state applies each
// light's authored record using the same entry point as a light group.
static FAutoConsoleCommandWithWorldAndArgs GSCLightStylesProbe(
    TEXT("sc.Lights.StyleProbe"),
    TEXT("sc.Lights.StyleProbe [Default|Emergency|Off] [index]: sample a ship light; optionally apply one state to all its lights"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        FName Key;
        int32 RequestedIndex = INDEX_NONE;
        for (const FString& Arg : Args)
        {
            if (Arg.Equals(TEXT("Default"), ESearchCase::IgnoreCase) ||
                Arg.Equals(TEXT("Emergency"), ESearchCase::IgnoreCase) ||
                Arg.Equals(TEXT("Off"), ESearchCase::IgnoreCase))
                Key = LightStateKey(Arg);
            else if (!LexTryParseString(RequestedIndex, *Arg))
                return;
        }
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
        {
            ASCVehicle* Vehicle = *It;
            if (Vehicle->Manifest.LightStyles.IsEmpty()) continue;
            int32 Changed = 0;
            if (!Key.IsNone())
                for (int32 I = 0; I < Vehicle->LightStates.Num(); ++I)
                    Changed += FSCLightImporter::ApplyState(Vehicle, I, Key);
            const int32 Index = RequestedIndex != INDEX_NONE ? RequestedIndex
                : (Vehicle->LoopingLights.IsEmpty() ? INDEX_NONE : *Vehicle->LoopingLights.begin());
            UE_LOG(LogSCLightControl, Display, TEXT("LIGHT_STYLES ship=%s definitions=%d lights=%d loops=%d changed=%d"),
                *Vehicle->Manifest.Root.EntityName, Vehicle->Manifest.LightStyles.Num(), Vehicle->LightStates.Num(),
                Vehicle->LoopingLights.Num(), Changed);
            if (!Vehicle->LightStates.IsValidIndex(Index)) continue;
            const FSCImportedLightState& State = Vehicle->LightStates[Index];
            if (!State.Component) continue;
            const FLinearColor Color = State.Component->GetLightColor();
            UE_LOG(LogSCLightControl, Display,
                TEXT("LIGHT_STYLE_SAMPLE ship=%s index=%d state=%s style=%d speed=%.3f phase=%.3f time=%.5f cd=%.6f rgb=(%.4f,%.4f,%.4f)"),
                *Vehicle->Manifest.Root.EntityName, Index, *State.ActiveState.ToString(), State.LightStyleId,
                State.AnimationSpeed, State.AnimationPhase, World->GetTimeSeconds(), State.Component->Intensity, Color.R, Color.G, Color.B);
        }
    }));
#endif
