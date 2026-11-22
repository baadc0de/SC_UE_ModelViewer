// SCHudPaint: the shared HUD paint helpers.
#include "UI/SCHudPaint.h"
#include "UI/SCHudStyle.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"

using namespace SCHudStyle;

namespace SCHudPaint
{
void Box(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& Colour)
{
    FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(Size, FSlateLayoutTransform(Pos)), White(), ESlateDrawEffect::None, Colour);
}
void Frame(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& Colour, float T)
{
    Box(Out, Layer, G, Pos, FVector2f(Size.X, T), Colour);
    Box(Out, Layer, G, FVector2f(Pos.X, Pos.Y + Size.Y - T), FVector2f(Size.X, T), Colour);
    Box(Out, Layer, G, Pos, FVector2f(T, Size.Y), Colour);
    Box(Out, Layer, G, FVector2f(Pos.X + Size.X - T, Pos.Y), FVector2f(T, Size.Y), Colour);
}
void Lines(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, TArray<FVector2f> Points, const FLinearColor& Colour, float Thickness)
{
    FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), MoveTemp(Points), ESlateDrawEffect::None, Colour, true, Thickness);
}
void Text(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, const FString& String, const FSlateFontInfo& Font, const FLinearColor& Colour)
{
    FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(Measure(String, Font), FSlateLayoutTransform(Pos)), String, Font, ESlateDrawEffect::None, Colour);
}
FVector2f Measure(const FString& String, const FSlateFontInfo& Font)
{
    if (!FSlateApplication::IsInitialized()) return FVector2f::ZeroVector;
    return FVector2f(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(String, Font));
}
void Hatch(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, float Step, const FLinearColor& Colour)
{
    // 45° lines clipped to the rect: from the top edge / right edge down-left to the left edge / bottom edge.
    for (float K = Step; K < Size.X + Size.Y; K += Step)
    {
        const FVector2f A(Pos.X + FMath::Min(K, Size.X), Pos.Y + FMath::Max(0.f, K - Size.X));
        const FVector2f B(Pos.X + FMath::Max(0.f, K - Size.Y), Pos.Y + FMath::Min(K, Size.Y));
        Lines(Out, Layer, G, {A, B}, Colour, 1);
    }
}
} // namespace SCHudPaint
