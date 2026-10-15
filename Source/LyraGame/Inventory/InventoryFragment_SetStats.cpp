// Copyright Epic Games, Inc. All Rights Reserved.

#include "InventoryFragment_SetStats.h"

#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/InventoryFragment_EquippableItem.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(InventoryFragment_SetStats)

namespace
{
	const ULyraRecoilProfile* FindWeaponConfig(const ULyraInventoryItemDefinition* Definition)
	{
		const auto* Equippable = Definition ? Cast<UInventoryFragment_EquippableItem>(
			Definition->FindFragmentByClass(UInventoryFragment_EquippableItem::StaticClass())) : nullptr;
		const ULyraEquipmentDefinition* Equipment = Equippable && Equippable->EquipmentDefinition
			? GetDefault<ULyraEquipmentDefinition>(Equippable->EquipmentDefinition) : nullptr;
		const ULyraRangedWeaponInstance* Weapon = Equipment && Equipment->InstanceType
			? Cast<ULyraRangedWeaponInstance>(Equipment->InstanceType->GetDefaultObject()) : nullptr;
		const ULyraRecoilProfile* Profile = Weapon ? Weapon->GetRecoilProfile() : nullptr;
		return Profile && Profile->bEnableWeaponConfig ? Profile : nullptr;
	}
}

void UInventoryFragment_SetStats::OnInstanceCreated(ULyraInventoryItemInstance* Instance) const
{
	for (const auto& KVP : InitialItemStats)
	{
		Instance->AddStatTagStack(KVP.Key, KVP.Value);
	}

	// Resolve through ID -> equipment -> instance -> profile, so there is one source of truth.
	// This hook runs only when creating an inventory item, never on equip/reload.
	const ULyraRecoilProfile* Profile = FindWeaponConfig(GetDefault<ULyraInventoryItemDefinition>(Instance->GetItemDef()));
	if (Profile)
	{
		auto SetStat = [Instance](const TCHAR* Name, int32 Count)
		{
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(Name));
			Instance->RemoveStatTagStack(Tag, Instance->GetStatTagStackCount(Tag));
			Instance->AddStatTagStack(Tag, Count);
		};
		SetStat(TEXT("Lyra.ShooterGame.Weapon.MagazineSize"), FMath::Max(1, Profile->MagazineSize));
		SetStat(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"), FMath::Max(1, Profile->MagazineSize));
		SetStat(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"), FMath::Max(0, Profile->SpareAmmo));
	}
}

int32 UInventoryFragment_SetStats::GetItemStatByTag(FGameplayTag Tag) const
{
	// Pickup spawners query this before an item exists. Use the same DA as item creation.
	if (const ULyraRecoilProfile* Profile = FindWeaponConfig(GetTypedOuter<ULyraInventoryItemDefinition>()))
	{
		const FName Name = Tag.GetTagName();
		if (Name == TEXT("Lyra.ShooterGame.Weapon.MagazineSize") || Name == TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo")) return FMath::Max(1, Profile->MagazineSize);
		if (Name == TEXT("Lyra.ShooterGame.Weapon.SpareAmmo")) return FMath::Max(0, Profile->SpareAmmo);
	}
	if (const int32* StatPtr = InitialItemStats.Find(Tag))
	{
		return *StatPtr;
	}

	return 0;
}
