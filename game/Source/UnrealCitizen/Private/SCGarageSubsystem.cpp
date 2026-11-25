// WP-M2. The fleet describes entities. World placement and ownership remain the viewer and Ship Link's responsibility.
#include "SCGarageSubsystem.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCEnvironment.h"
#include "SCFirstPersonCharacter.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "UCSettings.h"
#include "SCViewerSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UCScenario.h"

FString FSCGaragePolicy::StoreRefusal(bool bOwned, bool bAboard, bool bTransfer)
{
    if (!bOwned) return TEXT("Owner access required");
    if (bAboard) return TEXT("Leave the ship before storing it");
    if (bTransfer) return TEXT("A cargo transfer is using this ship");
    return {};
}

FString FSCGaragePolicy::BayRefusal(bool bOwned, bool bConfirmed, bool& bNeedsConfirmation)
{
    bNeedsConfirmation = bOwned && !bConfirmed;
    if (!bOwned) return TEXT("The bay is occupied by another owner");
    return bNeedsConfirmation ? FString(TEXT("Store the ship occupying the bay first")) : FString();
}

bool FSCGaragePolicy::IsFleetCandidate(const FSCEntityHit& Entity)
{
    const FString Name = Entity.DisplayName.TrimStartAndEnd();
    // The game localization marks internal placeholders with <= ... =>. Their geometry makes them browser
    // candidates, but they are not named fleet entries.
    const bool bPlaceholder = Name.StartsWith(TEXT("<=")) && Name.EndsWith(TEXT("=>"));
    return Entity.Category == TEXT("Ships") && !Entity.GeometryPath.IsEmpty() && !bPlaceholder;
}

bool FSCGaragePolicy::IsVehicleKiosk(const FSCUiBinding& Binding)
{
    return ASCVehicle::IsGarageKiosk(Binding);
}

const TCHAR* FSCGaragePolicy::StatusName(ESCGarageStatus Status)
{
    switch (Status)
    {
    case ESCGarageStatus::Retrieving: return TEXT("Retrieving");
    case ESCGarageStatus::InHangar: return TEXT("In hangar");
    case ESCGarageStatus::Unavailable: return TEXT("Unavailable");
    default: return TEXT("Stored");
    }
}

USCGarageSubsystem* USCGarageSubsystem::Get(const UObject* WorldContext)
{
    UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World && World->IsGameWorld() ? World->GetSubsystem<USCGarageSubsystem>() : nullptr;
}

USCViewerSubsystem* USCGarageSubsystem::Viewer() const { return USCViewerSubsystem::Get(this); }

bool USCGarageSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USCGarageSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USCGarageSubsystem, STATGROUP_Tickables);
}

FString USCGarageSubsystem::Canonical(FString Entity)
{
    Entity.TrimStartAndEndInline();
    Entity.RemoveFromStart(TEXT("EntityClassDefinition."), ESearchCase::IgnoreCase);
    return Entity;
}

