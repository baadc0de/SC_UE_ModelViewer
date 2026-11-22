#include "SCProcessedMeshCache.h"

#include "SCMeshImporter.h"
#include "SCProcessedCache.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/NaniteResources.h"
#include "RHIShaderPlatform.h"
#include "DynamicRHI.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/DevObjectVersion.h"
#if WITH_EDITOR
#include "../SCSpikePackageWriter.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "UObject/ArchiveCookContext.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
constexpr int32 RecordVersion = 1;
constexpr const TCHAR* AssetName = TEXT("StaticMesh");
constexpr const TCHAR* DataName = TEXT("mesh.bin");

struct FRecord
{
    FSCMeshBuildStats Stats;
    FCustomVersionContainer Versions;
    TArray<uint8> Collision;

    bool Serialize(FArchive& Ar)
    {
        int32 Format = RecordVersion;
        Ar << Format;
        if (Format != RecordVersion) return false;
        Ar << Stats.bNaniteEnabled << Stats.NaniteReason << Stats.Triangles << Stats.FallbackTriangles
           << Stats.CollisionTriangles << Stats.Clusters << Stats.Cards << Stats.bMeshCards
           << Stats.bDistanceField << Stats.bRayTracingGeometry << Stats.bCollision;
        Versions.Serialize(Ar);
        Ar << Collision;
        return !Ar.IsError() && Stats.bCollision == !Collision.IsEmpty();
    }
};

#if WITH_EDITOR
bool CookMesh(UStaticMesh& Mesh, const FString& PackageName, const FString& Filename)
{
    const FString PlatformName(ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
    const ITargetPlatform* Platform = GetTargetPlatformManagerRef().FindTargetPlatform(*PlatformName);
    if (!Platform || !Mesh.GetBodySetup()) return false;

    UObject* OriginalOuter = Mesh.GetOuter();
    const FString OriginalName = Mesh.GetName();
    const EObjectFlags OriginalFlags = Mesh.GetFlags() & (RF_Transient | RF_Public | RF_Standalone);
    const EObjectFlags OriginalBodyFlags = Mesh.GetBodySetup()->GetFlags() & RF_Transient;
    UBodySetup& Body = *Mesh.GetBodySetup();
    const bool bOriginalCookedData = Body.bHasCookedCollisionData;
    const bool bOriginalNeverCook = Body.bNeverNeedsCookedCollisionData;
    UPackage* Package = CreatePackage(*PackageName);
    constexpr ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
    if (!Mesh.Rename(AssetName, Package, RenameFlags)) return false;
    Mesh.ClearFlags(RF_Transient);
    Mesh.SetFlags(RF_Public | RF_Standalone);
    Body.ClearFlags(RF_Transient);
    // Saving the render variant as-is would recook collision from its (possibly
    // split) render triangles. The record carries the original Chaos bytes.
    Body.bNeverNeedsCookedCollisionData = true;
    ON_SCOPE_EXIT
    {
        Body.bHasCookedCollisionData = bOriginalCookedData;
        Body.bNeverNeedsCookedCollisionData = bOriginalNeverCook;
        Body.SetFlags(OriginalBodyFlags);
        Mesh.ClearFlags(RF_Transient | RF_Public | RF_Standalone);
        Mesh.SetFlags(OriginalFlags);
        Mesh.Rename(*OriginalName, OriginalOuter, RenameFlags);
    };

    FArchiveCookContext CookContext(Package, UE::Cook::ECookType::ByTheBook, UE::Cook::ECookingDLC::No, Platform, nullptr);
    FArchiveCookData CookData(*Platform, CookContext);
    auto* Writer = new FSCSpikePackageWriter;
    FSavePackageContext SaveContext(Platform, Writer); // owns Writer
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    Args.bSlowTask = false;
    Args.ArchiveCookData = &CookData;
    Args.SavePackageContext = &SaveContext;
    Package->SetPackageFlags(PKG_FilterEditorOnly);
    IPackageWriter::FBeginPackageInfo Begin;
    Begin.PackageName = Package->GetFName();
    Begin.LooseFilePath = Filename;
    Writer->BeginPackage(Begin);
    const bool bSaved = UPackage::SavePackage(Package, &Mesh, *Filename, Args);
    IPackageWriter::FCommitPackageInfo Commit;
    Commit.PackageName = Package->GetFName();
    Commit.Status = bSaved ? IPackageWriter::ECommitStatus::Success : IPackageWriter::ECommitStatus::Error;
    Commit.WriteOptions = IPackageWriter::EWriteOptions::Write;
    Writer->CommitPackage(MoveTemp(Commit));
    return bSaved && Writer->bWritten;
}
#endif
}

