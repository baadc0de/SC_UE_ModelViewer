// SSCModelBrowser: search, recents, options and load controls.
#include "SSCModelBrowser.h"
#include "SCVehicle.h"
#include "SCViewerStyle.h"
#include "SCViewerSubsystem.h"
#include "SCViewerSettings.h"
#include "SCViewOptions.h"
#include "SCDirt.h"
#include "Misc/Paths.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SComboButton.h"
#include "Misc/CommandLine.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCModelBrowser"
using namespace SCViewerStyle;

namespace
{
constexpr float SearchDebounceSeconds = 0.25f;
}

void SSCModelBrowser::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    FString Initial;
    FParse::Value(FCommandLine::Get(), TEXT("SCSearch="), Initial); // verification aid: prefill the search box
    Query = Initial;
    ChildSlot
    [
        SNew(SBorder).BorderImage(Fill()).BorderBackgroundColor(Panel()).Padding(10)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)[Header(LOCTEXT("Models", "MODELS"))]
            + SVerticalBox::Slot().AutoHeight()
            [
                SAssignNew(SearchBox, SSearchBox)
                .HintText(LOCTEXT("SearchHint", "Search entity or display name…"))
                .InitialText(FText::FromString(Initial))
                .OnTextChanged(this, &SSCModelBrowser::OnQueryChanged)
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
            [
                SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Dim()).Text_Lambda([this] { return Status; })
            ]
            + SVerticalBox::Slot().FillHeight(1.f)
            [
                SAssignNew(List, SListView<FHit>)
                .ListItemsSource(&Hits)
                .SelectionMode(ESelectionMode::Single)
                .OnGenerateRow(this, &SSCModelBrowser::MakeRow)
                .OnSelectionChanged_Lambda([this](FHit Hit, ESelectInfo::Type) {
                    if (Hit) Select(Hit->Name, Hit->DisplayName);
                })
                .OnMouseButtonDoubleClick_Lambda([this](FHit Hit) {
                    if (!Hit) return;
                    Select(Hit->Name, Hit->DisplayName);
                    Load();
                })
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)[Header(LOCTEXT("Recent", "RECENT"))]
            + SVerticalBox::Slot().AutoHeight()[SAssignNew(Recents, SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4, 4))]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)[Header(LOCTEXT("Options", "OPTIONS"))]
            + SVerticalBox::Slot().AutoHeight()[MakeOptions()]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 4)
            [
                SNew(STextBlock).Font(Font(10, true)).ColorAndOpacity(Text()).AutoWrapText(true).Text_Lambda([this] {
                    return SelectedEntity.IsEmpty() ? LOCTEXT("NoSelection", "Select a model")
                                                    : FText::FromString(SelectedLabel.IsEmpty() || SelectedLabel == SelectedEntity
                                                                            ? SelectedEntity
                                                                            : SelectedLabel + TEXT("  (") + SelectedEntity + TEXT(")"));
                })
            ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
                [
                    SNew(SButton).HAlign(HAlign_Center).OnClicked(this, &SSCModelBrowser::Load)
                    .IsEnabled_Lambda([this] {
                        return Subsystem.IsValid() && !SelectedEntity.IsEmpty() && !Subsystem->IsBusy() &&
                               Subsystem->GetArchiveState() == ESCArchiveState::Ready;
                    })
                    [SNew(STextBlock).Font(Font(10, true)).Text(LOCTEXT("Load", "Load"))]
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .IsEnabled_Lambda([this] { return Subsystem.IsValid() && Subsystem->IsBusy(); })
                    .OnClicked_Lambda([this] {
                        if (Subsystem.IsValid()) Subsystem->CancelLoad();
                        return FReply::Handled();
                    })
                    [SNew(STextBlock).Font(Font(10)).Text(LOCTEXT("Cancel", "Cancel"))]
                ]
            ]
        ]
    ];
    RebuildRecents();
}

