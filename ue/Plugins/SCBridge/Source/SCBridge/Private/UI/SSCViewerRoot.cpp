// SSCViewerRoot: layout, archive picker overlay (IDesktopPlatform), camera mode buttons, footer statistics.
#include "SSCViewerRoot.h"
#include "SCViewerStyle.h"
#include "SSCHierarchyPanel.h"
#include "SSCModelBrowser.h"
#include "SSCViewportInput.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCViewOptions.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/ViewportSplitScreen.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#if SC_WITH_DESKTOP_PLATFORM
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#endif

#define LOCTEXT_NAMESPACE "SCViewerRoot"
using namespace SCViewerStyle;

namespace
{
constexpr float SidePanelWidth = 380.f, HierarchyWidth = 400.f;
constexpr float SystemSampleSeconds = 1.f;
} // namespace

void SSCViewerRoot::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    SampleSystem(0, 0);
    RegisterActiveTimer(SystemSampleSeconds, FWidgetActiveTimerDelegate::CreateSP(this, &SSCViewerRoot::SampleSystem));
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[MakeTopBar()]
        + SVerticalBox::Slot().FillHeight(1.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(SidePanelWidth)[SNew(SSCModelBrowser).Subsystem(Subsystem.Get())]]
            + SHorizontalBox::Slot().FillWidth(1.f)
            [
                SAssignNew(View, SOverlay)
                + SOverlay::Slot()[SNew(SSCViewportInput).Subsystem(Subsystem.Get())]
                + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(10)
                [
                    SNew(STextBlock).Font(Font(8)).ColorAndOpacity(FLinearColor(1, 1, 1, 0.55f)).Visibility(EVisibility::HitTestInvisible)
                    .Text_Lambda([this] {
                        const bool bFly = Subsystem.IsValid() && Subsystem->GetCameraMode() == ESCCameraMode::Fly;
                        return bFly ? LOCTEXT("FlyHelp", "Drag: look · WASD move · E/Q up/down · Shift fast · Ctrl slow · wheel: speed · double-click: focus")
                                    : LOCTEXT("OrbitHelp", "Drag: orbit · Shift/middle drag: pan · wheel: zoom · double-click: focus part · F: frame all");
                    })
                ]
                + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[MakeArchiveOverlay()]
            ]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(HierarchyWidth)[SNew(SSCHierarchyPanel).Subsystem(Subsystem.Get())]]
        ]
        + SVerticalBox::Slot().AutoHeight()[MakeFooter()]
    ];
}

SSCViewerRoot::~SSCViewerRoot()
{
    SetPlayerViewRect(FVector2D::ZeroVector, FVector2D::UnitVector);
}
void SSCViewerRoot::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    if (!bOfferedPicker && Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed)
    {
        bOfferedPicker = true; // discovery failed: offer the native picker right away (outside this tick)
        RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float) {
            PickArchive();
            return EActiveTimerReturnType::Stop;
        }));
    }
    const FGeometry& ViewGeometry = View->GetTickSpaceGeometry();
    const FVector2D RootSize = Geometry.GetAbsoluteSize();
    if (RootSize.X < 1 || RootSize.Y < 1) return;
    SetPlayerViewRect((ViewGeometry.GetAbsolutePosition() - Geometry.GetAbsolutePosition()) / RootSize, ViewGeometry.GetAbsoluteSize() / RootSize);
}
void SSCViewerRoot::SetPlayerViewRect(const FVector2D& Origin, const FVector2D& Size)
{
    UGameViewportClient* Client = GEngine ? GEngine->GameViewport : nullptr;
    if (!Client || !Client->SplitscreenInfo.IsValidIndex(ESplitScreenType::None) ||
        Client->SplitscreenInfo[ESplitScreenType::None].PlayerData.IsEmpty())
        return;
    // Single-player layout entry; UGameViewportClient::LayoutPlayers copies it to the local player every frame.
    Client->SplitscreenInfo[ESplitScreenType::None].PlayerData[0] = FPerPlayerSplitscreenData(Size.X, Size.Y, Origin.X, Origin.Y);
}

TSharedRef<SWidget> SSCViewerRoot::MakeCameraButton(const FText& Label, uint8 Mode, const FText& Tip)
{
    const ESCCameraMode Target = ESCCameraMode(Mode);
    return SNew(SCheckBox)
        .Style(FCoreStyle::Get(), "ToggleButtonCheckbox")
        .ToolTipText(Tip)
        .IsEnabled(Target != ESCCameraMode::FirstPerson) // TODO(WP-P1): enable with the first-person character
        .IsChecked_Lambda([this, Target] {
            return Subsystem.IsValid() && Subsystem->GetCameraMode() == Target ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
        })
        .OnCheckStateChanged_Lambda([this, Target](ECheckBoxState) {
            if (Subsystem.IsValid()) Subsystem->SetCameraMode(Target);
        })
        [SNew(SBox).Padding(FMargin(8, 2))[SNew(STextBlock).Font(Font(9)).Text(Label)]];
}

