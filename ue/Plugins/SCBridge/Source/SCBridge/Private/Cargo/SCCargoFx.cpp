// FSCCargoFx: the tractor's and the box contacts' effects resolved from the data at run time (header: SCCargoFx.h).
#include "SCCargoFx.h"
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
} // namespace

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

UNiagaraSystem* FSCCargoFx::Contact(const FString& ShipEntity, float& OutMinScale, float& OutMaxScale)
{
    if (ShipEntity.IsEmpty() || !Client) return nullptr;
    if (FContact* Known = Contacts.Find(ShipEntity))
    {
        OutMinScale = Known->MinScale;
        OutMaxScale = Known->MaxScale;
        return Known->bDone ? Known->System.Get() : nullptr;
    }
    Contacts.Add(ShipEntity);
    TWeakPtr<FSCCargoFx> Weak = AsShared();
    const FString Record = ShipEntity.Contains(TEXT(".")) ? ShipEntity : TEXT("EntityClassDefinition.") + ShipEntity;
    Client->DataCoreRecord(Record, [Weak, ShipEntity, Record](const FSCTextResult& R) {
        TSharedPtr<FSCCargoFx> Self = Weak.Pin();
        if (!Self) return;
        FString CustomName;
        if (R.bOK)
        {
            TSharedPtr<FJsonValue> V;
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(R.Json), V);
            if (const FObj Params = FindType(V, TEXT("CollisionMFXEntityParams"))) Params->TryGetStringField(TEXT("customEffect"), CustomName);
        }
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
        FXmlFile File(R.Json, EConstructMethod::ConstructFromBuffer);
        const FXmlNode* Root = File.IsValid() ? File.GetRootNode() : nullptr;
        const FXmlNode* Customs = Root ? Root->FindChildNode(TEXT("CustomEffects")) : nullptr;
        if (Customs)
            for (const FXmlNode* C : Customs->GetChildrenNodes())
            {
                TMap<FString, FString>& Groups = Self->Custom.FindOrAdd(C->GetAttribute(TEXT("name")).ToLower());
                for (const FXmlNode* G : C->GetChildrenNodes())
                    if (G->GetTag() == TEXT("SurfaceTypeGroup")) Groups.Add(G->GetAttribute(TEXT("name")).ToLower(), G->GetAttribute(TEXT("fx")));
            }
        Self->Xml = Customs ? EXml::Ready : EXml::Failed;
        UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX MaterialEffects.xml custom_effects=%d%s"), Self->Custom.Num(), Customs ? TEXT("") : TEXT(" (no CustomEffects)"));
        TArray<TFunction<void()>> Run = MoveTemp(Self->AfterXml);
        if (Self->Xml == EXml::Ready)
            for (TFunction<void()>& F : Run) F();
    });
}

FString FSCCargoFx::CustomEntry(const FString& CustomName) const
{
    const TMap<FString, FString>* Groups = Custom.Find(CustomName.ToLower());
    if (!Groups) return FString();
    if (const FString* Fx = Groups->Find(Runtime().BoxGroup.ToLower())) return *Fx;
    const FString* Default = Groups->Find(TEXT("default"));
    return Default ? *Default : FString();
}

void FSCCargoFx::ResolveEntry(const FString& Entry, TFunction<void(const FString&, float, float)> Done)
{
    Client->MaterialEffect(Entry, [Entry, Done = MoveTemp(Done)](const FSCTextResult& R) {
        FString Reference;
        float Min = 1, Max = 1;
        if (const FObj O = R.bOK ? ParseObject(R.Json) : nullptr)
        {
            const TArray<TSharedPtr<FJsonValue>>* Particles;
            if (O->TryGetArrayField(TEXT("particles"), Particles) && !Particles->IsEmpty())
                if (const FObj P = (*Particles)[0]->AsObject())
                {
                    P->TryGetStringField(TEXT("reference"), Reference);
                    const TSharedPtr<FJsonObject>* Attributes;
                    if (P->TryGetObjectField(TEXT("attributes"), Attributes))
                    {
                        FString S;
                        if ((*Attributes)->TryGetStringField(TEXT("minscale"), S)) Min = FCString::Atof(*S);
                        if ((*Attributes)->TryGetStringField(TEXT("maxscale"), S)) Max = FCString::Atof(*S);
                    }
                }
        }
        else
        {
            UE_LOG(LogSCCargoFx, Warning, TEXT("CARGO_FX entry %s: %s"), *Entry, *R.Error.ToString());
        }
        Done(Reference, Min, FMath::Max(Min, Max));
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
    // The scale: the component's (positions and speeds of local-space emitters) and SC.Scale2 (sprite sizes).
    if (UNiagaraComponent* C = UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), System, Hit.ImpactPoint, Out.Rotation(), FVector(Scale), true, false, ENCPoolMethod::AutoRelease))
    {
        C->SetVariableVec2(TEXT("SC.Scale2"), FVector2D(Scale, Scale));
        C->Activate(true);
    }
    UE_LOG(LogSCCargoFx, Display, TEXT("CARGO_FX_CONTACT box=%d ship=%s at=%s dv_cm_s=%.0f scale=%.2f"), Box->BoxId, *Entity, *Hit.ImpactPoint.ToCompactString(), Dv, Scale);
}