TSharedRef<SWidget> SSCModelBrowser::MakeChoice(const FText& Label, const TArray<int32>& Values, TFunction<int32&()> Field)
{
    TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(Label)]];
    for (int32 Value : Values)
        Row->AddSlot().AutoWidth().Padding(0, 0, 2, 0)
        [
            SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([Field, Value] { return Field() == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([Field, Value](ECheckBoxState) { Field() = Value; })
            [SNew(SBox).MinDesiredWidth(26).HAlign(HAlign_Center).Padding(FMargin(4, 1))
                [SNew(STextBlock).Font(Font(9)).Text(FText::AsNumber(Value))]]
        ];
    return Row;
}
TSharedRef<SWidget> SSCModelBrowser::MakeToggle(const FText& Label, TFunction<bool&()> Field)
{
    return SNew(SCheckBox)
        .IsChecked_Lambda([Field] { return Field() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([Field](ECheckBoxState State) { Field() = State == ECheckBoxState::Checked; })
        [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(Label)];
}
TSharedRef<SWidget> SSCModelBrowser::MakeOptions()
{
    TWeakObjectPtr<USCViewerSubsystem> Weak = Subsystem;
    static int32 Dummy = 0;
    static bool DummyFlag = false;
    auto O = [Weak]() -> USCViewOptions* { return Weak.IsValid() ? Weak->GetOptions() : nullptr; };
    auto IntField = [O](int32 USCViewOptions::*Member) {
        return TFunction<int32&()>([O, Member]() -> int32& { return O() ? O()->*Member : Dummy; });
    };
    auto BoolField = [O](bool USCViewOptions::*Member) {
        return TFunction<bool&()>([O, Member]() -> bool& { return O() ? O()->*Member : DummyFlag; });
    };
    TSharedRef<SHorizontalBox> Materials = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("Materials", "Materials"))]];
    const TPair<ESCMaterialMode, FText> Modes[] = {{ESCMaterialMode::Textures, LOCTEXT("Textures", "Textures")},
                                                   {ESCMaterialMode::Colors, LOCTEXT("Colours", "Colours")},
                                                   {ESCMaterialMode::None, LOCTEXT("NoMaterials", "None")}};
    for (const auto& Mode : Modes)
        Materials->AddSlot().AutoWidth().Padding(0, 0, 2, 0)
        [
            SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([O, M = Mode.Key] { return O() && O()->MaterialMode == M ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([O, M = Mode.Key](ECheckBoxState) { if (O()) O()->MaterialMode = M; })
            [SNew(SBox).Padding(FMargin(5, 1))[SNew(STextBlock).Font(Font(9)).Text(Mode.Value)]]
        ];
    // Viewer dirt (WP-U2b): live on exterior surfaces, saved with the user settings; not a load option.
    TWeakObjectPtr<USCViewerSubsystem> WeakSubsystem = Subsystem;
    auto PushDirt = [WeakSubsystem](bool bSave) {
        FSCDirt::Push(WeakSubsystem.IsValid() ? WeakSubsystem->GetWorld() : nullptr);
        if (bSave) USCViewerSettings::Get()->Save();
    };
    TSharedRef<SHorizontalBox> DirtColours = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(78)];
    for (const FSCDirt::FPreset& Preset : FSCDirt::Presets())
        DirtColours->AddSlot().AutoWidth().Padding(0, 0, 2, 0)
        [
            SNew(SCheckBox)
            .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([C = Preset.Colour] { return USCViewerSettings::Get()->DirtColor.Equals(C) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([C = Preset.Colour, PushDirt](ECheckBoxState) {
                USCViewerSettings::Get()->DirtColor = C;
                PushDirt(true);
            })
            [SNew(SBox).Padding(FMargin(5, 1))[SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Preset.Name))]]
        ];
    TSharedRef<SHorizontalBox> Dirt = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("Dirt", "Dirt"))]]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SSlider)
            .Value_Lambda([] { return USCViewerSettings::Get()->DirtAmount; })
            .OnValueChanged_Lambda([PushDirt](float Value) {
                USCViewerSettings::Get()->DirtAmount = Value;
                PushDirt(false);
            })
            .OnMouseCaptureEnd_Lambda([PushDirt] { PushDirt(true); })
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
        [
            SNew(SBox).WidthOverride(34)
            [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text_Lambda([] {
                return FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(USCViewerSettings::Get()->DirtAmount * 100)));
            })]
        ];
    TSharedRef<SHorizontalBox> Underside = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("DirtUnderside", "Underside"))]]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SSlider)
            .Value_Lambda([] { return USCViewerSettings::Get()->DirtUnderside; })
            .OnValueChanged_Lambda([PushDirt](float Value) {
                USCViewerSettings::Get()->DirtUnderside = Value;
                PushDirt(false);
            })
            .OnMouseCaptureEnd_Lambda([PushDirt] { PushDirt(true); })
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
        [
            SNew(SBox).WidthOverride(34)
            [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text_Lambda([] {
                return FText::FromString(FString::Printf(TEXT("%d %%"), FMath::RoundToInt(USCViewerSettings::Get()->DirtUnderside * 100)));
            })]
        ];
    // Environment (LOOK-3): a game hangar from the archive, or the map's procedural hangar; saved, applied at once.
    auto EnvironmentLabel = [](const FString& Request) {
        if (Request.IsEmpty() || Request == TEXT("none")) return FString(TEXT("Procedural hangar"));
        const FString Clean = Request.Replace(TEXT("\\"), TEXT("/"));
        return FPaths::GetBaseFilename(FPaths::GetPath(Clean)) + TEXT(" / ") + FPaths::GetBaseFilename(Clean);
    };
    auto PickEnvironment = [WeakSubsystem](const FString& Request) {
        USCViewerSettings::Get()->Environment = Request;
        USCViewerSettings::Get()->Save();
        if (WeakSubsystem.IsValid()) WeakSubsystem->SetEnvironment(Request, true);
    };
    TSharedRef<SHorizontalBox> Environment = SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("Environment", "Environment"))]]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SComboButton)
            .ButtonContent()[SNew(STextBlock).Font(Font(9)).Text_Lambda([WeakSubsystem, EnvironmentLabel] {
                if (!WeakSubsystem.IsValid()) return FText::GetEmpty();
                const FString Label = EnvironmentLabel(WeakSubsystem->GetEnvironmentRequest());
                return FText::FromString(WeakSubsystem->IsEnvironmentLoading() ? Label + TEXT(" (loading)") : Label);
            })]
            .OnGetMenuContent_Lambda([WeakSubsystem, EnvironmentLabel, PickEnvironment] {
                FMenuBuilder Menu(true, nullptr);
                Menu.AddMenuEntry(FText::FromString(EnvironmentLabel(FString())), FText::GetEmpty(), FSlateIcon(),
                                  FUIAction(FExecuteAction::CreateLambda([PickEnvironment] { PickEnvironment(TEXT("none")); })));
                if (WeakSubsystem.IsValid())
                    for (const FString& Choice : WeakSubsystem->GetEnvironmentChoices())
                        Menu.AddMenuEntry(FText::FromString(EnvironmentLabel(Choice)), FText::FromString(Choice), FSlateIcon(),
                                          FUIAction(FExecuteAction::CreateLambda([PickEnvironment, Choice] { PickEnvironment(Choice); })));
                return SNew(SBox).MaxDesiredHeight(480)[Menu.MakeWidget()];
            })
        ];
    // Paint (tint system): the loaded vehicle's paints (paints.json); shown only when it has any. Remembered per vehicle.
    auto PaintLabel = [WeakSubsystem](int32 Index) {
        const ASCVehicle* V = WeakSubsystem.IsValid() ? WeakSubsystem->GetVehicle() : nullptr;
        if (!V || Index == INDEX_NONE || !V->Manifest.Paints.IsValidIndex(Index)) return FString(TEXT("Default paint"));
        return V->Manifest.Paints[Index].Label();
    };
    TSharedRef<SHorizontalBox> Paint = SNew(SHorizontalBox)
        .Visibility_Lambda([WeakSubsystem] {
            const ASCVehicle* V = WeakSubsystem.IsValid() ? WeakSubsystem->GetVehicle() : nullptr;
            return V && !V->Manifest.Paints.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
        })
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
        [SNew(SBox).WidthOverride(72)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(LOCTEXT("Paint", "Paint"))]]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SComboButton)
            .ButtonContent()[SNew(STextBlock).Font(Font(9)).Text_Lambda([WeakSubsystem, PaintLabel] {
                const ASCVehicle* V = WeakSubsystem.IsValid() ? WeakSubsystem->GetVehicle() : nullptr;
                return FText::FromString(PaintLabel(V ? V->GetPaint() : INDEX_NONE));
            })]
            .OnGetMenuContent_Lambda([WeakSubsystem, PaintLabel] {
                FMenuBuilder Menu(true, nullptr);
                const ASCVehicle* V = WeakSubsystem.IsValid() ? WeakSubsystem->GetVehicle() : nullptr;
                for (int32 Index = INDEX_NONE; V && Index < V->Manifest.Paints.Num(); ++Index)
                    Menu.AddMenuEntry(FText::FromString(PaintLabel(Index)),
                                      FText::FromString(Index == INDEX_NONE ? V->Manifest.Root.PaletteId : V->Manifest.Paints[Index].PaletteId), FSlateIcon(),
                                      FUIAction(FExecuteAction::CreateLambda([WeakSubsystem, Index] { if (WeakSubsystem.IsValid()) WeakSubsystem->SetPaint(Index); })));
                return SNew(SBox).MaxDesiredHeight(480)[Menu.MakeWidget()];
            })
        ];
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[MakeChoice(LOCTEXT("Lod", "LOD"), {0, 1, 2, 3}, IntField(&USCViewOptions::Lod))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[MakeChoice(LOCTEXT("Mip", "Texture mip"), {0, 2, 4}, IntField(&USCViewOptions::Mip))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[Materials]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
        [
            SNew(SUniformGridPanel).SlotPadding(FMargin(0, 1))
            + SUniformGridPanel::Slot(0, 0)[MakeToggle(LOCTEXT("Attachments", "Attachments"), BoolField(&USCViewOptions::bAttachments))]
            + SUniformGridPanel::Slot(1, 0)[MakeToggle(LOCTEXT("Interior", "Interior"), BoolField(&USCViewOptions::bInterior))]
            + SUniformGridPanel::Slot(0, 1)[MakeToggle(LOCTEXT("Lights", "Lights"), BoolField(&USCViewOptions::bLights))]
            + SUniformGridPanel::Slot(1, 1)[MakeToggle(LOCTEXT("Shields", "Shields"), BoolField(&USCViewOptions::bShields))]
            + SUniformGridPanel::Slot(0, 2)[MakeToggle(LOCTEXT("Animations", "Animations"), BoolField(&USCViewOptions::bAnimations))]
            + SUniformGridPanel::Slot(1, 2)[MakeToggle(LOCTEXT("DefaultPose", "Default pose"), BoolField(&USCViewOptions::bDefaultPose))]
            + SUniformGridPanel::Slot(0, 3)[MakeToggle(LOCTEXT("Collision", "Collision"), BoolField(&USCViewOptions::bCollision))]
            + SUniformGridPanel::Slot(1, 3)
            [
                SNew(SButton).OnClicked_Lambda([O] {
                    if (O()) O()->ResetToDefaults();
                    return FReply::Handled();
                })
                [SNew(STextBlock).Font(Font(8)).Text(LOCTEXT("Defaults", "Core defaults"))]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 1)[Dirt]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[Underside]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[DirtColours]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 1)[Environment]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 1)[Paint];
}

