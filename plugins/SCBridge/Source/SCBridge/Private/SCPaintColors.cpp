// Custom paint colours (ASCVehicle::SetPaintColors): a user's overrides of the active paint's palette channels, applied
// live. Each paint slot's material is described again (FSCMaterialBuilder::Describe) with the paint's palette and with
// the edited one; the parameters that differ (and a host-tinted material's HostTint) are set on an instance of the slot's
// paint material. Nothing is re-imported; the paint's own materials stay as they are for the other paints.
#include "SCVehicle.h"
#include "SCMaterialBuilder.h"
#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCPaintColors, Log, All);

namespace
{
TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
    TSharedPtr<FJsonObject> O;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), O);
    return O;
}
/** The JSON object and key a channel lives at in a palettes.json entry ("decal_red" is decal.red). */
TSharedPtr<FJsonObject> ChannelHolder(const TSharedPtr<FJsonObject>& Palette, const FString& Channel, FString& Key, bool bCreate)
{
    if (!Palette) return nullptr;
    FString Group, Leaf;
    if (!Channel.Split(TEXT("_"), &Group, &Leaf) || Group != TEXT("decal"))
    {
        Key = Channel;
        return Palette;
    }
    Key = Leaf;
    const TSharedPtr<FJsonObject>* Decal;
    if (Palette->TryGetObjectField(TEXT("decal"), Decal) && Decal->IsValid()) return *Decal;
    if (!bCreate) return nullptr;
    TSharedPtr<FJsonObject> Made = MakeShared<FJsonObject>();
    Palette->SetObjectField(TEXT("decal"), Made);
    return Made;
}
bool ReadChannel(const TSharedPtr<FJsonObject>& Palette, const FString& Channel, FLinearColor& Out)
{
    FString Key;
    const TSharedPtr<FJsonObject> Holder = ChannelHolder(Palette, Channel, Key, false);
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!Holder || !Holder->TryGetArrayField(Key, A) || A->Num() < 3) return false;
    Out = FLinearColor(float((*A)[0]->AsNumber()), float((*A)[1]->AsNumber()), float((*A)[2]->AsNumber()));
    return true;
}
} // namespace

const TArray<FString>& ASCVehicle::PaintChannels()
{
    static const TArray<FString> Channels = {TEXT("primary"), TEXT("secondary"), TEXT("tertiary"), TEXT("glass"),
                                             TEXT("decal_red"), TEXT("decal_green"), TEXT("decal_blue")};
    return Channels;
}

const FSCPalette* ASCVehicle::PaintPalette(int32 Paint) const
{
    const FString Id = Paint == INDEX_NONE ? Manifest.Root.PaletteId : Manifest.Paints.IsValidIndex(Paint) ? Manifest.Paints[Paint].PaletteId : FString();
    return Id.IsEmpty() ? nullptr : Manifest.Palettes.FindByPredicate([&](const FSCPalette& P) { return P.Id == Id; });
}

TMap<FString, FLinearColor> ASCVehicle::PaintColors(int32 Paint) const
{
    TMap<FString, FLinearColor> Out;
    const FSCPalette* Palette = PaintPalette(Paint);
    const TSharedPtr<FJsonObject> P = Palette ? ParseObject(Palette->Json) : nullptr;
    for (const FString& Channel : PaintChannels())
    {
        FLinearColor C;
        if (ReadChannel(P, Channel, C)) Out.Add(Channel, C);
    }
    return Out;
}

