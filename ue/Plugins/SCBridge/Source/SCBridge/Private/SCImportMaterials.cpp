// FSCImportMaterials: material phase of FSCImporter (describe -> decode textures -> textures + MIDs).
#include "SCImportMaterials.h"
#include "SCImporter.h"
#include "SCVehicle.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"

const FSCSubmaterial* FSCImportMaterials::Resolve(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, int32 Slot)
{
    if (!Set) return nullptr;
    const FSCSubmaterial* Sub = Set->Submaterials.IsValidIndex(Slot) ? &Set->Submaterials[Slot] : nullptr;
    const FString SourceName = SourceNames.IsValidIndex(Slot) ? SourceNames[Slot] : FString();
    if (!SourceName.IsEmpty() && (!Sub || Sub->Name != SourceName))
        if (const auto* Named = Set->Submaterials.FindByPredicate([&](const FSCSubmaterial& M) { return M.Name == SourceName; })) Sub = Named;
    return Sub;
}

void FSCImportMaterials::Describe(const FSCSceneManifest& Manifest)
{
    const double Start = FPlatformTime::Seconds();
    TSet<TPair<FString, FString>> Pairs;
    auto Collect = [&](const FSCSceneInstance& I) { if (!I.MaterialSidecar.IsEmpty()) Pairs.Add({I.MaterialSidecar, I.PaletteId}); };
    Collect(Manifest.Root); for (const auto& I : Manifest.Children) Collect(I);
    for (const auto& Interior : Manifest.Interiors) for (const auto& P : Interior.Placements) Collect(P);
    TSet<FSCTextureKey> Unique;
    for (const auto& Pair : Pairs)
    {
        const FSCMaterialSet* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Pair.Key; });
        if (!Set) continue;
        const FSCPalette* Palette = Manifest.Palettes.FindByPredicate([&](const FSCPalette& P) { return P.Id == Pair.Value; });
        FSet& Out = Sets.AddDefaulted_GetRef(); Out.Sidecar = Pair.Key; Out.PaletteId = Pair.Value; Out.Palette = Palette;
        for (const FSCSubmaterial& Sub : Set->Submaterials)
        {
            FSCMaterialDesc Desc = FSCMaterialBuilder::Describe(Sub, Palette);
            const bool bGlow = Manifest.EngineGlow.Targets.ContainsByPredicate([&](const FSCEngineGlowTarget& T)
                { return T.MaterialSidecar == Pair.Key && T.SourceMaterialIndex == Sub.SourceIndex; });
            if (bGlow) FSCMaterialBuilder::ApplyEngineGlow(Desc, FSCMaterialBuilder::EngineGlowToEmissive(Manifest.EngineGlow, Manifest.EngineGlow.DefaultStrength));
            for (const auto& Tex : Desc.Textures) { Unique.Add(Tex.Value); SampledKeys.Add(Tex.Value); }
            for (const auto& Mean : Desc.VectorMeans) { Unique.Add(Mean.Value); MeanKeys.Add(Mean.Value); }
            Out.Descs.Add(MoveTemp(Desc)); Out.EngineGlow.Add(bGlow);
        }
    }
    Keys = Unique.Array();
    Keys.Sort([](const FSCTextureKey& A, const FSCTextureKey& B) { return A.Path == B.Path ? A.Role < B.Role : A.Path < B.Path; });
    SetCount = Sets.Num();
    DescribeSeconds = FPlatformTime::Seconds() - Start;
}

void FSCImportMaterials::Decode(const FSCPackageSource& Source)
{
    const double Start = FPlatformTime::Seconds();
    Decoded = FSCTextureImporter::DecodeAll(Source, Keys);
    TextureStats.Requested = Keys.Num();
    TArray<FLinearColor> Mean; Mean.SetNum(Decoded.Num());
    TArray<bool> HasMean; HasMean.Init(false, Decoded.Num());
    ParallelFor(Decoded.Num(), [&](int32 I)
    {
        if (!MeanKeys.Contains(Decoded[I].Key)) return;
        HasMean[I] = FSCTextureImporter::MeanColor(Decoded[I], Mean[I]);
        if (!SampledKeys.Contains(Decoded[I].Key)) { Decoded[I].Mips.Empty(); Decoded[I].Dds.Empty(); } // mean only: never a texture
    });
    TMap<FSCTextureKey, FLinearColor> Means;
    for (int32 I = 0; I < Decoded.Num(); ++I) if (HasMean[I]) Means.Add(Decoded[I].Key, Mean[I]);
    for (FSet& Set : Sets) for (FSCMaterialDesc& Desc : Set.Descs) FSCMaterialBuilder::ApplyTextureMeans(Desc, Means);
    TextureStats.DecodeSeconds = FPlatformTime::Seconds() - Start;
}

