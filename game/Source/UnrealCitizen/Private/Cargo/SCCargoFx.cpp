// FSCCargoFx: the tractor's and the box contacts' effects resolved from the data at run time (header: SCCargoFx.h).
#include "SCCargoFx.h"
#include "Cargo/SCCargoFxMapping.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonWriter.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCBridgeClient.h"
#include "SCCargoState.h"
#include "SCFxNames.h"
#include "SCVehicle.h"
#include "SCVehicleInstances.h"
#include "SCViewerSubsystem.h"
#include "Components/BoxComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "XmlFile.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCargoFx, Log, All);

namespace
{
using FObj = TSharedPtr<FJsonObject>;
FObj ParseObject(const FString& Json)
{
    TSharedPtr<FJsonValue> V;
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), V) && V && V->Type == EJson::Object ? V->AsObject() : nullptr;
}
/** Depth-first: the first object whose `__type` (or `_Type_`) is Type. */
FObj FindType(const TSharedPtr<FJsonValue>& V, const FString& Type)
{
    if (!V) return nullptr;
    if (V->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue>& E : V->AsArray())
            if (FObj F = FindType(E, Type)) return F;
        return nullptr;
    }
    if (V->Type != EJson::Object) return nullptr;
    const FObj O = V->AsObject();
    FString T;
    if (!O->TryGetStringField(TEXT("__type"), T)) O->TryGetStringField(TEXT("_Type_"), T);
    if (T == Type) return O;
    for (const auto& Pair : O->Values)
        if (FObj F = FindType(Pair.Value, Type)) return F;
    return nullptr;
}
/** The record name of a ship or obstacle entity. */
FString EntityRecord(const FString& Entity)
{
    return Entity.Contains(TEXT(".")) ? Entity : TEXT("EntityClassDefinition.") + Entity;
}
/** A callback answer of the client, waited for on the game thread (a commandlet: its answers are posted there). */
FSCTextResult WaitForAnswer(TFunctionRef<void(TFunction<void(const FSCTextResult&)>)> Ask)
{
    const TSharedRef<TOptional<FSCTextResult>> Answer = MakeShared<TOptional<FSCTextResult>>();
    Ask([Answer](const FSCTextResult& R) { *Answer = R; });
    while (!Answer->IsSet())
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(0.005f);
    }
    return Answer->GetValue();
}
} // namespace

// ---------------------------------------------------------------------------------------------------- the mapping (WP-O1-4b-3b)