FString SCProcessedMeshCache::RenderFormat()
{
    auto Setting = [](const TCHAR* Name) {
        const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
        return Var ? Var->GetString() : FString();
    };
    const auto& MeshVersion = FDevSystemGuids::GetSystemGuid(FDevSystemGuids::Get().STATICMESH_DERIVEDDATA_VER);
    return FString::Printf(TEXT("mesh:%d|derive:%u|%s|%s|ddc=%s|rhi=%d,%d|render=%d|nanite=%s|df=%s,%s,%s|rt=%s"),
        RecordVersion, FSCMeshImporter::DerivedFormatVersion, *FEngineVersion::Current().ToString(),
        ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), *MeshVersion.ToString(), int32(RHIGetInterfaceType()),
        int32(GMaxRHIShaderPlatform), FApp::CanEverRender(),
        *Setting(TEXT("r.Nanite")), *Setting(TEXT("r.GenerateMeshDistanceFields")),
        *Setting(TEXT("r.DistanceFields.DefaultVoxelDensity")), *Setting(TEXT("r.DistanceFields.MaxPerMeshResolution")),
        *Setting(TEXT("r.RayTracing")));
}

bool SCProcessedMeshCache::Store(FSCProcessedCache& Cache, const FString& Key, UStaticMesh& Mesh, const FSCDerivedMesh& Data,
                                 const FSCMeshBuildStats& Stats)
{
#if WITH_EDITOR
    if (Stats.Triangles <= 0 || Key.IsEmpty()) return false;
    const FString Directory = Cache.BeginWrite();
    if (Directory.IsEmpty()) return false;
    ON_SCOPE_EXIT { Cache.AbortWrite(Directory); };
    TArray<uint8> Bytes;
    FMemoryWriter Ar(Bytes, true);
    FRecord Record{Stats, Data.CollisionVersions, Data.CollisionBytes};
    return Record.Serialize(Ar) && FFileHelper::SaveArrayToFile(Bytes, *(Directory / DataName)) &&
        CookMesh(Mesh, Cache.PackageName(Directory, AssetName), *(Directory / TEXT("StaticMesh.uasset"))) &&
        Cache.CommitWrite(Key, Directory);
#else
    return false;
#endif
}

UStaticMesh* SCProcessedMeshCache::Load(FSCProcessedCache& Cache, const FSCProcessedCache::FReadPin& Pin, FSCMeshBuildStats& Stats)
{
    const FString& Directory = Pin.Directory;
    bool bUsable = false;
    ON_SCOPE_EXIT { if (!bUsable) Pin.Invalidate(); };
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *(Directory / DataName))) return nullptr;
    FMemoryReader Ar(Bytes, true);
    FRecord Record;
    if (!Record.Serialize(Ar) || Ar.Tell() != Ar.TotalSize()) return nullptr;
    if (UStaticMesh* Resident = Cast<UStaticMesh>(Cache.GetLoaded(Directory)))
    {
        Stats = MoveTemp(Record.Stats);
        bUsable = true;
        return Resident;
    }

    const FString Path = Cache.PackageName(Directory, AssetName) + TEXT(".") + AssetName;
    auto* Mesh = LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
    if (!Mesh) return nullptr;
    Mesh->ClearFlags(RF_Standalone); // GC when the last vehicle releases it.
    Cache.KeepLoaded(Directory, Mesh, false); // failed validation still lets UE finish its reads
    if (!Mesh->GetPackage()->HasAnyPackageFlags(PKG_Cooked)) return nullptr;
    const FStaticMeshRenderData* Render = Mesh->GetRenderData();
    if (!Render || Render->LODResources.IsEmpty()) return nullptr;
    const auto& Lod = Render->LODResources[0];
    if ((Record.Stats.Clusters && !Render->HasValidNaniteData()) ||
        (Record.Stats.bDistanceField && !Lod.DistanceFieldData) ||
        (Record.Stats.bRayTracingGeometry && !Lod.RayTracingGeometry &&
            (!Render->RayTracingProxy || Render->RayTracingProxy->LODs.IsEmpty() || !Render->RayTracingProxy->LODs[0].RayTracingGeometry)))
        return nullptr;
#if WITH_EDITOR
    if (Record.Stats.bMeshCards && (!Lod.CardRepresentationData ||
        Record.Stats.Cards != Lod.CardRepresentationData->MeshCardsBuildData.CardBuildData.Num())) return nullptr;
#endif
    if (Record.Stats.bCollision)
    {
        FString Error;
        int32 Triangles = 0;
        if (!FSCMeshImporter::InstallCollision(Mesh->GetBodySetup(), Record.Collision, Record.Versions, Triangles, Error) ||
            Triangles != Record.Stats.CollisionTriangles) return nullptr;
    }
    Stats = MoveTemp(Record.Stats);
    Cache.KeepLoaded(Directory, Mesh);
    bUsable = true;
    return Mesh;
}
