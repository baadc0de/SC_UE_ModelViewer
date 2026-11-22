// Collision status, relative coordinates, nearest interaction and data-derived teleport choices: the ship's spawns and the
// hangar's waypoints, each section under its heading (SCTeleportMenu::Rows); keys and the mouse select across both.
#include "SSCFirstPersonHUD.h"
#include "SSCInteractionPrompt.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "SCVehicle.h"
#include "SCHangarWaypoints.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/CoreStyle.h"
#include "Misc/Paths.h"
void SSCFirstPersonHUD::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    SetVisibility(TAttribute<EVisibility>::CreateLambda([this] {
        return Subsystem.IsValid() && Subsystem->GetFirstPerson() ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
    }));
    ChildSlot[
        SNew(SOverlay)
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::FromString(TEXT("+"))).Visibility(EVisibility::HitTestInvisible)
            .ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor::Black)]
        // WP-I1-4: what E does on the thing under the crosshair, the wheel's choice among several.
        + SOverlay::Slot()[SNew(SSCInteractionPrompt).Subsystem(Subsystem.Get())]
        + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(12,12,12,40)
        // Dark backing: the text must stay readable over bright, lit interiors.
        [SNew(SBorder).Visibility(EVisibility::HitTestInvisible).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(0, 0, 0, 0.6f)).Padding(FMargin(8, 4))
            [SNew(STextBlock).ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor::Black).Text_Lambda([this] {
                auto* C = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr;
                return FText::FromString(C ? C->HUDText() : FString());
            })]]
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
        [SNew(SBorder).Visibility_Lambda([this] {
            auto* C = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr;
            return C && C->bTeleportMenuOpen ? EVisibility::Visible : EVisibility::Collapsed;
        })[SNew(SBox).WidthOverride(460)[SAssignNew(Menu, SVerticalBox)]]]
    ];
}
void SSCFirstPersonHUD::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry,Time,Delta);
    ASCFirstPersonCharacter* Character = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr;
    ASCVehicle* Vehicle = Subsystem.IsValid() ? Subsystem->GetVehicle() : nullptr;
    const int32 Revision = Vehicle ? Vehicle->SpawnRevision : INDEX_NONE;
    ASCVehicle* Hangar = Character ? Character->GetHangar() : nullptr;
    if (Character) Character->GetHangarSpots(); // brings the standing places up to date
    const int32 HangarCount = Character ? Character->HangarSpotsRevision : INDEX_NONE; // a new check rebuilds the rows
    const int32 HangarRevision = Hangar ? Hangar->WaypointRevision : INDEX_NONE;
    // The selected row stays in view while the keys move it.
    const int32 Selected = Character && Character->bTeleportMenuOpen ? Character->TeleportSelectionRow() : INDEX_NONE;
    if (Selected != ScrolledTo && RowScrolls.IsValidIndex(Selected) && RowScrolls[Selected])
    {
        ScrolledTo = Selected;
        RowScrolls[ScrolledTo]->ScrollDescendantIntoView(RowWidgets[ScrolledTo], true, EDescendantScrollDestination::IntoView);
    }
    if (Vehicle == Listed.Get() && Revision == ListedRevision && Hangar == ListedHangar.Get() && HangarCount == ListedHangarCount &&
        HangarRevision == ListedHangarRevision)
        return; // rebuild on a vehicle, spawn-list or hangar change
    Listed = Vehicle;
    ListedRevision = Revision;
    ListedHangar = Hangar;
    ListedHangarCount = HangarCount;
    ListedHangarRevision = HangarRevision;
    ScrolledTo = INDEX_NONE;
    RowWidgets.Reset();
    RowScrolls.Reset();
    Menu->ClearChildren();
    Menu->AddSlot().AutoHeight().Padding(4, 2)[SNew(STextBlock).Text(FText::FromString(
        TEXT("Teleport — Up / Down (W / S) select · Enter or click: go · T / Esc close")))];
    if (!Vehicle || !Character) return;
    const TArray<FSCTeleportRow> Rows = Character->TeleportRows();
    const TArray<FSCSpawnPoint>& Spots = Character->GetHangarSpots();
    const int32 ShipCount = Vehicle->SpawnPoints.Num();
    // Each section is its heading over its own scrolling list, so both headings stay in view however long the lists are.
    TSharedPtr<SScrollBox> Section;
    for (int32 Row = 0; Row < Rows.Num(); ++Row)
    {
        const FSCTeleportRow& R = Rows[Row];
        TSharedPtr<SWidget> Widget;
        if (!R.IsDestination())
        {
            const bool bShip = R.Index == int32(FSCTeleportRow::EType::Ship);
            Widget = SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.08f, 0.2f, 0.35f, 0.9f))
                .Padding(FMargin(6, 3))
                [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11)).Text(FText::FromString(bShip
                    ? FString::Printf(TEXT("Ship (%d)"), ShipCount) : FString::Printf(TEXT("Hangar (%d) · %s"), Spots.Num(), *FPaths::GetBaseFilename(Subsystem->GetEnvironmentRequest()))))];
            Menu->AddSlot().AutoHeight().Padding(0, Row ? 8 : 2, 0, 2)[Widget.ToSharedRef()];
            Menu->AddSlot().AutoHeight()[SNew(SBox).MaxDesiredHeight(bShip ? 360 : 300)[SAssignNew(Section, SScrollBox)]];
            RowWidgets.Add(Widget);
            RowScrolls.Add(nullptr);
            continue;
        }
        // The same numbers as sc.FP.Spawns / sc.FP.Spawn <index>: ship spawns first, then the hangar's places.
        const FString Label = R.Type == FSCTeleportRow::EType::Ship
            ? FString::Printf(TEXT("%d  %s · %s"), R.Index, *Vehicle->SpawnPoints[R.Index].Kind.ToString(), *Vehicle->SpawnPoints[R.Index].DisplayName)
            : FString::Printf(TEXT("%d  %s"), ShipCount + R.Index, *Spots[R.Index].DisplayName);
        // A row is its destination (section, index, label), not its position: the lists may change while the menu is open.
        const FSCTeleportRow Destination = R;
        const FString Shown = Character->DestinationLabel(R);
        Widget = SNew(SButton).Text(FText::FromString(Label)).IsFocusable(false) // focus stays with the view (the keys)
            .ButtonColorAndOpacity_Lambda([this, Destination, Shown] {
                const ASCFirstPersonCharacter* C = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr;
                const bool bSelected = C && C->TeleportSelection.Type == Destination.Type && C->TeleportSelection.Index == Destination.Index &&
                                       C->TeleportSelectionLabel == Shown;
                return bSelected ? FLinearColor(1.f, 0.75f, 0.2f) : FLinearColor::White;
            })
            .OnClicked_Lambda([this, Destination, Shown] {
                if (auto* C = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr) C->TeleportToDestination(Destination, Shown);
                return FReply::Handled();
            });
        Section->AddSlot()[Widget.ToSharedRef()];
        RowWidgets.Add(Widget);
        RowScrolls.Add(Section);
    }
}