void FSCImportMaterials::Instantiate(ASCVehicle* Vehicle, TArray<FString>& Warnings)
{
    check(IsInGameThread());
    double Start = FPlatformTime::Seconds();
    Owner = Vehicle;
    for (FSCDecodedTexture& D : Decoded)
    {
        if (!D.Error.IsEmpty()) { ++TextureStats.Failed; Warnings.Add(D.Key.Path + TEXT(": ") + D.Error); continue; }
        if (!SampledKeys.Contains(D.Key)) continue; // mean-only: resolved in Decode
        TextureStats.Bytes += D.UploadBytes();
        const FSCTextureKey Key = D.Key;
        if (UTexture2D* T = FSCTextureImporter::Create(MoveTemp(D), Vehicle)) { Textures.Add(Key, T); Vehicle->Textures.Add(T); ++TextureStats.Created; }
    }
    Decoded.Empty();
    TextureStats.CreateSeconds = FPlatformTime::Seconds() - Start;
    Start = FPlatformTime::Seconds();
    for (FSet& Set : Sets)
        for (int32 I = 0; I < Set.Descs.Num(); ++I)
        {
            const FSCMaterialDesc& Desc = Set.Descs[I];
            for (const FString& W : Desc.Warnings) Warnings.Add(FString::Printf(TEXT("%s[%d] %s: %s"), *Set.Sidecar, I, *Desc.Name, *W));
            UMaterialInstanceDynamic* MID = FSCMaterialBuilder::Create(Desc, Textures, Vehicle, Warnings);
            Set.Instances.Add(MID);
            if (!MID) continue;
            Vehicle->Materials.Add(MID); ++MaterialCount; ++MasterCounts.FindOrAdd(Desc.Master);
            if (Set.EngineGlow[I]) { Vehicle->EngineGlowMaterials.Add(MID); ++EngineGlowCount; }
        }
    MaterialSeconds = FPlatformTime::Seconds() - Start;
}

UMaterialInterface* FSCImportMaterials::Find(const FString& Sidecar, const FString& PaletteId, int32 Ordinal, const FString& HostChannel)
{
    FSet* Set = Sets.FindByPredicate([&](const FSet& S) { return S.Sidecar == Sidecar && S.PaletteId == PaletteId; });
    if (!Set || !Set->Instances.IsValidIndex(Ordinal)) return nullptr;
    FLinearColor Host;
    if (!Set->Descs[Ordinal].bHostTinted || !FSCMaterialBuilder::PaletteColor(Set->Palette, HostChannel, Host) || !Owner) return Set->Instances[Ordinal];
    if (UMaterialInterface** Existing = Set->HostVariants.Find({Ordinal, HostChannel})) return *Existing;
    FSCMaterialDesc Desc = Set->Descs[Ordinal];
    Desc.Name += TEXT("__host_") + HostChannel; Desc.Vectors.Add(TEXT("HostTint"), Host);
    TArray<FString> Ignored; // texture warnings were reported for the base instance
    UMaterialInstanceDynamic* MID = FSCMaterialBuilder::Create(Desc, Textures, Owner, Ignored);
    if (MID) { Owner->Materials.Add(MID); ++MaterialCount; }
    Set->HostVariants.Add({Ordinal, HostChannel}, MID);
    return MID ? MID : Set->Instances[Ordinal];
}

UMaterialInterface* FSCImportMaterials::Invisible()
{
    if (!InvisibleMaterial && Owner)
    {
        FSCMaterialDesc Desc; Desc.Name = TEXT("Invisible"); Desc.Master = ESCMaster::EmissiveUnlit; Desc.Blend = ESCBlend::Translucent;
        Desc.bHidden = true; Desc.Scalars.Add(TEXT("EmissiveStrength"), 0.f);
        TArray<FString> Ignored;
        if (UMaterialInstanceDynamic* MID = FSCMaterialBuilder::Create(Desc, Textures, Owner, Ignored)) { Owner->Materials.Add(MID); InvisibleMaterial = MID; }
    }
    return InvisibleMaterial;
}

FString FSCImportMaterials::HostChannel(const FSCMaterialSet* Set, const TArray<FString>& SourceNames, const TArray<int32>& SlotTriangles)
{
    static const TCHAR* Priority[] = {TEXT("primary"), TEXT("secondary"), TEXT("tertiary")};
    TMap<FString, int64> Weight;
    for (int32 Slot = 0; Slot < SlotTriangles.Num(); ++Slot)
        if (const FSCSubmaterial* Sub = Resolve(Set, SourceNames, Slot))
        {
            const FString Channel = FSCMaterialBuilder::HostChannel(*Sub);
            if (!Channel.IsEmpty() && SlotTriangles[Slot] > 0) Weight.FindOrAdd(Channel) += SlotTriangles[Slot]; // empty slots weigh nothing
        }
    FString Best; int64 BestWeight = -1;
    for (const TCHAR* Channel : Priority) if (const int64* W = Weight.Find(Channel); W && *W > BestWeight) { Best = Channel; BestWeight = *W; }
    return Best;
}
