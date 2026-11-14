// The lighting path (RTXPT-1): RTX = NvRTX RTXDI direct + ReSTIR PT indirect, denoised by DLSS Ray Reconstruction;
// Lumen = Lumen GI and reflections with MegaLights direct lighting (the Linux look). Switched live, no restart.
#pragma once
#include "CoreMinimal.h"
#include "SCRenderPath.generated.h"

UENUM(BlueprintType)
enum class ESCRenderPath : uint8
{
    RTX,
    Lumen,
};

/** What this process can render, queried from the running RHI and renderer (game thread). */
struct SCBRIDGE_API FSCRenderFeatures
{
    bool bRayTracing = false;      // hardware ray tracing active (r.RayTracing on a ray-tracing RHI)
    bool bSampledLighting = false; // NvRTX RTXDI / ReSTIR: its cvars exist and the Substrate G-buffer is blendable
    bool bMegaGeometry = false;    // RTX Mega Geometry: r.Nanite.RayTracing on and cluster operations supported
    bool bMegaLights = false;      // MegaLights compiled for this platform and allowed
    bool bDLSS = false;            // the DLSS plugin is loaded (RR denoising where the GPU supports it)

    static FSCRenderFeatures Query();
    bool Supports(ESCRenderPath Path) const;
    FString Describe() const;
};

namespace SCRenderPath
{
/** -SCRenderPath=rtx|lumen for this run, else USCViewerSettings::RenderPath; RTX falls back to Lumen when unsupported. */
SCBRIDGE_API ESCRenderPath Resolve();
/** Set the path's renderer state now (live). An unsupported path applies Lumen. Logs RENDER_PATH; returns what applied. */
SCBRIDGE_API ESCRenderPath Apply(ESCRenderPath Path);
/** The path last applied (Lumen before the first Apply). */
SCBRIDGE_API ESCRenderPath Current();
SCBRIDGE_API const TCHAR* Name(ESCRenderPath Path);
}
