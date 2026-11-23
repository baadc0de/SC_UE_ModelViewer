// WP-O1-4b: an editable source for a runtime texture (the cook and Keep as assets save textures as editor assets, and an
// uncooked editor package keeps no platform mips: without a source a saved texture has no pixels).
#pragma once
#include "CoreMinimal.h"

class UTexture2D;

namespace SCTextureSource
{
/** Texture's source from its mip 0 as BGRA8: the platform mips as uploaded (the package's DDS blocks, or BGRA8 from PNG),
 *  block formats decoded by the bridge (sc_texels_decode_bgra8). False with Error when the pixel format has no decode,
 *  there are no mips, or the decode fails (the texture then has no source). */
bool Add(UTexture2D* Texture, FString& Error);
}
