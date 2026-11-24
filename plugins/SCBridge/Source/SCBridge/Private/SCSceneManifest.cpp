// WP-O1-4c-3a: the scene manifest's methods (SCSceneManifest.h), split from SCPackageReader.cpp.
#include "SCSceneManifest.h"
bool FSCSceneManifest::PaintTarget(const FSCSceneInstance& I, int32 Variant, FString& OutSidecar, FString& OutPalette) const
{
    if (!Paints.IsValidIndex(Variant)) return false;
    const FSCPaintVariant& Paint = Paints[Variant];
    const bool bExterior = !Root.MaterialSidecar.IsEmpty() && I.MaterialSidecar == Root.MaterialSidecar && !Paint.ExteriorSidecar.IsEmpty();
    const bool bPalette = !Root.PaletteId.IsEmpty() && I.PaletteId == Root.PaletteId && !Paint.PaletteId.IsEmpty();
    if (!bExterior && !bPalette) return false;
    OutSidecar = bExterior ? Paint.ExteriorSidecar : I.MaterialSidecar;
    OutPalette = bPalette ? Paint.PaletteId : I.PaletteId;
    return true;
}
void FSCSceneInstance::PanelImages(TSet<FString>& Out, int32* Excluded, int32* Unclassified) const
{
    for (const FSCUiBinding& B : UiBindings)
    {
        if (B.ImagePath.IsEmpty()) continue;
        if (!B.bClassified || !B.bOwnerResolved) { if (Unclassified) ++*Unclassified; continue; } // unknown owner: dark
        if (!B.IsOperatingPanel()) { if (Excluded) ++*Excluded; continue; }
        if (!B.PanelImage().IsEmpty()) Out.Add(B.PanelImage());
        for (const FSCUiStateImage& S : B.PanelStateImages()) Out.Add(S.ImagePath);
        for (const FSCUiHoverImage& H : B.PanelHoverImages()) Out.Add(H.ImagePath);
    }
}
FString FSCSceneInstance::ScreenSlotImage(const TSet<FString>& MeshNodes, const TArray<TArray<FString>>& Owners, bool* bAmbiguous) const
{
    if (bAmbiguous) *bAmbiguous = false;
    // Bindings placed on a node of this mesh (helper_name), and the entity's own (no helper, or a helper of another
    // asset, such as a loadout item's attachment helper on its parent).
    // A binding propagated from another entity whose helper is not a node here has no face on this mesh: it claims
    // nothing (it is never spread over the entity's other faces).
    TMultiMap<FString, const FSCUiBinding*> AtHelper;
    TArray<const FSCUiBinding*> Own;
    for (const FSCUiBinding& B : UiBindings)
    {
        const FString Helper = B.HelperName.ToLower();
        if (!Helper.IsEmpty() && MeshNodes.Contains(Helper)) AtHelper.Add(Helper, &B);
        else if (!B.bPropagated) Own.Add(&B);
    }
    TSet<FString> Outcomes; // "" = a dark face
    for (const TArray<FString>& Chain : Owners)
    {
        TArray<const FSCUiBinding*> Claim;
        for (const FString& Node : Chain)
        {
            AtHelper.MultiFind(Node, Claim);
            if (!Claim.IsEmpty()) break; // the nearest helper owns the face
        }
        if (Claim.IsEmpty()) Claim = Own;
        if (Claim.IsEmpty()) Outcomes.Add(FString()); // a screen face no binding names
        for (const FSCUiBinding* B : Claim) Outcomes.Add(B->PanelImage());
    }
    if (Outcomes.Num() != 1)
    {
        if (bAmbiguous) *bAmbiguous = Outcomes.Num() > 1;
        return FString();
    }
    return *Outcomes.CreateConstIterator();
}
TArray<FString> FSCSceneManifest::MeshAssets() const
{
    TSet<FString> Paths;
    auto Add = [&](const FSCSceneInstance& I) { if (!I.MeshAsset.IsEmpty()) Paths.Add(I.MeshAsset); };
    Add(Root); for (const auto& I : Children) Add(I);
    for (const auto& I : Interiors) for (const auto& P : I.Placements) Add(P);
    TArray<FString> Result = Paths.Array(); Result.Sort(); return Result;
}