bool FSCCargoFxMapping::ParseCustomEffects(const FString& Xml, TMap<FString, TMap<FString, FString>>& Out)
{
    Out.Reset();
    FXmlFile File(Xml, EConstructMethod::ConstructFromBuffer);
    const FXmlNode* Root = File.IsValid() ? File.GetRootNode() : nullptr;
    const FXmlNode* Customs = Root ? Root->FindChildNode(TEXT("CustomEffects")) : nullptr;
    if (!Customs) return false;
    for (const FXmlNode* C : Customs->GetChildrenNodes())
    {
        TMap<FString, FString>& Groups = Out.FindOrAdd(C->GetAttribute(TEXT("name")).ToLower());
        for (const FXmlNode* G : C->GetChildrenNodes())
            if (G->GetTag() == TEXT("SurfaceTypeGroup")) Groups.Add(G->GetAttribute(TEXT("name")).ToLower(), G->GetAttribute(TEXT("fx")));
    }
    return true;
}
FString FSCCargoFxMapping::EntryFor(const TMap<FString, TMap<FString, FString>>& Custom, const FString& CustomName, const FString& Group)
{
    const TMap<FString, FString>* Groups = Custom.Find(CustomName.ToLower());
    if (!Groups) return FString();
    if (const FString* Fx = Groups->Find(Group.ToLower())) return *Fx;
    const FString* Default = Groups->Find(TEXT("default"));
    return Default ? *Default : FString();
}
FSCCargoFxMapping::FEffect FSCCargoFxMapping::ParseEntry(const FString& EntryJson)
{
    FEffect Out;
    const FObj O = ParseObject(EntryJson);
    const TArray<TSharedPtr<FJsonValue>>* Particles;
    if (O && O->TryGetArrayField(TEXT("particles"), Particles) && !Particles->IsEmpty())
        if (const FObj P = (*Particles)[0]->AsObject())
        {
            P->TryGetStringField(TEXT("reference"), Out.Reference);
            const TSharedPtr<FJsonObject>* Attributes;
            if (P->TryGetObjectField(TEXT("attributes"), Attributes))
            {
                FString S;
                if ((*Attributes)->TryGetStringField(TEXT("minscale"), S)) Out.MinScale = FCString::Atof(*S);
                if ((*Attributes)->TryGetStringField(TEXT("maxscale"), S)) Out.MaxScale = FCString::Atof(*S);
            }
        }
    Out.MaxScale = FMath::Max(Out.MinScale, Out.MaxScale);
    return Out;
}
FString FSCCargoFxMapping::ContactCustomEffect(const FString& RecordJson)
{
    TSharedPtr<FJsonValue> V;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RecordJson), V);
    FString CustomName;
    if (const FObj Params = FindType(V, TEXT("CollisionMFXEntityParams"))) Params->TryGetStringField(TEXT("customEffect"), CustomName);
    return CustomName;
}
FString FSCCargoFxMapping::ToJson() const
{
    auto Effect = [](const FEffect& E) {
        const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("reference"), E.Reference);
        O->SetNumberField(TEXT("min_scale"), E.MinScale);
        O->SetNumberField(TEXT("max_scale"), E.MaxScale);
        return O;
    };
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("box_group"), BoxGroup);
    Root->SetObjectField(TEXT("hit"), Effect(Hit));
    const TSharedRef<FJsonObject> Ships = MakeShared<FJsonObject>();
    for (const auto& [Entity, E] : Contacts) Ships->SetObjectField(Entity, Effect(E));
    Root->SetObjectField(TEXT("contacts"), Ships);
    FString Out;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Out));
    return Out;
}
bool FSCCargoFxMapping::Parse(const FString& Json, FSCCargoFxMapping& Out)
{
    Out = FSCCargoFxMapping();
    const FObj Root = ParseObject(Json);
    if (!Root) return false;
    auto Effect = [](const FObj& O, FEffect& E) {
        if (!O) return;
        O->TryGetStringField(TEXT("reference"), E.Reference);
        double Min = 1, Max = 1;
        O->TryGetNumberField(TEXT("min_scale"), Min);
        O->TryGetNumberField(TEXT("max_scale"), Max);
        E.MinScale = float(Min);
        E.MaxScale = float(Max);
    };
    Root->TryGetStringField(TEXT("box_group"), Out.BoxGroup);
    const TSharedPtr<FJsonObject>* Hit = nullptr;
    if (Root->TryGetObjectField(TEXT("hit"), Hit)) Effect(*Hit, Out.Hit);
    const TSharedPtr<FJsonObject>* Ships = nullptr;
    if (Root->TryGetObjectField(TEXT("contacts"), Ships))
        for (const auto& [Entity, Value] : (*Ships)->Values)
            if (Value && Value->Type == EJson::Object) Effect(Value->AsObject(), Out.Contacts.Add(FString(Entity)));
    return true;
}
bool FSCCargoFxMapping::Resolve(FSCBridgeClient& Client, const FSCCargoDroneDefinition& Drone, const TArray<FString>& Entities, FSCCargoFxMapping& Out,
                                FString& Error)
{
    Out = FSCCargoFxMapping();
    Out.BoxGroup = FSCCargoFx::Runtime().BoxGroup;
    const FSCTextResult Xml = WaitForAnswer([&](TFunction<void(const FSCTextResult&)> Done) {
        Client.P4kReadXml(TEXT("Data/Libs/MaterialEffects/MaterialEffects.xml"), MoveTemp(Done));
    });
    TMap<FString, TMap<FString, FString>> Custom;
    if (!Xml.bOK || !ParseCustomEffects(Xml.Json, Custom))
    {
        Error = TEXT("MaterialEffects.xml: ") + (Xml.bOK ? FString(TEXT("no CustomEffects")) : Xml.Error.ToString());
        return false;
    }
    auto Resolved = [&](const FString& CustomName) {
        const FString Entry = EntryFor(Custom, CustomName, Out.BoxGroup);
        if (Entry.IsEmpty()) return FEffect();
        const FSCTextResult R = WaitForAnswer([&](TFunction<void(const FSCTextResult&)> Done) { Client.MaterialEffect(Entry, MoveTemp(Done)); });
        return R.bOK ? ParseEntry(R.Json) : FEffect();
    };
    if (!Drone.HitEffectName.IsEmpty()) Out.Hit = Resolved(Drone.HitEffectName);
    for (const FString& Entity : Entities)
    {
        if (Entity.IsEmpty() || Out.Contacts.Contains(Entity)) continue;
        UE::Tasks::TTask<FSCTextResult> Record = Client.DataCoreRecord(EntityRecord(Entity));
        Record.Wait();
        const FString CustomName = Record.GetResult().bOK ? ContactCustomEffect(Record.GetResult().Json) : FString();
        Out.Contacts.Add(Entity, CustomName.IsEmpty() ? FEffect() : Resolved(CustomName));
    }
    return true;
}

