// FSCViewerCatalog: the core's pick-list JSON.
#include "SCViewerCatalog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FSCViewerCatalog::Parse(const FString& Json, FSCViewerCatalog& Out, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        OutError = TEXT("the catalogue is not a JSON object");
        return false;
    }
    auto Rows = [&](const TCHAR* Key, TArray<FSCCatalogEntry>& Into) {
        Into.Reset();
        const TArray<TSharedPtr<FJsonValue>>* List;
        if (!Root->TryGetArrayField(Key, List)) return;
        for (const TSharedPtr<FJsonValue>& V : *List)
        {
            const TSharedPtr<FJsonObject>* O;
            if (!V->TryGetObject(O)) continue;
            FSCCatalogEntry E;
            (*O)->TryGetStringField(TEXT("id"), E.Id);
            if (E.Id.IsEmpty()) continue;
            (*O)->TryGetStringField(TEXT("label"), E.Label);
            (*O)->TryGetStringField(TEXT("group"), E.Group);
            (*O)->TryGetStringField(TEXT("detail"), E.Detail);
            if (E.Label.IsEmpty()) E.Label = FPaths::GetBaseFilename(E.Id);
            Into.Add(MoveTemp(E));
        }
    };
    Rows(TEXT("environments"), Out.Environments);
    Rows(TEXT("vehicles"), Out.Vehicles);
    return true;
}
