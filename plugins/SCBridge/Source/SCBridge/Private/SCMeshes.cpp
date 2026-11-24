// WP-O1-4c-3a: the space conversions of FSCMeshes (SCMeshes.h), split from SCMeshImporter.cpp.
#include "SCMeshes.h"
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