void USCGarageSubsystem::RequestCatalog()
{
    if (!bScannedCooked)
    {
        bScannedCooked = true;
        LoadPreparedDetails(PreparedDetails); // their details as read at prep (none: from the archive, as before)
        IAssetRegistry& Assets = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        // The prepared ships' levels by level name, as ShipLevelPath names them from their entity (which drops characters): a
        // level is listed as the entity the table knows it by, and an obstacle's (an asteroid's) not at all.
        TSet<FString> ObstacleLevels;
        TMap<FString, FString> EntityOfLevel;
        for (const auto& [Entity, Known] : PreparedDetails)
        {
            const FString LevelName = FPackageName::GetShortName(USCViewerSubsystem::ShipLevelPath(Entity));
            if (!Known.bFleet) ObstacleLevels.Add(LevelName);
            else EntityOfLevel.Add(LevelName, Entity);
        }
        TArray<FAssetData> LevelCandidates;
        Assets.GetAssetsByPath(FName(TEXT("/Game/SC/Ships/Maps")), LevelCandidates, false);
        for (const FAssetData& Level : LevelCandidates)
            // An entity's own prepared level; a paint variant (a scenario's skin) is that entity's level too, not another ship.
            if (Level.AssetClassPath == UWorld::StaticClass()->GetClassPathName() && !USCViewerSubsystem::IsShipPaintLevel(Level.PackageName.ToString()))
            {
                const FString Entity = FPackageName::GetShortName(Level.PackageName);
                // An obstacle's level (an asteroid) is no fleet ship (WP-O1-4b-3c).
                if (ObstacleLevels.Contains(Entity)) continue;
                const FString* Known = EntityOfLevel.Find(Entity);
                const FString Listed = Known ? *Known : Entity;
                if (!USCViewerSubsystem::CookedShipLevel(Listed).IsEmpty()) Cooked.AddUnique(Listed);
            }
    }
    USCViewerSubsystem* V = Viewer();
    if (!V || V->GetArchiveState() != ESCArchiveState::Ready || V->IsShowcase() ||
        RequestedGeneration == V->GetArchiveGeneration()) return;
    RequestedGeneration = V->GetArchiveGeneration();
    Catalog.Reset();
    Details.Reset();
    DetailsQueue.Reset();
    ManufacturerCodes.Reset();
    bDetailsReading = false;
    V->SearchAsync(FString(), MAX_int32, [Weak = TWeakObjectPtr<USCGarageSubsystem>(this), Generation = RequestedGeneration](const FSCSearchResult& Result) {
        USCGarageSubsystem* Garage = Weak.Get();
        if (!Garage || !Garage->Viewer() || Garage->Viewer()->GetArchiveGeneration() != Generation || !Result.Error.IsEmpty()) return;
        for (const FSCEntityHit& Item : Result.Hits)
            if (FSCGaragePolicy::IsFleetCandidate(Item)) Garage->Catalog.Add(Item);
    });
}


void USCGarageSubsystem::RequestDetails(const FString& Name)
{
    const FString Entity = Canonical(Name);
    if (Entity.IsEmpty() || Details.Contains(FName(*Entity))) return;
    // A prepared ship's details were read at prep (WP-O1-4b-3c): no archive read.
    if (const FSCGarageShipDetails* Prepared = PreparedDetails.Find(Entity))
    {
        Details.Add(FName(*Entity), *Prepared);
        return;
    }
    // The details are DataCore records: without an archive reader (a packaged game) none can be read.
    if (const USCViewerSubsystem* V = Viewer(); V && !V->CanReadArchive()) return;
    Details.Add(FName(*Entity));
    DetailsQueue.Add(Entity);
}

bool USCGarageSubsystem::ReadClassDetails(const FString& ClassJson, FSCGarageShipDetails& Item, FString& ManufacturerRef)
{
    ManufacturerRef.Reset();
    TSharedPtr<FJsonObject> Class;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ClassJson), Class) || !Class) return false;
    Class->TryGetStringField(TEXT("vehicleRole"), Item.Role);
    if (Item.Role.StartsWith(TEXT("@"))) Item.Role.Reset(); // the live record carries the key only
    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    if (Class->TryGetArrayField(TEXT("Components"), Components) && Components)
        for (const auto& Value : *Components)
        {
            const TSharedPtr<FJsonObject>* Component = nullptr;
            const TSharedPtr<FJsonObject>* Attach = nullptr;
            if (!Value || !Value->TryGetObject(Component) || !Component || !*Component ||
                !(*Component)->TryGetObjectField(TEXT("AttachDef"), Attach) || !Attach || !*Attach) continue;
            (*Attach)->TryGetStringField(TEXT("Manufacturer"), ManufacturerRef);
            double Size = 0;
            if ((*Attach)->TryGetNumberField(TEXT("Size"), Size) && Size > 0) Item.Size = FString::FromInt(int32(Size));
            break;
        }
    return true;
}