TSharedRef<ITableRow> SSCModelBrowser::MakeRow(FHit Hit, const TSharedRef<STableViewBase>& Owner)
{
    const bool bNamed = !Hit->DisplayName.IsEmpty() && !Hit->DisplayName.StartsWith(TEXT("<"));
    return SNew(STableRow<FHit>, Owner).Padding(FMargin(4, 2))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Font(Font(10, true)).ColorAndOpacity(Text()).Text(FText::FromString(bNamed ? Hit->DisplayName : Hit->Name))]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Dim()).Text(FText::FromString(Hit->Name))]
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [Chip(FText::FromString(Hit->Category), Hit->GeometryPath.IsEmpty() ? Dim() : Accent())]
    ];
}

void SSCModelBrowser::OnQueryChanged(const FText& Text)
{
    Query = Text.ToString();
    if (SearchTimer) UnRegisterActiveTimer(SearchTimer.ToSharedRef());
    SearchTimer = RegisterActiveTimer(SearchDebounceSeconds, FWidgetActiveTimerDelegate::CreateSP(this, &SSCModelBrowser::RunSearch));
}
EActiveTimerReturnType SSCModelBrowser::RunSearch(double, float)
{
    SearchTimer.Reset();
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return EActiveTimerReturnType::Stop;
    bQueryDirty = true;
    if (S->GetArchiveState() != ESCArchiveState::Ready)
    {
        // Keep the query; Tick submits it when the archive becomes ready.
        Status = LOCTEXT("WaitingArchive", "Waiting for the archive…");
        return EActiveTimerReturnType::Stop;
    }
    bQueryDirty = false;
    const uint32 Id = ++Sequence;
    Status = LOCTEXT("Searching", "Searching…");
    TWeakPtr<SSCModelBrowser> Self = SharedThis(this);
    S->SearchAsync(Query.TrimStartAndEnd(), USCViewerSettings::Get()->SearchLimit, [Self, Id](const FSCSearchResult& Result) {
        if (auto Pinned = Self.Pin()) Pinned->OnResults(Result, Id);
    });
    return EActiveTimerReturnType::Stop;
}
void SSCModelBrowser::OnResults(const FSCSearchResult& Result, uint32 Id)
{
    if (Id != Sequence) return; // a newer query is in flight
    Hits.Reset();
    for (const FSCEntityHit& Hit : Result.Hits)
        Hits.Add(MakeShared<FSCEntityHit>(Hit));
    const int32 Limit = USCViewerSettings::Get()->SearchLimit;
    Status = !Result.Error.IsEmpty() ? Result.Error
             : FText::Format(LOCTEXT("ResultCount", "{0} {0}|plural(one=result,other=results){1} · {2} s"), Hits.Num(),
                             Hits.Num() >= Limit ? LOCTEXT("Limited", " (limit)") : FText(),
                             FText::FromString(FString::Printf(TEXT("%.3f"), Result.Seconds)));
    List->RequestListRefresh();
}
void SSCModelBrowser::Select(const FString& Entity, const FString& Label)
{
    SelectedEntity = Entity;
    SelectedLabel = Label.StartsWith(TEXT("<")) ? FString() : Label;
}
FReply SSCModelBrowser::Load()
{
    if (Subsystem.IsValid() && !SelectedEntity.IsEmpty()) Subsystem->LoadEntity(SelectedEntity, Subsystem->GetOptions());
    return FReply::Handled();
}
void SSCModelBrowser::RebuildRecents()
{
    const TArray<FString>& Names = USCViewerSettings::Get()->RecentEntities;
    RecentsBuiltFrom = GetTypeHash(FString::Join(Names, TEXT("|")));
    Recents->ClearChildren();
    if (Names.IsEmpty())
    {
        Recents->AddSlot()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Dim()).Text(LOCTEXT("NoRecents", "Loaded models appear here"))];
        return;
    }
    for (const FString& Name : Names)
        Recents->AddSlot()
        [
            SNew(SButton).ToolTipText(LOCTEXT("RecentTip", "Select (double-click a list row or press Load to view)"))
            .OnClicked_Lambda([this, Name] {
                Select(Name, FString());
                return FReply::Handled();
            })
            [SNew(STextBlock).Font(Font(8)).Text(FText::FromString(Name))]
        ];
}
void SSCModelBrowser::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    // Archive sessions: results belong to one session. Leaving Ready clears them and re-arms the query; reaching Ready
    // submits the pending query (the first one too: an empty query lists the first indexed entities).
    const bool bReady = S->GetArchiveState() == ESCArchiveState::Ready;
    if (bWasReady && !bReady)
    {
        ++Sequence; // a reply still in flight belongs to the old session
        Hits.Reset();
        List->RequestListRefresh();
        bQueryDirty = true;
    }
    bWasReady = bReady;
    if (bReady && bQueryDirty && !SearchTimer) RunSearch(0, 0);
    if (RecentsBuiltFrom != int32(GetTypeHash(FString::Join(USCViewerSettings::Get()->RecentEntities, TEXT("|"))))) RebuildRecents();
}

#undef LOCTEXT_NAMESPACE
