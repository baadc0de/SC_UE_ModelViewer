// Processed cache tests use synthetic bytes and never open the game archive.
#if WITH_DEV_AUTOMATION_TESTS
#include "SCProcessedCache.h"
#include "SCProcessedTextureCache.h"
#include "SCTextureImporter.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCProcessedCacheDiskTest, "SCBridge.Import.ProcessedCache.Disk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FSCProcessedCacheDiskTest::RunTest(const FString&)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("Tests") /
        FString::Printf(TEXT("ProcessedCache-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString A = FSCProcessedCache::Key(TEXT("build-1"), TEXT("a"));
    const FString B = FSCProcessedCache::Key(TEXT("build-1"), TEXT("b"));
    const FString C = FSCProcessedCache::Key(TEXT("build-1"), TEXT("c"));
    TestNotEqual(TEXT("Other archive builds have their own keys"), A, FSCProcessedCache::Key(TEXT("build-2"), TEXT("a")));
    TestNotEqual(TEXT("Key fields are framed"), FSCProcessedCache::Key(TEXT("ab"), TEXT("c")),
        FSCProcessedCache::Key(TEXT("a"), TEXT("bc")));

    const TArray<uint8> Value{1, 2, 3, 4};
    {
        FSCProcessedCache Cache(Root, 8);
        TestTrue(TEXT("Cache opens"), Cache.IsEnabled());
        TestTrue(TEXT("Write A"), Cache.WriteBlob(A, Value));
        TestTrue(TEXT("Write B"), Cache.WriteBlob(B, Value));
        TArray<uint8> Read;
        TestTrue(TEXT("Read A refreshes its usage"), Cache.ReadBlob(A, Read));
        TestTrue(TEXT("Data is unchanged"), Read == Value);
        TestTrue(TEXT("Write C evicts one entry"), Cache.WriteBlob(C, Value));
        TestFalse(TEXT("Least recently used B was evicted"), Cache.ReadBlob(B, Read));
        TestEqual(TEXT("Byte accounting"), Cache.Stats().Bytes, int64(8));
        TestTrue(TEXT("Manifest is published"), Cache.Flush());
    }
    {
        FSCProcessedCache Cache(Root, 8);
        TArray<uint8> Read;
        TestTrue(TEXT("A survives restart"), Cache.ReadBlob(A, Read));
        FString Directory;
        if (!TestTrue(TEXT("C survives restart"), Cache.Find(C, Directory))) return false;
        TestTrue(TEXT("Damage a payload without truncating it"), FFileHelper::SaveArrayToFile(TArray<uint8>{4, 3, 2, 1},
            *(Directory / FSCProcessedCache::PayloadName)));
        TestFalse(TEXT("Checksum mismatch is a miss"), Cache.ReadBlob(C, Read));
        TestTrue(TEXT("A corrupt entry can be rebuilt"), Cache.WriteBlob(C, Value));
        TestTrue(TEXT("Rebuilt C is readable"), Cache.ReadBlob(C, Read));
        Cache.Clear();
        TestEqual(TEXT("Clear frees the complete cache"), Cache.Stats().Entries, 0);
    }
    FSCProcessedCache Cache(Root, 8);
    TArray<uint8> Read;
    TestFalse(TEXT("Clear persists across restart"), Cache.ReadBlob(A, Read));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCProcessedCachePinTest, "SCBridge.Import.ProcessedCache.Pin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FSCProcessedCachePinTest::RunTest(const FString&)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("Tests") /
        FString::Printf(TEXT("ProcessedCache-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString Key = FSCProcessedCache::Key(TEXT("leases"), TEXT("fake-package"));
    const TArray<uint8> Bytes{1, 2, 3, 4};
    FSCProcessedCache Cache(Root, 16);
    TestTrue(TEXT("Write test entry"), Cache.WriteBlob(Key, Bytes));
    TUniquePtr<FSCProcessedCache::FReadPin> Pin = Cache.PinEntry(Key);
    if (!TestTrue(TEXT("Lookup holds a provisional package pin"), Pin.IsValid())) return false;
    const FString FirstDirectory = Pin->Directory;
    Cache.Clear();
    TestEqual(TEXT("Clear removes the pinned key from the index"), Cache.Stats().Entries, 0);
    TestEqual(TEXT("Retired files count towards disk usage"), Cache.Stats().Bytes, int64(4));
    TestTrue(TEXT("A pending load can still use its files"), IFileManager::Get().DirectoryExists(*FirstDirectory));
    Pin.Reset();
    TestFalse(TEXT("Cancel or load failure releases the provisional pin"), IFileManager::Get().DirectoryExists(*FirstDirectory));

    TestTrue(TEXT("Rebuild"), Cache.WriteBlob(Key, Bytes));
    Pin = Cache.PinEntry(Key);
    if (!TestTrue(TEXT("Read rebuilt package"), Pin.IsValid())) return false;
    const FString SecondDirectory = Pin->Directory;
    TStrongObjectPtr<UTexture2D> Asset(NewObject<UTexture2D>());
    Cache.KeepLoaded(SecondDirectory, Asset.Get());
    Pin.Reset();
    TestTrue(TEXT("Same key can be written with a new generation"), Cache.WriteBlob(Key, Bytes));
    Cache.Clear();
    CollectGarbage(RF_NoFlags, true);
    TestTrue(TEXT("In-use asset keeps its old generation even across a GC"), IFileManager::Get().DirectoryExists(*SecondDirectory));
    Asset.Reset();
    CollectGarbage(RF_NoFlags, true);
    TestFalse(TEXT("Unloading returns its old generation's space"), IFileManager::Get().DirectoryExists(*SecondDirectory));
    TestEqual(TEXT("Clear and purge return all bytes"), Cache.Stats().Bytes, int64(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCProcessedCacheTextureTest, "SCBridge.Import.ProcessedCache.Texture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FSCProcessedCacheTextureTest::RunTest(const FString&)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("Tests") /
        FString::Printf(TEXT("ProcessedCache-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    FSCProcessedCache Cache(Root / TEXT("processed"), 1024);
    IImageWrapperModule& Image = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Png = Image.CreateImageWrapper(EImageFormat::PNG);
    const TArray<uint8> Pixels = {10, 40, 70, 255, 100, 120, 130, 255};
    Png->SetRaw(Pixels.GetData(), Pixels.Num(), 2, 1, ERGBFormat::RGBA, 8);
    FFileHelper::SaveArrayToFile(Png->GetCompressed(), *(Root / TEXT("test.png")));
    FSCFolderPackageSource Source(Root);
    const TArray<FSCTextureKey> Keys = {{TEXT("test.png"), ESCTextureRole::Color}, {TEXT("test.png"), ESCTextureRole::Normal}};
    FSCTextureCacheRequest Request{&Cache, TEXT("fixture-build")};
    FSCTextureLimits Limits;
    Limits.MaxDimension = 256;
    const auto First = FSCTextureImporter::DecodeAll(Source, Keys, Limits, &Request);
    const auto Second = FSCTextureImporter::DecodeAll(Source, Keys, Limits, &Request);
    for (int32 I = 0; I < Keys.Num(); ++I)
    {
        TestTrue(TEXT("First decode creates the processed mips"), !First[I].bFromCache && First[I].Error.IsEmpty());
        TestTrue(TEXT("Second decode gets its processed mips from disk"), Second[I].bFromCache);
        TestTrue(TEXT("Mip bytes match exactly"), First[I].Mips == Second[I].Mips);
    }
    TestTrue(TEXT("Normal and color remain separate"), First[0].Mips != First[1].Mips);
    Request.ArchiveBuild = TEXT("different-build");
    TestFalse(TEXT("Other archive builds cannot hit"), FSCTextureImporter::DecodeAll(Source, Keys, Limits, &Request)[0].bFromCache);
    Limits.BudgetBytes = First[0].UploadBytes();
    const auto Budgeted = FSCTextureImporter::DecodeAll(Source, Keys, Limits, &Request);
    TestTrue(TEXT("Warm bytes still count against the texture budget"), Budgeted[0].bFromCache && !Budgeted[1].Error.IsEmpty());
    return true;
}
#endif
