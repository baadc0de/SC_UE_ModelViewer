#include "Modules/ModuleManager.h"
namespace SCSpike { void Start(); void Stop(); }
class FSCBridgeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override { SCSpike::Start(); }
    virtual void ShutdownModule() override { SCSpike::Stop(); }
};
IMPLEMENT_MODULE(FSCBridgeModule, SCBridge);