const FSCCargoFx::FRuntime& FSCCargoFx::Runtime()
{
    static const FRuntime R = [] {
        FRuntime Out;
        const FString File = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("FX"), TEXT("effects.json"));
        FString Text;
        const FObj Root = FFileHelper::LoadFileToString(Text, *File) ? ParseObject(Text) : nullptr;
        const TSharedPtr<FJsonObject>* Run = nullptr;
        if (Root && Root->TryGetObjectField(TEXT("runtime"), Run))
        {
            (*Run)->TryGetStringField(TEXT("box_surface_group"), Out.BoxGroup);
            (*Run)->TryGetNumberField(TEXT("contact_min_speed_cm_s"), Out.ContactMinSpeedCmS);
            (*Run)->TryGetNumberField(TEXT("contact_full_speed_cm_s"), Out.ContactFullSpeedCmS);
            (*Run)->TryGetNumberField(TEXT("contact_min_interval_s"), Out.ContactMinIntervalS);
            Out.bLoaded = true;
        }
        UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX runtime=%s box_group=%s contact_min_cm_s=%.0f full_cm_s=%.0f interval_s=%.2f"), Out.bLoaded ? *File : TEXT("(none)"),
               *Out.BoxGroup, Out.ContactMinSpeedCmS, Out.ContactFullSpeedCmS, Out.ContactMinIntervalS);
        return Out;
    }();
    return R;
}

UNiagaraSystem* FSCCargoFx::Load(const FString& Reference, const TCHAR* What)
{
    if (Reference.IsEmpty()) return nullptr;
    const FString Path = SCFxNames::SystemPathForReference(Reference);
    UNiagaraSystem* System = Path.IsEmpty() ? nullptr : LoadObject<UNiagaraSystem>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
    static TSet<FString> Logged;
    if (!Logged.Contains(Reference))
    {
        Logged.Add(Reference);
        if (System)
        {
            UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX %s %s -> %s"), What, *Reference, *Path);
        }
        else
        {
            UE_LOG(LogSCCargoFx, Warning, TEXT("CARGO_FX %s %s: no system %s (run -run=SCFxBuild with it in Config/FX/effects.json)"), What, *Reference,
                   Path.IsEmpty() ? TEXT("(not an effect reference)") : *Path);
        }
    }
    return System;
}