FString USCGarageSubsystem::ReadManufacturerCode(const FString& Json)
{
    FString Code;
    TSharedPtr<FJsonObject> Result;
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result) && Result) Result->TryGetStringField(TEXT("Code"), Code);
    return Code;
}

FString USCGarageSubsystem::PreparedDetailsFile()
{
    return FUCPrepared::Directory() / TEXT("Shared") / TEXT("garage_details.json");
}

bool USCGarageSubsystem::LoadPreparedDetails(TMap<FString, FSCGarageShipDetails>& Out, const FString& File)
{
    Out.Reset();
    // A save that did not finish (moved aside, not yet replaced) left the last table beside it: that one is read.
    const FString Older = File + TEXT(".older");
    const FString& Read = !IFileManager::Get().FileExists(*File) && IFileManager::Get().FileExists(*Older) ? Older : File;
    FString Text;
    TSharedPtr<FJsonObject> Root;
    if (!FFileHelper::LoadFileToString(Text, *Read) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root)
        return false;
    // Every row as the writer writes it (three strings, empty allowed; "fleet" a bool, true when a table from before has
    // none): anything else fails the read, so no prep rewrites the table without it and no record passes on it.
    for (const auto& [Entity, Value] : Root->Values)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        FSCGarageShipDetails D;
        // The JSON types themselves (TryGet*Field converts a number to a string or a bool).
        auto Typed = [&](const TCHAR* Name, EJson Type) {
            const TSharedPtr<FJsonValue> Field = (*Row)->TryGetField(Name);
            return Field && Field->Type == Type;
        };
        if (!Value || !Value->TryGetObject(Row) || !Row || !*Row || !Typed(TEXT("manufacturer"), EJson::String) ||
            !Typed(TEXT("role"), EJson::String) || !Typed(TEXT("size"), EJson::String) ||
            ((*Row)->HasField(TEXT("fleet")) && !Typed(TEXT("fleet"), EJson::Boolean)))
        {
            Out.Reset();
            return false;
        }
        D.Manufacturer = (*Row)->GetStringField(TEXT("manufacturer"));
        D.Role = (*Row)->GetStringField(TEXT("role"));
        D.Size = (*Row)->GetStringField(TEXT("size"));
        if ((*Row)->HasField(TEXT("fleet"))) D.bFleet = (*Row)->GetBoolField(TEXT("fleet"));
        if ((*Row)->HasField(TEXT("build")))
        {
            if (!Typed(TEXT("build"), EJson::String))
            {
                Out.Reset();
                return false;
            }
            D.Build = (*Row)->GetStringField(TEXT("build"));
        }
        Out.Add(Canonical(FString(Entity)), MoveTemp(D));
    }
    return true;
}

bool USCGarageSubsystem::SavePreparedDetails(const TMap<FString, FSCGarageShipDetails>& Details, FString& Error, const FString& File)
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<FString> Entities;
    Details.GetKeys(Entities);
    Entities.Sort(); // stable file for a stable set
    for (const FString& Entity : Entities)
    {
        const FSCGarageShipDetails& D = Details[Entity];
        const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("manufacturer"), D.Manufacturer);
        Row->SetStringField(TEXT("role"), D.Role);
        Row->SetStringField(TEXT("size"), D.Size);
        Row->SetBoolField(TEXT("fleet"), D.bFleet);
        Row->SetStringField(TEXT("build"), D.Build);
        Root->SetObjectField(Entity, Row);
    }
    FString Text;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
    // Written beside it, then moved into place; the older table is set aside first and put back if the move fails, so one
    // of them always exists (a reader never sees half a table).
    IFileManager& Files = IFileManager::Get();
    Files.MakeDirectory(*FPaths::GetPath(File), true);
    const FString Partial = File + TEXT(".partial"), Older = File + TEXT(".older");
    if (!FFileHelper::SaveStringToFile(Text, *Partial))
    {
        Files.Delete(*Partial, false, true, true);
        Error = TEXT("cannot write ") + Partial;
        return false;
    }
    // The table set aside (a .older left by a save that did not finish is that table already: kept, as the reader takes it).
    if (Files.FileExists(*File))
    {
        Files.Delete(*Older, false, true, true);
        if (!Files.Move(*Older, *File, false, true))
        {
            Files.Delete(*Partial, false, true, true);
            Error = TEXT("cannot set aside ") + File;
            return false;
        }
    }
    if (!Files.Move(*File, *Partial, false, true))
    {
        if (Files.FileExists(*Older)) Files.Move(*File, *Older, false, true); // the older table back
        Files.Delete(*Partial, false, true, true);
        Error = TEXT("cannot write ") + File;
        return false;
    }
    Files.Delete(*Older, false, true, true);
    return true;
}

