// Music library and transport for the mobiGlas shell.
#pragma once

#include "CoreMinimal.h"
#include "SCMusicSubsystem.h"
#include "Widgets/SCompoundWidget.h"

class FSCMobiGlasModel;
template<typename ItemType> class SListView;

class SSCMusic : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCMusic) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;

private:
    USCMusicSubsystem* Music() const;
    const FSCMusicCue* CurrentCue() const;
    FText LibraryStatus() const;
    FText TrackTitle() const;
    FText TrackTime() const;

    TSharedPtr<FSCMobiGlasModel> Model;
    TArray<TSharedPtr<FSCMusicCue>> Rows;
    TSharedPtr<SListView<TSharedPtr<FSCMusicCue>>> List;
    TSharedPtr<FSCMusicCue> Selected;
    int32 Generation = INDEX_NONE;
    int32 CuesRevision = INDEX_NONE;
};