bool ASCVehicle::SetPaintColors(const TMap<FString, FLinearColor>& Overrides)
{
    // Back to the active paint's own materials (an earlier edit's instances dropped).
    for (const FSCPaintSlot& Entry : PaintSlots)
    {
        UMeshComponent* C = Entry.Component.Get();
        if (!C) continue;
        UMaterialInterface* Material = ActivePaint == INDEX_NONE ? Entry.Default.Get()
                                       : Entry.Paints.IsValidIndex(ActivePaint) && Entry.Paints[ActivePaint].IsValid() ? Entry.Paints[ActivePaint].Get()
                                                                                                                       : Entry.Default.Get();
        if (Material && C->GetMaterial(Entry.Slot) != Material) C->SetMaterial(Entry.Slot, Material);
    }
    CustomPaintMaterials.Reset();
    PaintColorOverrides = Overrides;
    if (Overrides.IsEmpty()) return true;

    // The edited copy of each palette the active paint's slots use.
    TMap<FString, FSCPalette> Edited;
    auto EditedPalette = [&](const FString& Id) -> const FSCPalette* {
        if (const FSCPalette* Done = Edited.Find(Id)) return Done;
        const FSCPalette* Base = Manifest.Palettes.FindByPredicate([&](const FSCPalette& P) { return P.Id == Id; });
        const TSharedPtr<FJsonObject> P = Base ? ParseObject(Base->Json) : nullptr;
        if (!P) return nullptr;
        for (const auto& [Channel, Color] : Overrides)
        {
            FString Key;
            if (const TSharedPtr<FJsonObject> Holder = ChannelHolder(P, Channel, Key, true))
                Holder->SetArrayField(Key, {MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G), MakeShared<FJsonValueNumber>(Color.B)});
        }
        FSCPalette& Copy = Edited.Add(Id);
        Copy.Id = Id + TEXT("#custom");
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Copy.Json);
        FJsonSerializer::Serialize(P.ToSharedRef(), Writer);
        return &Copy;
    };
    // What the edit changes on one submaterial (described once per sidecar, ordinal, palette and host channel).
    struct FChange
    {
        TMap<FName, FLinearColor> Vectors;
        TMap<FName, float> Scalars;
        bool IsEmpty() const { return Vectors.IsEmpty() && Scalars.IsEmpty(); }
    };
    TMap<FString, FChange> Changes;
    auto ChangeOf = [&](const FSCPaintSource& Source) -> const FChange* {
        const FString Key = FString::Printf(TEXT("%s|%d|%s|%s"), *Source.Sidecar, Source.Ordinal, *Source.PaletteId, *Source.HostChannel);
        if (const FChange* Done = Changes.Find(Key)) return Done;
        FChange& Change = Changes.Add(Key);
        const FSCMaterialSet* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Source.Sidecar; });
        const FSCSubmaterial* Sub = Set ? Set->Submaterials.FindByPredicate([&](const FSCSubmaterial& S) { return S.Index == Source.Ordinal; }) : nullptr;
        const FSCPalette* Base = Manifest.Palettes.FindByPredicate([&](const FSCPalette& P) { return P.Id == Source.PaletteId; });
        const FSCPalette* Custom = EditedPalette(Source.PaletteId);
        if (!Sub || !Base || !Custom) return &Change;
        const FSCMaterialDesc Before = FSCMaterialBuilder::Describe(*Sub, Base), After = FSCMaterialBuilder::Describe(*Sub, Custom);
        for (const auto& [Name, Value] : After.Vectors)
            if (const FLinearColor* Old = Before.Vectors.Find(Name); !Old || !Old->Equals(Value)) Change.Vectors.Add(Name, Value);
        for (const auto& [Name, Value] : After.Scalars)
            if (const float* Old = Before.Scalars.Find(Name); !Old || !FMath::IsNearlyEqual(*Old, Value)) Change.Scalars.Add(Name, Value);
        // A host-tinted material (FSCImportMaterials::Find) takes its host channel's colour.
        FLinearColor Host;
        if (Before.bHostTinted && !Source.HostChannel.IsEmpty() && FSCMaterialBuilder::PaletteColor(Custom, Source.HostChannel, Host))
            Change.Vectors.Add(TEXT("HostTint"), Host);
        if (!Change.IsEmpty())
        {
            TArray<FString> Names;
            for (const auto& [Name, Value] : Change.Vectors) Names.Add(Name.ToString());
            for (const auto& [Name, Value] : Change.Scalars) Names.Add(Name.ToString());
            UE_LOG(LogSCPaintColors, Verbose, TEXT("PAINT_COLORS_CHANGE %s master=%d params=%s"), *Key, int32(After.Master), *FString::Join(Names, TEXT(",")));
        }
        return &Change;
    };

    // One instance per (paint material, change): slots that share a material share the edited instance.
    TMap<TPair<UMaterialInterface*, const FChange*>, UMaterialInstanceDynamic*> Instances;
    int32 Slots = 0;
    for (const FSCPaintSlot& Entry : PaintSlots)
    {
        UMeshComponent* C = Entry.Component.Get();
        const bool bPaint = ActivePaint != INDEX_NONE && Entry.Sources.IsValidIndex(ActivePaint) && Entry.Sources[ActivePaint].IsValid();
        const FSCPaintSource& Source = bPaint ? Entry.Sources[ActivePaint] : Entry.DefaultSource;
        UMaterialInterface* Material = C ? C->GetMaterial(Entry.Slot) : nullptr;
        if (!Material || !Source.IsValid()) continue;
        const FChange* Change = ChangeOf(Source);
        if (!Change || Change->IsEmpty()) continue;
        UMaterialInstanceDynamic*& MID = Instances.FindOrAdd({Material, Change});
        if (!MID)
        {
            // A copy of the paint's instance (its parent and parameter overrides), not an instance of the instance: the
            // renderer reads a dynamic instance's parameters from itself and its parent material only.
            UMaterialInstanceDynamic* PaintMID = Cast<UMaterialInstanceDynamic>(Material);
            MID = UMaterialInstanceDynamic::Create(PaintMID && PaintMID->Parent ? PaintMID->Parent.Get() : Material, this);
            if (PaintMID && PaintMID->Parent) MID->CopyParameterOverrides(PaintMID);
            for (const auto& [Name, Value] : Change->Vectors) MID->SetVectorParameterValue(Name, Value);
            for (const auto& [Name, Value] : Change->Scalars) MID->SetScalarParameterValue(Name, Value);
            CustomPaintMaterials.Add(MID);
        }
        C->SetMaterial(Entry.Slot, MID);
        ++Slots;
    }
    UE_LOG(LogSCPaintColors, Display, TEXT("PAINT_COLORS paint=%d channels=%d slots=%d instances=%d"), ActivePaint, Overrides.Num(), Slots, Instances.Num());
    return true;
}