void USCGarageSubsystem::ReadNextDetail()
{
    USCViewerSubsystem* V = Viewer();
    if (bDetailsReading || DetailsQueue.IsEmpty() || !V || !V->GetClient() || V->GetArchiveState() != ESCArchiveState::Ready ||
        V->IsBusy() || V->IsSpawningVehicles() || V->IsEnvironmentLoading()) return;
    const FString Entity = MoveTemp(DetailsQueue[0]);
    DetailsQueue.RemoveAt(0, EAllowShrinking::No);
    const FName Key(*Entity);
    const int32 Generation = V->GetArchiveGeneration();
    const TWeakObjectPtr<USCGarageSubsystem> Weak(this);
    bDetailsReading = true;
    V->GetClient()->DataCoreRecord(TEXT("EntityClassDefinition.") + Entity, [Weak, Key, Generation](const FSCTextResult& Record) {
        USCGarageSubsystem* G = Weak.Get();
        if (!G || !G->Viewer() || G->Viewer()->GetArchiveGeneration() != Generation) return;
        const auto Finish = [&] { G->bDetailsReading = false; };
        FDetails Read;
        FString ManufacturerRef;
        if (!Record.bOK || !ReadClassDetails(Record.Json, Read, ManufacturerRef))
        {
            Finish();
            return;
        }
        FDetails& Item = G->Details.FindOrAdd(Key);
        Item.Role = Read.Role;
        Item.Size = Read.Size;
        if (ManufacturerRef.IsEmpty() || !G->Viewer()->GetClient()) { Finish(); return; }
        if (const FString* Code = G->ManufacturerCodes.Find(ManufacturerRef)) { Item.Manufacturer = *Code; Finish(); return; }
        G->Viewer()->GetClient()->DataCoreRecord(ManufacturerRef, [Weak, Key, Generation, ManufacturerRef](const FSCTextResult& Manufacturer) {
            USCGarageSubsystem* Garage = Weak.Get();
            if (!Garage || !Garage->Viewer() || Garage->Viewer()->GetArchiveGeneration() != Generation) return;
            FString Code = Manufacturer.bOK ? ReadManufacturerCode(Manufacturer.Json) : FString();
            Garage->ManufacturerCodes.Add(ManufacturerRef, Code);
            Garage->Details.FindOrAdd(Key).Manufacturer = MoveTemp(Code);
            Garage->bDetailsReading = false;
        });
    });
}

bool USCGarageSubsystem::Available(const FString& Entity) const
{
    const USCViewerSubsystem* V = Viewer();
    return Cooked.ContainsByPredicate([&](const FString& Ship) { return Ship.Equals(Entity, ESearchCase::IgnoreCase); }) ||
        Catalog.ContainsByPredicate([&](const FSCEntityHit& Ship) { return Ship.Name.Equals(Entity, ESearchCase::IgnoreCase); }) ||
        (V && !V->GarageShipLevel(Entity).IsEmpty());
}

