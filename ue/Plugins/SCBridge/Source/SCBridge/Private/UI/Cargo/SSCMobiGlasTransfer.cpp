// SSCMobiGlasTransfer and SSCMobiGlasJobs: the transfer form with its fit preview, and the job list.
#include "SSCMobiGlasPanels.h"
#include "SSCMobiGlasWidgets.h"
#include "SCMobiGlasModel.h"
#include "SCMobiGlasStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCMobiGlas"
using namespace SCMobiGlasStyle;

namespace
{
FLinearColor JobColour(ESCCargoJobState S)
{
    switch (S)
    {
    case ESCCargoJobState::Moving: return Accent();
    case ESCCargoJobState::Done: return Ok();
    case ESCCargoJobState::Failed: return Alert();
    case ESCCargoJobState::Cancelled: return PaperDim();
    default: return Warn();
    }
}
/** A plan note (not a refusal): accent bar, ink text. */
TSharedRef<SWidget> Note(const FText& Text)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 8, 2)[SNew(SBox).WidthOverride(StripeWidth)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Accent())]]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).AutoWrapText(true).Text(Text)];
}
TSharedRef<SWidget> Refusal(const FText& Text)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 2, 8, 2)[SNew(SBox).WidthOverride(StripeWidth)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Alert())]]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Font(Body()).ColorAndOpacity(Alert()).AutoWrapText(true).Text(Text)];
}
} // namespace

