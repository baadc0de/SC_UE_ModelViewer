// Paints (tint system): which instances a paint touches and what it puts on them (FSCSceneManifest::PaintTarget).
#include "SCPackageReader.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPaintTarget, "SCBridge.Paint.Target", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPaintTarget::RunTest(const FString&)
{
    FSCSceneManifest M;
    M.Root.MaterialSidecar = TEXT("Data/ship/hull_TEX0.materials.json");
    M.Root.PaletteId = TEXT("palette/ship");
    FSCPaintVariant Livery;
    Livery.PaletteId = TEXT("palette/ship_red");
    Livery.ExteriorSidecar = TEXT("Data/ship/hull_red_TEX0.materials.json");
    FSCPaintVariant Recolour;
    Recolour.PaletteId = TEXT("palette/ship_blue"); // a palette-only paint keeps every material file
    M.Paints = {Livery, Recolour};

    FSCSceneInstance Hull = M.Root;
    FSCSceneInstance Thruster; // attached item on the ship's palette (resolved from its host)
    Thruster.MaterialSidecar = TEXT("Data/thruster_TEX0.materials.json");
    Thruster.PaletteId = TEXT("palette/ship");
    FSCSceneInstance Seat; // its own palette: no paint touches it
    Seat.MaterialSidecar = TEXT("Data/seat_TEX0.materials.json");
    Seat.PaletteId = TEXT("palette/seat");
    FSCSceneInstance Panel; // the hull's material file under its own palette: file swaps, palette stays
    Panel.MaterialSidecar = M.Root.MaterialSidecar;
    Panel.PaletteId = TEXT("palette/panel");

    FString Sidecar, Palette;
    TestTrue(TEXT("hull takes the livery"), M.PaintTarget(Hull, 0, Sidecar, Palette));
    TestEqual(TEXT("hull material file"), Sidecar, Livery.ExteriorSidecar);
    TestEqual(TEXT("hull palette"), Palette, Livery.PaletteId);
    TestTrue(TEXT("hull takes the recolour"), M.PaintTarget(Hull, 1, Sidecar, Palette));
    TestEqual(TEXT("recolour keeps the hull file"), Sidecar, M.Root.MaterialSidecar);
    TestEqual(TEXT("recolour palette"), Palette, Recolour.PaletteId);
    TestTrue(TEXT("palette follower"), M.PaintTarget(Thruster, 0, Sidecar, Palette));
    TestEqual(TEXT("follower keeps its file"), Sidecar, Thruster.MaterialSidecar);
    TestEqual(TEXT("follower palette"), Palette, Livery.PaletteId);
    TestFalse(TEXT("own palette untouched"), M.PaintTarget(Seat, 0, Sidecar, Palette));
    TestTrue(TEXT("hull file, own palette"), M.PaintTarget(Panel, 0, Sidecar, Palette));
    TestEqual(TEXT("panel file swaps"), Sidecar, Livery.ExteriorSidecar);
    TestEqual(TEXT("panel palette stays"), Palette, Panel.PaletteId);
    TestFalse(TEXT("no such paint"), M.PaintTarget(Hull, 2, Sidecar, Palette));
    return true;
}