TSharedRef<SWidget> SSCViewerRoot::MakeTopBar()
{
    return SNew(SBorder).BorderImage(Fill()).BorderBackgroundColor(Bar()).Padding(FMargin(12, 6))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 16, 0)
        [SNew(STextBlock).Font(Font(12, true)).ColorAndOpacity(Text()).Text(LOCTEXT("Title", "SC Model Viewer"))]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(STextBlock).Font(Font(9)).ColorAndOpacity_Lambda([this] {
                const ESCArchiveState State = Subsystem.IsValid() ? Subsystem->GetArchiveState() : ESCArchiveState::Closed;
                return FSlateColor(State == ESCArchiveState::Ready ? Dim() : State == ESCArchiveState::Failed ? Bad() : Warn());
            })
            .Text_Lambda([this] {
                if (!Subsystem.IsValid()) return FText();
                const FString Label = Subsystem->GetArchiveLabel();
                switch (Subsystem->GetArchiveState())
                {
                case ESCArchiveState::Ready: return FText::Format(LOCTEXT("ArchiveReady", "Archive: {0}"), FText::FromString(Label));
                case ESCArchiveState::Opening: return FText::Format(LOCTEXT("ArchiveOpening", "Opening {0}…"), FText::FromString(Label));
                case ESCArchiveState::Failed: return LOCTEXT("ArchiveFailed", "No archive");
                default: return FText();
                }
            })
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0)
        [
            SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Dim()).Text_Lambda([this] {
                ASCViewerPawn* Pawn = Subsystem.IsValid() ? Subsystem->GetViewerPawn() : nullptr;
                if (!Pawn || Subsystem->GetCameraMode() != ESCCameraMode::Fly) return FText();
                return FText::FromString(FString::Printf(TEXT("speed %.1f m/s"), Pawn->GetFlySpeed() / 100.f));
            })
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
        [MakeCameraButton(LOCTEXT("Orbit", "Orbit"), uint8(ESCCameraMode::Orbit), LOCTEXT("OrbitTip", "Orbit around the model; fits it on load"))]
        + SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
        [MakeCameraButton(LOCTEXT("Fly", "Fly"), uint8(ESCCameraMode::Fly), LOCTEXT("FlyTip", "Free-fly camera: drag to look, WASD/QE to move"))]
        + SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
        [MakeCameraButton(LOCTEXT("FirstPerson", "First person"), uint8(ESCCameraMode::FirstPerson),
                          LOCTEXT("FirstPersonTip", "Walk the ship with collision (not available yet)"))]
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
        [
            SNew(SButton)
            .Visibility_Lambda([this] { return Subsystem.IsValid() && USCViewerSubsystem::KeepHandler().IsBound() ? EVisibility::Visible : EVisibility::Collapsed; })
            .IsEnabled_Lambda([this] { return Subsystem.IsValid() && Subsystem->CanKeep(); })
            .ToolTipText(LOCTEXT("KeepTip", "Save the loaded objects as project assets under /Game/SC/<entity>/"))
            .OnClicked_Lambda([this] {
                if (Subsystem.IsValid()) Notice = FText::FromString(Subsystem->Keep().Message);
                return FReply::Handled();
            })
            [SNew(STextBlock).Font(Font(9, true)).Text(LOCTEXT("Keep", "Keep as assets"))]
        ]
    ];
}

