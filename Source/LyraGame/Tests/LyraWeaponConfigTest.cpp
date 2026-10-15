#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/InventoryFragment_EquippableItem.h"
#include "Weapons/LyraWeaponSpawner.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "Weapons/LyraGameplayAbility_RangedWeapon.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraWeaponConfigRuntimeTest,
	"Lyra.Recoil.WeaponConfig.Runtime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraWeaponConfigRuntimeTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	APawn* Pawn = World->SpawnActor<APawn>();
	ULyraInventoryManagerComponent* Inventory = NewObject<ULyraInventoryManagerComponent>(Pawn);
	ULyraAbilitySystemComponent* ASC = NewObject<ULyraAbilitySystemComponent>(Pawn);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Pawn, Pawn);
	const FGameplayTag Size = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineSize"));
	const FGameplayTag Loaded = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	const FGameplayTag Spare = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	for (const TCHAR* Path : {
		TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle.ID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Rifle/ID_ak47.ID_ak47_C"),
		TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/ID_Shotgun.ID_Shotgun_C")})
	{
		UClass* ItemClass = LoadClass<ULyraInventoryItemDefinition>(nullptr, Path);
		if (!TestNotNull(Path, ItemClass)) return false;
		const auto* Equippable = Cast<UInventoryFragment_EquippableItem>(GetDefault<ULyraInventoryItemDefinition>(ItemClass)->FindFragmentByClass(UInventoryFragment_EquippableItem::StaticClass()));
		if (!TestNotNull(TEXT("Equipment fragment"), Equippable)) return false;
		const auto* Equipment = GetDefault<ULyraEquipmentDefinition>(Equippable->EquipmentDefinition);
		const auto* WeaponCDO = Cast<ULyraRangedWeaponInstance>(Equipment->InstanceType->GetDefaultObject());
		ULyraRecoilProfile* Profile = WeaponCDO ? WeaponCDO->GetRecoilProfile() : nullptr;
		if (!TestNotNull(TEXT("Bound profile"), Profile)) return false;
		TestTrue(TEXT("Spread enabled"), Profile->bEnableProfileSpread);
		TestTrue(TEXT("Weapon config enabled"), Profile->bEnableWeaponConfig);
		TestEqual(TEXT("Weapon index"), Profile->WeaponItemDefinition.Get(), ItemClass);
		const int32 OriginalSize = Profile->MagazineSize, OriginalSpare = Profile->SpareAmmo;
		const float OriginalRPM = Profile->FireRateRPM;
		ON_SCOPE_EXIT { Profile->MagazineSize = OriginalSize; Profile->SpareAmmo = OriginalSpare; Profile->FireRateRPM = OriginalRPM; };
		Profile->MagazineSize = 17;
		Profile->SpareAmmo = 39;
		Profile->FireRateRPM = 240.0f;
		TestEqual(TEXT("Pickup default capacity follows DA"), ALyraWeaponSpawner::GetDefaultStatFromItemDef(ItemClass, Size), 17);
		TestEqual(TEXT("Pickup default loaded ammo follows DA"), ALyraWeaponSpawner::GetDefaultStatFromItemDef(ItemClass, Loaded), 17);
		TestEqual(TEXT("Pickup default reserve follows DA"), ALyraWeaponSpawner::GetDefaultStatFromItemDef(ItemClass, Spare), 39);
		ULyraInventoryItemInstance* Item = Inventory->AddItemDefinition(ItemClass, 1);
		TestEqual(TEXT("DA overrides legacy capacity"), Item->GetStatTagStackCount(Size), 17);
		TestEqual(TEXT("Initial magazine is full"), Item->GetStatTagStackCount(Loaded), 17);
		TestEqual(TEXT("DA overrides legacy spare ammo"), Item->GetStatTagStackCount(Spare), 39);
		Item->RemoveStatTagStack(Loaded, 3);
		Item->RemoveStatTagStack(Spare, 7);
		ULyraRangedWeaponInstance* Weapon = NewObject<ULyraRangedWeaponInstance>(Pawn);
		FindFProperty<FObjectProperty>(Weapon->GetClass(), TEXT("RecoilProfile"))->SetObjectPropertyValue_InContainer(Weapon, Profile);
		Weapon->SetInstigator(Item);
		Weapon->OnEquipped();
		Weapon->OnUnequipped();
		Weapon->OnEquipped();
		TestEqual(TEXT("Re-equip preserves spent magazine"), Item->GetStatTagStackCount(Loaded), 14);
		TestEqual(TEXT("Re-equip preserves spent reserve"), Item->GetStatTagStackCount(Spare), 32);
		FGameplayAbilitySpec Spec(ULyraGameplayAbility_RangedWeapon::StaticClass(), 1, INDEX_NONE, Weapon);
		const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
		auto* Ability = Cast<ULyraGameplayAbility_RangedWeapon>(ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance());
		if (!TestNotNull(TEXT("Instanced fire ability"), Ability)) return false;
		TestEqual(TEXT("Equipped weapon provides fire interval"), Ability->GetWeaponFireInterval(0.12f), 0.25f);
		Profile->FireRateRPM = 1200.0f;
		TestEqual(TEXT("Rate edits are read dynamically"), Ability->GetWeaponFireInterval(0.12f), 0.05f);
		Profile->bEnableWeaponConfig = false;
		TestEqual(TEXT("Unconfigured weapon keeps legacy interval"), Ability->GetWeaponFireInterval(0.12f), 0.12f);
		Profile->bEnableWeaponConfig = true;
		ASC->ClearAbility(Handle);
		Weapon->OnUnequipped();
	}
	return true;
}
#endif
