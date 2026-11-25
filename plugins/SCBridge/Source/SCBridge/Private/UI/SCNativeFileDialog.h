// SCNativeFileDialog: the operating system's own "open file" dialog, for builds without the DesktopPlatform developer
// module (Shipping). Windows only for now; elsewhere it reports false and the caller explains the -SCArchive= route.
#pragma once
#include "CoreMinimal.h"

namespace SCNativeFileDialog
{
/** Whether this platform has the dialog. */
bool IsAvailable();
/** Asks for one existing file (Pattern like "*.p4k"); false when cancelled or unavailable. */
bool OpenFile(const void* ParentWindowHandle, const FString& Title, const FString& FilterName, const FString& Pattern, FString& OutPath);
} // namespace SCNativeFileDialog
