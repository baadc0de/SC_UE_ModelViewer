// Collision status, relative coordinates, nearest interaction and data-derived teleport choices.
#include "SSCFirstPersonHUD.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "SCVehicle.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/CoreStyle.h"
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
        })[SNew(SBox).MaxDesiredHeight(400).WidthOverride(360)
          [SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(Menu,SVerticalBox)]]]]
    ];
}
void SSCFirstPersonHUD::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry,Time,Delta);
    ASCVehicle* Vehicle = Subsystem.IsValid() ? Subsystem->GetVehicle() : nullptr;
    const int32 Revision = Vehicle ? Vehicle->SpawnRevision : INDEX_NONE;
    if (Vehicle == Listed.Get() && Revision == ListedRevision) return; // rebuild on vehicle or spawn-list change
    Listed = Vehicle;
    ListedRevision = Revision;
    Menu->ClearChildren();
    Menu->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Teleport — T / Esc to close")))];
    if (!Vehicle) return;
    for (int32 I=0; I<Vehicle->SpawnPoints.Num(); ++I)
        Menu->AddSlot().AutoHeight()[SNew(SButton).Text(FText::FromString(FString::Printf(TEXT("%d  %s · %s"), I,
                *Vehicle->SpawnPoints[I].Kind.ToString(), *Vehicle->SpawnPoints[I].DisplayName)))
            .OnClicked_Lambda([this,I] {
                if (auto* C = Subsystem.IsValid() ? Subsystem->GetFirstPerson() : nullptr) C->TeleportToSpawn(I);
                return FReply::Handled();
            })];
}
