// SCRenderPath: live switch between the RTX (RTXDI + ReSTIR PT + DLSS-RR) and Lumen + MegaLights lighting paths.
//
// Every setting touched here is a run-time renderer cvar: sampled lighting (NvRTX), MegaLights (its shaders compile
// whenever r.MegaLights.Supported is on, whatever EnableForProject was at startup) and the DLSS denoiser mode. The
// read-only parts stay as configured at startup: Mega Geometry (r.Nanite.RayTracing, on in the Win64 layer; the engine
// itself gates it on cluster-operation support) and the Substrate G-buffer format (blendable on Win64, which the RTX
// path needs; Lumen + MegaLights runs on it too).
#include "SCRenderPath.h"
#include "SCViewerSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderUtils.h"
#include "RHIGlobals.h"
#include "RHIDefinitions.h"
#include "DynamicRHI.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCRenderPath, Log, All);

namespace
{
ESCRenderPath GCurrent = ESCRenderPath::Lumen;

IConsoleVariable* Find(const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name); }
bool Enabled(const TCHAR* Name)
{
    const IConsoleVariable* Var = Find(Name);
    return Var && Var->GetInt() != 0;
}
// Console priority, as if typed: the path wins over the ini layer and over earlier console / route settings.
void Set(const TCHAR* Name, int32 Value)
{
    if (IConsoleVariable* Var = Find(Name)) Var->Set(Value, ECVF_SetByConsole);
}
void SetDefault(const TCHAR* Name)
{
    if (IConsoleVariable* Var = Find(Name)) Var->Set(*Var->GetDefaultValue(), ECVF_SetByConsole);
}
}

FSCRenderFeatures FSCRenderFeatures::Query()
{
    FSCRenderFeatures F;
    if (GUsingNullRHI) return F;
    F.bRayTracing = IsRayTracingEnabled();
    // NvRTX's own gate (PlatformSupportsSampledLighting): no sampled lighting under Substrate's adaptive G-buffer.
    const bool bComplexSubstrate = Substrate::IsSubstrateEnabled() && !Substrate::IsSubstrateBlendableGBufferEnabled(GMaxRHIShaderPlatform);
    F.bSampledLighting = F.bRayTracing && Find(TEXT("r.SampledLighting.Direct")) && !bComplexSubstrate;
#if defined(NV_RAYTRACING_CLUSTER_OPS) && NV_RAYTRACING_CLUSTER_OPS
    F.bMegaGeometry = IsNaniteRayTracingEnabled();
#endif
    F.bMegaLights = ShouldCompileMegaLightsShaders(GMaxRHIShaderPlatform) && Enabled(TEXT("r.MegaLights.Allowed"));
    F.bDLSS = Find(TEXT("r.NGX.DLSS.DenoiserMode")) != nullptr;
    return F;
}

bool FSCRenderFeatures::Supports(ESCRenderPath Path) const
{
    // Lumen is always there; without MegaLights its direct lighting is the deferred path.
    return Path == ESCRenderPath::Lumen || bSampledLighting;
}

FString FSCRenderFeatures::Describe() const
{
    return FString::Printf(TEXT("ray_tracing=%d sampled_lighting=%d mega_geometry=%d megalights=%d dlss=%d"), bRayTracing, bSampledLighting,
                           bMegaGeometry, bMegaLights, bDLSS);
}

const TCHAR* SCRenderPath::Name(ESCRenderPath Path)
{
    return Path == ESCRenderPath::RTX ? TEXT("rtx") : TEXT("lumen");
}

ESCRenderPath SCRenderPath::Current()
{
    return GCurrent;
}

ESCRenderPath SCRenderPath::Resolve()
{
    ESCRenderPath Want = USCViewerSettings::Get()->RenderPath;
    FString Arg;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCRenderPath="), Arg))
    {
        if (Arg.Equals(TEXT("rtx"), ESearchCase::IgnoreCase)) Want = ESCRenderPath::RTX;
        else if (Arg.Equals(TEXT("lumen"), ESearchCase::IgnoreCase)) Want = ESCRenderPath::Lumen;
        else UE_LOG(LogSCRenderPath, Warning, TEXT("RENDER_PATH ignored -SCRenderPath=%s (rtx|lumen)"), *Arg);
    }
    return FSCRenderFeatures::Query().Supports(Want) ? Want : ESCRenderPath::Lumen;
}

ESCRenderPath SCRenderPath::Apply(ESCRenderPath Path)
{
    const FSCRenderFeatures F = FSCRenderFeatures::Query();
    const ESCRenderPath Wanted = Path;
    if (!F.Supports(Path)) Path = ESCRenderPath::Lumen;
    const bool bRTX = Path == ESCRenderPath::RTX;
    if (F.bSampledLighting)
    {
        Set(TEXT("r.SampledLighting.Direct"), bRTX);
        Set(TEXT("r.SampledLighting.Indirect"), bRTX);
        if (bRTX) Set(TEXT("r.SampledLighting.Indirect.Mode"), 2); // ReSTIR PT
    }
    Set(TEXT("r.MegaLights.EnableForProject"), !bRTX && F.bMegaLights);
    if (F.bDLSS)
    {
        // RTX: DLSS Ray Reconstruction denoises the sampled signals and replaces the built-in denoisers. Lumen +
        // MegaLights: DLSS as the upscaler / DLAA only, so Lumen and MegaLights keep their own denoisers.
        Set(TEXT("r.NGX.DLSS.DenoiserMode"), bRTX ? 1 : 0);
        for (const TCHAR* Name : {TEXT("r.Lumen.Reflections.BilateralFilter"), TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction"),
                                  TEXT("r.Lumen.Reflections.Temporal"), TEXT("r.Shadow.Denoiser")})
            bRTX ? Set(Name, 0) : SetDefault(Name);
    }
    GCurrent = Path;
    UE_LOG(LogSCRenderPath, Display, TEXT("RENDER_PATH path=%s requested=%s %s"), Name(Path), Name(Wanted), *F.Describe());
    return Path;
}

static FAutoConsoleCommand GSCRenderPathCommand(
    TEXT("sc.RenderPath"), TEXT("sc.RenderPath [rtx|lumen|toggle]: switch the lighting path live and remember it; no argument logs the state"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
        if (Args.IsEmpty())
        {
            UE_LOG(LogSCRenderPath, Display, TEXT("RENDER_PATH path=%s %s"), SCRenderPath::Name(SCRenderPath::Current()),
                   *FSCRenderFeatures::Query().Describe());
            return;
        }
        ESCRenderPath Path;
        if (Args[0].Equals(TEXT("rtx"), ESearchCase::IgnoreCase)) Path = ESCRenderPath::RTX;
        else if (Args[0].Equals(TEXT("lumen"), ESearchCase::IgnoreCase)) Path = ESCRenderPath::Lumen;
        else if (Args[0].Equals(TEXT("toggle"), ESearchCase::IgnoreCase))
            Path = SCRenderPath::Current() == ESCRenderPath::RTX ? ESCRenderPath::Lumen : ESCRenderPath::RTX;
        else
        {
            UE_LOG(LogSCRenderPath, Warning, TEXT("sc.RenderPath %s: expected rtx, lumen or toggle"), *Args[0]);
            return;
        }
        USCViewerSettings* Settings = USCViewerSettings::Get();
        Settings->RenderPath = SCRenderPath::Apply(Path);
        Settings->Save();
    }));