TSharedRef<SWidget> SSCViewerRoot::MakeArchiveOverlay()
{
    return SNew(SBorder)
        .Visibility_Lambda([this] {
            return Subsystem.IsValid() && Subsystem->GetArchiveState() != ESCArchiveState::Ready ? EVisibility::Visible : EVisibility::Collapsed;
        })
        .BorderImage(Fill()).BorderBackgroundColor(Bar()).Padding(24)
        [
            SNew(SBox).WidthOverride(520)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [SNew(STextBlock).Font(Font(14, true)).ColorAndOpacity(Text()).Text(LOCTEXT("ArchiveTitle", "Star Citizen archive"))]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [
                    SNew(STextBlock).Font(Font(10)).AutoWrapText(true).ColorAndOpacity_Lambda([this] {
                        return FSlateColor(Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed ? Bad() : Text());
                    })
                    .Text_Lambda([this] {
                        if (!Subsystem.IsValid()) return FText();
                        if (Subsystem->GetArchiveState() == ESCArchiveState::Failed)
                            return FText::Format(LOCTEXT("OpenFailed", "Could not open the archive: {0}\n\nChoose Data.p4k from a Star Citizen installation."),
                                                 FText::FromString(Subsystem->GetArchiveError()));
                        return FText::Format(LOCTEXT("Opening", "Opening {0} and loading the entity index…\nCache: {1}"),
                                             FText::FromString(Subsystem->GetArchiveLabel()),
                                             FText::FromString(Subsystem->GetCacheDir().IsEmpty() ? TEXT("disabled") : Subsystem->GetCacheDir()));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    .Visibility_Lambda([this] {
                        return Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [SNew(SButton).OnClicked(this, &SSCViewerRoot::PickArchive)[SNew(STextBlock).Font(Font(10, true)).Text(LOCTEXT("Choose", "Choose Data.p4k…"))]]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton).OnClicked_Lambda([this] {
                            if (Subsystem.IsValid()) Subsystem->OpenArchive(FString());
                            return FReply::Handled();
                        })
                        [SNew(STextBlock).Font(Font(10)).Text(LOCTEXT("Retry", "Retry"))]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Warn()).AutoWrapText(true).Text_Lambda([this] { return Notice; })]
            ]
        ];
}

FReply SSCViewerRoot::PickArchive()
{
#if SC_WITH_DESKTOP_PLATFORM
    TArray<FString> Files;
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop && Desktop->OpenFileDialog(Parent, TEXT("Choose Star Citizen Data.p4k"), FString(), TEXT("Data.p4k"),
                                           TEXT("Star Citizen archive (*.p4k)|*.p4k"), EFileDialogFlags::None, Files) &&
        !Files.IsEmpty() && Subsystem.IsValid())
        Subsystem->OpenArchive(FPaths::ConvertRelativePathToFull(Files[0]));
#else
    Notice = LOCTEXT("NoPicker", "No file dialog in this build; start with -SCArchive=<path to Data.p4k>.");
#endif
    return FReply::Handled();
}

TSharedRef<SWidget> SSCViewerRoot::MakeFooter()
{
    return SNew(SBorder).BorderImage(Fill()).BorderBackgroundColor(Bar()).Padding(FMargin(12, 4))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text(this, &SSCViewerRoot::FooterLoadText)]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Dim()).Text(this, &SSCViewerRoot::FooterSystemText)]
    ];
}

FText SSCViewerRoot::FooterLoadText() const
{
    if (!Subsystem.IsValid()) return FText();
    if (!Notice.IsEmpty() && Subsystem->GetArchiveState() == ESCArchiveState::Ready) return Notice;
    const FSCLoadReport& R = Subsystem->GetLastReport();
    if (R.Entity.IsEmpty() || Subsystem->IsBusy()) return LOCTEXT("NoLoad", "—");
    if (R.bReused) return FText::FromString(FString::Printf(TEXT("%s · reused loaded objects (same entity and options)"), *R.Entity));
    FString Text = FString::Printf(TEXT("%s · export %.2f s (%s) · parse %.2f s · mesh build %.2f s · collision %.2f s · materials %.2f s · total %.2f s"),
                                   *R.Entity, R.ExportSeconds, R.bExportCacheHit ? TEXT("cache hit") : TEXT("cache miss"), R.ParseSeconds,
                                   R.BuildSeconds, R.CollisionSeconds, R.MaterialSeconds, R.TotalSeconds);
    if (R.Result == ESCLoadPhase::Failed) Text = FString::Printf(TEXT("%s · failed: %s"), *R.Entity, *R.Error);
    if (R.Result == ESCLoadPhase::Cancelled) Text = FString::Printf(TEXT("%s · cancelled"), *R.Entity);
    return FText::FromString(Text);
}

FText SSCViewerRoot::FooterSystemText() const
{
    if (!Subsystem.IsValid()) return FText();
    const FSCCacheStats& Cache = Subsystem->GetCacheStats();
    const FString CacheText = Cache.bEnabled ? FString::Printf(TEXT("cache %s / %llu entries"), *Bytes(Cache.Bytes), Cache.Entries) : TEXT("cache off");
    return FText::FromString(FString::Printf(TEXT("%s · memory %s used, %s free · disk %s free of %s"), *CacheText, *Bytes(UsedMemory),
                                             *Bytes(FreeMemory), *Bytes(DiskFree), *Bytes(DiskTotal)));
}

EActiveTimerReturnType SSCViewerRoot::SampleSystem(double, float)
{
    const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
    UsedMemory = Stats.UsedPhysical;
    FreeMemory = Stats.AvailablePhysical;
    // Headroom of the volume holding the cache (the launch directory when the cache is disabled).
    const FString Dir = Subsystem.IsValid() && !Subsystem->GetCacheDir().IsEmpty() ? Subsystem->GetCacheDir() : FString(FPlatformMisc::LaunchDir());
    FPlatformMisc::GetDiskTotalAndFreeSpace(Dir, DiskTotal, DiskFree);
    return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
