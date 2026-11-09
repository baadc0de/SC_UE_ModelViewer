// Hierarchy panel: the loaded vehicle's component tree with kind / Nanite / triangle badges; selecting a row
// focuses the orbit camera on it, the isolate toggle hides everything outside the selected subtree.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class USCViewerSubsystem;
class ASCVehicle;

class SSCHierarchyPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCHierarchyPanel) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual ~SSCHierarchyPanel() override;
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;

    struct FNode
    {
        TWeakObjectPtr<USceneComponent> Component;
        TWeakPtr<FNode> Parent;
        FString Label;
        enum class EKind : uint8 { Group, Static, Skeletal, Light } Kind = EKind::Group;
        bool bNanite = false;
        int64 Triangles = 0; // own triangles for drawables, subtree sum for groups
        int32 Drawables = 0;
        TArray<TSharedPtr<FNode>> Children;
    };
    using FItem = TSharedPtr<FNode>;

private:
    void Rebuild(ASCVehicle* Vehicle);
    FItem BuildNode(USceneComponent* Component, const FItem& Parent, const TMap<const ULightComponent*, FString>& LightNames);
    TSharedRef<ITableRow> MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner);
    void OnSelection(FItem Item, ESelectInfo::Type Type);
    void OnFocused(USceneComponent* Component);
    FItem Find(const FItem& From, const USceneComponent* Component) const;

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TWeakObjectPtr<ASCVehicle> BuiltFor;
    TArray<FItem> Roots;
    TSharedPtr<STreeView<FItem>> Tree;
    bool bIsolate = false, bSyncingSelection = false;
    int64 TotalTriangles = 0;
    int32 TotalComponents = 0, TotalNanite = 0, TotalLights = 0;
    FDelegateHandle FocusHandle;
};
