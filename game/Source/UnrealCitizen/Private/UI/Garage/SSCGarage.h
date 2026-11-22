// mobiGlas Garage. The fleet list is virtualized and the configuration pane tracks the selected vehicle.
#pragma once

#include "CoreMinimal.h"
#include "SCGarageSubsystem.h"
#include "Widgets/SCompoundWidget.h"

class FSCMobiGlasModel;
class SBox;
template<typename ItemType> class SListView;

class SSCGarage : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCGarage) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;

private:
    USCGarageSubsystem* Garage() const;
    const FSCGarageRow* Selected() const;
    void Refresh();
    void SyncSelection(bool bRowsChanged);
    TSharedRef<SWidget> Details();
    void Retrieve(ESCGaragePlacement At);
    void Store();
    void AddToFleet();
    TSharedPtr<FSCMobiGlasModel> Model;
    TArray<TSharedPtr<FSCGarageRow>> Visible;
    TSharedPtr<SListView<TSharedPtr<FSCGarageRow>>> List;
    TSharedPtr<SBox> DetailBody;
    TArray<FSCGarageRow> Fleet;
    TMap<FName, FString> EntityHints;
    FString Search;
    FString LastSelectedEntity;
    FString DetailKey = TEXT("<uninitialized>");
    FString Error;
    FString ConfirmEntity;
    int32 ConfirmOccupant = INDEX_NONE;
    bool bBrowse = false;
    float Age = 1.f;
};