// ---- Transfer -------------------------------------------------------------------------------------------------------
void SSCMobiGlasTransfer::Construct(const FArguments& Args)
{
    Model = Args._Model;
    TSharedPtr<FSCMobiGlasModel> M = Model;
    auto Endpoint = [M](bool bFrom) {
        return SSCMobiGlasPicker::FGetOptions::CreateLambda([M, bFrom] {
            TArray<SSCMobiGlasPicker::FOption> Out;
            for (const FSCGridPick& P : M->PickOptions())
                Out.Add({M->PickLabel(P), FText(), (bFrom ? M->GetFrom() : M->GetTo()) == P, [M, P, bFrom] { bFrom ? M->SetFrom(P) : M->SetTo(P); }});
            return Out;
        });
    };
    auto Stops = [M] { return M->GetPreview().AchievableSCU; };
    ChildSlot
    [
        Panel(LOCTEXT("Transfer", "Transfer"), FText(),
        SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [
        SNew(SScrollBox) + SScrollBox::Slot()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
            [SNew(SSCMobiGlasPicker).Caption(LOCTEXT("From", "From")).Options(Endpoint(true)).Value_Lambda([M] { return M->PickLabel(M->GetFrom()); })]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
            [SNew(SSCMobiGlasPicker).Caption(LOCTEXT("To", "To")).Options(Endpoint(false)).Value_Lambda([M] { return M->PickLabel(M->GetTo()); })]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
            [
                SNew(SSCMobiGlasPicker).Caption(LOCTEXT("Commodity", "Commodity"))
                .Value_Lambda([M] {
                    const FSCCargoCommodityView* C = M->GetCommodities().FindByPredicate([M](const FSCCargoCommodityView& V) { return V.Commodity == M->GetCommodity(); });
                    return C ? C->Label : LOCTEXT("NoCommodity", "Nothing stored at the source");
                })
                .Options(SSCMobiGlasPicker::FGetOptions::CreateLambda([M] {
                    TArray<SSCMobiGlasPicker::FOption> Out;
                    for (const FSCCargoCommodityView& C : M->GetCommodities())
                    {
                        const FName Name = C.Commodity;
                        Out.Add({C.Label, FText::Format(LOCTEXT("CommodityScu", "{0} SCU"), C.TotalSCU), Name == M->GetCommodity(), [M, Name] { M->SetCommodity(Name); }});
                    }
                    return Out;
                }))
            ]
            // Amount: the slider moves over the achievable stops only.
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Bottom)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()[Txt(LOCTEXT("Amount", "AMOUNT · WHOLE BOXES ONLY"), Label(), Ink2())]
                    + SVerticalBox::Slot().AutoHeight()[Txt(TAttribute<FText>::CreateLambda([M] {
                      const FSCCargoCommodityView* C = M->GetCommodities().FindByPredicate([M](const FSCCargoCommodityView& V) { return V.Commodity == M->GetCommodity(); });
                      return C ? FText::Format(LOCTEXT("OfTotal", "OF {0} SCU AT SOURCE"), C->TotalSCU) : FText();
                  }), Label(), Ink2())]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
                [Txt(TAttribute<FText>::CreateLambda([M] { return FText::AsNumber(M->GetAmount()); }), NumericLg(), Ink())]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 5)[Txt(LOCTEXT("ScuUnit", "SCU"), Body(), Ink2())]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SSlider).Style(&Slider()).MinValue(0).MaxValue(1).MouseUsesStep(true).IsFocusable(false)
                .IsEnabled_Lambda([Stops] { return Stops().Num() > 1; })
                .StepSize_Lambda([Stops] { return 1.f / FMath::Max(1, Stops().Num() - 1); })
                .Value_Lambda([M, Stops] {
                    const TArray<int32> S = Stops();
                    return S.Num() > 1 ? float(FMath::Max(0, S.IndexOfByKey(M->GetAmount()))) / (S.Num() - 1) : 1.f;
                })
                .OnValueChanged_Lambda([M, Stops](float V) {
                    const TArray<int32> S = Stops();
                    if (!S.IsEmpty()) M->SetAmount(S[FMath::Clamp(FMath::RoundToInt32(V * (S.Num() - 1)), 0, S.Num() - 1)]);
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
            [
                SNew(STextBlock).Font(Label()).ColorAndOpacity(Ink2()).Text_Lambda([Stops] {
                    const TArray<int32> S = Stops();
                    if (S.IsEmpty()) return LOCTEXT("NoStops", "NO WHOLE BOXES FIT");
                    FString Line;
                    for (int32 V : S) Line += (Line.IsEmpty() ? TEXT("") : TEXT(" · ")) + FString::FromInt(V);
                    return FText::Format(LOCTEXT("Stops", "STOPS {0} SCU"), FText::FromString(Line));
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)[Txt(LOCTEXT("Fit", "FIT PREVIEW"), Label(), Ink2())]
            + SVerticalBox::Slot().AutoHeight()[SAssignNew(List, SVerticalBox)]
        ]
        ]
        // ETA and Confirm stay pinned under the scrolling form.
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 10)[HRule()]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Txt(LOCTEXT("Eta", "ESTIMATED TIME"), Label(), Ink2())]
                + SHorizontalBox::Slot().AutoWidth()[Txt(TAttribute<FText>::CreateLambda([M] {
                      return M->GetPreview().bValid ? SCMobiGlasText::Duration(M->GetPreview().EstimatedSeconds) : FText::FromString(TEXT("—"));
                  }), Numeric(), Ink())]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SButton).ButtonStyle(&PrimaryButton()).ContentPadding(FMargin(0, 10)).HAlign(HAlign_Center)
                .IsEnabled_Lambda([M] { return M->GetPreview().bValid; })
                .OnClicked_Lambda([M] { M->Confirm(); return FReply::Handled(); })
                [SNew(STextBlock).Font(Section()).Text(LOCTEXT("Confirm", "CONFIRM TRANSFER")).ColorAndOpacity(FSlateColor::UseForeground())]
            ]
        )
    ];
}
void SSCMobiGlasTransfer::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    if (BuiltStamp != Model->GetPreviewStamp() + 1) Rebuild();
}
void SSCMobiGlasTransfer::Rebuild()
{
    BuiltStamp = Model->GetPreviewStamp() + 1;
    List->ClearChildren();
    const FSCCargoTransferPreview& P = Model->GetPreview();
    if (!Model->HasSource()) { List->AddSlot().AutoHeight()[Txt(LOCTEXT("NoSourceFit", "No cargo system in this scene."), Body(), Ink2())]; }
    for (const FSCCargoRefusal& R : P.Refusals) List->AddSlot().AutoHeight().Padding(0, 3)[Refusal(R.Reason)]; // what cannot go first
    // Repeated moves read as one line ("2 × 8 SCU → MISC Hull A · Grid 2").
    struct FLine { FSCCargoMovePlan Move; int32 Count = 0; };
    TArray<FLine> Lines;
    for (const FSCCargoMovePlan& Move : P.Moves)
    {
        FLine* L = Lines.FindByPredicate([&Move](const FLine& X) {
            return X.Move.SCU == Move.SCU && X.Move.ToGrid == Move.ToGrid && X.Move.DronesNeeded == Move.DronesNeeded && X.Move.bRotated == Move.bRotated;
        });
        if (!L) L = &Lines.Add_GetRef(FLine{Move, 0});
        ++L->Count;
    }
    for (const FLine& Line : Lines)
    {
        const FSCCargoMovePlan& Move = Line.Move;
        const FText What = Line.Count > 1 ? FText::Format(LOCTEXT("MoveN", "{0} × {1} SCU"), Line.Count, Move.SCU) : FText::Format(LOCTEXT("Move1", "{0} SCU"), Move.SCU);
        List->AddSlot().AutoHeight().Padding(0, 2)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                 .Text(FText::Format(LOCTEXT("Move", "{0} → {1}"), What, Model->ShortGridName(Move.ToGrid)))]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
            [Txt(FText::Format(LOCTEXT("MoveDrones", "{0}{1} {1}|plural(one=DRONE,other=DRONES)"), Move.bRotated ? LOCTEXT("Rot", "TURNED · ") : FText(), Move.DronesNeeded),
                 Label(), Ink2())]
        ];
    }
    // Cargo rides only on extended grids: say which side extends first.
    if (!P.Moves.IsEmpty())
    {
        const TArray<int32> Dest = Model->NeedExtending(Model->Resolve(Model->GetTo())), Src = Model->NeedExtending(Model->Resolve(Model->GetFrom()));
        if (!Src.IsEmpty())
            List->AddSlot().AutoHeight().Padding(0, 3)[Note(FText::Format(LOCTEXT("ExtendSource", "{0} will extend before unloading."), Model->DescribeGrids(Src)))];
        if (!Dest.IsEmpty())
            List->AddSlot().AutoHeight().Padding(0, 3)[Note(FText::Format(LOCTEXT("ExtendDest", "{0} will extend before loading."), Model->DescribeGrids(Dest)))];
    }
    if (Model->HasSource() && P.Moves.IsEmpty() && P.Refusals.IsEmpty())
        List->AddSlot().AutoHeight()[Txt(LOCTEXT("NothingPlanned", "Choose a source, a destination and a commodity."), Body(), Ink2())];
    // A refused Confirm shows its error inline with the plan (the preview is recomputed after every submit).
    if (!Model->GetSubmitError().IsEmpty()) List->AddSlot().AutoHeight().Padding(0, 6, 0, 0)[Refusal(Model->GetSubmitError())];
}