void FSCCargoFx::Start(const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe>& InClient, const FSCCargoDroneDefinition& Drone)
{
    const FString Key = Drone.BeamEffect + Drone.MuzzleEffect + Drone.HitEffectName;
    if (Key == StartedFor && !Key.IsEmpty()) return;
    StartedFor = Key;
    Client = InClient;
    StrengthValues = Drone.Strengths;
    if (Drone.BeamEffect.IsEmpty())
    {
        Why = Client ? TEXT("the tractor record names no beam effect") : TEXT("no archive (FFI): the effects come from the tractor record");
        UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX tractor effects unavailable: %s"), *Why);
        return;
    }
    BeamSystem.Reset(Load(Drone.BeamEffect, TEXT("beam")));
    MuzzleSystem.Reset(Load(Drone.MuzzleEffect, TEXT("muzzle")));
    Why = BeamSystem ? FString() : TEXT("the beam system is not built");
    UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX tractor beam=%s muzzle=%s hit=%s strengths stable=%.2f unstable=%.2f no_target=%.2f impact=%.2f/%.2f blend=%.2f"),
           *Drone.BeamEffect, *Drone.MuzzleEffect, *Drone.HitEffectName, StrengthValues.Stable, StrengthValues.Unstable, StrengthValues.NoTarget,
           StrengthValues.ImpactControlling, StrengthValues.ImpactInvalid, StrengthValues.BlendCoeff);
    if (Drone.HitEffectName.IsEmpty()) return;
    const FString Hit = Drone.HitEffectName;
    TWeakPtr<FSCCargoFx> Weak = AsShared();
    WithMaterialEffects([Weak, Hit] {
        TSharedPtr<FSCCargoFx> Self = Weak.Pin();
        if (!Self) return;
        const FString Entry = Self->CustomEntry(Hit);
        if (Entry.IsEmpty())
        {
            UE_LOG(LogSCCargoFx, Warning, TEXT("CARGO_FX hit %s: no MaterialEffects entry for surface group %s"), *Hit, *Runtime().BoxGroup);
            return;
        }
        Self->ResolveEntry(Entry, [Weak, Hit, Entry](const FString& Reference, float, float) {
            if (TSharedPtr<FSCCargoFx> S = Weak.Pin(); S && !Reference.IsEmpty()) S->HitSystem.Reset(Load(Reference, TEXT("hit")));
            UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX hit custom=%s entry=%s particle=%s"), *Hit, *Entry, *Reference);
        });
    });
}

void FSCCargoFx::StartPrepared(const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe>& InClient, const FSCCargoDroneDefinition& Drone,
                               const FSCCargoFxMapping& Mapping)
{
    // The beam and the muzzle as Start sets them (the tractor record's references), the hit and the contacts from the mapping;
    // nothing is asked of the archive. A ship the mapping does not cover asks it later (Contact), when it is open.
    FSCCargoDroneDefinition NoHit = Drone;
    NoHit.HitEffectName.Reset();
    Start(nullptr, NoHit);
    StartedFor = Drone.BeamEffect + Drone.MuzzleEffect + Drone.HitEffectName;
    Client = InClient;
    HitSystem.Reset(Load(Mapping.Hit.Reference, TEXT("hit")));
    for (const auto& [Entity, E] : Mapping.Contacts)
    {
        FContact& C = Contacts.FindOrAdd(Entity);
        C.bDone = true;
        C.System.Reset(Load(E.Reference, TEXT("contact")));
        C.MinScale = E.MinScale;
        C.MaxScale = E.MaxScale;
    }
    UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX source=prepared hit=%s contacts=%d box_group=%s"), Mapping.Hit.Reference.IsEmpty() ? TEXT("none") : *Mapping.Hit.Reference,
           Mapping.Contacts.Num(), *Mapping.BoxGroup);
}

