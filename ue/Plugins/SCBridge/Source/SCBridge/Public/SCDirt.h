// Viewer dirt (WP-U2b, docs/work-packages/WP-U2b-dirt.md). The masters port the game's dirt rule (layerLib.cfi
// CalcDirtBlendAlpha / LayerAttribs_ApplyDirt); the game feeds it a per-instance dirt amount and colour at run time
// (a ship's authored `Dirt` is 0), so the viewer supplies its own: USCViewerSettings DirtAmount / DirtColor through the
// material parameter collection MPC_SCDirt, on exterior components only (custom primitive data slot
// ExteriorDataIndex; the game's object-container template sets InteriorDirtFactor 0).
#pragma once
#include "CoreMinimal.h"
class ASCVehicle;
class UWorld;

class SCBRIDGE_API FSCDirt
{
public:
    /** Custom primitive data slot the masters read as the exterior factor (create_materials.py DIRT_EXTERIOR_INDEX). */
    static constexpr int32 ExteriorDataIndex = 0;
    /** Named dirt colours (linear). Viewer choices, not game data: no DataCore field or P4k file names a dirt colour. */
    struct FPreset
    {
        const TCHAR* Name;
        FLinearColor Colour;
    };
    static TConstArrayView<FPreset> Presets();
    /** Write the settings' amount and colour into the world's MPC_SCDirt instance (no-op without the asset). */
    static void Push(UWorld* World);
    /** SH2 linear coefficients (object space) of the viewer dirt: (0, 0, -Underside x Amount); the constant band
     *  is the amount itself (game vertexLib.cfi: dirt = SH2_SampleValue_Clamped(scaled object normal, dirtAmountSH)). */
    static FLinearColor DirectionCoefficients(const class USCViewerSettings& Settings);
    /** Exterior factor per primitive: 1 outside every room (lighting channel 0 only, FSCLightAreas), else 0. */
    static void MarkExterior(ASCVehicle* Vehicle);
};
