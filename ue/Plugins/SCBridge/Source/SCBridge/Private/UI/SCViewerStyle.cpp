// SCViewerStyle widget factories and number formatting.
#include "SCViewerStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

namespace SCViewerStyle
{
TSharedRef<SWidget> Header(const FText& Text)
{
    return SNew(STextBlock).Text(Text).Font(Font(9, true)).ColorAndOpacity(Dim());
}
TSharedRef<SWidget> Chip(const FText& Text, const FLinearColor& Color)
{
    return SNew(SBorder)
        .BorderImage(Fill())
        .BorderBackgroundColor(Color.CopyWithNewOpacity(0.22f))
        .Padding(FMargin(5, 1))
        [SNew(STextBlock).Text(Text).Font(Font(8, true)).ColorAndOpacity(Color)];
}
FString Compact(int64 Value)
{
    if (Value >= 1000000) return FString::Printf(TEXT("%.2fM"), Value / 1e6);
    if (Value >= 10000) return FString::Printf(TEXT("%.1fk"), Value / 1e3);
    return FString::Printf(TEXT("%lld"), Value);
}
FString Bytes(uint64 Value)
{
    if (Value >= (1ull << 30)) return FString::Printf(TEXT("%.1f GB"), double(Value) / double(1ull << 30));
    if (Value >= (1ull << 20)) return FString::Printf(TEXT("%.0f MB"), double(Value) / double(1ull << 20));
    return FString::Printf(TEXT("%.0f KB"), double(Value) / 1024.0);
}
} // namespace SCViewerStyle
