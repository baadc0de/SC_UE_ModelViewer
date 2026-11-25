// SSCViewerRoot: layout, archive picker overlay (IDesktopPlatform), camera mode buttons, footer statistics.
#include "SSCViewerRoot.h"
#include "SCViewerStyle.h"
#include "SSCViewportInput.h"
#include "SSCFirstPersonHUD.h"
#include "SCViewerShell.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCViewOptions.h"
#include "RenderUtils.h"
#include "SCNativeFileDialog.h"
#include "SCViewerSettings.h"
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
#include "Widgets/Notifications/SProgressBar.h"
#if SC_WITH_DESKTOP_PLATFORM
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#endif

#define LOCTEXT_NAMESPACE "SCViewerRoot"
using namespace SCViewerStyle;

namespace
{
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
        + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this] { return ChromeVisibility(); })[MakeTopBar()]]
        + SVerticalBox::Slot().FillHeight(1.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f)
            [
                SAssignNew(View, SOverlay)
                + SOverlay::Slot()[SAssignNew(ViewportInput, SSCViewportInput).Subsystem(Subsystem.Get())]
                + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(10)
                [
                    SNew(SBox).Visibility_Lambda([this] { return MenuHidden(); })
                    [
                    SNew(SBorder).BorderImage(Fill()).BorderBackgroundColor(FLinearColor(0, 0, 0, 0.5f)).Padding(FMargin(8, 4))
                    .Visibility(EVisibility::HitTestInvisible)
                    [
                    SNew(STextBlock).Font(Font(9)).ColorAndOpacity(FLinearColor(1, 1, 1, 0.85f))
                    .Text_Lambda([this] {
                        if (Subsystem.IsValid() && Subsystem->GetCameraMode() == ESCCameraMode::FirstPerson)
                            return SCViewerShell::HasMenu()
                                ? FText::Format(LOCTEXT("FirstHelpMenu", "Click view: mouse look · Esc: release cursor · F1: {0}"), SCViewerShell::MenuName())
                                : LOCTEXT("FirstHelp", "Click view: mouse look · Esc: release cursor");
                        const bool bFly = Subsystem.IsValid() && Subsystem->GetCameraMode() == ESCCameraMode::Fly;
                        return bFly ? LOCTEXT("FlyHelp", "Drag: look · WASD move · E/Q up/down · Shift fast · Ctrl slow · wheel: speed · double-click: focus")
                                    : LOCTEXT("OrbitHelp", "Drag: orbit · Shift/middle drag: pan · wheel: zoom · double-click: focus part · F: frame all");
                    })
                    ]
                    ]
                ]
                // The first-person HUD steps aside while the menu is up (it would show through the menu's scrim).
                + SOverlay::Slot()[SNew(SBox).Visibility_Lambda([this] { return MenuHidden(); })[SNew(SSCFirstPersonHUD).Subsystem(Subsystem.Get())]]
                + SOverlay::Slot()[SAssignNew(Overlays, SOverlay)]
                + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0, 0, 0, 36))[MakeLoadingOverlay()]
                + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[MakeArchiveOverlay()]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this] { return ChromeVisibility(); })[MakeFooter()]]
    ];
    // The application's layers (SCViewerShell), in registration order; a menu among them gives focus back to the view on close.
    FSCViewOverlayArgs OverlayArgs;
    OverlayArgs.Subsystem = Subsystem.Get();
    OverlayArgs.FocusView = [Weak = TWeakPtr<SWidget>(ViewportInput)] {
        if (const TSharedPtr<SWidget> Input = Weak.Pin(); Input && FSlateApplication::IsInitialized())
            FSlateApplication::Get().SetAllUserFocus(Input, EFocusCause::SetDirectly);
    };
    for (const SCViewerShell::FMakeOverlay& Make : SCViewerShell::Overlays()) Overlays->AddSlot()[Make(OverlayArgs)];
}

