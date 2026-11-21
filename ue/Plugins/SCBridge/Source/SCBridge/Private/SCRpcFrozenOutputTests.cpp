// Frozen scene colour isolates output-device routing from temporal history and stochastic lighting.
#if WITH_DEV_AUTOMATION_TESTS && SC_WITH_RPC
#include "Misc/AutomationTest.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineModule.h"
#include "LegacyScreenPercentageDriver.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RendererInterface.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "SCRpcRender.h"
#include "SCLookLighting.h"
#include "TextureResource.h"
#include "UnrealClient.h"

namespace
{
constexpr int32 ChartWidth = 64, ChartHeight = 32;
class FChartOutput final : public FRenderTarget
{
public:
    FChartOutput(FTextureRenderTargetResource* InResource, bool bInHDR) : Resource(InResource), bHDR(bInHDR) {}
    FIntPoint GetSizeXY() const override { return FIntPoint(ChartWidth, ChartHeight); }
    const FTextureRHIRef& GetRenderTargetTexture() const override { return Resource->GetRenderTargetTexture(); }
    EDisplayOutputFormat GetDisplayOutputFormat() const override
    { return bHDR ? EDisplayOutputFormat::HDR_ACES_1000nit_ST2084 : EDisplayOutputFormat::SDR_sRGB; }
    bool GetSceneHDREnabled() const override { return bHDR; }
    float GetMaximumLuminanceInNits() const override { return bHDR ? 1000.f : 100.f; }
private:
    FTextureRenderTargetResource* Resource;
    bool bHDR;
};

class FFrozenChart final : public FSceneViewExtensionBase
{
public:
    FFrozenChart(const FAutoRegister& Register, const FRenderTarget* Output)
        : FSceneViewExtensionBase(Register), Target(Output) {}
    void SetupViewFamily(FSceneViewFamily&) override {}
    void SetupView(FSceneViewFamily&, FSceneView&) override {}
    void BeginRenderViewFamily(FSceneViewFamily&) override {}
    void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
        FPostProcessingPassDelegateArray& Callbacks, bool) override
    {
        if (Pass == EPostProcessingPass::BeforeDOF && View.Family->RenderTarget == Target)
            Callbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FFrozenChart::Write));
    }
    bool bWritten = false; // read by the game thread only after FlushRenderingCommands
private:
    const FRenderTarget* Target;
    FScreenPassTexture Write(FRDGBuilder& Graph, const FSceneView&, const FPostProcessMaterialInputs& Inputs)
    {
        FScreenPassTexture Color = FScreenPassTexture::CopyFromSlice(Graph, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
        const FIntRect Rect = Color.ViewRect;
        AddClearRenderTargetPass(Graph, Color.Texture, FLinearColor(0.02f, 0.03f, 0.04f), Rect);
        AddClearRenderTargetPass(Graph, Color.Texture, FLinearColor(4.f, 1.f, 0.25f),
            FIntRect((Rect.Min.X + Rect.Max.X) / 2, Rect.Min.Y, Rect.Max.X, Rect.Max.Y));
        // Neutral patches above scene white distinguish native highlight detail from an expanded SDR plateau.
        for (int32 I = 0; I < 3; ++I)
        {
            const float Level = float(1 << (2 * I));
            AddClearRenderTargetPass(Graph, Color.Texture, FLinearColor(Level, Level, Level),
                FIntRect(Rect.Min.X + I * Rect.Width() / 3, Rect.Min.Y,
                         Rect.Min.X + (I + 1) * Rect.Width() / 3, Rect.Min.Y + Rect.Height() / 4));
        }
        bWritten = true;
        return Color;
    }
};

