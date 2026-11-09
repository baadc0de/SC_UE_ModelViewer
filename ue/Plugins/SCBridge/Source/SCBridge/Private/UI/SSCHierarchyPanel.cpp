// SSCHierarchyPanel: component tree model, badges, selection <-> camera focus, isolate.
#include "SSCHierarchyPanel.h"
#include "SCViewerStyle.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Misc/Paths.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCHierarchy"
using namespace SCViewerStyle;

namespace
{
int64 StaticTriangles(const UStaticMesh* Mesh)
{
    const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
    return Data && !Data->LODResources.IsEmpty() ? Data->LODResources[0].GetNumTriangles() : 0;
}
int64 SkeletalTriangles(const USkeletalMesh* Mesh)
{
    const FSkeletalMeshRenderData* Data = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    return Data && !Data->LODRenderData.IsEmpty() ? Data->LODRenderData[0].GetTotalFaces() : 0;
}
} // namespace

void SSCHierarchyPanel::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    if (Subsystem.IsValid()) FocusHandle = Subsystem->OnComponentFocused.AddSP(this, &SSCHierarchyPanel::OnFocused);
    ChildSlot
    [
        SNew(SBorder).BorderImage(Fill()).BorderBackgroundColor(Panel()).Padding(10)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)[Header(LOCTEXT("Hierarchy", "HIERARCHY"))]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
            [
                SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Dim()).AutoWrapText(true).Text_Lambda([this] {
                    if (!BuiltFor.IsValid()) return LOCTEXT("Empty", "No model loaded");
                    return FText::FromString(FString::Printf(TEXT("%d components · %d Nanite · %d lights · %s triangles"), TotalComponents,
                                                             TotalNanite, TotalLights, *Compact(TotalTriangles)));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda([this] { return bIsolate ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
                        bIsolate = State == ECheckBoxState::Checked;
                        const TArray<FItem> Selected = Tree->GetSelectedItems();
                        if (Subsystem.IsValid())
                            Subsystem->IsolateComponent(bIsolate && !Selected.IsEmpty() ? Selected[0]->Component.Get() : nullptr);
                    })
                    [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("Isolate", "Isolate selection"))]
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton).OnClicked_Lambda([this] {
                        if (Subsystem.IsValid()) Subsystem->FrameAll(); // its nullptr focus broadcast clears our state
                        return FReply::Handled();
                    })
                    [SNew(STextBlock).Font(Font(8)).Text(LOCTEXT("FrameAll", "Frame all (F)"))]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1.f)
            [
                SAssignNew(Tree, STreeView<FItem>)
                .TreeItemsSource(&Roots)
                .SelectionMode(ESelectionMode::Single)
                .OnGenerateRow(this, &SSCHierarchyPanel::MakeRow)
                .OnGetChildren_Lambda([](FItem Item, TArray<FItem>& Out) { Out = Item->Children; })
                .OnSelectionChanged(this, &SSCHierarchyPanel::OnSelection)
            ]
        ]
    ];
}
SSCHierarchyPanel::~SSCHierarchyPanel()
{
    if (Subsystem.IsValid()) Subsystem->OnComponentFocused.Remove(FocusHandle);
}

void SSCHierarchyPanel::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    ASCVehicle* Current = Subsystem.IsValid() ? Subsystem->GetVehicle() : nullptr;
    if (Current != BuiltFor.Get() || (!Current && !Roots.IsEmpty())) Rebuild(Current);
}

void SSCHierarchyPanel::Rebuild(ASCVehicle* Vehicle)
{
    BuiltFor = Vehicle;
    Roots.Reset();
    TotalTriangles = 0;
    TotalComponents = TotalNanite = TotalLights = 0;
    bIsolate = false;
    if (Vehicle && Vehicle->GetRootComponent())
    {
        TMap<const ULightComponent*, FString> LightNames;
        for (const auto& State : Vehicle->LightStates)
            if (State.Component) LightNames.Add(State.Component, State.Name);
        FItem Root = BuildNode(Vehicle->GetRootComponent(), nullptr, LightNames);
        Root->Label = Vehicle->Manifest.Root.EntityName.IsEmpty() ? Root->Label : Vehicle->Manifest.Root.EntityName;
        Roots.Add(Root);
        TotalTriangles = Root->Triangles;
    }
    Tree->RequestTreeRefresh();
    for (const FItem& Root : Roots)
    {
        Tree->SetItemExpansion(Root, true);
        for (const FItem& Child : Root->Children)
            Tree->SetItemExpansion(Child, Child->Children.Num() <= 8);
    }
}