SSCViewerRoot::~SSCViewerRoot()
{
    SetPlayerViewRect(FVector2D::ZeroVector, FVector2D::UnitVector);
}
void SSCViewerRoot::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    // A failed open shows the archive screen (MakeArchiveOverlay): the user reads why before choosing Data.p4k there.
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
        .IsEnabled_Lambda([this, Target] { return Target != ESCCameraMode::FirstPerson ||
            (Subsystem.IsValid() && Subsystem->GetVehicle() && Subsystem->GetVehicle()->bCollisionReady && !Subsystem->IsBusy()); })
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
        [SNew(STextBlock).Font(Font(12, true)).ColorAndOpacity(Text()).Text_Lambda([] { return SCViewerShell::AppName(); })]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(STextBlock).Font(Font(9)).ColorAndOpacity_Lambda([this] {
                const ESCArchiveState State = Subsystem.IsValid() ? Subsystem->GetArchiveState() : ESCArchiveState::Closed;
                return FSlateColor(State == ESCArchiveState::Ready ? Dim() : State == ESCArchiveState::Failed ? Bad() : Warn());
            })
            .ToolTipText_Lambda([this] {
                return Subsystem.IsValid() ? FText::Format(LOCTEXT("ArchiveTip", "Game files: {0}"), FText::FromString(Subsystem->GetArchiveLabel())) : FText();
            })
            .Text_Lambda([this] {
                if (!Subsystem.IsValid()) return FText();
                switch (Subsystem->GetArchiveState())
                {
                case ESCArchiveState::Ready: return LOCTEXT("ArchiveReady", "Star Citizen files found");
                case ESCArchiveState::Opening: return LOCTEXT("ArchiveOpening", "Reading Star Citizen files…");
                case ESCArchiveState::Failed: return LOCTEXT("ArchiveFailed", "Star Citizen not found");
                default: return FText();
                }
            })
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0)
        [
            // Without hardware ray tracing (a GPU without DirectX Raytracing, or -noraytracing) MegaLights and ray-traced
            // Lumen switch off: the lights keep shadow maps (SCLightJson::MegaLightsActive and the shadow radius rule) and
            // indirect light is screen-space. Said here once, in words, instead of the renderer's debug warnings.
            SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Warn())
            .Visibility_Lambda([] { return IsRayTracingEnabled() ? EVisibility::Collapsed : EVisibility::Visible; })
            .Text(LOCTEXT("BasicLighting", "Basic lighting"))
            .ToolTipText(LOCTEXT("BasicLightingTip", "This graphics card (or its driver) has no hardware ray tracing, so the viewer "
                                                     "uses shadow maps for every light and lighting from what is on screen: bounce "
                                                     "light and reflections are simpler, and the resolution drops by itself on a "
                                                     "slower card to stay smooth. Everything else works."))
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
                          LOCTEXT("FirstPersonTip", "Walk around and inside the ship (available once a ship has loaded)"))]
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 2, 0)
        [
            // The application's menu (SCViewerShell; the viewer's picker, UnrealCitizen's mobiGlas), also F1.
            SNew(SButton)
            .Visibility_Lambda([] { return SCViewerShell::HasMenu() ? EVisibility::Visible : EVisibility::Collapsed; })
            .ToolTipText(LOCTEXT("MenuTip", "Open the menu (F1)"))
            .OnClicked_Lambda([this] {
                SCViewerShell::SetMenuOpen(Subsystem.Get(), !SCViewerShell::IsMenuOpen(Subsystem.Get()));
                return FReply::Handled();
            })
            [SNew(STextBlock).Font(Font(9)).Text_Lambda([] { return FText::Format(LOCTEXT("MenuButton", "{0} (F1)"), SCViewerShell::MenuName()); })]
        ]
        + SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
        [SNew(SButton).Text(LOCTEXT("SpawnLook", "Walk from here"))
            .ToolTipText(LOCTEXT("SpawnLookTip", "Aim the view at a spot on the ship, then click: you start walking there in first person"))
            .IsEnabled_Lambda([this] { return Subsystem.IsValid() && Subsystem->GetVehicle() && Subsystem->GetVehicle()->bCollisionReady && !Subsystem->IsBusy(); })
            .OnClicked_Lambda([this] { if (Subsystem.IsValid()) Subsystem->SpawnWhereLooking(); return FReply::Handled(); })]
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
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
        [SNew(SButton).Text(LOCTEXT("Quit", "Quit")).ToolTipText(LOCTEXT("QuitTip", "Close the viewer"))
            .OnClicked_Lambda([] { FPlatformMisc::RequestExit(false, TEXT("SSCViewerRoot.Quit")); return FReply::Handled(); })]
    ];
}

EVisibility SSCViewerRoot::ChromeVisibility() const
{
    return Subsystem.IsValid() && Subsystem->IsShowcase() ? EVisibility::Collapsed : EVisibility::Visible;
}
EVisibility SSCViewerRoot::MenuHidden() const
{
    return SCViewerShell::IsMenuOpen(Subsystem.Get()) ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible;
}