TArray<FSCGarageRow> USCGarageSubsystem::Rows(const FString& EntityFilter, bool bIncludeDefaultStored) const
{
    const USCViewerSubsystem* V = Viewer();
    const USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    TArray<FString> Names = UUCSettings::Get()->GarageFleet;
    if (Names.IsEmpty() && bIncludeDefaultStored)
    {
        Names = Cooked;
        for (const FSCEntityHit& Hit : Catalog) Names.Add(Hit.Name);
    }
    // A ship stored from a level (a scenario's paint, the only level a package may hold) is in the fleet whatever the fleet
    // setting lists: it was the player's when stored.
    if (V) Names.Append(V->GetStoredShipEntities());
    const TArray<FSCVehicleInstanceInfo> Instances = V ? V->GetVehicleInstances() : TArray<FSCVehicleInstanceInfo>();
    TMap<FName, const FSCVehicleInstanceInfo*> Paired;
    const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(GetWorld());
    if (Link)
        for (const auto& Info : Instances)
            // (A cargo setup's obstacle, an asteroid, is no fleet ship whoever it is paired with.)
            if (Link->RoleOf(Link->GetPlayer(), Info.Actor.Get()) == ESCShipRole::Owner && !(Cargo && Cargo->IsObstacleInstance(Info.Id)))
            {
                const FString Name = Canonical(Info.Entity);
                const FName Key(*Name);
                if (!Paired.Contains(Key)) Paired.Add(Key, &Info);
                Names.Add(Name);
            }
    if (Pending.IsSet()) Names.Add(Pending->Entity);
    for (const auto& Failure : Failures) Names.Add(Failure.Key);
    if (!EntityFilter.IsEmpty())
        Names.RemoveAll([&](const FString& Name) { return !Canonical(Name).Equals(EntityFilter, ESearchCase::IgnoreCase); });

    TMap<FName, const FSCEntityHit*> Hits;
    if (Names.Num() > 16)
    {
        Hits.Reserve(Catalog.Num());
        for (const auto& Hit : Catalog) Hits.Add(FName(*Hit.Name), &Hit);
    }
    TArray<FSCGarageRow> Out;
    TSet<FName> Seen;
    const ASCFirstPersonCharacter* Player = V ? V->GetFirstPerson() : nullptr;
    for (const FString& Name : Names)
    {
        const FString Entity = Canonical(Name);
        const FName Key(*Entity);
        if (Key.IsNone() || Seen.Contains(Key)) continue;
        Seen.Add(Key);
        FSCGarageRow& Row = Out.AddDefaulted_GetRef();
        Row.Entity = Entity;
        const FSCEntityHit* Hit = Hits.IsEmpty()
            ? Catalog.FindByPredicate([&](const FSCEntityHit& Item) { return Item.Name.Equals(Entity, ESearchCase::IgnoreCase); })
            : Hits.FindRef(Key);
        if (Hit)
            Row.DisplayName = Hit->DisplayName;
        if (Row.DisplayName.IsEmpty()) Row.DisplayName = Entity.Replace(TEXT("_"), TEXT(" "));
        if (const FDetails* Metadata = Details.Find(Key))
        {
            Row.Manufacturer = Metadata->Manufacturer;
            Row.Role = Metadata->Role;
            Row.Size = Metadata->Size;
        }
        Row.Error = Failures.FindRef(Entity.ToLower());
        if (!Row.Error.IsEmpty()) Row.Status = ESCGarageStatus::Unavailable;
        if (Pending.IsSet() && Pending->Entity.Equals(Entity, ESearchCase::IgnoreCase))
        {
            Row.Status = ESCGarageStatus::Retrieving;
            if (Pending->Id == 0 && V) Row.Progress = V->GetLoadProgress();
            else if (const auto* Info = Instances.FindByPredicate([&](const auto& I) { return I.Id == Pending->Id; }))
                Row.Progress = Info->Progress;
        }
        if (const auto* Info = Paired.FindRef(Key))
        {
            const ASCVehicle* Ship = Info->Actor.Get();
            if (!Ship) continue;
            Row.Status = ESCGarageStatus::InHangar;
            Row.Ship = Info->Id;
            Row.Progress = 1;
            Row.Error.Reset();
            Row.DistanceM = Player ? FVector::Dist(Ship->GetActorLocation(), Player->GetActorLocation()) / 100.0 : 0;
            const FSCSceneInstance& Root = Ship->Manifest.Root;
            if (!Root.DisplayName.IsEmpty()) Row.DisplayName = Root.DisplayName;
            if (Row.Role.IsEmpty()) Row.Role = Root.ItemSubType;
            if (Row.Size.IsEmpty()) Row.Size = Root.ItemSize;
            const int32 ActivePaint = Ship->GetPaint();
            Row.Paint = Ship->Manifest.Paints.IsValidIndex(ActivePaint) ? Ship->Manifest.Paints[ActivePaint].PaletteId : TEXT("default");
            for (const FSCPaintVariant& Paint : Ship->Manifest.Paints) Row.PaintOptions.Add(Paint.PaletteId);
        }
    }
    Out.Sort([](const FSCGarageRow& A, const FSCGarageRow& B) {
        const bool bAActive = A.Status == ESCGarageStatus::InHangar || A.Status == ESCGarageStatus::Retrieving;
        const bool bBActive = B.Status == ESCGarageStatus::InHangar || B.Status == ESCGarageStatus::Retrieving;
        if (bAActive != bBActive) return bAActive;
        return A.DisplayName == B.DisplayName ? A.Entity < B.Entity : A.DisplayName < B.DisplayName;
    });
    return Out;
}

