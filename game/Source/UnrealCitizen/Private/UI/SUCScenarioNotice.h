// WP-O1-4: a view overlay that says why the scenario does not run (refused: not prepared or stale, with the prep command;
// failed: a prepared level did not load). Invisible otherwise.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class USCViewerSubsystem;

class SUCScenarioNotice : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUCScenarioNotice) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);

private:
    class UUCScenarioSubsystem* Scenario() const;
    EVisibility NoticeVisibility() const;
    FText Heading() const;
    FText Body() const;

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
};
