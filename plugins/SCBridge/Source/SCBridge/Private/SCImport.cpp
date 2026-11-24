// WP-O1-4c-3b: the importer's registration (SCImport.h); the importer's module sets it.
#include "SCImport.h"
namespace
{
ISCImporting* GImporter = nullptr;
} // namespace
ISCImporting* ISCImporting::Get() { return GImporter; }
void ISCImporting::Set(ISCImporting* Importer) { GImporter = Importer; }
