#include "SSCShipLink.h"
#include "SCShipLinkModel.h"
#include "../Cargo/SCMobiGlasModel.h"
#include "UI/SCHudStyle.h"
#include "SCVehicle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using namespace SCHudStyle;

namespace
{
FText LabelText(const TCHAR* Value) { return FText::FromString(Value); }

TSharedRef<SWidget> Action(const TAttribute<FText>& Text, TFunction<void()> Click, bool bPrimary = false)
{
    return SNew(SButton).ButtonStyle(bPrimary ? &PrimaryButton() : &FlatButton()).ContentPadding(FMargin(8, 4))
        .OnClicked_Lambda([Click = MoveTemp(Click)] { Click(); return FReply::Handled(); })
        [Txt(Text, Label(), bPrimary ? Paper() : Ink())];
}

TSharedRef<SWidget> DisabledRow(const TCHAR* Name, const TCHAR* Description)
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[HRule()]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 2)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[Txt(LabelText(Name), Body(), Ink2())]
            + SHorizontalBox::Slot().AutoWidth()[Txt(LabelText(TEXT("NOT INSTALLED")), Label(), Ink2())]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
        [SNew(STextBlock).Text(LabelText(Description)).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true)];
}
}

void SSCShipLink::Construct(const FArguments& Args)
{
    Model = Args._Model;
    ChildSlot[SAssignNew(Content, SBox)];
}

void SSCShipLink::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    FSCShipLinkModel& Link = Model->GetShipLink();
    if (SeenRevision != Link.GetRevision())
    {
        SeenRevision = Link.GetRevision();
        SeenDoor = INDEX_NONE;
        DoorRows.Reset();
        Content->SetContent(Build());
    }
    if (SeenDoor != Link.GetDoor())
    {
        SeenDoor = Link.GetDoor();
        if (const TWeakPtr<SWidget>* Row = DoorRows.Find(SeenDoor); Row && DoorScroll)
            DoorScroll->ScrollDescendantIntoView(Row->Pin(), true, EDescendantScrollDestination::IntoView, 8);
    }
}