bool USCGarageSubsystem::AddToFleet(const FString& Name)
{
    const FString Entity = Canonical(Name);
    if (!Available(Entity)) return false;
    UUCSettings* Settings = UUCSettings::Get();
    if (!Settings->GarageFleet.ContainsByPredicate([&](const FString& InFleet) { return InFleet.Equals(Entity, ESearchCase::IgnoreCase); }))
    {
        Settings->GarageFleet.Add(Entity);
        Settings->Save();
    }
    return true;
}

FSCGarageOrder USCGarageSubsystem::Retrieve(const FString& Name, ESCGaragePlacement At, bool bConfirmStore, int32 ConfirmedOccupant)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    FSCGarageOrder Result;
    auto Refuse = [&](const FString& Why) { Result.Refusal = Why; return Result; };
    if (!V || !Link) return Refuse(TEXT("The garage is unavailable"));
    if (Pending.IsSet() || V->IsBusy() || V->IsSpawningVehicles() || V->IsEnvironmentLoading()) return Refuse(TEXT("Placement is busy"));
    if (V->IsSpace()) return Refuse(TEXT("Retrieve ships from a hangar"));
    RequestCatalog();
    const FString Entity = Canonical(Name);
    const TArray<FSCGarageRow> Fleet = Rows(Entity);
    const auto* Row = Fleet.FindByPredicate([&](const FSCGarageRow& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
    if (!Row) return Refuse(TEXT("This ship is not in your fleet"));
    if (Row->Status == ESCGarageStatus::InHangar) return Refuse(TEXT("This ship is already in the hangar"));
    FBox Bay(ForceInit);
    const bool bHasBay = At == ESCGaragePlacement::Bay && FSCEnvironment::FindShipBay(V->GetEnvironment(), Bay);
    // The bay's occupant (stored below, once the retrieval can go ahead).
    ASCVehicle* Occupant = nullptr;
    if (At == ESCGaragePlacement::Bay)
        for (ASCVehicle* Candidate : V->GetVehicles())
        {
            const FBox Current = FSCEnvironment::SolidBounds(Candidate);
            const bool bOccupies = bHasBay
                ? Current.IsValid && Current.IntersectXY(Bay) && FMath::IsNearlyEqual(Current.Min.Z, Bay.Max.Z, 100.0)
                : Candidate == V->GetVehicle();
            if (!bOccupies) continue;
            Occupant = Candidate;
            break;
        }
    // The loaded ship's slot is free once the occupant is stored (it is the loaded ship, or there is none): then this
    // retrieval loads the loaded ship (CanLoad: its prepared level, or the archive); else it is an extra copy (its prepared
    // level - the loaded ship's paint for more of it - or the archive).
    const bool bLoadsPrimary = At == ESCGaragePlacement::Bay && (!V->GetVehicle() || Occupant == V->GetVehicle());
    if (bLoadsPrimary ? !V->CanLoadGarage(Entity)
                      : V->PreparedShipLevelFor(Entity).IsEmpty() && !(V->CanReadArchive() && V->GetArchiveState() == ESCArchiveState::Ready))
        return Refuse(V->CanReadArchive() ? TEXT("The ship is unavailable until the archive is open") : TEXT("The ship is unavailable in this build"));
    if (Occupant)
    {
        Result.Occupant = V->GetVehicleInstanceId(Occupant);
        const bool bOwned = Link->RoleOf(Link->GetPlayer(), Occupant) == ESCShipRole::Owner;
        const bool bConfirmedThisShip = bConfirmStore && (ConfirmedOccupant == INDEX_NONE || ConfirmedOccupant == Result.Occupant);
        const FString BlockingReason = FSCGaragePolicy::BayRefusal(bOwned, bConfirmedThisShip, Result.bConfirmStore);
        if (!BlockingReason.IsEmpty()) return Refuse(BlockingReason);
        const FString Stored = Store(Result.Occupant);
        if (!Stored.IsEmpty()) return Refuse(Stored);
    }

    if (At == ESCGaragePlacement::Bay && !V->GetVehicle())
    {
        if (!V->LoadGarageVehicle(Entity)) return Refuse(TEXT("Could not start retrieval"));
        Result.Ship = 0;
    }
    else
    {
        if (bHasBay)
        {
            FVector Top = Bay.GetCenter();
            Top.Z = Bay.Max.Z;
            Result.Ship = V->SpawnVehicleInstance(Entity, FTransform(Top));
        }
        else Result.Ship = V->SpawnGarageFloorVehicle(Entity);
        if (Result.Ship == INDEX_NONE) return Refuse(TEXT("Could not queue an extra vehicle"));
    }
    Pending = FPending{Entity, Result.Ship};
    Failures.Remove(Entity.ToLower());
    return Result;
}

FString USCGarageSubsystem::Store(int32 Id)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    ASCVehicle* Ship = Link ? Link->FindShip(Id) : nullptr;
    if (!Ship || !V) return TEXT("No such ship");
    if (const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(GetWorld()); Cargo && Cargo->IsObstacleInstance(Id))
        return TEXT("Not a ship");
    const bool bOwned = Link->RoleOf(Link->GetPlayer(), Ship) == ESCShipRole::Owner;
    const ASCFirstPersonCharacter* Player = V->GetFirstPerson();
    bool bAboard = false;
    if (Player)
    {
        const FVector At = Player->GetActorLocation();
        const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
        const FVector Feet = Capsule ? At - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight()) : At;
        const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Player->GetMovementBaseObject());
        bAboard = (Base && Base->GetOwner() == Ship) || Ship->IsInPhysicsGrid(At) || Ship->IsInsideLightArea(At) ||
            Ship->LiftRiddenBy(Feet, Base) != INDEX_NONE;
    }
    const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(this);
    const FString Refusal = FSCGaragePolicy::StoreRefusal(bOwned, bAboard, Cargo && Cargo->IsVehicleTransferActive(Ship));
    if (!Refusal.IsEmpty()) return Refusal;
    return (Id == 0 ? V->StoreGaragePrimary() : V->DestroyVehicleInstance(Id)) ? FString() : FString(TEXT("The ship could not be stored"));
}

