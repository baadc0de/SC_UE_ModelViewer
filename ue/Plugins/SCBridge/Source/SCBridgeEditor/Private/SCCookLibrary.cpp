// USCCookLibrary: environment -> persistent level actor with pooled assets (WP-C1).
#include "SCCookLibrary.h"
#include "SCBridgeClient.h" // scbridge.h: sc_texels_decode_bgra8
#include "SCEnvironment.h"
#include "SCHangarIndex.h"
#include "SCInstancing.h"
#include "SCVehicle.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCook, Log, All);

namespace
{
FString SafeName(FString Name)
{
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name.Left(80);
}

struct FCooker
{
    FString Root, SharedRoot;
    TMap<UObject*, UObject*> Remap; // runtime object -> persistent asset
    TArray<UPackage*> ToSave;
    TArray<FString> Warnings;
    int32 Reused = 0, Created = 0;

    void MakePersistent(UObject* Object)
    {
        Object->ClearFlags(RF_Transient);
        ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); });
    }
    /** Runtime textures carry only platform mips (the package's DDS blocks as uploaded, or BGRA8 from PNG): an editable
     *  BGRA8 source from mip 0, block formats decoded by the bridge (sc_texels_decode_bgra8). */
    void AddTextureSource(UTexture2D* Texture)
    {
        FTexturePlatformData* Platform = Texture->GetPlatformData();
        if (!Platform || Platform->Mips.IsEmpty())
        {
            Warnings.Add(Texture->GetName() + TEXT(": no platform mips; saved without source"));
            return;
        }
        uint32 Dxgi = 0;
        switch (Platform->PixelFormat)
        {
        case PF_B8G8R8A8: Dxgi = 87; break;
        case PF_R8G8B8A8: Dxgi = 28; break;
        case PF_G8: Dxgi = 61; break;
        case PF_DXT1: Dxgi = 71; break;
        case PF_DXT3: Dxgi = 74; break;
        case PF_DXT5: Dxgi = 77; break;
        case PF_BC4: Dxgi = 80; break;
        case PF_BC5: Dxgi = 83; break;
        case PF_BC7: Dxgi = 98; break;
        default:
            Warnings.Add(FString::Printf(TEXT("%s: pixel format %s has no source decode; saved without source"), *Texture->GetName(),
                                         GPixelFormats[Platform->PixelFormat].Name));
            return;
        }
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        TArray64<uint8> Bgra;
        Bgra.SetNumUninitialized(int64(Mip.SizeX) * Mip.SizeY * 4);
        const bool bOK = Data && sc_texels_decode_bgra8(Data, size_t(Mip.BulkData.GetBulkDataSize()), Dxgi, uint32(Mip.SizeX), uint32(Mip.SizeY),
                                                        Bgra.GetData(), size_t(Bgra.Num())) == SC_STATUS_OK;
        Mip.BulkData.Unlock();
        if (bOK)
            Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, TSF_BGRA8, Bgra.GetData());
        else
            Warnings.Add(FString::Printf(TEXT("%s: decode failed (%s); saved without source"), *Texture->GetName(), UTF8_TO_TCHAR(sc_last_error(nullptr))));
    }
    /** The pooled asset for a source key: an existing one (this run or on disk), else this object moved into the pool. */
    UObject* Pool(UObject* Object, const FString& Key, const FString& Folder)
    {
        if (!Object) return nullptr;
        if (UObject** Found = Remap.Find(Object)) return *Found;
        const FString Name = SafeName(FPaths::GetBaseFilename(Key.Left(Key.Find(TEXT("#"))).IsEmpty() ? Key : Key.Left(Key.Find(TEXT("#"))))) +
                             TEXT("_") + FMD5::HashAnsiString(*Key).Left(8);
        const FString PackageName = SharedRoot / Folder / Name;
        const FString ObjectPath = PackageName + TEXT(".") + Name;
        UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
        if (!Existing && FPackageName::DoesPackageExist(PackageName)) Existing = LoadObject<UObject>(nullptr, *ObjectPath);
        if (Existing)
        {
            ++Reused;
            Remap.Add(Object, Existing);
            return Existing;
        }
        if (auto* Texture = Cast<UTexture2D>(Object)) AddTextureSource(Texture);
        UPackage* Package = CreatePackage(*PackageName);
        Object->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->SetFlags(RF_Public | RF_Standalone);
        MakePersistent(Object);
        Package->MarkPackageDirty();
        ToSave.Add(Package);
        ++Created;
        Remap.Add(Object, Object);
        return Object;
    }
    /** Constant copy of a dynamic instance under Root/Materials, textures pointed at the pool. */
    UMaterialInterface* Constant(UMaterialInterface* Material)
    {
        auto* MID = Cast<UMaterialInstanceDynamic>(Material);
        if (!MID) return Material;
        if (UObject** Found = Remap.Find(MID)) return Cast<UMaterialInterface>(*Found);
        FString Name = SafeName(TEXT("MI_") + MID->GetName());
        const FString Base = Name;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)); ++Suffix) Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / TEXT("Materials") / Name));
        auto* MIC = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MIC->SetParentEditorOnly(Constant(MID->Parent));
        for (const FScalarParameterValue& P : MID->ScalarParameterValues) MIC->SetScalarParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FVectorParameterValue& P : MID->VectorParameterValues) MIC->SetVectorParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FTextureParameterValue& P : MID->TextureParameterValues)
        {
            UTexture* Texture = P.ParameterValue;
            if (UObject** Pooled = Texture ? Remap.Find(Texture) : nullptr) Texture = Cast<UTexture>(*Pooled);
            MIC->SetTextureParameterValueEditorOnly(P.ParameterInfo, Texture);
        }
        MIC->PostEditChange();
        Package->MarkPackageDirty();
        ToSave.Add(Package);
        Remap.Add(MID, MIC);
        return MIC;
    }
};
} // namespace

