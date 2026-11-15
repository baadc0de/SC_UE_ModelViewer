// mobiGlas / callout design tokens (WP-K1c), the single home of every colour, type size, metric and timing of the
// Cargo app and the world-space callouts: docs/cargo/research-starfield-ui.md §7. Values are 1080p reference pixels
// (= Slate units at DPI scale 1); Slate's DPI curve scales them. Fonts are the engine's Roboto (FCoreStyle).
#pragma once
#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SWidget.h"

namespace SCMobiGlasStyle
{
// ---- Colour (§7 Colour) ------------------------------------------------------------------------------------------
FLinearColor Paper();     // #ECEAE4 panel fill
FLinearColor PaperDim();  // #D6D3CB secondary fill, bar track, disabled
FLinearColor Ink();       // #1B1C20 text on paper
FLinearColor Ink2();      // #5B5D64 secondary text, units
FLinearColor Rule();      // #A9A69E 1 px dividers on paper
FLinearColor Scrim();     // #0C0E12 @ 72 % backdrop
FLinearColor ScrimOpen(); // the same ink at 40 %: behind the mobiGlas, whose centre keeps the world (and callouts) visible
FLinearColor ScrimSolid(); // the scrim ink, opaque: fills inside 1 px on-scrim frames (key caps, apps rail)
FLinearColor OnScrim();   // #F2F1EC text and lines over the world
FLinearColor Accent();    // #3A78C2 interactable, selection, target
FLinearColor Ok();        // #5FAF6A loaded, complete
FLinearColor Warn();      // #E3A23A in progress, near capacity
FLinearColor Alert();     // #C8452F blocked, refused
FLinearColor Stripe(int32 Index); // 0..3: #B4422F #DB7A2C #E5B43C #3B6EA8
/** Commodity fill palette (UI tokens, not game data); index assigned per commodity by the model. */
FLinearColor Commodity(int32 Index);
int32 NumCommodityColours();
/** Ink or paper, whichever reads on Fill (text on commodity boxes). */
FLinearColor TextOn(const FLinearColor& Fill);

// ---- Type scale (§7, px at 1080p; Slate sizes are points = px * 0.75) ------------------------------------------------
FSlateFontInfo Display();   // 32/36 bold, UPPER +6 %
FSlateFontInfo Title();     // 22/28 bold, UPPER +8 %
FSlateFontInfo Section();   // 15/20 medium, UPPER +14 %
FSlateFontInfo Body();      // 16/22 regular
FSlateFontInfo Label();     // 13/16 medium, UPPER +12 %
FSlateFontInfo NumericLg(); // 28/32 medium (Roboto figures are tabular)
FSlateFontInfo Numeric();   // 16/20 mono
/** Upper-case for the caps tokens (display, title, section, label). */
FText Caps(const FText& Text);
FString Caps(const FString& Text);

// ---- Geometry (§7 Geometry) ------------------------------------------------------------------------------------------
inline constexpr float GridUnit = 8, Pad = 16, Gutter = 24, SafeMargin = 48;
inline constexpr float RuleWidth = 1, BarHeight = 6, FocusWidth = 2;
inline constexpr float StripeWidth = 4, StripeHeight = 24, StripeGap = 2;
inline constexpr float RailWidth = 72, SideWidth = 440, RowGap = 8;
inline constexpr float MinLayoutWidth = 1600; // the four panels need this much width at scale 1 (narrower views scale down)
inline constexpr float LatticeMaxCell = 96, LatticeHeight = 96;  // grid-detail lattice: largest cell, least height (it fills the panel)
inline constexpr float ContentsMaxHeight = 150;                   // grid-detail contents table before it scrolls
inline constexpr float HatchStep = 6;                             // blocked-cell hatching pitch

// ---- Callouts (§7 World-space callouts) ------------------------------------------------------------------------------
inline constexpr float CalloutDot = 5, CalloutHalo = 1, LeaderWidth = 1.5f, LeaderSelectedWidth = 2;
inline constexpr float CalloutMargin = 16, CalloutMinRun = 24, CalloutMaxRun = 64, CalloutMinShelf = 16, CalloutGap = 6, CalloutMaxRise = 160;
inline constexpr float CalloutPadX = 8, CalloutPadY = 4, CalloutBarGap = 4;
inline constexpr int32 CalloutMaxLabels = 8;
inline constexpr float CalloutScreenInset = 8; // labels stay this far inside the view

// ---- Motion (§7 Motion), seconds --------------------------------------------------------------------------------------
inline constexpr float CalloutLineIn = 0.14f, CalloutFadeIn = 0.12f, CalloutSlide = 6, CalloutOut = 0.09f;
// Callouts are re-solved every frame (painted placements are always checked ones); the side hysteresis is the solver's
// "keep the previous side unless it collides" rule.
inline constexpr float PanelIn = 0.2f, PanelOut = 0.14f, PanelSlide = 12, BarTau = 0.12f;
float EaseOutCubic(float T);
float EaseInQuad(float T);

// ---- Brushes and widget styles ---------------------------------------------------------------------------------------
const FSlateBrush* White();
const FButtonStyle& FlatButton();    // paper; hover paper-dim; ink foreground
const FButtonStyle& PrimaryButton(); // accent fill, paper foreground; disabled paper-dim / ink-2
const FButtonStyle& RowButton();     // transparent; hover paper-dim
const FButtonStyle& RailButton();    // transparent on scrim
const FSliderStyle& Slider();        // 6 px bar, 4 x 18 px ink thumb

// ---- Small factories -------------------------------------------------------------------------------------------------
/** The four-stripe motif (vertical bars); at most once per panel. */
TSharedRef<SWidget> StripeMotif();
/** 1 px horizontal rule in the rule colour (or on-scrim at 30 %). */
TSharedRef<SWidget> HRule(bool bOnScrim = false);
/** Panel: paper fill, 0 radius, 16 px padding, section header with an optional right-hand note. */
TSharedRef<SWidget> Panel(const FText& Title, const TAttribute<FText>& Note, const TSharedRef<SWidget>& Body);
/** Text in a type token and colour. */
TSharedRef<SWidget> Txt(const TAttribute<FText>& Text, const FSlateFontInfo& Font, const TAttribute<FSlateColor>& Colour);
/** Status chip: 1 px outline + caps label in Colour (colour always pairs with text, §7). */
TSharedRef<SWidget> Chip(const TAttribute<FText>& Text, const TAttribute<FSlateColor>& Colour);
} // namespace SCMobiGlasStyle
