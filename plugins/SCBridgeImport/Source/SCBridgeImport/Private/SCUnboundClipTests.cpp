// A clip without receiving bones (FSCAnimImporter::Finalize, no binding) keeps a metadata-only sequence that the engine
// can compress: its skeleton has a bone, also for a vehicle without a root rig (the ANVL Spartan's item clips asserted
// in the animation compression task on an empty skeleton, InBoneContainer->IsValid()).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCAnimImporter.h"
#include "SCClipCatalog.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCUnboundClipSequence, "SCBridge.Rig.UnboundClipSequence",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCUnboundClipSequence::RunTest(const FString&)
{
    FSCImportedClip Clip;
    Clip.Name = TEXT("unbound");
    Clip.Fps = 30;
    Clip.LastFrame = 10;
    FSCAnimImporter::Finalize(nullptr, GetTransientPackage(), Clip); // no root rig, no binding
    if (!TestNotNull(TEXT("metadata sequence"), Clip.Sequence.Get())) return false;
    const USkeleton* Skeleton = Clip.Sequence->GetSkeleton();
    TestTrue(TEXT("its skeleton has a bone (a valid bone container for compression)"), Skeleton && Skeleton->GetReferenceSkeleton().GetNum() > 0);
    // A bone tree for its bone (editor tools index it): a bone's retargeting mode is kept there; without one it reads the
    // default back.
    if (USkeleton* Mutable = const_cast<USkeleton*>(Skeleton))
    {
        Mutable->SetBoneTranslationRetargetingMode(0, EBoneTranslationRetargetingMode::Skeleton, false);
        TestEqual(TEXT("a bone tree for its bone"), int32(Mutable->GetBoneTranslationRetargetingMode(0)), int32(EBoneTranslationRetargetingMode::Skeleton));
    }
#if WITH_EDITOR
    Clip.Sequence->WaitOnExistingCompression();
    TestTrue(TEXT("compressed"), Clip.Sequence->IsCompressedDataValid());
    TestEqual(TEXT("the clip's length"), double(Clip.Sequence->GetPlayLength()), 10.0 / 30.0, 1e-4);
#endif
    return true;
}
#endif
