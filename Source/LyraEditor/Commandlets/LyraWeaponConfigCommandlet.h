#pragma once

#include "Commandlets/Commandlet.h"
#include "LyraWeaponConfigCommandlet.generated.h"

/**
 * -run=LyraWeaponConfig: export asset/Blueprint inspection to Saved/WeaponConfigInspection.txt.
 * -migrate: initialize DA ammo/RPM from existing item/ability defaults and wire the fire timer.
 * -reticles: wire DPI-correct radius and unclamped diameter for the three weapon reticles.
 * -addpistol: append the pistol to the two-weapon starting loadout; -loadout: inspect it.
 * Existing enabled weapon settings and all recoil patterns are preserved.
 */
UCLASS()
class ULyraWeaponConfigCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	virtual int32 Main(const FString& Params) override;
};