// On-screen loading bars over the 3D view: the model and the environment (hangar), each while it loads; a failed model
// load stays visible with its reason until the next load.
TSharedRef<SWidget> SSCViewerRoot::MakeLoadingOverlay()
{
    auto Row = [this](TFunction<bool()> Shown, TFunction<float()> Fraction, TFunction<FSlateColor()> Colour, TFunction<FText()> Label) {
        return SNew(SVerticalBox)
            .Visibility_Lambda([Shown] { return Shown() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 2)
            [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text()).Text_Lambda([Label] { return Label(); })]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SBox).HeightOverride(6)[SNew(SProgressBar).Percent_Lambda([Fraction] { return TOptional<float>(Fraction()); })
                                              .FillColorAndOpacity_Lambda([Colour] { return Colour(); })]];
    };
    return SNew(SBorder)
        .Visibility_Lambda([this] {
            const bool bModel = ModelRowShown();
            // A hangar waiting for an archive that failed to open says nothing useful: the archive screen speaks.
            const bool bEnv = Subsystem.IsValid() && Subsystem->IsEnvironmentLoading() && Subsystem->GetArchiveState() != ESCArchiveState::Failed;
            return bModel || bEnv ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
        })
        .BorderImage(Fill()).BorderBackgroundColor(FLinearColor(0, 0, 0, 0.55f)).Padding(FMargin(14, 6, 14, 10))
        [
            SNew(SBox).WidthOverride(520)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [Row([this] { return ModelRowShown(); },
                     [this] { return Subsystem.IsValid() ? Subsystem->GetLoadProgress() : 0.f; },
                     [this] { return FSlateColor(Subsystem.IsValid() && Subsystem->GetLoadPhase() == ESCLoadPhase::Failed ? Bad() : Accent()); },
                     [this] {
                         if (!Subsystem.IsValid()) return FText();
                         const FString Entity = Subsystem->IsBusy() ? Subsystem->GetPendingEntity() : Subsystem->GetLastReport().Entity;
                         const FString Name = VehicleName(Entity);
                         if (!Subsystem->IsBusy()) // failed
                             return FText::Format(LOCTEXT("LoadFailed", "{0} could not be loaded: {1}"), FText::FromString(Name),
                                                  FText::FromString(Subsystem->GetLastReport().Error));
                         const FText Percent = FText::AsNumber(FMath::RoundToInt(100.f * Subsystem->GetLoadProgress()));
                         if (Name.IsEmpty()) return FText::Format(LOCTEXT("LoadingOther", "Loading…  {0}%"), Percent);
                         return FText::Format(LOCTEXT("LoadingShip", "Loading {0}…  {1}%"), FText::FromString(Name), Percent);
                     })]
                + SVerticalBox::Slot().AutoHeight()
                [Row([this] { return Subsystem.IsValid() && Subsystem->IsEnvironmentLoading() && Subsystem->GetArchiveState() != ESCArchiveState::Failed; },
                     [this] { return Subsystem.IsValid() ? Subsystem->GetEnvironmentProgress() : 0.f; },
                     [] { return FSlateColor(Accent()); },
                     [this] {
                         if (!Subsystem.IsValid()) return FText();
                         return FText::Format(LOCTEXT("LoadingHangar", "Loading hangar {0}…  {1}%"),
                                              FText::FromString(EnvironmentName(Subsystem->GetEnvironmentRequest())),
                                              FText::AsNumber(FMath::RoundToInt(100.f * Subsystem->GetEnvironmentProgress())));
                     })]
            ]
        ];
}