TSharedRef<SWidget> SSCShipLink::Build()
{
    FSCShipLinkModel* M = &Model->GetShipLink();
    const ASCVehicle* Ship = M->GetShip();
    if (!Ship)
        return Panel(LabelText(TEXT("SHIP LINK")), FText(),
            Txt(LabelText(TEXT("Place a ship to pair it with your mobiGlas.")), Body(), Ink2()));

    TSharedRef<SVerticalBox> Access = SNew(SVerticalBox);
    TSharedRef<SVerticalBox> FleetList = SNew(SVerticalBox);
    for (const TWeakObjectPtr<ASCVehicle>& Candidate : M->GetFleet())
        FleetList->AddSlot().AutoHeight()
        [Action(FText::FromString(FSCShipLinkModel::ShipLabel(Candidate.Get())), [M, Candidate] { if (Candidate.IsValid()) M->SelectShip(Candidate.Get()); })];
    if (M->GetFleet().Num() > 1)
        Access->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SComboButton)
            .ButtonContent()[Txt(FText::FromString(FSCShipLinkModel::ShipLabel(Ship)), Body(), Ink())]
            .MenuContent()[FleetList]
        ];

    TSharedRef<SHorizontalBox> Quick = SNew(SHorizontalBox);
    Quick->AddSlot().AutoWidth().Padding(0, 0, 6, 0)[Action(LabelText(TEXT("SECURE SHIP")), [M] { M->Secure(); }, true)];
    if (Ship->ShipDoors.ContainsByPredicate([](const ASCVehicle::FShipDoor& D) { return D.bRamp && D.Group == ESCShipDoorGroup::Exterior; }))
        Quick->AddSlot().AutoWidth().Padding(0, 0, 6, 0)[Action(LabelText(TEXT("RAMP")), [M] { M->Ramp(); })];
    if (!Ship->LightGroups.IsEmpty())
        Quick->AddSlot().AutoWidth().Padding(0, 0, 6, 0)[Action(LabelText(TEXT("LIGHTS")), [M] { M->Lights(); })];
    Quick->AddSlot().AutoWidth()[Action(TAttribute<FText>::CreateLambda([M] {
        const auto* Link = M->GetLink();
        return LabelText(Link && Link->IsLocating(M->GetShip()) ? TEXT("STOP LOCATING") : TEXT("FIND MY SHIP"));
    }), [M] { M->ToggleLocate(); })];
    Access->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[Quick];
    Access->AddSlot().AutoHeight()[SNew(STextBlock).Font(Body()).ColorAndOpacity(Alert()).AutoWrapText(true)
        .Text_Lambda([M] { return FText::FromString(M->GetNotice()); })
        .Visibility_Lambda([M] { return M->GetNotice().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })];

    TSharedRef<SScrollBox> AccessRows = SAssignNew(DoorScroll, SScrollBox);
    Access->AddSlot().FillHeight(1)[AccessRows];
    for (ESCShipDoorGroup Group : {ESCShipDoorGroup::Exterior, ESCShipDoorGroup::Interior, ESCShipDoorGroup::Other})
    {
        TArray<int32> Members;
        for (int32 Id = 0; Id < Ship->Doors.Num(); ++Id)
            if (FSCShipLinkModel::Listed(Ship, Id) && Ship->ShipDoors.IsValidIndex(Id) && Ship->ShipDoors[Id].Group == Group)
                Members.Add(Id);
        if (Members.IsEmpty()) continue;
        TSharedRef<SHorizontalBox> Heading = SNew(SHorizontalBox);
        Heading->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Txt(FText::FromString(Caps(FSCShipDoorPolicy::GroupName(Group))), Section(), Ink())];
        Heading->AddSlot().AutoWidth().Padding(4, 0)[Action(LabelText(TEXT("OPEN ALL")), [M, Group] { M->OrderGroup(Group, ESCShipDoorAction::Open); })];
        Heading->AddSlot().AutoWidth().Padding(4, 0)[Action(LabelText(TEXT("CLOSE ALL")), [M, Group] { M->OrderGroup(Group, ESCShipDoorAction::Close); })];
        Heading->AddSlot().AutoWidth().Padding(4, 0)[Action(LabelText(TEXT("LOCK ALL")), [M, Group] { M->OrderGroup(Group, ESCShipDoorAction::Lock); })];
        Heading->AddSlot().AutoWidth().Padding(4, 0)[Action(LabelText(TEXT("UNLOCK ALL")), [M, Group] { M->OrderGroup(Group, ESCShipDoorAction::Unlock); })];
        AccessRows->AddSlot().Padding(0, 6)[Heading];
        for (int32 Id : Members)
        {
            const FString Name = Ship->ShipDoorLabel(Id);
            TSharedRef<SBorder> Row = SNew(SBorder).BorderImage(White()).Padding(FMargin(5, 4))
                .BorderBackgroundColor_Lambda([M, Id] { return M->GetDoor() == Id ? PaperDim() : Paper(); })
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()[HRule()]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                        [SNew(STextBlock).Text(FText::FromString(Name)).Font(Body()).ColorAndOpacity(Ink()).AutoWrapText(true)]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
                        [Txt(TAttribute<FText>::CreateLambda([M, Id] { return FText::FromString(FSCShipLinkModel::DoorStatus(M->GetShip(), Id)); }), Label(), Ink2())]
                        + SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
                        [Action(TAttribute<FText>::CreateLambda([M, Id] {
                            const ASCVehicle* V = M->GetShip();
                            return LabelText(V && V->IsDoorOpen(Id) ? TEXT("CLOSE") : TEXT("OPEN"));
                        }), [M, Id] {
                            const ASCVehicle* V = M->GetShip();
                            if (V) M->Order(Id, V->IsDoorOpen(Id) ? ESCShipDoorAction::Close : ESCShipDoorAction::Open);
                        })]
                        + SHorizontalBox::Slot().AutoWidth()
                        [Action(TAttribute<FText>::CreateLambda([M, Id] {
                            const ASCVehicle* V = M->GetShip();
                            return LabelText(V && V->IsDoorLocked(Id) ? TEXT("UNLOCK") : TEXT("LOCK"));
                        }), [M, Id] {
                            const ASCVehicle* V = M->GetShip();
                            if (V) M->Order(Id, V->IsDoorLocked(Id) ? ESCShipDoorAction::Unlock : ESCShipDoorAction::Lock);
                        })]
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(STextBlock).Font(Body()).ColorAndOpacity(Alert()).AutoWrapText(true)
                        .Text_Lambda([M, Id] { return FText::FromString(M->DoorRefusal(Id)); })
                        .Visibility_Lambda([M, Id] { return M->DoorRefusal(Id).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })]
                ];
            DoorRows.Add(Id, Row);
            AccessRows->AddSlot()[Row];
        }
    }
    AccessRows->AddSlot().Padding(0, 6)[DisabledRow(TEXT("Lift control"), TEXT("Call or lock the ship's lifts from anywhere."))];
    AccessRows->AddSlot()[DisabledRow(TEXT("Lockdown"), TEXT("Lock every door, inside and out."))];

    TSharedRef<SScrollBox> Systems = SNew(SScrollBox);
    for (int32 Id = 0; Id < Ship->LightGroups.Num(); ++Id)
    {
        const FSCLightGroup& Group = Ship->LightGroups[Id];
        Systems->AddSlot().Padding(0, 3)[HRule()];
        Systems->AddSlot().Padding(0, 2)[Txt(FText::FromString(Group.Name.Replace(TEXT("_"), TEXT(" "))), Body(), Ink())];
        TSharedRef<SHorizontalBox> Modes = SNew(SHorizontalBox);
        TSet<FString> SeenModes;
        for (const auto& Interaction : Group.Interactions)
            if (!Interaction.LightState.IsEmpty() && !Interaction.LinkingTag.IsEmpty() && !SeenModes.Contains(Interaction.LightState))
            {
                const FString Mode = Interaction.LightState;
                SeenModes.Add(Mode);
                Modes->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
                [Action(TAttribute<FText>::CreateLambda([M, Id, Mode] {
                    const ASCVehicle* V = M->GetShip();
                    const bool bCurrent = V && V->LightGroups.IsValidIndex(Id) && V->LightGroups[Id].LightState == Mode;
                    return FText::FromString((bCurrent ? TEXT("• ") : TEXT("")) + Caps(Mode));
                }), [M, Id, Mode] { M->SetLight(Id, Mode); })];
            }
        Systems->AddSlot().Padding(0, 2, 0, 8)[Modes];
    }
    Systems->AddSlot()[DisabledRow(TEXT("Power"), TEXT("Bring ship power up or down."))];
    if (!Ship->Thrusters.IsEmpty())
    {
        TSharedRef<SHorizontalBox> Modes = SNew(SHorizontalBox);
        const TPair<ESCThrusterPreviewMode, const TCHAR*> Choices[] = {
            {ESCThrusterPreviewMode::Off, TEXT("OFF")},
            {ESCThrusterPreviewMode::Minimum, TEXT("MINIMUM")},
            {ESCThrusterPreviewMode::Full, TEXT("FULL")},
            {ESCThrusterPreviewMode::Boosted, TEXT("BOOSTED")}
        };
        for (const auto& [Mode, Label] : Choices)
        {
            Modes->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
            [Action(TAttribute<FText>::CreateLambda([M, Mode, Label] {
                const ASCVehicle* V = M->GetShip();
                const bool bCurrent = V && V->bThrusterPreviewActive && V->ThrusterPreview == Mode;
                return FText::FromString((bCurrent ? FString(TEXT("• ")) : FString()) + Label);
            }), [M, Mode] { M->SetThrusterPreview(Mode); })];
        }
        Systems->AddSlot().Padding(0, 3)[HRule()];
        Systems->AddSlot().Padding(0, 2)[Txt(LabelText(TEXT("THRUSTER VFX")), Section(), Ink())];
        Systems->AddSlot().Padding(0, 2, 0, 8)[Modes];
    }
    Systems->AddSlot().Padding(0, 12, 0, 8)[Txt(LabelText(TEXT("FLIGHT")), Section(), Ink())];
    Systems->AddSlot()[DisabledRow(TEXT("Hold position"), TEXT("Stop the ship drifting and keep it where it is."))];
    Systems->AddSlot()[DisabledRow(TEXT("Auto-land"), TEXT("Find a pad or flat ground and set the ship down."))];
    Systems->AddSlot()[DisabledRow(TEXT("Recall"), TEXT("Bring the ship to you."))];

    TSharedRef<SScrollBox> Other = SNew(SScrollBox);
    Other->AddSlot()[DisabledRow(TEXT("Ignition lock"), TEXT("Only paired players can start the ship."))];
    Other->AddSlot()[DisabledRow(TEXT("Crew access"), TEXT("Grant or revoke crew and guest access."))];
    Other->AddSlot()[DisabledRow(TEXT("Event log"), TEXT("See who opened, locked or flew the ship."))];
    Other->AddSlot().Padding(0, 12, 0, 8)[Txt(LabelText(TEXT("LOADOUT")), Section(), Ink())];
    Other->AddSlot().Padding(0, 4)
    [
        Action(LabelText(TEXT("CONFIGURE IN GARAGE")), [Master = Model, Entity = Ship->Manifest.Root.EntityName] {
            Master->SetGarageEntity(Entity);
            Master->SetApp(ESCMobiGlasApp::Garage);
        })
    ];
    for (const FSCSceneInstance& Child : Ship->Manifest.Children)
        if (!Child.PortPath.IsEmpty() && !Child.EntityName.IsEmpty())
            Other->AddSlot().Padding(0, 4)
            [
                SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).AutoWrapText(true)
                .Text(FText::FromString(Child.PortPath.Last().Replace(TEXT("_"), TEXT(" ")) + TEXT(" · ") +
                    (Child.DisplayName.IsEmpty() ? Child.EntityName.Replace(TEXT("_"), TEXT(" ")) : Child.DisplayName)))
            ];

    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(640)[Panel(LabelText(TEXT("Access")), FText(), Access)]
        + SHorizontalBox::Slot().FillWidth(540).Padding(Gutter, 0, 0, 0)[Panel(LabelText(TEXT("Systems")), FText(), Systems)]
        + SHorizontalBox::Slot().FillWidth(490).Padding(Gutter, 0, 0, 0)[Panel(LabelText(TEXT("Security")), FText(), Other)];
}
