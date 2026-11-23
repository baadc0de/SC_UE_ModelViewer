// WP-O1-4 (was game/scripts/cook_audio.py, WP-A1): a prepared level has no package at run time, so its audio travels
// beside it as loose files the runtime reads the way it reads a package: Content/SCAudio/<level>/Packages/<package>/
// audio.json, the media (Audio/<id>.ogg) and impulse responses (Audio/ir/<id>.f32) it lists, and Packages/<package>/
// audio_interiors.json — per interior the scene's audio_spots, audio_areas and room_volumes (the prepared actor keeps
// its interior anchors). The packaging settings stage Content/SCAudio as non-UFS files (DefaultGame.ini);
// ASCVehicle::BeginPlay loads them (ASCVehicle::CookedAudio). The files are game-derived: Content/SCAudio is gitignored.
#pragma once
#include "CoreMinimal.h"

class FSCPackageSource;

namespace SCPrepareAudio
{
/** Copy the audio of Source's package Scene (Packages/<name>/scene.json) to the folder Target. The previous staging is
 *  replaced only once every file is read and written. False with Error when the package has no audio.json or a media /
 *  impulse-response file it lists is missing. */
bool Stage(const FSCPackageSource& Source, const FString& Scene, const FString& Target, int32& OutFiles, FString& Error);
} // namespace SCPrepareAudio