UNiagaraSystem* FSCCargoFx::Contact(const FString& ShipEntity, float& OutMinScale, float& OutMaxScale)
{
    if (ShipEntity.IsEmpty()) return nullptr;
    // Known first: a prepared mapping's contacts answer without the archive.
    if (FContact* Known = Contacts.Find(ShipEntity))
    {
        OutMinScale = Known->MinScale;
        OutMaxScale = Known->MaxScale;
        return Known->bDone ? Known->System.Get() : nullptr;
    }
    if (!Client) return nullptr;
    Contacts.Add(ShipEntity);
    TWeakPtr<FSCCargoFx> Weak = AsShared();
    const FString Record = EntityRecord(ShipEntity);
    Client->DataCoreRecord(Record, [Weak, ShipEntity, Record](const FSCTextResult& R) {
        TSharedPtr<FSCCargoFx> Self = Weak.Pin();
        if (!Self) return;
        const FString CustomName = R.bOK ? FSCCargoFxMapping::ContactCustomEffect(R.Json) : FString();
        if (CustomName.IsEmpty())
        {
            Self->Contacts.FindOrAdd(ShipEntity).bDone = true;
            UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX contact ship=%s: no CollisionMFXEntityParams.customEffect (%s)"), *ShipEntity, R.bOK ? TEXT("record") : *R.Error.ToString());
            return;
        }
        Self->WithMaterialEffects([Weak, ShipEntity, CustomName] {
            TSharedPtr<FSCCargoFx> S = Weak.Pin();
            if (!S) return;
            const FString Entry = S->CustomEntry(CustomName);
            if (Entry.IsEmpty())
            {
                S->Contacts.FindOrAdd(ShipEntity).bDone = true;
                UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX contact ship=%s custom=%s: no entry for surface group %s"), *ShipEntity, *CustomName, *Runtime().BoxGroup);
                return;
            }
            S->ResolveEntry(Entry, [Weak, ShipEntity, CustomName, Entry](const FString& Reference, float Min, float Max) {
                TSharedPtr<FSCCargoFx> T = Weak.Pin();
                if (!T) return;
                FContact& C = T->Contacts.FindOrAdd(ShipEntity);
                C.bDone = true;
                C.System.Reset(Load(Reference, TEXT("contact")));
                C.MinScale = Min;
                C.MaxScale = Max;
                UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX contact ship=%s custom=%s group=%s entry=%s particle=%s scale=%.2f..%.2f"), *ShipEntity, *CustomName,
                       *Runtime().BoxGroup, *Entry, *Reference, Min, Max);
            });
        });
    });
    return nullptr;
}

void FSCCargoFx::WithMaterialEffects(TFunction<void()> Then)
{
    if (Xml == EXml::Ready) { Then(); return; }
    if (Xml == EXml::Failed || !Client) return;
    AfterXml.Add(MoveTemp(Then));
    if (Xml == EXml::Loading) return;
    Xml = EXml::Loading;
    TWeakPtr<FSCCargoFx> Weak = AsShared();
    Client->P4kReadXml(TEXT("Data/Libs/MaterialEffects/MaterialEffects.xml"), [Weak](const FSCTextResult& R) {
        TSharedPtr<FSCCargoFx> Self = Weak.Pin();
        if (!Self) return;
        if (!R.bOK)
        {
            Self->Xml = EXml::Failed;
            Self->AfterXml.Reset();
            UE_LOG(LogSCCargoFx, Warning, TEXT("CARGO_FX MaterialEffects.xml: %s"), *R.Error.ToString());
            return;
        }
        const bool bCustoms = FSCCargoFxMapping::ParseCustomEffects(R.Json, Self->Custom);
        Self->Xml = bCustoms ? EXml::Ready : EXml::Failed;
        UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX MaterialEffects.xml custom_effects=%d%s"), Self->Custom.Num(), bCustoms ? TEXT("") : TEXT(" (no CustomEffects)"));
        TArray<TFunction<void()>> Run = MoveTemp(Self->AfterXml);
        if (Self->Xml == EXml::Ready)
            for (TFunction<void()>& F : Run) F();
    });
}

FString FSCCargoFx::CustomEntry(const FString& CustomName) const
{
    return FSCCargoFxMapping::EntryFor(Custom, CustomName, Runtime().BoxGroup);
}

