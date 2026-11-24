// WP-O1-4c-3a: the log category of importing and of the import-shaped records the runtime keeps (light areas, dirt,
// light verification), exported so the importer module (SCBridgeImport, O1-4c-3b) logs to it too.
#pragma once
#include "CoreMinimal.h"
SCBRIDGE_API DECLARE_LOG_CATEGORY_EXTERN(LogSCImport, Log, All);
