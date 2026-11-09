#pragma once
#include "Containers/Ticker.h"
/** Optional command-line acceptance harness, isolated from module startup and importer policy. */
class FSCImportHarness
{
public:
    void Start();
    void Stop();
private:
    FTSTicker::FDelegateHandle Tick;
};
