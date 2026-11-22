// Ship Link's three scrollable panels inside the shared mobiGlas app shell.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FSCMobiGlasModel;
class SBox;
class SScrollBox;

class SSCShipLink : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCShipLink) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;

private:
    TSharedRef<SWidget> Build();
    TSharedPtr<FSCMobiGlasModel> Model;
    TSharedPtr<SBox> Content;
    TSharedPtr<SScrollBox> DoorScroll;
    TMap<int32, TWeakPtr<SWidget>> DoorRows;
    int32 SeenRevision = INDEX_NONE;
    int32 SeenDoor = INDEX_NONE;
};
