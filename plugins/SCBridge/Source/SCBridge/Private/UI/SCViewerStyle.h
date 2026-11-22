// Shared look of the viewer's Slate UI: fonts, panel colours and small widget factories (runtime CoreStyle only).
#pragma once
#include "CoreMinimal.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"

namespace SCViewerStyle
{
inline FSlateFontInfo Font(int32 Size, bool bBold = false)
{
    return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
}
inline const FSlateBrush* Fill() { return FCoreStyle::Get().GetBrush("GenericWhiteBox"); }
inline FLinearColor Panel() { return FLinearColor(0.012f, 0.014f, 0.018f, 0.88f); }
inline FLinearColor Bar() { return FLinearColor(0.006f, 0.007f, 0.009f, 0.94f); }
inline FLinearColor Row() { return FLinearColor(1, 1, 1, 0.04f); }
inline FLinearColor Text() { return FLinearColor(0.86f, 0.88f, 0.9f); }
inline FLinearColor Dim() { return FLinearColor(0.45f, 0.48f, 0.52f); }
inline FLinearColor Accent() { return FLinearColor(0.25f, 0.62f, 1.0f); }
inline FLinearColor Good() { return FLinearColor(0.35f, 0.8f, 0.45f); }
inline FLinearColor Warn() { return FLinearColor(0.95f, 0.7f, 0.25f); }
inline FLinearColor Bad() { return FLinearColor(0.95f, 0.35f, 0.3f); }

/** Upper-case section caption. */
TSharedRef<SWidget> Header(const FText& Text);
/** Rounded-less chip with a tinted background, used for badges. */
TSharedRef<SWidget> Chip(const FText& Text, const FLinearColor& Color);
/** 12345 -> "12.3k", 1234567 -> "1.23M". */
FString Compact(int64 Value);
/** Bytes -> "312 MB" / "57.1 GB". */
FString Bytes(uint64 Value);
} // namespace SCViewerStyle