bool USCCookLibrary::CookEnvironment(ASCVehicle* Environment, const FString& Root, const FString& SharedRoot)
{
    if (!Environment) return false;
    const double Start = FPlatformTime::Seconds();
    FSCEnvironment::BakeRigsStatic(Environment);
    const FSCInstancingStats Instancing = FSCInstancing::Consolidate(Environment); // pieces freed from the rigs
    // Runtime-only proxies (emissive Lumen copies with generated meshes) are rebuilt when the level loads.
    TArray<UActorComponent*> Proxies = Environment->GetComponentsByTag(UPrimitiveComponent::StaticClass(), TEXT("SCEmissiveExperiment"));
    for (UActorComponent* Proxy : Proxies) Proxy->DestroyComponent();

    FCooker Cook;
    Cook.Root = Root;
    Cook.SharedRoot = SharedRoot;
    for (const auto& [Object, Key] : Environment->SourcePaths)
        if (Object) Cook.Pool(Object, Key, Cast<UStaticMesh>(Object) ? TEXT("Meshes") : TEXT("Textures"));
    TArray<UStaticMeshComponent*> MeshComponents;
    Environment->GetComponents(MeshComponents);
    for (UStaticMeshComponent* C : MeshComponents)
    {
        if (UObject** Pooled = C->GetStaticMesh() ? Cook.Remap.Find(C->GetStaticMesh()) : nullptr)
            if (*Pooled != C->GetStaticMesh()) C->SetStaticMesh(Cast<UStaticMesh>(*Pooled));
        for (int32 Slot = 0; Slot < C->OverrideMaterials.Num(); ++Slot)
            if (C->OverrideMaterials[Slot]) C->SetMaterial(Slot, Cook.Constant(C->OverrideMaterials[Slot]));
    }
    TArray<ULightComponent*> Lights;
    Environment->GetComponents(Lights);
    for (ULightComponent* L : Lights)
        if (L->LightFunctionMaterial) L->SetLightFunctionMaterial(Cook.Constant(L->LightFunctionMaterial));
    // The actor and its components persist with the level. Runtime-only objects the actor owns (clip animations of the
    // baked rigs, skeletal meshes, dynamic materials now replaced by constants) stay transient and are unreferenced, so
    // they are not saved (a saved clip without its skeleton fails to load).
    Environment->ClipCatalog = nullptr;
    Environment->SkeletalMeshes.Empty();
    Environment->Materials.Empty();
    Environment->EngineGlowMaterials.Empty();
    Environment->ClearFlags(RF_Transient);
    for (UActorComponent* C : Environment->GetComponents()) Cook.MakePersistent(C);

    int32 Saved = 0;
    for (UPackage* Package : Cook.ToSave)
    {
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        if (UPackage::SavePackage(Package, Package->FindAssetInPackage(), *File, Args)) ++Saved;
        else Cook.Warnings.Add(TEXT("save failed: ") + Package->GetName());
    }
    for (const FString& W : Cook.Warnings) UE_LOG(LogSCCook, Warning, TEXT("COOK_WARNING %s"), *W);
    UE_LOG(LogSCCook, Display, TEXT("COOK root=%s pooled_new=%d pooled_reused=%d packages=%d saved=%d instanced=%d warnings=%d seconds=%.1f"), *Root,
           Cook.Created, Cook.Reused, Cook.ToSave.Num(), Saved, Instancing.Instanced, Cook.Warnings.Num(), FPlatformTime::Seconds() - Start);
    return Saved == Cook.ToSave.Num();
}

bool USCCookLibrary::RecordHangarCapacity(ASCVehicle* Environment, const FString& Style, const FString& Name)
{
    FBox Bay;
    FString Elevator;
    if (!FSCEnvironment::FindShipBay(Environment, Bay, &Elevator))
    {
        UE_LOG(LogSCCook, Warning, TEXT("HANGAR_CAPACITY %s/%s: no ship elevator"), *Style, *Name);
        return false;
    }
    const double Clearance = FSCEnvironment::BayClearance(Environment, Bay);
    const FString PackageName = FPackageName::ObjectPathToPackageName(FString(USCHangarIndex::AssetPath));
    USCHangarIndex* Index = USCHangarIndex::Load();
    UPackage* Package = Index ? Index->GetPackage() : CreatePackage(*PackageName);
    if (!Index)
        Index = NewObject<USCHangarIndex>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
    FSCHangarCapacity& Entry = Index->FindOrAdd(Style, Name);
    Entry.BayCm = FVector2D(Bay.GetSize());
    Entry.ClearanceCm = Clearance;
    Entry.Elevator = Elevator;
    Entry.bTopLoading = FSCEnvironment::IsTopLoading(Environment, Bay);
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Index, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
    UE_LOG(LogSCCook, Display, TEXT("HANGAR_CAPACITY %s/%s elevator=%s bay_m=%.1fx%.1f clearance_m=%.1f top_loading=%d saved=%d hangars=%d"), *Style, *Name,
           *Elevator, Entry.BayCm.X / 100, Entry.BayCm.Y / 100, Clearance / 100, Entry.bTopLoading, bSaved, Index->Hangars.Num());
    return bSaved;
}
