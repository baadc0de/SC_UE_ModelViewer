// SCKeepAssets: rename transient import objects into /Game packages (references between them stay intact),
// convert dynamic material instances to constants, bake mesh slot materials from component overrides, save.
#include "SCKeepAssets.h"
#include "SCMeshImporter.h"
#include "SCRigAssets.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCClipCatalog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "SkinnedAssetCompiler.h"
#include "Engine/SkinnedAsset.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCKeep, Log, All);

namespace
{
FString SafeName(FString Name)
{
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name;
}

struct FKeeper
{
    FString Root;
    TArray<UPackage*> Packages;
    TMap<UObject*, UObject*> Kept; // transient source -> asset (identity for renamed objects)
    TArray<FString> Warnings;
    bool bFailed = false; // something asked for was not kept (a rig mesh without a source, or one that does not build)

    /** Move Object (and its subobjects) into its own package under Root/Folder, public + standalone. */
    UObject* Move(UObject* Object, const FString& Folder, const FString& PreferredName = FString())
    {
        if (!Object) return nullptr;
        if (UObject** Found = Kept.Find(Object)) return *Found;
        const FString Base = SafeName(PreferredName.IsEmpty() ? Object->GetName() : PreferredName);
        FString Name = Base;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / Folder / Name)) || FPackageName::DoesPackageExist(Root / Folder / Name); ++Suffix)
            Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / Folder / Name));
        Package->FullyLoad();
        Object->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->ClearFlags(RF_Transient);
        Object->SetFlags(RF_Public | RF_Standalone);
        ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); }); // nested by default
        Package->MarkPackageDirty();
        Packages.Add(Package);
        Kept.Add(Object, Object);
        FAssetRegistryModule::AssetCreated(Object);
        return Object;
    }
    /** Runtime textures carry only platform mips; give the asset an editable source from mip 0. */
    void AddTextureSource(UTexture2D* Texture)
    {
        FTexturePlatformData* Platform = Texture->GetPlatformData();
        if (!Platform || Platform->Mips.IsEmpty() || Platform->PixelFormat != PF_B8G8R8A8)
        {
            Warnings.Add(Texture->GetName() + TEXT(": no BGRA8 mip data; saved without source"));
            return;
        }
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const int64 Expected = int64(Mip.SizeX) * Mip.SizeY * 4;
        const void* Data = Mip.BulkData.LockReadOnly();
        if (Data && Mip.BulkData.GetBulkDataSize() == Expected)
            Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, TSF_BGRA8, static_cast<const uint8*>(Data));
        else
            Warnings.Add(Texture->GetName() + TEXT(": mip 0 not CPU-resident; saved without source"));
        Mip.BulkData.Unlock();
    }
    /** Constant copy of a dynamic instance: same parent, every overridden parameter. */
    UMaterialInterface* Constant(UMaterialInterface* Material)
    {
        auto* MID = Cast<UMaterialInstanceDynamic>(Material);
        if (!MID) return Material;
        if (UObject** Found = Kept.Find(MID)) return Cast<UMaterialInterface>(*Found);
        const FString Base = SafeName(TEXT("MI_") + MID->GetName());
        FString Name = Base;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)); ++Suffix)
            Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / TEXT("Materials") / Name));
        auto* MIC = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MIC->SetParentEditorOnly(MID->Parent);
        for (const FScalarParameterValue& P : MID->ScalarParameterValues)
            MIC->SetScalarParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FVectorParameterValue& P : MID->VectorParameterValues)
            MIC->SetVectorParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FTextureParameterValue& P : MID->TextureParameterValues)
            MIC->SetTextureParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        MIC->PostEditChange();
        Package->MarkPackageDirty();
        Packages.Add(Package);
        Kept.Add(MID, MIC);
        FAssetRegistryModule::AssetCreated(MIC);
        return MIC;
    }
};
} // namespace