FString USCGarageSubsystem::PaintEntity(const FString& Entity, const FString& Palette)
{
    const TArray<FSCGarageRow> Fleet = Rows(Entity);
    const auto* Row = Fleet.FindByPredicate([&](const FSCGarageRow& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
    if (!Row) return TEXT("This ship is not in your fleet");
    return Row->Ship != INDEX_NONE ? Paint(Row->Ship, Palette) : FString(TEXT("Retrieve the ship before choosing its paint"));
}

FString USCGarageSubsystem::Paint(int32 Id, const FString& Palette)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    ASCVehicle* Ship = Link ? Link->FindShip(Id) : nullptr;
    if (!V || !Ship || Link->RoleOf(Link->GetPlayer(), Ship) != ESCShipRole::Owner) return TEXT("Owner access required");
    const bool bDefault = Palette.IsEmpty() || Palette.Equals(TEXT("default"), ESearchCase::IgnoreCase);
    const int32 Index = bDefault ? INDEX_NONE : Ship->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) {
        return P.PaletteId.Equals(Palette, ESearchCase::IgnoreCase) || P.Label().Equals(Palette, ESearchCase::IgnoreCase);
    });
    if (!bDefault && Index == INDEX_NONE) return TEXT("This ship does not have that paint");
    if (!Ship->SetPaint(Index) && Ship->GetPaint() != Index) return TEXT("This instance cannot apply the paint");
    const TArray<FSCVehicleInstanceInfo> Instances = V->GetVehicleInstances();
    const auto* Info = Instances.FindByPredicate([Id](const auto& I) { return I.Id == Id; });
    if (!Info) return TEXT("The ship is gone");
    const FString Entity = Canonical(Info->Entity);
    USCViewerSettings* Settings = USCViewerSettings::Get();
    if (bDefault) Settings->Paints.Remove(Entity);
    else Settings->Paints.Add(Entity, Ship->Manifest.Paints[Index].PaletteId);
    Settings->Save();
    return {};
}