bool RenderChart(UWorld* World, ESceneCaptureSource Source, bool bHDR, TArray<FFloat16Color>& Pixels, bool bNativeHDR = true)
{
    TStrongObjectPtr<UTextureRenderTarget2D> Texture(NewObject<UTextureRenderTarget2D>());
    Texture->InitCustomFormat(ChartWidth, ChartHeight, PF_FloatRGBA, true);
    Texture->UpdateResourceImmediate();
    auto* Resource = Texture->GameThread_GetRenderTargetResource();
    FChartOutput Target(Resource, bHDR);
    auto Chart = FSceneViewExtensions::NewExtension<FFrozenChart>(&Target);
    FEngineShowFlags Flags(ESFIM_Game);
    Flags.SetAntiAliasing(false);
    Flags.SetMotionBlur(false);
    Flags.SetBloom(false);
    Flags.SetEyeAdaptation(false);
    Flags.SetScreenPercentage(false);
    FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(&Target, World->Scene, Flags)
        .SetTime(FGameTime::CreateUndilated(0, 1.f/30.f)));
    Family.SceneCaptureSource = Source;
    Family.ViewExtensions.Add(Chart);
    Family.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(Family, 1.f));
    FSceneViewInitOptions Init;
    Init.ViewFamily = &Family;
    Init.SetViewRectangle(FIntRect(0, 0, ChartWidth, ChartHeight));
    Init.ViewOrigin = FVector::ZeroVector;
    Init.ViewRotationMatrix = FMatrix::Identity;
    Init.ProjectionMatrix = FReversedZPerspectiveMatrix(UE_HALF_PI / 2, 1.f, 2.f, 10.f);
    FSceneView* View = new FSceneView(Init);
    Family.Views.Add(View);
    View->ShowOnlyPrimitives.Emplace();
    View->StartFinalPostprocessSettings(Init.ViewOrigin);
    View->EndFinalPostprocessSettings(Init);
    View->FinalPostProcessSettings.VignetteIntensity = 0.f;
    View->FinalPostProcessSettings.FilmGrainIntensity = 0.f;
    View->FinalPostProcessSettings.TonemappingMethod = ETonemappingMethod::HuePreservingShoulder;
    SCLookLighting::ApplyDisplayOutput(Family, *View, bNativeHDR);
    FCanvas Canvas(&Target, nullptr, World, World->GetFeatureLevel());
    GetRendererModule().BeginRenderingViewFamily(&Canvas, &Family);
    FlushRenderingCommands();
    return Chart->bWritten && Resource->ReadFloat16Pixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax)) &&
        Pixels.Num() == ChartWidth * ChartHeight;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFrozenOutputDeviceTest, "SCBridge.RenderGpu.FrozenOutputDevice",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FSCFrozenOutputDeviceTest::RunTest(const FString&)
{
    UWorld* World = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
    if (!World || !World->Scene) { AddError(TEXT("Run in a rendered game world")); return false; }
    for (ESceneCaptureSource Source : {FSCRpcRender::PngSource, FSCRpcRender::ExrSource, SCS_FinalColorLDR})
    {
        TArray<FFloat16Color> Sdr, Hdr;
        if (!RenderChart(World, Source, false, Sdr) || !RenderChart(World, Source, true, Hdr))
        { AddError(TEXT("Frozen chart injection/readback failed")); return false; }
        TestTrue(TEXT("chart has both dark and bright regions"),
            Sdr[ChartWidth * 16 + 48].R.GetFloat() > Sdr[ChartWidth * 16 + 16].R.GetFloat());
        float Peak = 0;
        int32 MismatchedPixels = 0;
        bool bFinite = true;
        for (int32 I = 0; I < Sdr.Num(); ++I)
        {
            const FFloat16Color& A = Sdr[I];
            const FFloat16Color& B = Hdr[I];
            MismatchedPixels += A.R.Encoded != B.R.Encoded || A.G.Encoded != B.G.Encoded || A.B.Encoded != B.B.Encoded;
            const FLinearColor Delta = FLinearColor(A) - FLinearColor(B);
            bFinite &= FMath::IsFinite(Delta.R) && FMath::IsFinite(Delta.G) && FMath::IsFinite(Delta.B);
            Peak = FMath::Max(Peak, FMath::Max3(FMath::Abs(Delta.R), FMath::Abs(Delta.G), FMath::Abs(Delta.B)));
        }
        AddInfo(FString::Printf(TEXT("source=%d frozen HDR/SDR mismatched RGB pixels=%d/%d, linear target peak=%g"),
            int32(Source), MismatchedPixels, Sdr.Num(), Peak));
        TestTrue(TEXT("frozen chart RGB is finite on both devices"), bFinite);
        if (Source == SCS_FinalColorLDR)
        {
            TestTrue(TEXT("control follows the advertised output device"), Peak > 1.f/1023);
            TArray<FFloat16Color> Legacy;
            if (!RenderChart(World, Source, true, Legacy, false)) { AddError(TEXT("HDR comparison chart failed")); return false; }
            const int32 Offset = ChartWidth * (ChartHeight / 8);
            const float White = Hdr[Offset + ChartWidth / 6].R.GetFloat();
            const float Bright = Hdr[Offset + ChartWidth / 2].R.GetFloat();
            const float Hot = Hdr[Offset + 5 * ChartWidth / 6].R.GetFloat();
            const float LegacyBright = Legacy[Offset + ChartWidth / 2].R.GetFloat();
            const float LegacyHot = Legacy[Offset + 5 * ChartWidth / 6].R.GetFloat();
            AddInfo(FString::Printf(TEXT("1000-nit PQ output: neutral scene values 1/4/16 -> %.5f/%.5f/%.5f; legacy 4/16 -> %.5f/%.5f"),
                White, Bright, Hot, LegacyBright, LegacyHot));
            TestTrue(TEXT("distinct HDR highlight levels at scene white, four and sixteen"), Bright > White && Hot > Bright);
            TestTrue(TEXT("native transform retains more upper highlight separation"), Hot - Bright > LegacyHot - LegacyBright);
        }
        else TestEqual(TEXT("offline RGB half-float codes are identical on SDR and HDR targets"), MismatchedPixels, 0);
    }
    return true;
}
#endif