FSCKeepResult SCKeepAssets::Keep(ASCVehicle* Vehicle, const FString& Entity)
{
    FSCKeepResult Result;
    if (!Vehicle)
    {
        Result.Message = TEXT("Nothing loaded");
        return Result;
    }
    const double Start = FPlatformTime::Seconds();
    FKeeper K;
    const FString Base = TEXT("/Game/SC/") + SafeName(Entity);
    K.Root = Base;
    for (int32 Suffix = 2; FPackageName::DoesPackageExist(K.Root / TEXT("Clips")) || FindPackage(nullptr, *(K.Root / TEXT("Clips"))); ++Suffix)
        K.Root = FString::Printf(TEXT("%s_%d"), *Base, Suffix); // never overwrite an earlier Keep

    for (UTexture2D* Texture : Vehicle->Textures)
        if (Texture && !K.Kept.Contains(Texture))
        {
            K.AddTextureSource(Texture);
            K.Move(Texture, TEXT("Textures"));
        }
    for (auto& Pair : Vehicle->GoboTextures)
        if (Pair.Value) K.Move(Pair.Value, TEXT("Textures"));
    // Slot materials: the first component override of each mesh slot becomes the asset's material.
    TArray<UStaticMeshComponent*> MeshComponents;
    Vehicle->GetComponents<UStaticMeshComponent>(MeshComponents);
    for (UStaticMeshComponent* C : MeshComponents)
    {
        UStaticMesh* Mesh = C->GetStaticMesh();
        if (!Mesh || !Vehicle->Meshes.Contains(Mesh)) continue;
        for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
            if (UMaterialInterface* Override = C->OverrideMaterials.IsValidIndex(Slot) ? C->OverrideMaterials[Slot].Get() : nullptr)
                if (!K.Kept.Contains(Mesh)) Mesh->GetStaticMaterials()[Slot].MaterialInterface = K.Constant(Override);
    }
    for (UStaticMesh* Mesh : Vehicle->Meshes)
        K.Move(Mesh, TEXT("Meshes"));
    // Rigs (WP-O1-4b; the WP-U3 follow-up): each rig mesh is kept as a copy with its skeleton — a runtime mesh given an
    // editor source model from the rig source the import kept (SCRigAssets), a prepared one (already an asset) rebuilt
    // from its own — its slots taking the rig components' current materials. The live meshes, skeletons and rig bodies
    // stay as they are; the kept clip catalog is a copy bound to the kept meshes (its editor-only sequences left out:
    // playback reads the bone tracks).
    TMap<const USkeletalMesh*, USkeletalMesh*> KeptRigs;
    for (USkeletalMesh* Mesh : Vehicle->SkeletalMeshes)
    {
        if (!Mesh) continue;
        const TSharedPtr<const FSCDerivedMesh>* Source = Vehicle->RigSources.Find(Mesh);
        const bool bAsset = Mesh->HasMeshDescription(0);
        if (!Source && !bAsset)
        {
            K.Warnings.Add(Mesh->GetName() + TEXT(": rig mesh not kept (no rig source and no source model)"));
            K.bFailed = true;
            continue;
        }
        USkeleton* Skeleton = DuplicateObject(Mesh->GetSkeleton(), GetTransientPackage());
        USkeletalMesh* Copy = DuplicateObject(Mesh, GetTransientPackage());
        Copy->SetSkeleton(Skeleton);
        FString Error;
        bool bBuilt = true;
        if (Source) bBuilt = SCRigAssets::MakeSavable(Copy, **Source, Error);
        else
        {
            Copy->Build();
            FSkinnedAssetCompilingManager::Get().FinishCompilation(TArray<USkinnedAsset*>{Copy});
        }
        if (!bBuilt)
        {
            K.Warnings.Add(Mesh->GetName() + TEXT(": rig mesh not kept (") + Error + TEXT(")"));
            K.bFailed = true;
            continue;
        }
        // As for static meshes: the first rig override of each slot becomes the kept mesh's material.
        TSet<int32> Assigned;
        for (USCRigComponent* Rig : Vehicle->Rigs)
            if (Rig && Rig->GetSkeletalMeshAsset() == Mesh)
                for (int32 Slot = 0; Slot < Copy->GetMaterials().Num(); ++Slot)
                    if (UMaterialInterface* Override = Rig->OverrideMaterials.IsValidIndex(Slot) ? Rig->OverrideMaterials[Slot].Get() : nullptr;
                        Override && !Assigned.Contains(Slot))
                    {
                        Copy->GetMaterials()[Slot].MaterialInterface = K.Constant(Override);
                        Assigned.Add(Slot);
                    }
        K.Move(Skeleton, TEXT("Rigs"), Mesh->GetSkeleton()->GetName());
        K.Move(Copy, TEXT("Rigs"), Mesh->GetName());
        KeptRigs.Add(Mesh, Copy);
    }
    if (const USCClipCatalog* Catalog = Vehicle->ClipCatalog)
    {
        USCClipCatalog* Kept = DuplicateObject(Catalog, GetTransientPackage());
        for (FSCImportedClip& Clip : Kept->Clips)
        {
            Clip.Sequence = nullptr;
            for (FSCClipBinding& Binding : Clip.Bindings)
            {
                Binding.Sequences.Reset();
                USkeletalMesh* const* Mesh = KeptRigs.Find(Binding.Mesh.Get());
                Binding.Mesh = Mesh ? *Mesh : nullptr;
            }
        }
        K.Move(Kept, FString(), TEXT("Clips"));
    }

    int32 Saved = 0;
    for (UPackage* Package : K.Packages)
    {
        UObject* Asset = Package->FindAssetInPackage();
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (UPackage::SavePackage(Package, Asset, *File, Args))
            ++Saved;
        else
            K.Warnings.Add(TEXT("save failed: ") + Package->GetName());
    }
    for (const FString& W : K.Warnings)
        UE_LOG(LogSCKeep, Warning, TEXT("KEEP_WARNING %s"), *W);
    UE_LOG(LogSCKeep, Display, TEXT("KEEP root=%s packages=%d saved=%d warnings=%d seconds=%.3f"), *K.Root, K.Packages.Num(), Saved,
           K.Warnings.Num(), FPlatformTime::Seconds() - Start);
    Result.Root = K.Root;
    Result.Saved = Saved;
    Result.bOK = Saved == K.Packages.Num() && Saved > 0 && !K.bFailed;
    Result.Message = FString::Printf(TEXT("Kept %d assets under %s%s"), Saved, *K.Root,
                                     K.Warnings.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%d warnings, see log)"), K.Warnings.Num()));
    return Result;
}