SSCHierarchyPanel::FItem SSCHierarchyPanel::BuildNode(USceneComponent* Component, const FItem& Parent,
                                                      const TMap<const ULightComponent*, FString>& LightNames)
{
    FItem Node = MakeShared<FNode>();
    Node->Component = Component;
    Node->Parent = Parent;
    Node->Label = Component->GetName();
    ++TotalComponents;
    if (const auto* Mesh = Cast<UStaticMeshComponent>(Component))
    {
        Node->Kind = FNode::EKind::Static;
        const UStaticMesh* Asset = Mesh->GetStaticMesh();
        Node->Label = Asset ? Asset->GetName() : Node->Label;
        Node->bNanite = Asset && Asset->IsNaniteEnabled() && Asset->HasValidNaniteData();
        Node->Triangles = StaticTriangles(Asset);
        Node->Drawables = 1;
        TotalNanite += Node->bNanite;
    }
    else if (const auto* Skin = Cast<USkeletalMeshComponent>(Component))
    {
        Node->Kind = FNode::EKind::Skeletal;
        const auto* Rig = Cast<USCRigComponent>(Skin);
        Node->Label = TEXT("Rig ") + (Rig && !Rig->MeshAsset.IsEmpty() ? FPaths::GetBaseFilename(Rig->MeshAsset) : Node->Label);
        Node->Triangles = SkeletalTriangles(Skin->GetSkeletalMeshAsset());
        Node->Drawables = 1;
    }
    else if (const auto* Light = Cast<ULightComponent>(Component))
    {
        Node->Kind = FNode::EKind::Light;
        if (const FString* Name = LightNames.Find(Light)) Node->Label = *Name;
        ++TotalLights;
    }
    for (USceneComponent* Child : Component->GetAttachChildren())
        if (Child && Child->GetOwner() == Component->GetOwner())
        {
            FItem ChildNode = BuildNode(Child, Node, LightNames);
            Node->Triangles += ChildNode->Triangles;
            Node->Drawables += ChildNode->Drawables;
            Node->Children.Add(ChildNode);
        }
    // Groups first, then drawables, each by label: stable and scannable.
    Node->Children.Sort([](const FItem& A, const FItem& B) {
        return A->Kind != B->Kind ? A->Kind < B->Kind : A->Label < B->Label;
    });
    return Node;
}

TSharedRef<ITableRow> SSCHierarchyPanel::MakeRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
    TSharedRef<SHorizontalBox> Badges = SNew(SHorizontalBox);
    auto AddBadge = [&](const FText& Text, const FLinearColor& Color) { Badges->AddSlot().AutoWidth().Padding(3, 0, 0, 0)[Chip(Text, Color)]; };
    switch (Item->Kind)
    {
    case FNode::EKind::Static:
        AddBadge(LOCTEXT("Static", "static"), Accent());
        AddBadge(Item->bNanite ? LOCTEXT("Nanite", "Nanite") : LOCTEXT("NoNanite", "no Nanite"), Item->bNanite ? Good() : Dim());
        break;
    case FNode::EKind::Skeletal: AddBadge(LOCTEXT("Skeletal", "skeletal"), Warn()); break;
    case FNode::EKind::Light: AddBadge(LOCTEXT("Light", "light"), FLinearColor(1.0f, 0.9f, 0.5f)); break;
    case FNode::EKind::Group: break;
    }
    if (Item->Triangles > 0) AddBadge(FText::FromString(Compact(Item->Triangles) + TEXT(" tri")), Dim());
    return SNew(STableRow<FItem>, Owner).Padding(FMargin(2, 1))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(STextBlock).Font(Font(9, Item->Kind == FNode::EKind::Group)).ColorAndOpacity(Item->Kind == FNode::EKind::Group ? Text() : Dim())
            .Text(FText::FromString(Item->Label)).OverflowPolicy(ETextOverflowPolicy::Ellipsis).ToolTipText(FText::FromString(Item->Label))
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Badges]
    ];
}

void SSCHierarchyPanel::OnSelection(FItem Item, ESelectInfo::Type)
{
    if (bSyncingSelection || !Subsystem.IsValid() || !Item) return;
    Subsystem->FocusComponent(Item->Component.Get());
    if (bIsolate) Subsystem->IsolateComponent(Item->Component.Get());
}
SSCHierarchyPanel::FItem SSCHierarchyPanel::Find(const FItem& From, const USceneComponent* Component) const
{
    if (From->Component.Get() == Component) return From;
    for (const FItem& Child : From->Children)
        if (FItem Found = Find(Child, Component)) return Found;
    return nullptr;
}
void SSCHierarchyPanel::OnFocused(USceneComponent* Component)
{
    // Keep the tree in sync with focus changes from elsewhere (viewport double-click, F / Frame all, scripts).
    TGuardValue<bool> Guard(bSyncingSelection, true);
    if (!Component)
    {
        bIsolate = false; // FrameAll already restored visibility
        Tree->ClearSelection();
        return;
    }
    if (Roots.IsEmpty()) return;
    FItem Item = Find(Roots[0], Component);
    if (!Item || Tree->IsItemSelected(Item)) return;
    for (TSharedPtr<FNode> P = Item->Parent.Pin(); P; P = P->Parent.Pin())
        Tree->SetItemExpansion(P, true);
    Tree->SetSelection(Item);
    Tree->RequestScrollIntoView(Item);
}

#undef LOCTEXT_NAMESPACE
