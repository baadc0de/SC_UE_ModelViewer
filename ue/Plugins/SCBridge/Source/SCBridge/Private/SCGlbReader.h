// Bounded GLB 2 buffer/accessor reader used by CPU mesh derivation.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using FObject = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Array(const FObject& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
int32 Int(const FObject& O, const TCHAR* Key, int32 Default = 0)
{
    int32 V;
    return O && O->TryGetNumberField(Key, V) ? V : Default;
}
FObject ObjectAt(const FObject& O, const TCHAR* Key, int32 Index)
{
    const auto& A = Array(O, Key);
    return A.IsValidIndex(Index) ? A[Index]->AsObject() : nullptr;
}
uint32 U32(const uint8* P)
{
    uint32 V;
    FMemory::Memcpy(&V, P, 4);
    return V;
}
struct FAccessor
{
    const uint8* Data = nullptr;
    int32 Count = 0, Components = 0, Type = 0, ComponentSize = 0, Stride = 0;
    bool Normalized = false;
    double Get(int32 Index, int32 Component) const
    {
        const uint8* P = Data + int64(Index) * Stride + Component * ComponentSize;
        switch (Type)
        {
        case 5126: {
            float V;
            FMemory::Memcpy(&V, P, 4);
            return V;
        }
        case 5125:
            return U32(P);
        case 5123: {
            uint16 V;
            FMemory::Memcpy(&V, P, 2);
            return Normalized ? V / 65535.0 : V;
        }
        case 5122: {
            int16 V;
            FMemory::Memcpy(&V, P, 2);
            return Normalized ? FMath::Max(-1.0, V / 32767.0) : V;
        }
        case 5121:
            return Normalized ? *P / 255.0 : *P;
        case 5120: {
            int8 V = *P;
            return Normalized ? FMath::Max(-1.0, V / 127.0) : V;
        }
        default:
            return 0;
        }
    }
    FVector Vector(int32 I) const
    {
        return FVector(Get(I, 0), Get(I, 1), Get(I, 2));
    }
};
struct FGlb
{
    FObject Json;
    const uint8* Bin = nullptr;
    int64 BinSize = 0;
    FString Error;
    bool Accessor(int32 Index, FAccessor& A)
    {
        const auto O = ObjectAt(Json, TEXT("accessors"), Index);
        if (!O || O->HasField(TEXT("sparse")))
        {
            Error = TEXT("Missing or unsupported sparse accessor");
            return false;
        }
        const auto View = ObjectAt(Json, TEXT("bufferViews"), Int(O, TEXT("bufferView"), -1));
        if (!View || Int(View, TEXT("buffer")) != 0)
        {
            Error = TEXT("Missing/external bufferView");
            return false;
        }
        FString Type;
        O->TryGetStringField(TEXT("type"), Type);
        A.Components = Type == TEXT("SCALAR") ? 1
                       : Type == TEXT("VEC2") ? 2
                       : Type == TEXT("VEC3") ? 3
                       : Type == TEXT("VEC4") ? 4
                       : Type == TEXT("MAT4") ? 16
                                              : 0;
        A.Type = Int(O, TEXT("componentType"));
        A.Count = Int(O, TEXT("count"));
        A.ComponentSize = A.Type == 5120 || A.Type == 5121   ? 1
                          : A.Type == 5122 || A.Type == 5123 ? 2
                          : A.Type == 5125 || A.Type == 5126 ? 4
                                                             : 0;
        A.Stride = Int(View, TEXT("byteStride"), A.Components * A.ComponentSize);
        O->TryGetBoolField(TEXT("normalized"), A.Normalized);
        const int64 ViewOffset = Int(View, TEXT("byteOffset")), ViewSize = Int(View, TEXT("byteLength")),
                    Offset = Int(O, TEXT("byteOffset"));
        const int64 Needed = int64(A.Count - 1) * A.Stride + A.Components * A.ComponentSize;
        if (!A.Components || !A.ComponentSize || A.Count <= 0 || A.Stride < A.Components * A.ComponentSize || Offset < 0 ||
            ViewOffset < 0 || ViewSize < 0 || ViewOffset + ViewSize > BinSize || Needed < 0 || Offset + Needed > ViewSize)
        {
            Error = TEXT("Accessor exceeds buffer bounds or has invalid layout");
            return false;
        }
        A.Data = Bin + ViewOffset + Offset;
        return true;
    }
    bool Parse(const TArray<uint8>& Bytes)
    {
        if (Bytes.Num() < 20 || U32(Bytes.GetData()) != 0x46546c67 || U32(Bytes.GetData() + 4) != 2 ||
            U32(Bytes.GetData() + 8) != uint32(Bytes.Num()))
        {
            Error = TEXT("Invalid GLB 2 header");
            return false;
        }
        for (int64 Offset = 12; Offset + 8 <= Bytes.Num();)
        {
            const uint32 Size = U32(Bytes.GetData() + Offset), Type = U32(Bytes.GetData() + Offset + 4);
            Offset += 8;
            if (Offset + Size > Bytes.Num())
            {
                Error = TEXT("Truncated GLB chunk");
                return false;
            }
            if (Type == 0x4e4f534a)
            {
                const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), Size));
                if (Json || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json))
                {
                    Error = TEXT("Invalid GLB JSON");
                    return false;
                }
            }
            else if (Type == 0x004e4942)
            {
                Bin = Bytes.GetData() + Offset;
                BinSize = Size;
            }
            Offset += Size;
        }
        if (!Json || !Bin)
        {
            Error = TEXT("GLB needs JSON and BIN chunks");
            return false;
        }
        if (!Array(Json, TEXT("extensionsRequired")).IsEmpty())
        {
            Error = TEXT("Required glTF extensions are unsupported");
            return false;
        }
        return true;
    }
    bool NodeMatrix(const FObject& N, FMatrix& M)
    {
        M = FMatrix::Identity;
        const auto& Values = Array(N, TEXT("matrix"));
        if (!Values.IsEmpty())
        {
            if (Values.Num() != 16) return false;
            for (int32 I = 0; I < 16; ++I)
                M.M[I / 4][I % 4] = Values[I]->AsNumber();
        }
        else
        {
            FVector T(0), S(1);
            FQuat Q = FQuat::Identity;
            const auto& Translation = Array(N, TEXT("translation"));
            const auto& Scale = Array(N, TEXT("scale"));
            const auto& Rotation = Array(N, TEXT("rotation"));
            if (Translation.Num() == 3) T = FVector(Translation[0]->AsNumber(), Translation[1]->AsNumber(), Translation[2]->AsNumber());
            if (Scale.Num() == 3) S = FVector(Scale[0]->AsNumber(), Scale[1]->AsNumber(), Scale[2]->AsNumber());
            if (Rotation.Num() == 4)
                Q = FQuat(Rotation[0]->AsNumber(), Rotation[1]->AsNumber(), Rotation[2]->AsNumber(), Rotation[3]->AsNumber())
                        .GetNormalized();
            M = FTransform(Q, T, S).ToMatrixWithScale();
        }
        return !M.ContainsNaN();
    }
};
} // namespace
