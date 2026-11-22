// SCHudStyle: token values, fonts, brushes, widget styles and the small paper-panel factories.
#include "UI/SCHudStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace SCHudStyle
{
namespace
{
FLinearColor Hex(const TCHAR* Value, float Alpha = 1)
{
    // sRGB hex -> linear; Slate converts back on output, so the screen shows the token's hex.
    return FLinearColor(FColor::FromHex(Value)).CopyWithNewOpacity(Alpha);
}
/** px at 1080p -> Slate font size (points at 96 DPI). LetterSpacing is in 1/1000 em. */
FSlateFontInfo Font(const char* Typeface, float Px, int32 Tracking)
{
    FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(Typeface, 10);
    Info.Size = Px * 0.75f;
    Info.LetterSpacing = Tracking;
    return Info;
}
FSlateColorBrush& Solid(const FLinearColor& Colour)
{
    // Brushes live for the process (styles reference them by value, widgets by pointer).
    static TArray<TUniquePtr<FSlateColorBrush>> Brushes;
    return *Brushes.Add_GetRef(MakeUnique<FSlateColorBrush>(Colour));
}
FButtonStyle MakeButton(const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Pressed, const FLinearColor& Disabled,
                        const FLinearColor& Fore, const FLinearColor& DisabledFore)
{
    return FButtonStyle()
        .SetNormal(Solid(Normal)).SetHovered(Solid(Hover)).SetPressed(Solid(Pressed)).SetDisabled(Solid(Disabled))
        .SetNormalForeground(Fore).SetHoveredForeground(Fore).SetPressedForeground(Fore).SetDisabledForeground(DisabledFore)
        .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
}
} // namespace

FLinearColor Paper() { return Hex(TEXT("ECEAE4")); }
FLinearColor PaperDim() { return Hex(TEXT("D6D3CB")); }
FLinearColor Ink() { return Hex(TEXT("1B1C20")); }
FLinearColor Ink2() { return Hex(TEXT("5B5D64")); }
FLinearColor Rule() { return Hex(TEXT("A9A69E")); }
FLinearColor Scrim() { return Hex(TEXT("0C0E12"), 0.72f); }
FLinearColor ScrimOpen() { return Hex(TEXT("0C0E12"), 0.40f); }
FLinearColor ScrimSolid() { return Hex(TEXT("0C0E12")); }
FLinearColor OnScrim() { return Hex(TEXT("F2F1EC")); }
FLinearColor Accent() { return Hex(TEXT("3A78C2")); }
FLinearColor Ok() { return Hex(TEXT("5FAF6A")); }
FLinearColor Warn() { return Hex(TEXT("E3A23A")); }
FLinearColor Alert() { return Hex(TEXT("C8452F")); }
FLinearColor Stripe(int32 Index)
{
    static const TCHAR* Values[] = {TEXT("B4422F"), TEXT("DB7A2C"), TEXT("E5B43C"), TEXT("3B6EA8")};
    return Hex(Values[FMath::Clamp(Index, 0, 3)]);
}
FLinearColor Commodity(int32 Index)
{
    // Muted industrial hues that stay apart on paper and under each other's labels.
    static const TCHAR* Values[] = {TEXT("3B6EA8"), TEXT("DB7A2C"), TEXT("5FAF6A"), TEXT("B4422F"),
                                    TEXT("E5B43C"), TEXT("7A5C9E"), TEXT("3E8E8A"), TEXT("8C8778")};
    return Hex(Values[((Index % 8) + 8) % 8]);
}
int32 NumCommodityColours() { return 8; }
FLinearColor TextOn(const FLinearColor& Fill)
{
    return Fill.GetLuminance() > 0.25f ? Ink() : Paper(); // linear luminance; 0.25 ~ mid-grey on screen
}

FSlateFontInfo Display() { return Font("Bold", 32, 60); }
FSlateFontInfo Title() { return Font("Bold", 22, 80); }
FSlateFontInfo Section() { return Font("Medium", 15, 140); }
FSlateFontInfo Body() { return Font("Regular", 16, 0); }
FSlateFontInfo Label() { return Font("Medium", 13, 120); }
FSlateFontInfo NumericLg() { return Font("Medium", 28, 0); }
FSlateFontInfo Numeric() { return Font("Mono", 16, 0); }
FText Caps(const FText& Text) { return Text.ToUpper(); }
FString Caps(const FString& Text) { return Text.ToUpper(); }

float EaseOutCubic(float T)
{
    T = FMath::Clamp(T, 0.f, 1.f);
    return 1 - FMath::Pow(1 - T, 3.f);
}
float EaseInQuad(float T)
{
    T = FMath::Clamp(T, 0.f, 1.f);
    return T * T;
}

const FSlateBrush* White() { return FCoreStyle::Get().GetBrush("WhiteBrush"); }
const FButtonStyle& FlatButton()
{
    static const FButtonStyle Style = MakeButton(Paper(), PaperDim(), Rule(), Paper(), Ink(), Rule());
    return Style;
}
const FButtonStyle& PrimaryButton()
{
    static const FButtonStyle Style = MakeButton(Accent(), Hex(TEXT("2F66A8")), Hex(TEXT("27578F")), PaperDim(), Paper(), Ink2());
    return Style;
}
const FButtonStyle& RowButton()
{
    static const FButtonStyle Style = MakeButton(FLinearColor::Transparent, PaperDim(), Rule(), FLinearColor::Transparent, Ink(), Ink2());
    return Style;
}
const FButtonStyle& RailButton()
{
    static const FButtonStyle Style =
        MakeButton(FLinearColor::Transparent, OnScrim().CopyWithNewOpacity(0.12f), OnScrim().CopyWithNewOpacity(0.2f), FLinearColor::Transparent,
                   OnScrim(), OnScrim().CopyWithNewOpacity(0.38f));
    return Style;
}
const FSliderStyle& Slider()
{
    static const FSliderStyle Style = [] {
        FSlateColorBrush& Bar = Solid(PaperDim());
        FSlateColorBrush& Thumb = Solid(Ink());
        Thumb.ImageSize = FVector2D(4, 18);
        FSlateColorBrush& ThumbHover = Solid(Accent());
        ThumbHover.ImageSize = FVector2D(4, 18);
        FSlateColorBrush& ThumbOff = Solid(Rule());
        ThumbOff.ImageSize = FVector2D(4, 18);
        return FSliderStyle()
            .SetNormalBarImage(Bar).SetHoveredBarImage(Bar).SetDisabledBarImage(Bar)
            .SetNormalThumbImage(Thumb).SetHoveredThumbImage(ThumbHover).SetDisabledThumbImage(ThumbOff)
            .SetBarThickness(BarHeight);
    }();
    return Style;
}

TSharedRef<SWidget> StripeMotif()
{
    TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox).Visibility(EVisibility::HitTestInvisible);
    for (int32 I = 0; I < 4; ++I)
        Box->AddSlot().AutoWidth().Padding(0, 0, I < 3 ? StripeGap : 0, 0)
        [SNew(SBox).WidthOverride(StripeWidth).HeightOverride(StripeHeight)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Stripe(I))]];
    return Box;
}
TSharedRef<SWidget> HRule(bool bOnScrim)
{
    return SNew(SBox).HeightOverride(RuleWidth).Visibility(EVisibility::HitTestInvisible)
        [SNew(SBorder).BorderImage(White()).BorderBackgroundColor(bOnScrim ? OnScrim().CopyWithNewOpacity(0.3f) : Rule())];
}
TSharedRef<SWidget> Txt(const TAttribute<FText>& Text, const FSlateFontInfo& Font, const TAttribute<FSlateColor>& Colour)
{
    return SNew(STextBlock).Text(Text).Font(Font).ColorAndOpacity(Colour);
}
TSharedRef<SWidget> Chip(const TAttribute<FText>& Text, const TAttribute<FSlateColor>& Colour)
{
    return SNew(SBorder).BorderImage(White()).Padding(FMargin(6, 1, 6, 1)).VAlign(VAlign_Center)
        .BorderBackgroundColor_Lambda([Colour] { return Colour.Get(); })
        [SNew(STextBlock).Font(Label()).Text(Text).ColorAndOpacity_Lambda([Colour] {
            return FSlateColor(TextOn(Colour.Get().GetSpecifiedColor()));
        })];
}
TSharedRef<SWidget> Panel(const FText& Title, const TAttribute<FText>& Note, const TSharedRef<SWidget>& Body)
{
    return SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Paper()).Padding(FMargin(Pad))
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            // The four-stripe motif, once per panel, beside its title (as in the mock).
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)[StripeMotif()]
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
              [SNew(STextBlock).Font(Section()).ColorAndOpacity(Ink()).Text(Caps(Title)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)[Txt(Note, Label(), Ink2())]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, Pad - 4)[HRule()]
        + SVerticalBox::Slot().FillHeight(1)[Body]
    ];
}
} // namespace SCHudStyle
