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
    /** Lumen always; RTX needs sampled lighting and active Mega Geometry (proxy BLASes break the sampled-lighting rays). */
    bool Supports(ESCRenderPath Path) const;
    FString Describe() const;
};

namespace SCRenderPath
{
/** The requested path: -SCRenderPath=rtx|lumen for this run, else USCViewerSettings::RenderPath. Apply falls back to
 *  Lumen where RTX is unsupported (including Mega Geometry off), without changing the saved choice. */
SCBRIDGE_API ESCRenderPath Resolve();
/** Set the path's renderer state now (live). An unsupported path applies Lumen. Logs RENDER_PATH; returns what applied. */
SCBRIDGE_API ESCRenderPath Apply(ESCRenderPath Path);
/** The path last applied (Lumen before the first Apply). */
SCBRIDGE_API ESCRenderPath Current();
SCBRIDGE_API const TCHAR* Name(ESCRenderPath Path);

/** RTX Mega Geometry can be built on this GPU / RHI (NvRTX cluster operations), whether or not it is on. */
SCBRIDGE_API bool MegaGeometrySupported();
/** Mega Geometry for the next start. r.Nanite.RayTracing is read-only at run time, so the choice is written to the
 *  user's engine config ([SystemSettings] in Saved/Config/<Platform>/Engine.ini) and takes effect after a restart;
 *  FSCRenderFeatures::bMegaGeometry is what this run has. */
SCBRIDGE_API bool MegaGeometryOnNextStart();
SCBRIDGE_API void SetMegaGeometryOnNextStart(bool bOn);
}
