// The version of the generated FX contract (WP-E1 E1-6 review): SCFxBuild writes SCFxRules::FxContractVersion as the system's
// user parameter SC.FxContract; a system without it, or with an older one, was built for another runtime contract (before E1-6 the
// runtime set SC.SV<i>, SC.BR<i> and SC.Scale2, which it no longer does) and plays with stale behaviour until it is rebuilt.
#pragma once
#include "CoreMinimal.h"
#include "NiagaraSystem.h"
#include "UObject/ObjectKey.h"
#include "SCFxRules.h"

namespace SCFxContract
{
/** The contract version a built system carries (0: none). */
inline int32 Of(const UNiagaraSystem& System)
{
    const FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), FName(FString(TEXT("User.")) + SCFxRules::FxContractParameter));
    const FNiagaraUserRedirectionParameterStore& User = System.GetExposedParameters();
    return User.IndexOf(Var) != INDEX_NONE ? User.GetParameterValue<int32>(Var) : 0;
}

/** Non-shipping: a warning, once per system, for a system older than the contract (the showcase package refuses those). */
inline void Check(const UNiagaraSystem* System)
{
#if !UE_BUILD_SHIPPING
    static TSet<FObjectKey> Warned;
    if (!System || Warned.Contains(System)) return;
    Warned.Add(System);
    const int32 Version = Of(*System);
    UE_CLOG(Version < SCFxRules::FxContractVersion, LogTemp, Warning,
            TEXT("SC_FX_STALE %s contract=%d (current %d): built for an older runtime contract; rebuild it with -run=SCFxBuild"),
            *System->GetPathName(), Version, SCFxRules::FxContractVersion);
#endif
}
} // namespace SCFxContract
