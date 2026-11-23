// SUCScenarioNotice (SUCScenarioNotice.h): the scenario's refusal or failure over the view, in the HUD look.
#include "UI/SUCScenarioNotice.h"
#include "SCViewerSubsystem.h"
#include "UCScenarioSubsystem.h"
#include "UI/SCHudStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UCScenarioNotice"

void SUCScenarioNotice::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    SetVisibility(TAttribute<EVisibility>::CreateSP(this, &SUCScenarioNotice::NoticeVisibility));
    ChildSlot.HAlign(HAlign_Center).VAlign(VAlign_Center)
    [
        SNew(SBox).MaxDesiredWidth(SCHudStyle::SideWidth * 2)
        [
            SNew(SBorder).BorderImage(SCHudStyle::White()).BorderBackgroundColor(SCHudStyle::Paper()).Padding(SCHudStyle::Gutter)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, SCHudStyle::Pad)
                [
                    SNew(STextBlock).Font(SCHudStyle::Title()).ColorAndOpacity(SCHudStyle::Alert())
                        .Text(TAttribute<FText>::CreateSP(this, &SUCScenarioNotice::Heading))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Font(SCHudStyle::Body()).ColorAndOpacity(SCHudStyle::Ink()).AutoWrapText(true)
                        .Text(TAttribute<FText>::CreateSP(this, &SUCScenarioNotice::Body))
                ]
            ]
        ]
    ];
}

UUCScenarioSubsystem* SUCScenarioNotice::Scenario() const
{
    return UUCScenarioSubsystem::Get(Subsystem.Get());
}

EVisibility SUCScenarioNotice::NoticeVisibility() const
{
    const UUCScenarioSubsystem* S = Scenario();
    return S && (S->GetState() == UUCScenarioSubsystem::EState::Refused || S->GetState() == UUCScenarioSubsystem::EState::Failed)
               ? EVisibility::HitTestInvisible
               : EVisibility::Collapsed;
}

FText SUCScenarioNotice::Heading() const
{
    const UUCScenarioSubsystem* S = Scenario();
    if (!S) return FText::GetEmpty();
    const FText Name = FText::FromString(S->GetId());
    return SCHudStyle::Caps(S->GetState() == UUCScenarioSubsystem::EState::Failed
                                ? FText::Format(LOCTEXT("Failed", "Scenario {0} failed"), Name)
                                : FText::Format(LOCTEXT("Refused", "Scenario {0} cannot start"), Name));
}

FText SUCScenarioNotice::Body() const
{
    const UUCScenarioSubsystem* S = Scenario();
    return S ? FText::FromString(S->GetProblem()) : FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE
