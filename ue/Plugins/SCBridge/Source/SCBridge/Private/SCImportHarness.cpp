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
                // Review camera: -SCView=yaw,pitch,distance (degrees, x bounds radius) orbits the bounds
                // centre, -SCTarget=x,y,z offsets the look-at point in cm. Defaults frame the whole model.
                FVector Center = Bounds.GetCenter(); const double Radius = Bounds.GetExtent().Size();
                FString View, Target; TArray<FString> V, T;
                double Yaw = 45, Pitch = 23, Distance = 1.536;
                if (FParse::Value(FCommandLine::Get(), TEXT("SCView="), View, false) && View.ParseIntoArray(V, TEXT(",")) == 3)
                { Yaw = FCString::Atod(*V[0]); Pitch = FCString::Atod(*V[1]); Distance = FCString::Atod(*V[2]); }
                if (FParse::Value(FCommandLine::Get(), TEXT("SCTarget="), Target, false) && Target.ParseIntoArray(T, TEXT(",")) == 3)
                    Center += FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
                const FVector Offset = FRotator(Pitch, Yaw, 0).Vector() * Radius * Distance;
                auto* Camera = World->SpawnActor<ACameraActor>(Center + Offset, FRotator::ZeroRotator);
                Camera->SetActorRotation((Center - Camera->GetActorLocation()).Rotation());
                if (auto* PC = World->GetFirstPlayerController()) PC->SetViewTarget(Camera);
                float Glow;
                if (FParse::Value(FCommandLine::Get(), TEXT("SCEngineGlow="), Glow)) Vehicle->SetEngineGlow(Glow); // review aid
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
