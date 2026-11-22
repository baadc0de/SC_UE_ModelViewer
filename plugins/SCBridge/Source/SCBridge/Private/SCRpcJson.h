// JSON helpers of the RPC harness and its state log (WP-H1): world values in metres (UE centimetres / 100), rounded so a
// state record stays short; NaN and infinities are written as null (strict JSON) and counted by the caller as anomalies.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace SCRpcJson
{
using FObj = TSharedRef<FJsonObject>;

/** V at Step (0.001, 0.01, 0.1, 1 …), written with just those decimals (a double would print 17 digits). */
inline TSharedRef<FJsonValue> Number(double V, double Step = 0.001)
{
    if (!FMath::IsFinite(V)) return MakeShared<FJsonValueNull>();
    const int32 Decimals = FMath::Clamp(FMath::CeilToInt(-FMath::LogX(10.0, Step) - 1e-9), 0, 9);
    FString S = FString::Printf(TEXT("%.*f"), Decimals, FMath::RoundToDouble(V / Step) * Step);
    if (Decimals > 0)
    {
        while (S.EndsWith(TEXT("0"))) S.LeftChopInline(1);
        if (S.EndsWith(TEXT("."))) S.LeftChopInline(1);
    }
    if (S == TEXT("-0")) S = TEXT("0");
    return MakeShared<FJsonValueNumberString>(S);
}
/** A vector in metres (from centimetres). */
inline TSharedRef<FJsonValue> Metres(const FVector& Cm)
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Number(Cm.X / 100), Number(Cm.Y / 100), Number(Cm.Z / 100)});
}
/** A vector as is (newtons, degrees), at Step. */
inline TSharedRef<FJsonValue> Vector(const FVector& V, double Step = 0.01)
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Number(V.X, Step), Number(V.Y, Step), Number(V.Z, Step)});
}
/** Pitch, yaw, roll in degrees. */
inline TSharedRef<FJsonValue> Rotation(const FRotator& R)
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Number(R.Pitch, 0.01), Number(R.Yaw, 0.01), Number(R.Roll, 0.01)});
}
inline TSharedRef<FJsonValue> Strings(const TArray<FString>& In)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& S : In) Out.Add(MakeShared<FJsonValueString>(S));
    return MakeShared<FJsonValueArray>(Out);
}
inline TSharedRef<FJsonValue> Ints(const TArray<int32>& In)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const int32 I : In) Out.Add(MakeShared<FJsonValueNumber>(I));
    return MakeShared<FJsonValueArray>(Out);
}
inline bool Finite(const FVector& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); }

/** One line of condensed JSON (no newline). */
inline FString Print(const FObj& O)
{
    FString Out;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    FJsonSerializer::Serialize(O, W);
    return Out;
}
inline TSharedPtr<FJsonObject> Parse(const FString& Text)
{
    TSharedPtr<FJsonObject> O;
    const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
    return FJsonSerializer::Deserialize(R, O) ? O : nullptr;
}
} // namespace SCRpcJson
