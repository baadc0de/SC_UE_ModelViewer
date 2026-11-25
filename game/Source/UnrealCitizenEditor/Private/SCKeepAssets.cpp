// SCKeepAssets: rename transient import objects into /Game packages (references between them stay intact),
// convert dynamic material instances to constants, bake mesh slot materials from component overrides, save.
#include "SCKeepAssets.h"
#include "SCMeshes.h"
#include "SCRigAssets.h"
#include "SCRigComponent.h"
#include "SCTextureSource.h"
#include "SCVehicle.h"
#include "SCClipCatalog.h"
#include "SCAnimImporter.h"
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
    /** Runtime textures carry only platform mips, which an editor package does not save: an editable source
     *  (SCTextureSource), or the Keep fails (a saved texture without one has no pixels). */
    void AddTextureSource(UTexture2D* Texture)
    {
        FString Error;
        if (SCTextureSource::Add(Texture, Error)) return;
        Warnings.Add(Texture->GetName() + TEXT(": ") + Error + TEXT("; saved without source"));
        bFailed = true;
    }
    /** Constant copy of a dynamic instance: same parent, every overridden parameter. */
    UMaterialInterface* Constant(UMaterialInterface* Material)
    {
        auto* MID = Cast<UMaterialInstanceDynamic>(Material);
        if (!MID) return Material;
        if (UObject** Found = Kept.Find(MID)) return Cast<UMaterialInterface>(*Found);
        const FString Base = SafeName(TEXT("MI_") + MID->GetName());
        FString Name = Base;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)) || FPackageName::DoesPackageExist(Root / TEXT("Materials") / Name); ++Suffix)
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
    // Rigs (WP-O1-4b; the WP-U3 follow-up): each rig mesh is kept as a copy with its skeleton (SCRigAssets::SavableCopy:
    // a runtime mesh made from the rig source the import kept, a prepared one rebuilt from its own source model), its
    // slots taking the rig components' current materials. The live meshes, skeletons and rig bodies stay as they are;
    // the kept clip catalog is a copy bound to the kept meshes, its sequences copies on the kept skeletons.
    TMap<const USkeletalMesh*, USkeletalMesh*> KeptRigs;
    TMap<const USkeleton*, USkeleton*> KeptSkeletons;
    for (USkeletalMesh* Mesh : Vehicle->SkeletalMeshes)
    {
        if (!Mesh) continue;
        const TSharedPtr<const FSCDerivedMesh>* Source = Vehicle->RigSources.Find(Mesh);
        FString Error;
        USkeletalMesh* Copy = SCRigAssets::SavableCopy(Mesh, Source ? Source->Get() : nullptr, Error);
        if (!Copy)
        {
            K.Warnings.Add(Mesh->GetName() + TEXT(": rig mesh not kept (") + Error + TEXT(")"));
            K.bFailed = true;
            continue;
        }
        USkeleton* Skeleton = Copy->GetSkeleton();
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
        KeptSkeletons.Add(Mesh->GetSkeleton(), Skeleton);
    }
    if (const USCClipCatalog* Catalog = Vehicle->ClipCatalog)
    {
        USCClipCatalog* Kept = DuplicateObject(Catalog, GetTransientPackage());
        // The vehicle's root rig mesh (USCClipCatalog::RootMesh, placed or not), kept: the skeleton of its clips without
        // receiving bones.
        USkeletalMesh* const* KeptRootEntry = KeptRigs.Find(Catalog->RootMesh.Get());
        USkeletalMesh* KeptRoot = KeptRootEntry ? *KeptRootEntry : nullptr;
        Kept->RootMesh = KeptRoot;
        // The editable sequences (one per binding timing, and the clip's primary): each a copy on the kept skeleton of its
        // own, saved under Animations. A skeleton no kept rig mesh carries (a metadata-only sequence's own: a clip without
        // receiving bones) is kept as a copy on its first use.
        TMap<const UAnimSequence*, UAnimSequence*> KeptSequences;
        auto KeepSequence = [&](UAnimSequence* Sequence, const FString& ClipName) -> UAnimSequence* {
            if (!Sequence) return nullptr;
            if (UAnimSequence** Found = KeptSequences.Find(Sequence)) return *Found;
            USkeleton* Skeleton = nullptr;
            if (USkeleton** Kept = KeptSkeletons.Find(Sequence->GetSkeleton())) Skeleton = *Kept;
            else if (Sequence->GetSkeleton())
            {
                Skeleton = DuplicateObject(Sequence->GetSkeleton(), GetTransientPackage());
                K.Move(Skeleton, TEXT("Rigs"), Sequence->GetSkeleton()->GetName());
                KeptSkeletons.Add(Sequence->GetSkeleton(), Skeleton);
            }
            UAnimSequence* Copy = nullptr;
            if (Skeleton)
            {
                Copy = DuplicateObject(Sequence, GetTransientPackage());
                Copy->SetSkeleton(Skeleton);
                K.Move(Copy, TEXT("Animations"), ClipName);
            }
            else
            {
                K.Warnings.Add(Sequence->GetName() + TEXT(": sequence not kept (no skeleton)"));
                K.bFailed = true;
            }
            KeptSequences.Add(Sequence, Copy);
            return Copy;
        };
        for (FSCImportedClip& Clip : Kept->Clips)
        {
            const FString ClipName = FPaths::GetBaseFilename(Clip.Name);
            Clip.Sequence = KeepSequence(Clip.Sequence, ClipName);
            for (FSCClipBinding& Binding : Clip.Bindings)
            {
                for (TObjectPtr<UAnimSequence>& Sequence : Binding.Sequences) Sequence = KeepSequence(Sequence, ClipName);
                USkeletalMesh* const* Mesh = KeptRigs.Find(Binding.Mesh.Get());
                Binding.Mesh = Mesh ? *Mesh : nullptr;
                // A prepared vehicle's bindings keep only their bone tracks (prep drops the editor sequences): built here,
                // on the kept mesh's skeleton, from the tracks playback reads.
                if (Binding.Sequences.IsEmpty() && !Binding.Tracks.IsEmpty() && Binding.Mesh)
                {
                    FSCAnimImporter::BuildSequences(Binding.Mesh, GetTransientPackage(), Binding);
                    for (const TObjectPtr<UAnimSequence>& Sequence : Binding.Sequences) K.Move(Sequence, TEXT("Animations"), ClipName);
                }
            }
            // The clip's primary sequence: its first binding's, or (no binding: a clip without receiving bones) a
            // metadata-only one of its rate and length, as the import makes it: on the root rig's kept skeleton (the
            // vehicle's own rig), a skeleton of its own only for a vehicle without one.
            if (!Clip.Sequence)
            {
                FSCAnimImporter::Finalize(KeptRoot, GetTransientPackage(), Clip);
                if (Clip.Bindings.IsEmpty() && Clip.Sequence)
                {
                    // On the kept root rig's skeleton (saved with it), else on a skeleton of its own (Finalize's, when the
                    // vehicle has no root rig or it has no bones): kept with the sequence.
                    if (KeptRoot && Clip.Sequence->GetSkeleton() == KeptRoot->GetSkeleton()) K.Move(Clip.Sequence, TEXT("Animations"), ClipName);
                    else Clip.Sequence = KeepSequence(Clip.Sequence, ClipName);
                }
            }
            if (!Clip.Sequence)
            {
                K.Warnings.Add(Clip.Name + TEXT(": no sequence kept"));
                K.bFailed = true;
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
