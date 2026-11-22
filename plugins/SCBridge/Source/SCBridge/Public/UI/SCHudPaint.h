// Paint helpers of the shared HUD (WP-K1c, shared since WP-O1-2): flat boxes, frames, polylines, text, hatching and text
// measurement, in the units of SCHudStyle. Used by the interaction popout, the mobiGlas widgets and the world callouts.
#pragma once
#include "CoreMinimal.h"

class FSlateWindowElementList;
struct FGeometry;
struct FSlateFontInfo;

namespace SCHudPaint
{
SCBRIDGE_API void Box(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, const FLinearColor& Colour);
/** 1 px (or Thickness) outline of a rect. */
SCBRIDGE_API void Frame(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, const FLinearColor& Colour, float Thickness = 1);
SCBRIDGE_API void Lines(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, TArray<FVector2f> Points, const FLinearColor& Colour, float Thickness = 1);
SCBRIDGE_API void Text(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour);
SCBRIDGE_API FVector2f Measure(const FString& Text, const FSlateFontInfo& Font);
/** Diagonal hatching inside a square-cornered rect (blocked cells). */
SCBRIDGE_API void Hatch(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, float Step, const FLinearColor& Colour);
} // namespace SCHudPaint
