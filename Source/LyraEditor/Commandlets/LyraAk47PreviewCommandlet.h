#pragma once
#include "Commandlets/Commandlet.h"
#include "LyraAk47PreviewCommandlet.generated.h"

/** Render the actual imported mesh in rifle idle/reload poses to Saved/AK47Work.
 * Run with -run=LyraAk47Preview -AllowCommandletRendering (without -nullrhi).
 * Does not save or change project assets.
 */
UCLASS()
class ULyraAk47PreviewCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    virtual int32 Main(const FString& Params) override;
};
