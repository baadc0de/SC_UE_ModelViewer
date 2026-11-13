// USCHangarIndex: cooked hangars' ship capacity (see SCHangarIndex.h).
#include "SCHangarIndex.h"

bool FSCHangarCapacity::Holds(const FBox& Ship) const
{
    if (!Ship.IsValid || BayCm.X <= 0 || BayCm.Y <= 0) return false;
    const FVector2D Half = BayCm / 2;
    const bool bFootprint = Ship.Min.X >= -Half.X && Ship.Max.X <= Half.X && Ship.Min.Y >= -Half.Y && Ship.Max.Y <= Half.Y;
    return bFootprint && (ClearanceCm < 0 || Ship.Max.Z <= ClearanceCm);
}

USCHangarIndex* USCHangarIndex::Load()
{
    return LoadObject<USCHangarIndex>(nullptr, AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

const FSCHangarCapacity* USCHangarIndex::Find(const FString& Style, const FString& Name) const
{
    return Hangars.FindByPredicate([&](const FSCHangarCapacity& H) { return H.Style == Style && H.Name == Name; });
}

FSCHangarCapacity& USCHangarIndex::FindOrAdd(const FString& Style, const FString& Name)
{
    if (FSCHangarCapacity* Found = Hangars.FindByPredicate([&](const FSCHangarCapacity& H) { return H.Style == Style && H.Name == Name; }))
        return *Found;
    FSCHangarCapacity& Added = Hangars.AddDefaulted_GetRef();
    Added.Style = Style;
    Added.Name = Name;
    return Added;
}