// ---- Transfers (jobs) -----------------------------------------------------------------------------------------------
void SSCMobiGlasJobs::Construct(const FArguments& Args)
{
    Model = Args._Model;
    TSharedPtr<FSCMobiGlasModel> M = Model;
    ChildSlot
    [
        Panel(LOCTEXT("Jobs", "Transfers"),
              TAttribute<FText>::CreateLambda([M] {
                  int32 Active = 0;
                  for (const FSCCargoJobView& J : M->GetJobs()) Active += J.bMine && FSCMobiGlasModel::IsActive(J.State);
                  return Active ? FText::Format(LOCTEXT("ActiveCount", "{0} ACTIVE"), Active) : FText();
              }),
              SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(List, SVerticalBox)])
    ];
}
void SSCMobiGlasJobs::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    if (BuiltStamp != Model->GetStamp()) Rebuild();
}
void SSCMobiGlasJobs::Rebuild()
{
    BuiltStamp = Model->GetStamp();
    List->ClearChildren();
    if (!Model->GetJobs().ContainsByPredicate([](const FSCCargoJobView& J) { return J.bMine; }))
    {
        List->AddSlot().AutoHeight()[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true)
                                         .Text(LOCTEXT("NoJobs", "No transfers yet. Plan one under Transfer."))];
    }
    TSharedPtr<FSCMobiGlasModel> M = Model;
    for (const FSCCargoJobView& Job : Model->GetJobs())
    {
        if (!Job.bMine) continue; // other owners' jobs: the compact section below
        const int32 Id = Job.JobId;
        auto Live = [M, Id]() -> const FSCCargoJobView* { return M->FindJob(Id); };
        const bool bActive = FSCMobiGlasModel::IsActive(Job.State);
        TSharedRef<SVerticalBox> Row = SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).AutoWrapText(true).Text(Job.Summary)]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 6)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [Chip(TAttribute<FText>::CreateLambda([Live] { const FSCCargoJobView* J = Live(); return J ? Caps(J->StateText) : FText(); }),
                      TAttribute<FSlateColor>::CreateLambda([Live] { const FSCCargoJobView* J = Live(); return FSlateColor(JobColour(J ? J->State : ESCCargoJobState::Done)); }))]
                + SHorizontalBox::Slot().FillWidth(1)[SNew(SSpacer)]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Rule()).Padding(1)
                    .Visibility_Lambda([Live] { const FSCCargoJobView* J = Live(); return J && FSCMobiGlasModel::IsActive(J->State) ? EVisibility::Visible : EVisibility::Collapsed; })
                    [SNew(SButton).ButtonStyle(&FlatButton()).ContentPadding(FMargin(10, 2)).OnClicked_Lambda([M, Id] { M->CancelJob(Id); return FReply::Handled(); })
                         [SNew(STextBlock).Font(Label()).Text(LOCTEXT("Cancel", "CANCEL")).ColorAndOpacity(FSlateColor::UseForeground())]]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBox).HeightOverride(BarHeight)
                [SNew(SSCCargoBar).Value_Lambda([Live] { const FSCCargoJobView* J = Live(); return J ? J->Progress : 0.f; })
                     .FillColour_Lambda([Live] { const FSCCargoJobView* J = Live(); return J && J->State == ESCCargoJobState::Done ? Ok() : J && J->State == ESCCargoJobState::Failed ? Alert() : Ink(); })]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
            [
                SNew(STextBlock).Font(Numeric()).ColorAndOpacity(Ink2()).Text_Lambda([Live] {
                    const FSCCargoJobView* J = Live();
                    if (!J) return FText();
                    const FText Base = FText::Format(LOCTEXT("JobNumbers", "{0} · {1} {1}|plural(one=drone,other=drones)"), SCMobiGlasText::Scu(J->DoneSCU, J->TotalSCU), J->Drones.Num());
                    return FSCMobiGlasModel::IsActive(J->State) ? FText::Format(LOCTEXT("JobEta", "{0} · ETA {1}"), Base, SCMobiGlasText::Duration(J->EtaSeconds)) : Base;
                })
            ];
        // Which drone does what (carrying, holding a box aside, returning).
        Row->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
        [
            SNew(STextBlock).Font(Label()).ColorAndOpacity(Ink2()).AutoWrapText(true)
            .Visibility_Lambda([Live] { const FSCCargoJobView* J = Live(); return J && FSCMobiGlasModel::IsActive(J->State) && !J->Drones.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
            .Text_Lambda([M, Live] {
                const FSCCargoJobView* J = Live();
                FString Line;
                for (const FSCCargoDroneView& D : M->GetDrones())
                    if (J && (D.JobId == J->JobId || J->Drones.Contains(D.DroneId)))
                        Line += (Line.IsEmpty() ? TEXT("") : TEXT("   ")) + Caps(D.Label.ToString()) + TEXT(" · ") + SCMobiGlasText::DroneState(uint8(D.State));
                return FText::FromString(Line);
            })
        ];
        if (!Job.FailureReason.IsEmpty()) Row->AddSlot().AutoHeight().Padding(0, 6, 0, 0)[Refusal(Job.FailureReason)];
        List->AddSlot().AutoHeight().Padding(0, 0, 0, 10)[Row];
        List->AddSlot().AutoHeight().Padding(0, 0, 0, 10)[HRule()];
        (void)bActive;
    }
    // Other traffic: other owners' active transfers through a shared grid (the freight elevator), with their state
    // (queued / waiting for the platform), so a wait on ours is explained.
    bool bHeader = false;
    for (const FSCCargoJobView& Job : Model->GetJobs())
    {
        if (Job.bMine || !FSCMobiGlasModel::IsActive(Job.State)) continue;
        TArray<int32> Touched = Job.FromGrids;
        Touched.Append(Job.ToGrids);
        if (!Touched.ContainsByPredicate([this](int32 Id) { const FSCCargoGridView* G = Model->FindGrid(Id); return G && G->Kind == ESCCargoGridKind::Environment; }))
            continue;
        if (!bHeader)
        {
            bHeader = true;
            List->AddSlot().AutoHeight().Padding(0, 6, 0, 6)[Txt(LOCTEXT("OtherTraffic", "OTHER TRAFFIC"), Label(), Ink2())];
        }
        const int32 Id = Job.JobId;
        List->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
            [SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true)
                 .Text(Job.OwnerLabel.IsEmpty() ? Job.Summary : FText::Format(LOCTEXT("OtherJob", "{0} · {1}"), Job.OwnerLabel, Job.Summary))]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [Chip(TAttribute<FText>::CreateLambda([this, Id] { const FSCCargoJobView* J = Model->FindJob(Id); return J ? Caps(J->StateText) : FText(); }),
                  FSlateColor(PaperDim()))]
        ];
    }
}

#undef LOCTEXT_NAMESPACE
