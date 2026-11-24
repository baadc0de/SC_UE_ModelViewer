// WP-O1-4c-3b: ISCArchive's factory (SCArchive.h).
#include "SCArchive.h"
namespace
{
FCriticalSection GFactoryLock;
TFunction<TSharedPtr<ISCArchive, ESPMode::ThreadSafe>()> GFactory;
} // namespace
TSharedPtr<ISCArchive, ESPMode::ThreadSafe> ISCArchive::Create()
{
    FScopeLock Guard(&GFactoryLock);
    return GFactory ? GFactory() : nullptr;
}
void ISCArchive::SetFactory(TFunction<TSharedPtr<ISCArchive, ESPMode::ThreadSafe>()> Factory)
{
    FScopeLock Guard(&GFactoryLock);
    GFactory = MoveTemp(Factory);
}
void FSCExportProgress::Report(const char* InPhase, uint64 Done, uint64 Total)
{
    Permille = uint32(Total ? FMath::Min<uint64>(1000, Done * 1000 / Total) : 0);
    FScopeLock Guard(&Lock);
    Phase = UTF8_TO_TCHAR(InPhase ? InPhase : "");
}
FString FSCExportProgress::GetPhase() const
{
    FScopeLock Guard(&Lock);
    return Phase;
}
int FSCExportProgress::Callback(const char* Phase, uint64_t Done, uint64_t Total, void* User)
{
    // May run on rayon threads; must not unwind (no throwing code here).
    auto* Progress = static_cast<FSCExportProgress*>(User);
    Progress->Report(Phase, Done, Total);
    return Progress->IsCancelRequested() ? 1 : 0;
}

