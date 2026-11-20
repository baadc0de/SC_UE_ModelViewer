#include "SCPackageReader.h"
#include "SCMeshImporter.h"
#include "Cargo/SCCargoRecords.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"

namespace
{
using FReaderObject = TSharedPtr<FJsonObject>;
FString String(const FReaderObject& O, const TCHAR* Key) { FString S; if (O) O->TryGetStringField(Key, S); return S; }
FString Json(const FReaderObject& O) { FString S; FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&S)); return S; }
const TArray<TSharedPtr<FJsonValue>>& ReaderArray(const FReaderObject& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
bool ReadJson(const FSCPackageSource& Source, const FString& Path, FReaderObject& Out, FString& Error, FString* Hash = nullptr)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Path, Bytes, Error)) return false;
    if (Hash) *Hash = FSCPackageReader::Hash(Bytes);
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out) return true;
    Error = TEXT("Invalid JSON: ") + Path; return false;
}
bool Matrix(const FReaderObject& O, const TCHAR* Key, FMatrix& Out, FString& Error)
{
    Out = FMatrix::Identity;
    if (!O->HasField(Key)) return true;
    const auto& Columns = ReaderArray(O, Key);
    if (Columns.Num() != 4) { Error = TEXT("Expected 4x4 matrix: ") + FString(Key); return false; }
    for (int32 C = 0; C < 4; ++C)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values;
        if (!Columns[C]->TryGetArray(Values) || Values->Num() != 4) { Error = TEXT("Invalid matrix column"); return false; }
        for (int32 R = 0; R < 4; ++R)
        {
            double V;
            if (!(*Values)[R]->TryGetNumber(V) || !FMath::IsFinite(V)) { Error = TEXT("Non-finite matrix"); return false; }
            Out.M[C][R] = V; // UE row vectors: transpose of glTF column-vector matrix.
        }
    }
    if (!FMath::IsNearlyZero(Out.M[0][3]) || !FMath::IsNearlyZero(Out.M[1][3]) || !FMath::IsNearlyZero(Out.M[2][3]) || !FMath::IsNearlyEqual(Out.M[3][3], 1.0))
    { Error = TEXT("Non-affine matrix"); return false; }
    return true;
}
bool Instance(const FReaderObject& O, FSCSceneInstance& Out, FString& Error, const TCHAR* MatrixKey = TEXT("local_transform_sc"))
{
    if (!O) { Error = TEXT("Expected scene instance object"); return false; }
    for (const auto& V : ReaderArray(O, TEXT("lights"))) if (V->Type == EJson::Object) Out.LightJson.Add(Json(V->AsObject()));
    Out.EntityName = String(O, TEXT("entity_name"));
    Out.ParentEntityName = String(O, TEXT("parent_entity_name"));
    Out.ParentNodeName = String(O, TEXT("parent_node_name"));
    Out.ItemType = String(O, TEXT("item_type"));
    Out.PortFlags = String(O, TEXT("port_flags"));
    Out.SourceTransformBasis = String(O, TEXT("source_transform_basis"));
    Out.PaletteId = String(O, TEXT("palette_id"));
    Out.bInvisible = String(O, TEXT("port_flags")).Contains(TEXT("invisible"), ESearchCase::IgnoreCase);
    for (auto Pair : {TPair<const TCHAR*, FString*>(TEXT("mesh_asset"), &Out.MeshAsset), {TEXT("material_sidecar"), &Out.MaterialSidecar}})
    {
        const FString Path = String(O, Pair.Key);
        if (!Path.IsEmpty() && !FSCPackageSource::Normalize(Path, *Pair.Value)) { Error = TEXT("Unsafe asset path: ") + Path; return false; }
    }
    if (!Out.SourceTransformBasis.IsEmpty() && Out.SourceTransformBasis != TEXT("gltf_y_up") && Out.SourceTransformBasis != TEXT("cryengine_z_up"))
    { Error = TEXT("Unsupported transform basis: ") + Out.SourceTransformBasis; return false; }
    Out.Id = String(O, TEXT("id"));
    Out.PortPath = SCCargoRecords::PortPath(O);
    // WP-I1: the UI screens the entity carries. An image path outside the package is dropped (no image), not trusted.
    for (const auto& V : ReaderArray(O, TEXT("ui_bindings")))
    {
        const FReaderObject B = V && V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!B) continue;
        FSCUiBinding& Binding = Out.UiBindings.AddDefaulted_GetRef();
        Binding.Kind = String(B, TEXT("binding_kind"));
        Binding.SourceEntityName = String(B, TEXT("source_entity_name"));
        Binding.HelperName = String(B, TEXT("helper_name"));
        Binding.CanvasRecordName = String(B, TEXT("canvas_record_name"));
        Binding.ContentCanvasRecordName = String(B, TEXT("content_canvas_record_name"));
        B->TryGetBoolField(TEXT("default_state_is_off"), Binding.bDefaultStateIsOff);
        if (!FSCPackageSource::Normalize(String(B, TEXT("generated_image_path")), Binding.ImagePath)) Binding.ImagePath.Reset();
        double Aspect = 0;
        if (B->TryGetNumberField(TEXT("ui_screen_aspect_w_over_h"), Aspect) && FMath::IsFinite(Aspect) && Aspect > 0) Binding.AspectWOverH = float(Aspect);
        // source_item_type / source_owner_resolved (scviewer WP-I1 review): always written by current exporters; the item
        // type is null for a resolved owner that is not an attachable item, and unknown when the owner is unresolved.
        Binding.bClassified = B->HasField(TEXT("source_item_type")) && B->HasField(TEXT("source_owner_resolved")) &&
                              B->HasField(TEXT("source_kiosk_interaction"));
        Binding.SourceItemType = String(B, TEXT("source_item_type"));
        B->TryGetBoolField(TEXT("source_owner_resolved"), Binding.bOwnerResolved);
        B->TryGetBoolField(TEXT("source_kiosk_interaction"), Binding.bOperated);
        // Cache format 40: how far from its screen the owner's UI is operated (metres; absent / null: not known).
        double Distance = 0;
        Binding.bInteractionDistanceExported = B->HasField(TEXT("interaction_distance"));
        if (B->TryGetNumberField(TEXT("interaction_distance"), Distance) && FMath::IsFinite(Distance) && Distance > 0)
            Binding.InteractionDistance = float(Distance);
        // WP-I1 I1-3: the owner's other states, each with its render; cache format 39 adds `states[]` (one pin for a
        // single-state render, two for a combined one: a combined render has no state_type / state of its own).
        for (const auto& S : ReaderArray(B, TEXT("state_images")))
        {
            const FReaderObject SI = S && S->Type == EJson::Object ? S->AsObject() : nullptr;
            if (!SI) continue;
            FSCUiStateImage Image;
            Image.StateType = String(SI, TEXT("state_type"));
            Image.State = String(SI, TEXT("state"));
            for (const auto& P : ReaderArray(SI, TEXT("states")))
                if (const FReaderObject Pin = P && P->Type == EJson::Object ? P->AsObject() : nullptr)
                    if (const FString T = String(Pin, TEXT("state_type")), St = String(Pin, TEXT("state")); !T.IsEmpty() && !St.IsEmpty())
                        Image.States.AddUnique(T + TEXT(".") + St);
            if (Image.States.IsEmpty() && !Image.StateType.IsEmpty() && !Image.State.IsEmpty()) Image.States.Add(Image.StateType + TEXT(".") + Image.State);
            Image.States.Sort();
            if (Image.States.IsEmpty() || (Image.States.Num() == 1 && (Image.StateType.IsEmpty() || Image.State.IsEmpty())) ||
                !FSCPackageSource::Normalize(String(SI, TEXT("generated_image_path")), Image.ImagePath) || Image.ImagePath.IsEmpty())
                continue;
            Binding.StateImages.Add(MoveTemp(Image));
        }
        // Cache format 39: the render's clickable nodes and the owner's state machine.
        const TArray<TSharedPtr<FJsonValue>>& Size = ReaderArray(B, TEXT("image_size_px"));
        if (Size.Num() == 2) Binding.ImageSizePx = FVector2D(Size[0]->AsNumber(), Size[1]->AsNumber());
        for (const auto& BV : ReaderArray(B, TEXT("buttons")))
        {
            const FReaderObject BO = BV && BV->Type == EJson::Object ? BV->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>& R = ReaderArray(BO, TEXT("rect_px"));
            if (!BO || R.Num() != 4) continue;
            FSCUiButton Button;
            Button.Node = String(BO, TEXT("node"));
            Button.Interaction = FName(*String(BO, TEXT("interaction")));
            const FVector2D Min(R[0]->AsNumber(), R[1]->AsNumber());
            Button.RectPx = FBox2D(Min, Min + FVector2D(R[2]->AsNumber(), R[3]->AsNumber()));
            if (!Button.Interaction.IsNone() && Button.RectPx.GetArea() > 0) Binding.Buttons.Add(MoveTemp(Button));
        }
        for (const auto& TV : ReaderArray(B, TEXT("state_types")))
        {
            const FReaderObject TO = TV && TV->Type == EJson::Object ? TV->AsObject() : nullptr;
            if (!TO) continue;
            FSCUiStateType& Type = Binding.StateTypes.AddDefaulted_GetRef();
            Type.Name = String(TO, TEXT("name"));
            Type.DefaultState = String(TO, TEXT("default_state"));
            for (const auto& SV : ReaderArray(TO, TEXT("states"))) if (SV->Type == EJson::String) Type.States.Add(SV->AsString());
            for (const auto& XV : ReaderArray(TO, TEXT("transitions")))
                if (const FReaderObject X = XV && XV->Type == EJson::Object ? XV->AsObject() : nullptr)
                    Type.Transitions.Add({String(X, TEXT("from")), FName(*String(X, TEXT("interaction"))), String(X, TEXT("state"))});
            for (const auto& LV : ReaderArray(TO, TEXT("linked")))
                if (const FReaderObject L = LV && LV->Type == EJson::Object ? LV->AsObject() : nullptr)
                    Type.Linked.Add({String(L, TEXT("tag")), String(L, TEXT("state"))});
        }
    }
    // Door state (placements, and loadout children from cache format 39) and the links an item forwards to vehicle ports.
    const FReaderObject* Door;
    if (O->TryGetObjectField(TEXT("door"), Door))
    {
        Out.Door.bValid = true;
        double Speed = 0;
        Out.Door.bSpeedKnown = (*Door)->TryGetNumberField(TEXT("animation_speed"), Speed) && FMath::IsFinite(Speed);
        Out.Door.AnimationSpeed = Out.Door.bSpeedKnown ? float(Speed) : 1.f;
        (*Door)->TryGetBoolField(TEXT("use_auto_close_delay"), Out.Door.bUseAutoCloseDelay);
        double Delay = 0;
        Out.Door.AutoCloseDelay = (*Door)->TryGetNumberField(TEXT("auto_close_delay"), Delay) && FMath::IsFinite(Delay) ? float(Delay) : 0.f;
        bool bOpened = false;
        if ((*Door)->TryGetBoolField(TEXT("is_opened"), bOpened)) Out.Door.bIsOpened = bOpened;
        Out.Door.AnimationParams = String(*Door, TEXT("animation_params"));
        auto Tag = [&](const TCHAR* Key) { const FString S = String(*Door, Key); return S.IsEmpty() ? FName() : FName(*S); };
        Out.Door.TransitionTag = Tag(TEXT("transition_tag"));
        Out.Door.OpenTag = Tag(TEXT("open_tag"));
        Out.Door.CloseTag = Tag(TEXT("close_tag"));
    }
    for (const auto& V : ReaderArray(O, TEXT("item_port_links")))
        if (const FReaderObject L = V && V->Type == EJson::Object ? V->AsObject() : nullptr)
            Out.ItemPortLinks.Add({FName(*String(L, TEXT("source_slot"))), String(L, TEXT("target_port")), FName(*String(L, TEXT("target_slot")))});
    for (const auto& V : ReaderArray(O, TEXT("sendable_interactions")))
        if (const FReaderObject S = V && V->Type == EJson::Object ? V->AsObject() : nullptr)
            Out.SendableInteractions.Add({FName(*String(S, TEXT("name"))), String(S, TEXT("linking_tag"))});
    const FReaderObject* Port;
    if (O->TryGetObjectField(TEXT("port"), Port))
    {
        Out.PortJson = Json(*Port);
        // A bone port without a resolved transform is placed by its port record (player kits): the item's
        // offset helper meets the attach joint plus the port offset. Identity local transform, joint frame.
        if (String(*Port, TEXT("attachment")) == TEXT("bone") && !O->HasField(MatrixKey) && Out.SourceTransformBasis.IsEmpty())
        {
            Out.bBonePort = true;
            Out.SourceTransformBasis = TEXT("gltf_y_up");
            Out.PortHelper = String(*Port, TEXT("item_offset_helper"));
            if (Out.ParentNodeName.IsEmpty()) Out.ParentNodeName = String(*Port, TEXT("attach_bone"));
            Out.PortOffset = FSCPackageReader::PortOffset(*Port);
        }
    }
    return Matrix(O, MatrixKey, Out.LocalMatrix, Error);
}
// WP-P5: interaction wiring of a placement or entity (decomposed-export-contract "Interaction wiring, entities and lifts").
double Number(const FReaderObject& O, const TCHAR* Key, double Default)
{
    double V = Default;
    return O && O->TryGetNumberField(Key, V) && FMath::IsFinite(V) ? V : Default;
}
TArray<FSCInteractionLink> InteractionLinks(const FReaderObject& O)
{
    TArray<FSCInteractionLink> Out;
    for (const auto& V : ReaderArray(O, TEXT("interaction_links")))
        if (const FReaderObject L = V && V->Type == EJson::Object ? V->AsObject() : nullptr)
            Out.Add({FName(*String(L, TEXT("source_slot"))), String(L, TEXT("target_entity_id")), FName(*String(L, TEXT("target_slot")))});
    return Out;
}
FSCParentAttachment ParentAttachment(const FReaderObject& O)
{
    FSCParentAttachment Out;
    const FReaderObject* P;
    if (O && O->TryGetObjectField(TEXT("parent_attachment"), P))
    {
        Out.ParentEntityId = String(*P, TEXT("parent_entity_id"));
        Out.AttachmentType = String(*P, TEXT("attachment_type"));
        Out.AttachmentTarget = String(*P, TEXT("attachment_target"));
    }
    return Out;
}
TArray<FSCInteractionPoint> InteractionPoints(const FReaderObject& O)
{
    TArray<FSCInteractionPoint> Out;
    for (const auto& V : ReaderArray(O, TEXT("interaction_points")))
    {
        const FReaderObject P = V && V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!P) continue;
        FSCInteractionPoint& Point = Out.AddDefaulted_GetRef();
        Point.HelperName = String(P, TEXT("helper_name"));
        Point.Radius = float(Number(P, TEXT("radius"), 0));
        double Angle = 0;
        if (P->TryGetNumberField(TEXT("angle_range"), Angle) && FMath::IsFinite(Angle)) Point.AngleRange = float(Angle);
        if (P->TryGetNumberField(TEXT("view_angle_limit"), Angle) && FMath::IsFinite(Angle)) Point.ViewAngleLimit = float(Angle);
        FString Ignored;
        Point.bHasHelperTransform = P->HasTypedField<EJson::Array>(TEXT("helper_transform")) && Matrix(P, TEXT("helper_transform"), Point.HelperTransform, Ignored);
        for (const auto& IV : ReaderArray(P, TEXT("interactions")))
            if (const FReaderObject I = IV && IV->Type == EJson::Object ? IV->AsObject() : nullptr)
            {
                FSCPointInteraction& Interaction = Point.Interactions.AddDefaulted_GetRef();
                Interaction.Name = FName(*String(I, TEXT("name")));
                Interaction.DisplayName = String(I, TEXT("display_name"));
                I->TryGetBoolField(TEXT("primary"), Interaction.bPrimary);
            }
    }
    return Out;
}
void ReadSeatRecords(const FReaderObject& Object, FSCSceneInstance& Instance)
{
    const FReaderObject* Data = nullptr;
    if (Object && Object->TryGetObjectField(TEXT("seat"), Data))
    {
        FSCSeatDefinition Seat;
        auto Name = [](const FReaderObject& Obj, const TCHAR* Field) { return FName(*String(Obj, Field)); };
        auto Limit = [Data](const TCHAR* Field) -> TOptional<double> {
            double Value = 0;
            return (*Data)->TryGetNumberField(Field, Value) && FMath::IsFinite(Value) ? TOptional<double>(Value) : TOptional<double>();
        };
        bool bDriver = false;
        if ((*Data)->TryGetBoolField(TEXT("can_drive"), bDriver)) Seat.Role = bDriver ? ESCSeatRole::Driver : ESCSeatRole::Crew;
        Seat.ClassTag = String(*Data, TEXT("class_tag"));
        Seat.TransitionTemplate = String(*Data, TEXT("transition_template"));
        Seat.ControlTemplate = String(*Data, TEXT("control_template"));
        Seat.MinYaw = Limit(TEXT("min_yaw")); Seat.MaxYaw = Limit(TEXT("max_yaw"));
        Seat.MinPitch = Limit(TEXT("min_pitch")); Seat.MaxPitch = Limit(TEXT("max_pitch"));
        if (const FReaderObject* Sub; (*Data)->TryGetObjectField(TEXT("attachment"), Sub)) Seat.Attachment = Name(*Sub, TEXT("bone"));
        if (const FReaderObject* Sub; (*Data)->TryGetObjectField(TEXT("dashboard"), Sub)) Seat.DashboardPort = String(*Sub, TEXT("port"));
        if (const FReaderObject* Sub; (*Data)->TryGetObjectField(TEXT("usable"), Sub))
        {
            bool bValidate = false;
            if ((*Sub)->TryGetBoolField(TEXT("validate_exit"), bValidate)) Seat.ValidateExit = bValidate;
        }
        for (const auto& Value : ReaderArray(*Data, TEXT("user_fragments")))
        {
            const FReaderObject Fragment = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
            if (!Fragment) continue;
            bool bLook = false;
            Fragment->TryGetBoolField(TEXT("look_layer"), bLook);
            if (bLook) continue;
            FSCSeatAction Action;
            Action.State = Name(Fragment, TEXT("state"));
            for (const auto& Tag : ReaderArray(Fragment, TEXT("query_frag_tags")))
                if (Tag && Tag->Type == EJson::String) Action.QueryTags.Add(FName(*Tag->AsString()));
            for (const auto& LayerValue : ReaderArray(Fragment, TEXT("layers")))
                if (const FReaderObject Layer = LayerValue && LayerValue->Type == EJson::Object ? LayerValue->AsObject() : nullptr;
                    Layer && Number(Layer, TEXT("layer"), -1) == 0)
                {
                    Action.Animation = Name(Layer, TEXT("animation"));
                    Action.Speed = float(Number(Layer, TEXT("speed"), 1.0));
                    Action.BlendIn = float(Number(Layer, TEXT("blend_in"), 0.0));
                    Layer->TryGetBoolField(TEXT("loop"), Action.bLoop);
                    break;
                }
            for (const auto& ProcedureValue : ReaderArray(Fragment, TEXT("procedurals")))
                if (const FReaderObject Procedure = ProcedureValue && ProcedureValue->Type == EJson::Object ? ProcedureValue->AsObject() : nullptr;
                    Procedure && String(Procedure, TEXT("type")).Equals(TEXT("PositionAdjust"), ESearchCase::IgnoreCase))
                    Action.PositionAdjust = float(Number(Procedure, TEXT("blend"), 0.0));
            if (!Action.State.IsNone() && !Action.Animation.IsNone()) Seat.Actions.Add(MoveTemp(Action));
        }
        Instance.Seat = MoveTemp(Seat);
    }
    if (!Object || !Object->TryGetObjectField(TEXT("seat_access"), Data)) return;
    auto Transition = [](const FReaderObject& Entrance, const TCHAR* Field, FSCSeatTransition& Result)
    {
        const FReaderObject* Step;
        if (!Entrance->TryGetObjectField(Field, Step)) return;
        Result.Interaction = FName(*String(*Step, TEXT("interaction")));
        Result.FragmentTags = String(*Step, TEXT("fragment_tags"));
        Result.SkipState = FName(*String(*Step, TEXT("skip_state")));
        if (const FReaderObject* Attachment; (*Step)->TryGetObjectField(TEXT("attachment"), Attachment))
            Result.Attachment = FName(*String(*Attachment, TEXT("bone")));
    };
    for (const auto& Value : ReaderArray(*Data, TEXT("entrances")))
    {
        const FReaderObject O = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        if (!O) continue;
        FSCSeatEntrance Entrance;
        Entrance.Name = String(O, TEXT("name"));
        for (const auto& Segment : ReaderArray(O, TEXT("seat_port_path")))
            if (Segment && Segment->Type == EJson::String) Entrance.SeatPortPath.Add(Segment->AsString());
        Entrance.DashboardPort = String(O, TEXT("dashboard_port"));
        Transition(O, TEXT("enter"), Entrance.Enter);
        Transition(O, TEXT("exit"), Entrance.Exit);
        Entrance.bHasGateways = !ReaderArray(O, TEXT("linked_gateways")).IsEmpty();
        if (const FReaderObject* Conditions; O->TryGetObjectField(TEXT("conditions"), Conditions))
            for (const auto& Field : (*Conditions)->Values)
                Entrance.bRequiresWorldState |= Field.Value && Field.Value->Type == EJson::String &&
                    !Field.Value->AsString().Equals(TEXT("Either"), ESearchCase::IgnoreCase);
        Instance.SeatEntrances.Add(MoveTemp(Entrance));
    }
}
void ReadInteractions(const FReaderObject& O, FSCSceneInstance& Out)
{
    Out.SocEntityName = String(O, TEXT("entity_name"));
    Out.InteractionLinks = InteractionLinks(O);
    Out.Parent = ParentAttachment(O);
    Out.InteractionPoints = InteractionPoints(O);
    // `door`, `item_port_links` and `sendable_interactions` are read by Instance (every instance carries them).
}
template <typename FWarn>
void ReadLiftRecords(const FReaderObject& O, FSCInterior& I, FWarn&& Warn)
{
    FString Detail;
    for (const auto& V : ReaderArray(O, TEXT("entities")))
    {
        const FReaderObject E = V && V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!E) continue;
        FSCInteriorEntity Entity;
        if (!Matrix(E, TEXT("transform"), Entity.LocalMatrix, Detail)) { Warn(TEXT("interior entity"), Detail); Detail.Reset(); continue; }
        Entity.EntityId = String(E, TEXT("entity_id"));
        Entity.EntityName = String(E, TEXT("entity_name"));
        Entity.EntityClass = String(E, TEXT("entity_class"));
        Entity.EntityClassGuid = String(E, TEXT("entity_class_guid")).ToLower();
        Entity.Kind = String(E, TEXT("kind"));
        for (const auto& L : ReaderArray(E, TEXT("links"))) if (L->Type == EJson::String) Entity.Links.Add(L->AsString());
        Entity.InteractionLinks = InteractionLinks(E);
        Entity.Parent = ParentAttachment(E);
        Entity.InteractionPoints = InteractionPoints(E);
        for (const auto& TV : ReaderArray(E, TEXT("traversal_links")))
            if (const FReaderObject T = TV && TV->Type == EJson::Object ? TV->AsObject() : nullptr)
                Entity.TraversalLinks.Add({FName(*String(T, TEXT("local_interaction"))), String(T, TEXT("item_port_name")),
                                           FName(*String(T, TEXT("linked_interaction_name")))});
        // Cache format 39: what a light group receives (and the light state each ends in), what a logic entity sends.
        const FReaderObject* Group;
        if (E->TryGetObjectField(TEXT("light_group"), Group))
            for (const auto& GV : ReaderArray(*Group, TEXT("interactions")))
                if (const FReaderObject G = GV && GV->Type == EJson::Object ? GV->AsObject() : nullptr)
                    Entity.LightGroupInteractions.Add({FName(*String(G, TEXT("name"))), String(G, TEXT("linking_tag")), String(G, TEXT("light_state"))});
        for (const auto& SV : ReaderArray(E, TEXT("sendable_interactions")))
            if (const FReaderObject S = SV && SV->Type == EJson::Object ? SV->AsObject() : nullptr)
                Entity.SendableInteractions.Add({FName(*String(S, TEXT("name"))), String(S, TEXT("linking_tag"))});
        I.Entities.Add(MoveTemp(Entity));
    }
    for (const auto& V : ReaderArray(O, TEXT("lifts")))
    {
        const FReaderObject L = V && V->Type == EJson::Object ? V->AsObject() : nullptr;
        if (!L) continue;
        FSCLiftRecord Lift;
        Lift.EntityId = String(L, TEXT("entity_id"));
        Lift.EntityName = String(L, TEXT("entity_name"));
        Lift.EntityClass = String(L, TEXT("entity_class"));
        Lift.GridGeometry = String(L, TEXT("grid_geometry"));
        Lift.RestNodeId = String(L, TEXT("rest_node_id"));
        const FReaderObject* P;
        if (L->TryGetObjectField(TEXT("params"), P))
        {
            Lift.MaxSpeed = float(Number(*P, TEXT("max_speed"), -1));
            Lift.EasingDistance = float(Number(*P, TEXT("easing_distance"), -1));
            Lift.OpenWaitTime = float(Number(*P, TEXT("open_wait_time"), -1));
            Lift.QueueWaitTime = float(Number(*P, TEXT("queue_wait_time"), -1));
            Lift.IdlingPeriod = float(Number(*P, TEXT("idling_period"), -1));
            (*P)->TryGetBoolField(TEXT("slow_down_at_bends"), Lift.bSlowDownAtBends);
            Lift.OpenInnerDoorInteraction = FName(*String(*P, TEXT("open_inner_door_interaction")));
            Lift.OpenInnerDoorFinishedInteraction = FName(*String(*P, TEXT("open_inner_door_finished_interaction")));
            Lift.CloseAllDoorsInteraction = FName(*String(*P, TEXT("close_all_doors_interaction")));
            Lift.CloseInnerDoorFinishedInteraction = FName(*String(*P, TEXT("close_inner_door_finished_interaction")));
        }
        for (const auto& SV : ReaderArray(L, TEXT("stops")))
        {
            const FReaderObject S = SV && SV->Type == EJson::Object ? SV->AsObject() : nullptr;
            if (!S) continue;
            FSCLiftStopRecord Stop;
            Stop.Index = int32(Number(S, TEXT("index"), Lift.Stops.Num()));
            Stop.NodeEntityId = String(S, TEXT("node_entity_id"));
            Stop.NodeEntityName = String(S, TEXT("node_entity_name"));
            Stop.Kind = String(S, TEXT("kind"));
            const auto& Pos = ReaderArray(S, TEXT("position"));
            if (Pos.Num() == 3) Stop.Position = FVector(Pos[0]->AsNumber(), Pos[1]->AsNumber(), Pos[2]->AsNumber());
            for (const auto& D : ReaderArray(S, TEXT("landing_door_ids"))) if (D->Type == EJson::String) Stop.LandingDoorIds.Add(D->AsString());
            Stop.TeleportTimeScale = float(Number(S, TEXT("teleport_time_scale"), 1));
            Stop.GatewayTimingInTransit = float(Number(S, TEXT("gateway_timing_in_transit"), -1));
            Stop.GatewayInteraction = String(S, TEXT("gateway_interaction"));
            const FReaderObject* Access;
            if (S->TryGetObjectField(TEXT("accessibility"), Access))
            {
                Stop.AccessibilityKind = String(*Access, TEXT("kind"));
                Stop.AccessibleTimingInTransit = float(Number(*Access, TEXT("accessible_timing_in_transit"), -1));
            }
            Lift.Stops.Add(MoveTemp(Stop));
        }
        for (const auto& R : ReaderArray(L, TEXT("riders"))) if (R->Type == EJson::String) Lift.Riders.Add(R->AsString());
        const FReaderObject* Style;
        if (L->TryGetObjectField(TEXT("panel_style"), Style))
        {
            FSCLiftPanelStyle& PS = Lift.PanelStyle;
            const FReaderObject* Source;
            if ((*Style)->TryGetObjectField(TEXT("source"), Source))
            {
                PS.SourceEntityId = String(*Source, TEXT("entity_id"));
                PS.SourceCanvas = String(*Source, TEXT("canvas"));
                PS.StyleRecord = String(*Source, TEXT("style_record"));
            }
            for (const auto& CV : ReaderArray(*Style, TEXT("palette")))
                if (const FReaderObject C = CV && CV->Type == EJson::Object ? CV->AsObject() : nullptr)
                    if (const auto& Rgba = ReaderArray(C, TEXT("srgba")); Rgba.Num() == 4)
                        PS.Palette.Add(String(C, TEXT("role")), FColor(uint8(Rgba[0]->AsNumber()), uint8(Rgba[1]->AsNumber()), uint8(Rgba[2]->AsNumber()),
                                                                       uint8(Rgba[3]->AsNumber())));
            const FReaderObject* Label;
            if ((*Style)->TryGetObjectField(TEXT("label"), Label))
            {
                PS.FontStyle = String(*Label, TEXT("font_style"));
                PS.FontSymbol = String(*Label, TEXT("font_symbol"));
                PS.LabelColourRole = String(*Label, TEXT("colour_token"));
                PS.FontSize = float(Number(*Label, TEXT("font_size"), 0));
                PS.LetterSpacing = float(Number(*Label, TEXT("letter_spacing"), 0));
                (*Label)->TryGetBoolField(TEXT("upper_case"), PS.bUpperCase);
            }
            for (const auto& LV : ReaderArray(*Style, TEXT("labels")))
                if (const FReaderObject E = LV && LV->Type == EJson::Object ? LV->AsObject() : nullptr)
                {
                    FSCLiftStopLabel& Out = PS.Labels.AddDefaulted_GetRef();
                    Out.Stop = int32(Number(E, TEXT("stop"), INDEX_NONE));
                    Out.Text = String(E, TEXT("text"));
                    Out.Image = String(E, TEXT("image"));
                    Out.Width = int32(Number(E, TEXT("width"), 0));
                    Out.Height = int32(Number(E, TEXT("height"), 0));
                    Out.EmPx = float(Number(E, TEXT("em_px"), 0));
                }
        }
        // The motion profile needs both speeds (a lift without them cannot move: reported, not invented).
        if (Lift.Stops.Num() < 2 || Lift.MaxSpeed <= 0 || Lift.EasingDistance < 0)
        {
            Warn(TEXT("lift"), FString::Printf(TEXT("%s: fewer than two stops or no MaxSpeed/EasingDistance; not operable"), *Lift.EntityId));
            continue;
        }
        I.Lifts.Add(MoveTemp(Lift));
    }
}
}
FMatrix FSCPackageReader::PortOffset(const TSharedPtr<FJsonObject>& Port)
{
    // Native SC axes: position in metres, Ang3 Euler degrees (x pitch, y roll, z yaw), Matrix33::CreateRotationXYZ
    // = Rz * Ry * Rx for column vectors, i.e. Rx * Ry * Rz in this row-vector convention.
    FVector P = FVector::ZeroVector, R = FVector::ZeroVector;
    const FReaderObject* Offset;
    if (Port && Port->TryGetObjectField(TEXT("offset"), Offset))
    {
        const auto& Pos = ReaderArray(*Offset, TEXT("position"));
        const auto& Rot = ReaderArray(*Offset, TEXT("rotation_degrees"));
        if (Pos.Num() == 3) P = FVector(Pos[0]->AsNumber(), Pos[1]->AsNumber(), Pos[2]->AsNumber());
        if (Rot.Num() == 3) R = FVector(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
    }
    auto Axis = [](int32 A, double Degrees) {
        const double S = FMath::Sin(FMath::DegreesToRadians(Degrees)), C = FMath::Cos(FMath::DegreesToRadians(Degrees));
        FMatrix M = FMatrix::Identity;
        const int32 U = (A + 1) % 3, V = (A + 2) % 3;
        M.M[U][U] = C; M.M[U][V] = S; M.M[V][U] = -S; M.M[V][V] = C; // row-vector rotation about axis A
        return M;
    };
    FMatrix M = Axis(0, R.X) * Axis(1, R.Y) * Axis(2, R.Z);
    M.SetOrigin(P);
    return M;
}
FString FSCPackageReader::Hash(const TArray<uint8>& Bytes)
{
    FSHA1 H; H.Update(Bytes.GetData(), Bytes.Num()); H.Final(); uint8 Digest[20]; H.GetHash(Digest);
    return BytesToHex(Digest, 20);
}
bool FSCSceneManifest::PaintTarget(const FSCSceneInstance& I, int32 Variant, FString& OutSidecar, FString& OutPalette) const
{
    if (!Paints.IsValidIndex(Variant)) return false;
    const FSCPaintVariant& Paint = Paints[Variant];
    const bool bExterior = !Root.MaterialSidecar.IsEmpty() && I.MaterialSidecar == Root.MaterialSidecar && !Paint.ExteriorSidecar.IsEmpty();
    const bool bPalette = !Root.PaletteId.IsEmpty() && I.PaletteId == Root.PaletteId && !Paint.PaletteId.IsEmpty();
    if (!bExterior && !bPalette) return false;
    OutSidecar = bExterior ? Paint.ExteriorSidecar : I.MaterialSidecar;
    OutPalette = bPalette ? Paint.PaletteId : I.PaletteId;
    return true;
}
void FSCSceneInstance::PanelImages(TSet<FString>& Out, int32* Excluded, int32* Unclassified) const
{
    for (const FSCUiBinding& B : UiBindings)
    {
        if (B.ImagePath.IsEmpty()) continue;
        if (!B.bClassified || !B.bOwnerResolved) { if (Unclassified) ++*Unclassified; continue; } // unknown owner: dark
        if (!B.IsOperatingPanel()) { if (Excluded) ++*Excluded; continue; }
        if (!B.PanelImage().IsEmpty()) Out.Add(B.PanelImage());
        for (const FSCUiStateImage& S : B.PanelStateImages()) Out.Add(S.ImagePath);
    }
}
FString FSCSceneInstance::ScreenSlotImage(const TSet<FString>& MeshNodes, const TArray<TArray<FString>>& Owners, bool* bAmbiguous) const
{
    if (bAmbiguous) *bAmbiguous = false;
    // Bindings placed on a node of this mesh (helper_name), and the entity's own (no helper, or a helper of another
    // asset, such as a loadout item's attachment helper on its parent).
    // A binding propagated from another entity whose helper is not a node here has no face on this mesh: it claims
    // nothing (it is never spread over the entity's other faces).
    TMultiMap<FString, const FSCUiBinding*> AtHelper;
    TArray<const FSCUiBinding*> Own;
    for (const FSCUiBinding& B : UiBindings)
    {
        const FString Helper = B.HelperName.ToLower();
        if (!Helper.IsEmpty() && MeshNodes.Contains(Helper)) AtHelper.Add(Helper, &B);
        else if (!B.bPropagated) Own.Add(&B);
    }
    TSet<FString> Outcomes; // "" = a dark face
    for (const TArray<FString>& Chain : Owners)
    {
        TArray<const FSCUiBinding*> Claim;
        for (const FString& Node : Chain)
        {
            AtHelper.MultiFind(Node, Claim);
            if (!Claim.IsEmpty()) break; // the nearest helper owns the face
        }
        if (Claim.IsEmpty()) Claim = Own;
        if (Claim.IsEmpty()) Outcomes.Add(FString()); // a screen face no binding names
        for (const FSCUiBinding* B : Claim) Outcomes.Add(B->PanelImage());
    }
    if (Outcomes.Num() != 1)
    {
        if (bAmbiguous) *bAmbiguous = Outcomes.Num() > 1;
        return FString();
    }
    return *Outcomes.CreateConstIterator();
}
TArray<FString> FSCSceneManifest::MeshAssets() const
{
    TSet<FString> Paths;
    auto Add = [&](const FSCSceneInstance& I) { if (!I.MeshAsset.IsEmpty()) Paths.Add(I.MeshAsset); };
    Add(Root); for (const auto& I : Children) Add(I);
    for (const auto& I : Interiors) for (const auto& P : I.Placements) Add(P);
    TArray<FString> Result = Paths.Array(); Result.Sort(); return Result;
}
bool FSCPackageReader::Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath, TArray<FString>* Warnings)
{
    Out = FSCSceneManifest(); Error.Reset();
    auto Warn = [&](const FString& Path, const FString& Message) { if (Warnings) Warnings->Add(Path + TEXT(": ") + Message); };
    auto Object = [](const TSharedPtr<FJsonValue>& V) -> FReaderObject { return V && V->Type == EJson::Object ? V->AsObject() : nullptr; };
    Out.ScenePath = ScenePath;
    if (Out.ScenePath.IsEmpty())
    {
        TArray<FString> Scenes;
        for (const FString& File : Source.ListFiles()) if (FPaths::GetCleanFilename(File) == TEXT("scene.json")) Scenes.Add(File);
        if (Scenes.Num() != 1) { Error = FString::Printf(TEXT("Expected one scene.json, found %d; select ScenePath"), Scenes.Num()); return false; }
        Out.ScenePath = Scenes[0];
    }
    FReaderObject Scene;
    if (!ReadJson(Source, Out.ScenePath, Scene, Error, &Out.ContentHash)) return false;
    double Version = 0;
    // Schema v2 (WP-R5) and v3 (WP-K1: port paths, inventory containers, item occupancy) only add fields.
    if (!Scene->TryGetNumberField(TEXT("version"), Version) || (Version < 1 || Version > 3 || Version != FMath::RoundToDouble(Version)) ||
        String(Scene, TEXT("export_kind")) != TEXT("Decomposed"))
    { Error = TEXT("Expected decomposed scene version 1 to 3"); return false; }
    const FReaderObject* Root;
    if (!Scene->TryGetObjectField(TEXT("root_entity"), Root)) { Error = TEXT("Scene has no root_entity object"); return false; }
    FString Detail;
    if (!Instance(*Root, Out.Root, Detail)) { Out.Root.ImportWarning = Detail; Warn(TEXT("root_entity"), Detail); }
    // WP-P3: the vehicle's physics-grid volume, in the native SC frame (metres) of the root entity.
    if (const FReaderObject* Grid; (*Root)->TryGetObjectField(TEXT("physics_grid"), Grid))
    {
        const TArray<TSharedPtr<FJsonValue>>* Shapes;
        if (!(*Grid)->TryGetArrayField(TEXT("shapes"), Shapes)) Warn(TEXT("physics_grid"), TEXT("no shapes"));
        else
            for (const auto& Value : *Shapes)
            {
                TArray<FVector> Tris;
                FString ShapeError;
                const auto Shape = Object(Value);
                if (!Shape || !FSCMeshImporter::PhysicsShapeTriangles(Shape, Tris, [](int32, TArray<FVector>&) { return false; }, ShapeError))
                {
                    Warn(TEXT("physics_grid"), ShapeError.IsEmpty() ? TEXT("malformed shape") : ShapeError);
                    Out.PhysicsGrid.Reset();
                    Out.PhysicsGridShapeEnds.Reset();
                    break;
                }
                for (const FVector& P : Tris) Out.PhysicsGrid.Add(FSCMeshImporter::Position(FSCMeshImporter::CryToGltf().TransformPosition(P)));
                Out.PhysicsGridShapeEnds.Add(Out.PhysicsGrid.Num());
            }
    }
    // Nested assemblies (a personal weapon's own children) follow their parent depth-first; ParentId binds
    // them to that instance even when the authored parent name differs from the instance's entity name.
    TArray<FString> NestedPackages; // nested assemblies keep their own palettes next to their manifest
    TFunction<void(const TArray<TSharedPtr<FJsonValue>>&, const FString&, const FString&)> ReadChildren =
        [&](const TArray<TSharedPtr<FJsonValue>>& List, const FString& ParentKey, const FString& ParentEntity) {
            // Inside a nested assembly every entry binds to the containing instance (its authored parent names the
            // assembly's own root, e.g. the weapon's class). Accessory-to-accessory parenting (weapon -> mount ->
            // optic) is not resolved: nested entries carry no ids. Guard: say so loudly instead of misplacing it.
            TSet<FString> Siblings;
            if (!ParentKey.IsEmpty())
                for (const auto& V : List)
                    if (const auto O = Object(V)) Siblings.Add(String(O, TEXT("entity_name")));
            for (const auto& V : List)
            {
                FSCSceneInstance I;
                const FReaderObject Child = Object(V);
                if (!Instance(Child, I, Detail)) I.ImportWarning = Detail;
                I.InteractionPoints = InteractionPoints(Child);
                ReadSeatRecords(Child, I);
                I.ParentId = ParentKey;
                // A child carries its own UI, and that of geometry-less children propagated onto it (another source).
                for (FSCUiBinding& B : I.UiBindings) B.bPropagated = B.SourceEntityName != I.EntityName;
                if (!ParentKey.IsEmpty() && Siblings.Contains(I.ParentEntityName) && I.ParentEntityName != I.EntityName)
                    Warn(I.EntityName, FString::Printf(TEXT("nested parent '%s' is another entry of the '%s' assembly; accessory-to-accessory "
                        "parenting is not supported, attached to the assembly root (known limit)"), *I.ParentEntityName, *ParentEntity));
                if (I.SourceTransformBasis.IsEmpty() && I.ImportWarning.IsEmpty())
                    I.ImportWarning = I.MeshAsset.IsEmpty() && !I.PortJson.IsEmpty() ? FString() // metadata-only item
                                                                                     : TEXT("Child lacks resolved source_transform_basis");
                if (!I.ImportWarning.IsEmpty()) Warn(I.EntityName, I.ImportWarning);
                const FString Key = I.Id;
                const bool bMetadataOnly = I.SourceTransformBasis.IsEmpty() && I.ImportWarning.IsEmpty();
                if (!bMetadataOnly) Out.Children.Add(MoveTemp(I));
                if (const auto O = Object(V); O && !Key.IsEmpty())
                {
                    FString PackageDir;
                    if (FSCPackageSource::Normalize(String(O, TEXT("package_dir")), PackageDir)) NestedPackages.AddUnique(PackageDir);
                    ReadChildren(ReaderArray(O, TEXT("children")), Key, String(O, TEXT("entity_name")));
                }
            }
        };
    ReadChildren(ReaderArray(Scene, TEXT("children")), FString(), FString());
    // WP-K1 cargo: the vehicle's inventory containers and the root item's occupancy (interiors' below, by index).
    SCCargoRecords::ParseOccupancy(*Root, Out.RootOccupancy);
    SCCargoRecords::AddContainers(Scene, INDEX_NONE, Out, Warn);
    for (const auto& V : ReaderArray(Scene, TEXT("interiors")))
    {
        const auto O = Object(V); FSCInterior I;
        if (!Instance(O, I.Container, Detail, TEXT("container_transform"))) { Warn(TEXT("interior"), Detail); continue; }
        I.Container.EntityName = String(O, TEXT("name"));
        I.Container.SourceTransformBasis = TEXT("cryengine_z_up");
        for (const auto& R : ReaderArray(O, TEXT("room_volumes"))) if (R->Type == EJson::Object) I.RoomVolumeJson.Add(Json(R->AsObject()));
        for (const auto& R : ReaderArray(O, TEXT("audio_spots"))) if (R->Type == EJson::Object) I.AudioSpotJson.Add(Json(R->AsObject()));
        for (const auto& R : ReaderArray(O, TEXT("room_connectors"))) if (R->Type == EJson::Object) I.RoomConnectorJson.Add(Json(R->AsObject()));
        for (const auto& R : ReaderArray(O, TEXT("audio_areas"))) if (R->Type == EJson::Object) I.AudioAreaJson.Add(Json(R->AsObject()));
        for (const auto& P : ReaderArray(O, TEXT("placements")))
        {
            FSCSceneInstance Placement;
            if (!Instance(Object(P), Placement, Detail, TEXT("transform"))) { Warn(TEXT("placement"), Detail); continue; }
            Placement.EntityName = String(P->AsObject(), TEXT("cgf_path"));
            Placement.SourceTransformBasis = TEXT("cryengine_z_up");
            Placement.EntityId = String(P->AsObject(), TEXT("entity_id"));
            Placement.EntityClassGuid = String(P->AsObject(), TEXT("entity_class_guid")).ToLower();
            Placement.LoadoutOwnerEntityId = String(P->AsObject(), TEXT("loadout_owner_entity_id"));
            for (const auto& L : ReaderArray(P->AsObject(), TEXT("links"))) if (L->Type == EJson::String) Placement.Links.Add(L->AsString());
            ReadInteractions(P->AsObject(), Placement);
            I.Placements.Add(MoveTemp(Placement));
        }
        for (const auto& S : ReaderArray(O, TEXT("proximity_sensors")))
        {
            const auto SO = Object(S);
            if (!SO) continue;
            FSCProximitySensor Sensor;
            if (!Matrix(SO, TEXT("transform"), Sensor.LocalMatrix, Detail)) { Warn(TEXT("proximity sensor"), Detail); Detail.Reset(); continue; }
            Sensor.EntityClass = String(SO, TEXT("entity_class"));
            Sensor.EntityId = String(SO, TEXT("entity_id"));
            double Delay = 0;
            if (SO->TryGetNumberField(TEXT("exit_delay"), Delay)) Sensor.ExitDelay = float(Delay);
            const FReaderObject* Volume;
            if (SO->TryGetObjectField(TEXT("volume"), Volume) && String(*Volume, TEXT("kind")) == TEXT("box"))
            {
                const auto& E = ReaderArray(*Volume, TEXT("extent"));
                const auto& Off = ReaderArray(*Volume, TEXT("offset"));
                if (E.Num() == 3) Sensor.Extent = FVector(E[0]->AsNumber(), E[1]->AsNumber(), E[2]->AsNumber());
                if (Off.Num() == 3) Sensor.Offset = FVector(Off[0]->AsNumber(), Off[1]->AsNumber(), Off[2]->AsNumber());
            }
            for (const auto& L : ReaderArray(SO, TEXT("links"))) if (L->Type == EJson::String) Sensor.Links.Add(L->AsString());
            if (Sensor.Extent.IsNearlyZero() || Sensor.Links.IsEmpty()) { Warn(TEXT("proximity sensor"), TEXT("no box volume or no linked door: ") + Sensor.EntityId); continue; }
            I.ProximitySensors.Add(MoveTemp(Sensor));
        }
        ReadLiftRecords(O, I, Warn);
        SCCargoRecords::AddContainers(O, Out.Interiors.Num(), Out, Warn);
        Out.Interiors.Add(MoveTemp(I));
    }
    // Paints (optional `paints.json` beside scene.json): each variant's exterior material file is loaded with the
    // scene's, so a paint can be put on without another read.
    if (const FString PaintsPath = FPaths::GetPath(Out.ScenePath) / TEXT("paints.json"); Source.Exists(PaintsPath))
    {
        FReaderObject O;
        if (!ReadJson(Source, PaintsPath, O, Detail)) { Warn(PaintsPath, Detail); Detail.Reset(); }
        for (const auto& V : ReaderArray(O, TEXT("paint_variants")))
        {
            if (!Object(V)) { Warn(PaintsPath, TEXT("Invalid paint variant")); continue; }
            FSCPaintVariant Paint;
            Paint.DisplayName = String(V->AsObject(), TEXT("display_name"));
            Paint.PaletteId = String(V->AsObject(), TEXT("palette_id"));
            Paint.SubgeometryTag = String(V->AsObject(), TEXT("subgeometry_tag"));
            const FString Sidecar = String(V->AsObject(), TEXT("exterior_material_sidecar"));
            if (!Sidecar.IsEmpty() && !FSCPackageSource::Normalize(Sidecar, Paint.ExteriorSidecar)) { Warn(PaintsPath, TEXT("Unsafe paint sidecar: ") + Sidecar); continue; }
            Out.Paints.Add(MoveTemp(Paint));
        }
    }
    TSet<FString> Sidecars;
    auto Collect = [&](const FSCSceneInstance& I) { if (!I.MaterialSidecar.IsEmpty()) Sidecars.Add(I.MaterialSidecar); };
    Collect(Out.Root); for (const auto& I : Out.Children) Collect(I);
    for (const auto& I : Out.Interiors) for (const auto& P : I.Placements) Collect(P);
    for (const auto& Paint : Out.Paints) if (!Paint.ExteriorSidecar.IsEmpty()) Sidecars.Add(Paint.ExteriorSidecar);
    TArray<FString> Sorted = Sidecars.Array(); Sorted.Sort();
    for (const FString& Path : Sorted)
    {
        FSCMaterialSet Set; Set.Path = Path; FReaderObject O;
        if (!ReadJson(Source, Path, O, Detail, &Set.ContentHash) || !O->HasTypedField<EJson::Array>(TEXT("submaterials")))
        { Warn(Path, Detail.IsEmpty() ? TEXT("Missing submaterials array") : Detail); Detail.Reset(); continue; }
        // The .mtl's own default tint palette, as the exporter names palette ids: palette/<record name, lowercase>.
        const FReaderObject* Authored;
        if (O->TryGetObjectField(TEXT("authored_material_set"), Authored))
            for (const auto& V : ReaderArray(*Authored, TEXT("attributes")))
                if (const auto A = Object(V); A && String(A, TEXT("name")).Equals(TEXT("DefaultPalette"), ESearchCase::IgnoreCase))
                {
                    const FString Record = FPaths::GetCleanFilename(String(A, TEXT("value")).Replace(TEXT("\\"), TEXT("/")));
                    if (!Record.IsEmpty()) Set.DefaultPaletteId = TEXT("palette/") + Record.ToLower();
                    break;
                }
        double SidecarVersion = 1;
        if (O->TryGetNumberField(TEXT("version"), SidecarVersion) && SidecarVersion != 1 && SidecarVersion != 2)
            Warn(Path, FString::Printf(TEXT("Unknown material sidecar version %g; reading v2 fields"), SidecarVersion));
        for (const auto& V : ReaderArray(O, TEXT("submaterials")))
        {
            const auto Sub = Object(V);
            FSCSubmaterial M; M.Index = Set.Submaterials.Num();
            if (!Sub) { Warn(Path, TEXT("Invalid submaterial slot; using placeholder")); Set.Submaterials.Add(MoveTemp(M)); continue; }
            double Index;
            if (Sub->TryGetNumberField(TEXT("index"), Index)) M.SourceIndex = int32(Index);
            M.Name = String(Sub, TEXT("submaterial_name")); M.ShaderFamily = String(Sub, TEXT("shader_family")); M.Json = Json(Sub);
            const FReaderObject* Alpha;
            if (Sub->TryGetObjectField(TEXT("alpha_config"), Alpha))
            {
                (*Alpha)->TryGetBoolField(TEXT("blended"), M.bBlended);
                FString Mode = String(*Alpha, TEXT("mode"));
                M.bBlended |= Mode.Equals(TEXT("blend"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("blended"), ESearchCase::IgnoreCase);
            }
            else { FString AlphaMode = String(Sub, TEXT("alpha_config")); M.bBlended = AlphaMode.Equals(TEXT("blended"), ESearchCase::IgnoreCase) || AlphaMode.Equals(TEXT("blend"), ESearchCase::IgnoreCase); }
            Set.Submaterials.Add(MoveTemp(M));
        }
        Out.Materials.Add(MoveTemp(Set));
    }
    auto ValidateSidecar = [&](FSCSceneInstance& I)
    {
        if (!I.MaterialSidecar.IsEmpty() && !Out.Materials.ContainsByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; }))
            I.ImportWarning = TEXT("Unavailable material sidecar: ") + I.MaterialSidecar;
    };
    ValidateSidecar(Out.Root); for (auto& I : Out.Children) ValidateSidecar(I);
    for (auto& I : Out.Interiors) for (auto& P : I.Placements) ValidateSidecar(P);
    const FString Directory = FPaths::GetPath(Out.ScenePath);
    TArray<FString> PaletteFiles = {Directory / TEXT("palettes.json")};
    for (const FString& Nested : NestedPackages) PaletteFiles.Add(Nested / TEXT("palettes.json"));
    for (const FString& PalettesPath : PaletteFiles)
    {
        if (!Source.Exists(PalettesPath)) continue;
        FReaderObject O; if (!ReadJson(Source, PalettesPath, O, Detail)) Warn(PalettesPath, Detail);
        for (const auto& V : ReaderArray(O, TEXT("palettes")))
        {
            if (!Object(V)) { Warn(PalettesPath, TEXT("Invalid palette")); continue; }
            FSCPalette P; P.Id = String(V->AsObject(), TEXT("id")); P.Json = Json(V->AsObject());
            // Repeated items share ids: the first definition wins (identical by contract).
            if (!Out.Palettes.ContainsByPredicate([&](const FSCPalette& E) { return E.Id == P.Id; })) Out.Palettes.Add(MoveTemp(P));
        }
    }
    // A paint whose palette or exterior material file the package lacks is dropped (warned): it could not be put on.
    Out.Paints.RemoveAll([&](const FSCPaintVariant& Paint) {
        const bool bPalette = Paint.PaletteId.IsEmpty() || Out.Palettes.ContainsByPredicate([&](const FSCPalette& P) { return P.Id == Paint.PaletteId; });
        const bool bSidecar = Paint.ExteriorSidecar.IsEmpty() || Out.Materials.ContainsByPredicate([&](const FSCMaterialSet& M) { return M.Path == Paint.ExteriorSidecar; });
        if (!bPalette || !bSidecar) Warn(TEXT("paints.json"), TEXT("Paint without its palette or material file: ") + Paint.Label());
        return !bPalette || !bSidecar;
    });
    // Palette resolution, the chain the contract's other consumers apply: the instance's own palette, a
    // placement's container's, the material set's authored DefaultPalette, then the palette of the entity the
    // instance is attached to. An id the package carries no palette for does not resolve; with no resolvable
    // candidate the authored id is kept.
    auto Known = [&](const FString& Id) { return !Id.IsEmpty() && Out.Palettes.ContainsByPredicate([&](const FSCPalette& P) { return P.Id == Id; }); };
    auto ResolvePalette = [&](FSCSceneInstance& I, const FString& Container, const FString& Host)
    {
        const FSCMaterialSet* Set = Out.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; });
        for (const FString& Id : {I.PaletteId, Container, Set ? Set->DefaultPaletteId : FString(), Host})
            if (Known(Id)) { I.PaletteId = Id; return; }
    };
    ResolvePalette(Out.Root, FString(), FString());
    for (int32 Index = 0; Index < Out.Children.Num(); ++Index) // depth-first: a nested entry follows its assembly
    {
        FSCSceneInstance& I = Out.Children[Index];
        const FSCSceneInstance* Assembly = I.ParentId.IsEmpty() ? nullptr
            : Out.Children.FindByPredicate([&](const FSCSceneInstance& C) { return C.Id == I.ParentId; });
        ResolvePalette(I, FString(), Assembly ? Assembly->PaletteId : Out.Root.PaletteId);
    }
    for (auto& I : Out.Interiors) for (auto& P : I.Placements) ResolvePalette(P, I.Container.PaletteId, Out.Root.PaletteId);
    for (const auto& V : ReaderArray(*Root, TEXT("animations")))
    {
        if (!Object(V)) { Warn(Out.ScenePath, TEXT("Invalid clip record")); continue; }
        FSCClipRecord Clip; Clip.Name = String(V->AsObject(), TEXT("name")); Clip.Json = Json(V->AsObject());
        const FString Path = String(V->AsObject(), TEXT("sidecar"));
        if (!Path.IsEmpty() && !FSCPackageSource::Normalize(Directory / Path, Clip.Sidecar)) { Warn(Out.ScenePath, TEXT("Unsafe clip sidecar")); continue; }
        Out.Clips.Add(MoveTemp(Clip));
    }
    const FReaderObject* Controls; const FReaderObject* Glow;
    if (Scene->TryGetObjectField(TEXT("controls"), Controls) && (*Controls)->TryGetObjectField(TEXT("engine_glow"), Glow))
    {
        double V = 0;
        if ((*Glow)->TryGetNumberField(TEXT("min_strength"), V)) Out.EngineGlow.MinStrength = float(V);
        if ((*Glow)->TryGetNumberField(TEXT("max_strength"), V)) Out.EngineGlow.MaxStrength = float(V);
        if ((*Glow)->TryGetNumberField(TEXT("default_strength"), V)) Out.EngineGlow.DefaultStrength = float(V);
        (*Glow)->TryGetStringField(TEXT("units"), Out.EngineGlow.Units);
        for (const auto& T : ReaderArray(*Glow, TEXT("targets")))
        {
            FSCEngineGlowTarget Target; const auto O = Object(T);
            if (!O || !FSCPackageSource::Normalize(String(O, TEXT("material_sidecar")), Target.MaterialSidecar) || !O->TryGetNumberField(TEXT("source_material_index"), V))
            { Warn(Out.ScenePath, TEXT("Invalid engine_glow target")); continue; }
            Target.SourceMaterialIndex = int32(V); Out.EngineGlow.Targets.Add(MoveTemp(Target));
        }
    }
    // Mesh existence/format failures are recovered per asset by FSCImporter.
    return true;
}