void USCGarageSubsystem::FinishRetrieve(const FString& Error, ASCVehicle* Ship)
{
    const FString Entity = Pending->Entity;
    Pending.Reset();
    if (!Error.IsEmpty()) Failures.Add(Entity.ToLower(), Error);
    if (Ship)
        if (const FString* Saved = USCViewerSettings::Get()->Paints.Find(Entity))
        {
            const int32 Index = Ship->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return P.PaletteId == *Saved; });
            if (Index != INDEX_NONE) Ship->SetPaint(Index);
        }
}

void USCGarageSubsystem::Tick(float)
{
    if (!Pending.IsSet()) { ReadNextDetail(); return; }
    USCViewerSubsystem* V = Viewer();
    if (!V) return;
    if (Pending->Id == 0)
    {
        if (V->IsBusy()) return;
        const FSCLoadReport Report = V->GetLastReport();
        if (Report.Result == ESCLoadPhase::Succeeded && V->GetLoadedEntity().Equals(Pending->Entity, ESearchCase::IgnoreCase))
            FinishRetrieve(FString(), V->GetVehicle());
        else FinishRetrieve(Report.Error.IsEmpty() ? FString(TEXT("Retrieval did not finish")) : Report.Error, nullptr);
        return;
    }
    const TArray<FSCVehicleInstanceInfo> Instances = V->GetVehicleInstances();
    const auto* Info = Instances.FindByPredicate([&](const auto& I) { return I.Id == Pending->Id; });
    if (!Info) FinishRetrieve(TEXT("The queued ship was removed"), nullptr);
    else if (Info->State == ESCVehicleInstanceState::Failed) FinishRetrieve(Info->Error, nullptr);
    else if (Info->State == ESCVehicleInstanceState::Ready) FinishRetrieve(FString(), Info->Actor.Get());
}