void FSCCargoFx::ResolveEntry(const FString& Entry, TFunction<void(const FString&, float, float)> Done)
{
    Client->MaterialEffect(Entry, [Entry, Done = MoveTemp(Done)](const FSCTextResult& R) {
        if (!R.bOK) UE_LOG(LogSCCargoFx, Warning, TEXT("CARGO_FX entry %s: %s"), *Entry, *R.Error.ToString());
        const FSCCargoFxMapping::FEffect E = R.bOK ? FSCCargoFxMapping::ParseEntry(R.Json) : FSCCargoFxMapping::FEffect();
        Done(E.Reference, E.MinScale, E.MaxScale);
    });
}

// ---------------------------------------------------------------------------------------------------- subsystem hooks

FSCCargoFx* USCCargoSubsystem::GetFx() const
{
    return State ? State->Fx.Get() : nullptr;
}

void USCCargoSubsystem::OnBoxContact(ASCCargoContainer* Box, AActor* Other, const FHitResult& Hit, const FVector& NormalImpulse)
{
    FSCCargoFx* Fx = GetFx();
    const ASCVehicle* Ship = Cast<ASCVehicle>(Other);
    USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    if (!Fx || !Box || !Box->Body || !Ship || !Viewer) return;
    const FSCCargoFx::FRuntime& Run = FSCCargoFx::Runtime();
    // The speed change along the contact normal (NormalImpulse is kg cm/s), and one effect per box per interval.
    const double Mass = FMath::Max<double>(Box->Body->GetMass(), 1e-3);
    const double Dv = NormalImpulse.Size() / Mass;
    if (!Run.bLoaded || Dv < Run.ContactMinSpeedCmS) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (const double* Last = State->ContactAt.Find(Box->BoxId); Last && Now - *Last < Run.ContactMinIntervalS) return;
    // A ship of the set (the loaded one or an instance; scenario obstacles too): the hangar, other boxes and the
    // drones are not (the data plays no particles for a box on the hangar floor or on another box).
    const int32 Instance = Viewer->GetVehicleInstanceId(Ship);
    if (Instance == INDEX_NONE) return;
    FString Entity = Instance == 0 ? Viewer->GetLoadedEntity() : FString();
    for (const FSCVehicleInstanceInfo& I : Viewer->GetVehicleInstances())
        if (I.Id == Instance) Entity = I.Entity;
    float MinScale = 1, MaxScale = 1;
    UNiagaraSystem* System = Fx->Contact(Entity, MinScale, MaxScale);
    if (!System) return;
    State->ContactAt.Add(Box->BoxId, Now);
    // The entry's scale range over the contact's speed change (threshold .. full); the data gives no rule between.
    const double Full = FMath::Max(Run.ContactFullSpeedCmS, Run.ContactMinSpeedCmS + 1.0);
    const float Scale = FMath::Lerp(MinScale, MaxScale, float(FMath::Clamp((Dv - Run.ContactMinSpeedCmS) / (Full - Run.ContactMinSpeedCmS), 0.0, 1.0)));
    // The effect's +X is the emit axis: out of the ship's surface, towards the box.
    const FVector Normal = (Hit.ImpactNormal.IsNearlyZero() ? -NormalImpulse : FVector(Hit.ImpactNormal)).GetSafeNormal();
    const FVector Out = FVector::DotProduct(Normal, Box->GetActorLocation() - Hit.ImpactPoint) >= 0 ? Normal : -Normal;
    // The scale: the component's (positions and speeds of local-space emitters and, in the built systems, sprite sizes).
    if (UNiagaraComponent* C = UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), System, Hit.ImpactPoint, Out.Rotation(), FVector(Scale), true, false, ENCPoolMethod::AutoRelease))
        C->Activate(true);
    UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX_CONTACT box=%d ship=%s at=%s dv_cm_s=%.0f scale=%.2f"), Box->BoxId, *Entity, *Hit.ImpactPoint.ToCompactString(), Dv, Scale);
}
