// WP-O1-4 (was game/scripts/cook_audio.py, WP-A1): a prepared level has no package at run time, so its audio travels
// with it. Its media become SoundWave assets beside the level's other assets (MakeWaves; WP-O1-4c-1): UE cooks and
// decodes them, the media's loop region kept as a loop cue point, and the prepared actor references them
// (ASCVehicle::CookedAudioMedia). The rest travels as loose files the runtime reads the way it reads a package:
// Content/SCAudio/<level>/Packages/<package>/audio.json, the impulse responses (Audio/ir/<id>.f32) it lists, and
// Packages/<package>/audio_interiors.json — per interior the scene's audio_spots, audio_areas and room_volumes (the
// prepared actor keeps its interior anchors). The packaging settings stage Content/SCAudio as non-UFS files
// (DefaultGame.ini); ASCVehicle::BeginPlay loads them (ASCVehicle::CookedAudio). The files and assets are game-derived:
// Content/SCAudio and Content/SC are gitignored.
#pragma once
#include "CoreMinimal.h"

class FSCPackageSource;
class USoundWave;

namespace SCPrepareAudio
{
/** Copy the audio records of Source's package Scene (Packages/<name>/scene.json) to the folder Target: audio.json, its
 *  impulse responses and the interiors' records (not its media: MakeWaves). The previous staging is replaced only once
 *  every file is read and written. False with Error when the package has no audio.json or an impulse response it lists
 *  is missing. */
bool Stage(const FSCPackageSource& Source, const FString& Scene, const FString& Target, int32& OutFiles, FString& Error);

/** Save every media audio.json lists as a SoundWave asset <Folder>/M_<id>: decoded from its Ogg once, then FillSoundWave
 *  (SCAudioSource.h: Bink Audio, inline, its loop region and exact length as cue points). The folder must hold no older
 *  assets (the prep step deletes a level's folder first). False with Error when a media is missing, does not decode or
 *  does not save. */
bool MakeWaves(const FSCPackageSource& Source, const FString& Scene, const FString& Folder, TMap<uint32, TObjectPtr<USoundWave>>& Out,
               FString& Error);
} // namespace SCPrepareAudio
