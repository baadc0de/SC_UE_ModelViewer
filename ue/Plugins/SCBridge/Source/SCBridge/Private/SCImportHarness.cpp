#include "SCImportHarness.h"
#include "SCImporter.h"
#include "SCVehicle.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"

void FSCImportHarness::Start()
{
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder)) return;
    Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Folder](float)
    {
        UWorld* World = nullptr;
        for (const auto& Context : GEngine->GetWorldContexts()) if (Context.World() && Context.World()->IsGameWorld() && Context.World()->HasBegunPlay()) World = Context.World();
        if (!World) return true;
        FString Tar;
        if (FParse::Value(FCommandLine::Get(), TEXT("SCVerifyTar="), Tar))
        {
            const bool bOK = FSCImporter::Verify(Folder, Tar, World);
            FPlatformMisc::RequestExitWithStatus(false, bOK ? 0 : 1);
        }
        else
        {
            ASCVehicle* Vehicle = USCImportLibrary::ImportFolder(World, Folder);
            if (Vehicle)
            {
                const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
                const FVector Center = Bounds.GetCenter(); const double Radius = Bounds.GetExtent().Size();
                auto* Camera = World->SpawnActor<ACameraActor>(Center + FVector(Radius, Radius, Radius * 0.6), FRotator::ZeroRotator);
                Camera->SetActorRotation((Center - Camera->GetActorLocation()).Rotation());
                if (auto* PC = World->GetFirstPlayerController()) PC->SetViewTarget(Camera);
            }
            if (!FParse::Param(FCommandLine::Get(), TEXT("SCStay"))) FPlatformMisc::RequestExitWithStatus(false, Vehicle ? 0 : 1);
        }
        return false;
    }), 0.5f);
}
void FSCImportHarness::Stop()
{
    FTSTicker::GetCoreTicker().RemoveTicker(Tick);
}
