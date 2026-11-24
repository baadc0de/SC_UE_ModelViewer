// WP-O1-4c-3a: the space conversions of FSCMeshes (SCMeshes.h), split from SCMeshImporter.cpp.
#include "SCMeshes.h"
#include "Dom/JsonObject.h"
FMatrix FSCMeshes::CryToGltf()
{
    return FMatrix(FPlane(1, 0, 0, 0), FPlane(0, 0, -1, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
}
FMatrix FSCMeshes::Transform(const FMatrix& Gltf)
{
    const FMatrix Basis(FPlane(0, 100, 0, 0), FPlane(0, 0, 100, 0), FPlane(-100, 0, 0, 0), FPlane(0, 0, 0, 1));
    return Basis.Inverse() * Gltf * Basis;
}
FMatrix FSCMeshes::InstanceTransform(const FMatrix& Local, const FMatrix& AssetRoot, bool bSourceFrame, bool bCryBasis)
{
    if (bSourceFrame) return Transform((AssetRoot.Determinant() != 0 ? AssetRoot.Inverse() : FMatrix::Identity) * Local);
    return Transform(bCryBasis ? CryToGltf().Inverse() * Local * CryToGltf() : Local);
}
namespace
{
using FReaderObject = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& ReaderArray(const FReaderObject& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
} // namespace
FMatrix FSCMeshes::PortOffset(const TSharedPtr<FJsonObject>& Port)
{
    // Native SC axes: position in metres, Ang3 Euler degrees (x pitch, y roll, z yaw), Matrix33::CreateRotationXYZ
    // = Rz * Ry * Rx for column vectors, i.e. Rx * Ry * Rz in this row-vector convention.
    FVector P = FVector::ZeroVector, R = FVector::ZeroVector;
    const FReaderObject* Offset;
    if (Port && Port->TryGetObjectField(TEXT("offset"), Offset))
    {
        const auto& Pos = ReaderArray(*Offset, TEXT("position"));
        const auto& Rot = ReaderArray(*Offset, TEXT("rotation_degrees"));
        if (Pos.Num() == 3) P = FVector(Pos[0]->AsNumber(), Pos[1]->AsNumber(), Pos[2]->AsNumber());
        if (Rot.Num() == 3) R = FVector(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
    }
    auto Axis = [](int32 A, double Degrees) {
        const double S = FMath::Sin(FMath::DegreesToRadians(Degrees)), C = FMath::Cos(FMath::DegreesToRadians(Degrees));
        FMatrix M = FMatrix::Identity;
        const int32 U = (A + 1) % 3, V = (A + 2) % 3;
        M.M[U][U] = C; M.M[U][V] = S; M.M[V][U] = -S; M.M[V][V] = C; // row-vector rotation about axis A
        return M;
    };
    FMatrix M = Axis(0, R.X) * Axis(1, R.Y) * Axis(2, R.Z);
    M.SetOrigin(P);
    return M;
}