TSharedRef<SWidget> SSCViewerRoot::MakeArchiveOverlay()
{
    return SNew(SBorder)
        .Visibility_Lambda([this] {
            // Opening or failed; a closed archive (none asked for, or no archive reader in this build) shows nothing.
            const ESCArchiveState State = Subsystem.IsValid() ? Subsystem->GetArchiveState() : ESCArchiveState::Closed;
            return Subsystem.IsValid() && !Subsystem->GetVehicle() && (State == ESCArchiveState::Opening || State == ESCArchiveState::Failed)
                ? EVisibility::Visible : EVisibility::Collapsed;
        })
        .BorderImage(Fill()).BorderBackgroundColor(Bar()).Padding(24)
        [
            SNew(SBox).WidthOverride(520)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(STextBlock).Font(Font(14, true)).ColorAndOpacity(Text()).Text_Lambda([this] {
                        return Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed
                            ? LOCTEXT("ArchiveTitleFailed", "Where is Star Citizen installed?")
                            : LOCTEXT("ArchiveTitle", "Reading your Star Citizen files…");
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [
                    SNew(STextBlock).Font(Font(10)).AutoWrapText(true).ColorAndOpacity(Text())
                    .Text_Lambda([this] {
                        if (!Subsystem.IsValid()) return FText();
                        if (Subsystem->GetArchiveState() == ESCArchiveState::Failed)
                            return LOCTEXT("OpenFailed", "The viewer could not find your Star Citizen game files. Choose the file Data.p4k in "
                                                         "the LIVE folder of your RSI library, for example\nD:\\Roberts Space Industries\\StarCitizen\\LIVE\\Data.p4k\n"
                                                         "The RSI Launcher's settings show where your library is.\n\n"
                                                         "The viewer only reads the file; your game is not changed.");
                        return LOCTEXT("Opening", "The viewer reads the list of ships and hangars from your game. "
                                                  "The first start takes longer; later starts reuse what it read.");
                    })
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    .Visibility_Lambda([this] {
                        return Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [SNew(SButton).OnClicked(this, &SSCViewerRoot::PickArchive)[SNew(STextBlock).Font(Font(10, true)).Text(LOCTEXT("Choose", "Find Data.p4k…"))]]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton).ToolTipText(LOCTEXT("RetryTip", "Look for Star Citizen in its usual install folder again"))
                        .OnClicked_Lambda([this] {
                            USCViewerSettings* Settings = USCViewerSettings::Get();
                            if (!Settings->ArchivePath.IsEmpty())
                            {
                                Settings->ArchivePath.Reset(); // the usual folder, not the last choice
                                Settings->Save();
                            }
                            if (Subsystem.IsValid()) Subsystem->OpenArchive(FString());
                            return FReply::Handled();
                        })
                        [SNew(STextBlock).Font(Font(10)).Text(LOCTEXT("Retry", "Look again"))]
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
                    [
                        SNew(SButton).ButtonStyle(FCoreStyle::Get(), "NoBorder")
                        .OnClicked_Lambda([this] { bArchiveDetails = !bArchiveDetails; return FReply::Handled(); })
                        [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Dim()).Text_Lambda([this] {
                            return bArchiveDetails ? LOCTEXT("HideDetails", "Hide details") : LOCTEXT("ShowDetails", "Details");
                        })]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [
                    SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Dim()).AutoWrapText(true)
                    .Visibility_Lambda([this] {
                        return bArchiveDetails && Subsystem.IsValid() && Subsystem->GetArchiveState() == ESCArchiveState::Failed
                            ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .Text_Lambda([this] {
                        return Subsystem.IsValid() ? FText::Format(LOCTEXT("ArchiveDetail", "Tried: {0}\n{1}"), FText::FromString(Subsystem->GetArchiveLabel()),
                                                                   FText::FromString(Subsystem->GetArchiveError()))
                                                   : FText();
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Warn()).AutoWrapText(true).Text_Lambda([this] { return Notice; })]
            ]
        ];
}

FReply SSCViewerRoot::PickArchive()
{
#if SC_WITH_DESKTOP_PLATFORM && !PLATFORM_WINDOWS // Windows: the system dialog in every build (SCNativeFileDialog)
    TArray<FString> Files;
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
    if (Desktop && Desktop->OpenFileDialog(Parent, TEXT("Choose Star Citizen Data.p4k"), FString(), TEXT("Data.p4k"),
                                           TEXT("Star Citizen archive (*.p4k)|*.p4k"), EFileDialogFlags::None, Files) &&
        !Files.IsEmpty() && Subsystem.IsValid())
        Subsystem->OpenArchive(FPaths::ConvertRelativePathToFull(Files[0]));
#else
    FString File;
    if (!SCNativeFileDialog::IsAvailable())
        Notice = LOCTEXT("NoPicker", "No file dialog in this build; start with -SCArchive=<path to Data.p4k>.");
    else if (SCNativeFileDialog::OpenFile(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
                                          TEXT("Choose Star Citizen Data.p4k"), TEXT("Star Citizen archive (Data.p4k)"), TEXT("*.p4k"), File) &&
             Subsystem.IsValid())
        Subsystem->OpenArchive(File);
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
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
        [
            SNew(SButton).ButtonStyle(FCoreStyle::Get(), "NoBorder")
            .ToolTipText(LOCTEXT("FooterDetailsTip", "Show or hide load timings, cache and memory details"))
            .OnClicked_Lambda([this] { bFooterDetails = !bFooterDetails; return FReply::Handled(); })
            [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Dim()).Text_Lambda([this] {
                return bFooterDetails ? LOCTEXT("FooterLess", "Less") : LOCTEXT("FooterMore", "Details");
            })]
        ]
    ];
}

FText SSCViewerRoot::FooterLoadText() const
{
    if (!Subsystem.IsValid()) return FText();
    if (!Notice.IsEmpty() && Subsystem->GetArchiveState() == ESCArchiveState::Ready) return Notice;
    const FSCLoadReport& R = Subsystem->GetLastReport();
    if (R.Entity.IsEmpty() || Subsystem->IsBusy()) return FText();
    const FString Name = VehicleName(R.Entity);
    if (R.Result == ESCLoadPhase::Failed) return FText::FromString(FString::Printf(TEXT("%s · failed: %s"), *Name, *R.Error));
    if (R.Result == ESCLoadPhase::Cancelled) return FText::FromString(FString::Printf(TEXT("%s · cancelled"), *Name));
    if (!bFooterDetails)
        return FText::FromString(R.bReused ? Name : FString::Printf(TEXT("%s · loaded in %.1f s"), *Name, R.TotalSeconds));
    // Developer details: the entity id, each load step, the caches (L1 exports, L2 processed meshes).
    if (R.bReused) return FText::FromString(FString::Printf(TEXT("%s · reused loaded objects (same entity and options)"), *R.Entity));
    FString Text = FString::Printf(TEXT("%s · export %.2f s (%s) · parse %.2f s · mesh build %.2f s · collision %.2f s · materials %.2f s · total %.2f s"),
                                   *R.Entity, R.ExportSeconds, R.bExportCacheHit ? TEXT("L1 hit") : TEXT("L1 miss"), R.ParseSeconds,
                                   R.BuildSeconds, R.CollisionSeconds, R.MaterialSeconds, R.TotalSeconds);
    if (Subsystem->GetProcessedCacheStats().bEnabled && R.CachedMeshes + R.MeshBuilds > 0)
        Text += FString::Printf(TEXT(" · L2 %d mesh hits, %d built"), R.CachedMeshes, R.MeshBuilds);
    if (Subsystem->GetVehicle() && Subsystem->GetVehicle()->bCollisionReady) Text += TEXT(" · collision ready");
    return FText::FromString(Text);
}

FText SSCViewerRoot::FooterSystemText() const
{
    if (!Subsystem.IsValid()) return FText();
    const FSCCacheStats& Cache = Subsystem->GetCacheStats();
    const FSCCacheStats Processed = Subsystem->GetProcessedCacheStats();
    if (!bFooterDetails)
        return FText::FromString(Cache.bEnabled ? FString::Printf(TEXT("cache %s · disk %s free"), *Bytes(Cache.Bytes + Processed.Bytes), *Bytes(DiskFree))
                                                : FString::Printf(TEXT("disk %s free"), *Bytes(DiskFree)));
    const FString CacheText = Cache.bEnabled ? FString::Printf(TEXT("L1 %s · L2 %s"), *Bytes(Cache.Bytes), *Bytes(Processed.Bytes)) : TEXT("cache off");
    return FText::FromString(FString::Printf(TEXT("%s · memory %s used, %s free · disk %s free of %s"), *CacheText, *Bytes(UsedMemory),
                                             *Bytes(FreeMemory), *Bytes(DiskFree), *Bytes(DiskTotal)));
}

bool SSCViewerRoot::ModelRowShown() const
{
    // A load of a named entity, running or failed; work without one (none pending) has no row.
    if (!Subsystem.IsValid()) return false;
    if (Subsystem->IsBusy()) return !Subsystem->GetPendingEntity().IsEmpty();
    return Subsystem->GetLoadPhase() == ESCLoadPhase::Failed;
}

FString SSCViewerRoot::VehicleName(const FString& Entity) const
{
    const FSCCatalogEntry* Row = Subsystem.IsValid() ? Subsystem->GetCatalog().Vehicles.FindByPredicate([&](const FSCCatalogEntry& E) {
        return E.Id.Equals(Entity, ESearchCase::IgnoreCase);
    }) : nullptr;
    return Row && !Row->Label.IsEmpty() ? Row->Label : Entity;
}

FString SSCViewerRoot::EnvironmentName(const FString& Request) const
{
    const FString Wanted = Request.Replace(TEXT("\\"), TEXT("/"));
    const FSCCatalogEntry* Row = Subsystem.IsValid() ? Subsystem->GetCatalog().Environments.FindByPredicate([&](const FSCCatalogEntry& E) {
        return E.Id.Replace(TEXT("\\"), TEXT("/")).Equals(Wanted, ESearchCase::IgnoreCase);
    }) : nullptr;
    return Row && !Row->Label.IsEmpty() ? Row->Label : FPaths::GetBaseFilename(Request).Replace(TEXT("_"), TEXT(" "));
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
