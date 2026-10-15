#include "LyraWeaponConfigCommandlet.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/ObjectRedirector.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "GameplayTagContainer.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponConfig, Log, All);

namespace
{
	bool SaveAsset(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->FullyLoad();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	bool CompileAndSave(UBlueprint* BP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
		return BP->Status != BS_Error && SaveAsset(BP);
	}

	int32 ReadInitialStat(UObject* ItemCDO, const TCHAR* TagName)
	{
		TArray<UObject*> Subobjects;
		GetObjectsWithOuter(ItemCDO, Subobjects, EGetObjectsFlags::IncludeNestedObjects);
		for (UObject* Object : Subobjects)
		{
			FMapProperty* Map = FindFProperty<FMapProperty>(Object->GetClass(), TEXT("InitialItemStats"));
			if (!Map) continue;
			FScriptMapHelper Values(Map, Map->ContainerPtrToValuePtr<void>(Object));
			for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
			{
				if (Values.IsValidIndex(Index) && reinterpret_cast<FGameplayTag*>(Values.GetKeyPtr(Index))->GetTagName() == FName(TagName))
					return *reinterpret_cast<int32*>(Values.GetValuePtr(Index));
			}
		}
		return INDEX_NONE;
	}

	int32 MigrateWeaponConfig()
	{
		struct FWeapon { const TCHAR* Profile; const TCHAR* Folder; const TCHAR* Weapon; const TCHAR* Ability; };
		const FWeapon Weapons[] = {
			{TEXT("Rifle_S"), TEXT("Rifle"), TEXT("Rifle"), TEXT("Rifle_Auto")},
			{TEXT("ak47"), TEXT("Rifle"), TEXT("ak47"), TEXT("Rifle_Auto")},
			{TEXT("Pistol"), TEXT("Pistol"), TEXT("Pistol"), TEXT("Pistol")},
			{TEXT("Shotgun"), TEXT("Shotgun"), TEXT("Shotgun"), TEXT("Shotgun")},
			// Legacy rifle preset remains available; the equipped rifle uses Rifle_S.
			{TEXT("Rifle"), TEXT("Rifle"), TEXT("Rifle"), TEXT("Rifle_Auto")}
		};
		for (const FWeapon& Spec : Weapons)
		{
			ULyraRecoilProfile* Profile = LoadObject<ULyraRecoilProfile>(nullptr, *FString::Printf(TEXT("/Game/Weapons/Recoil/DA_Recoil_%s.DA_Recoil_%s"), Spec.Profile, Spec.Profile));
			const FString Base = FString::Printf(TEXT("/ShooterCore/Weapons/%s/"), Spec.Folder);
			UBlueprint* Item = LoadObject<UBlueprint>(nullptr, *(Base + TEXT("ID_") + Spec.Weapon));
			UBlueprint* Weapon = LoadObject<UBlueprint>(nullptr, *(Base + TEXT("B_WeaponInstance_") + Spec.Weapon));
			UBlueprint* Ability = LoadObject<UBlueprint>(nullptr, *(Base + TEXT("GA_Weapon_Fire_") + Spec.Ability));
			if (!Profile || !Item || !Weapon || !Ability) return 1;
			if (!Profile->bEnableWeaponConfig)
			{
				Profile->MagazineSize = ReadInitialStat(Item->GeneratedClass->GetDefaultObject(), TEXT("Lyra.ShooterGame.Weapon.MagazineSize"));
				Profile->SpareAmmo = ReadInitialStat(Item->GeneratedClass->GetDefaultObject(), TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
				FNumericProperty* Delay = FindFProperty<FNumericProperty>(Ability->GeneratedClass, TEXT("FireDelayTimeSecs"));
				if (!Delay) return 1;
				const double Seconds = Delay->GetFloatingPointPropertyValue(Delay->ContainerPtrToValuePtr<void>(Ability->GeneratedClass->GetDefaultObject()));
				if (Seconds <= 0.0 || Profile->MagazineSize < 1 || Profile->SpareAmmo < 0) return 1;
				Profile->FireRateRPM = 60.0 / Seconds;
				// ProjectContent profiles must not retain a reference back into the
				// ShooterCore GameFeature plugin. The equipped weapon instance is the
				// authoritative profile binding; this field is informational only.
				Profile->WeaponItemDefinition.Reset();
				Profile->bEnableWeaponConfig = true;
				TArray<FString> Errors;
				if (!Profile->ValidateProfile(Errors) || !SaveAsset(Profile)) return 1;
			}
			UE_LOG(LogWeaponConfig, Display, TEXT("%s: magazine=%d spare=%d rpm=%g spread=%d"), Spec.Profile, Profile->MagazineSize, Profile->SpareAmmo, Profile->FireRateRPM, Profile->bEnableProfileSpread);
			if (FString(Spec.Profile) != TEXT("Rifle"))
			{
				FObjectProperty* Ref = FindFProperty<FObjectProperty>(Weapon->GeneratedClass, TEXT("RecoilProfile"));
				if (!Ref) return 1;
				if (Ref->GetObjectPropertyValue_InContainer(Weapon->GeneratedClass->GetDefaultObject()) != Profile)
				{
					Ref->SetObjectPropertyValue_InContainer(Weapon->GeneratedClass->GetDefaultObject(), Profile);
					if (!CompileAndSave(Weapon)) return 1;
				}
			}
		}

		UBlueprint* Fire = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Weapons/GA_Weapon_Fire.GA_Weapon_Fire"));
		if (!Fire) return 1;
		UFunction* Function = Fire->ParentClass->FindFunctionByName(TEXT("GetWeaponFireInterval"));
		if (!Function) return 1;
		TArray<UEdGraph*> Graphs;
		Fire->GetAllGraphs(Graphs);
		int32 Patched = 0;
		for (UEdGraph* Graph : Graphs)
		{
			const auto Nodes = Graph->Nodes;
			for (UEdGraphNode* Node : Nodes)
			{
				// The fire interval, not montage length, controls normal completion.
				// Otherwise a slow RPM is cut short when the animation finishes first.
				if (Node->FindPin(TEXT("MontageToPlay")))
				{
					if (UEdGraphPin* Completed = Node->FindPin(TEXT("OnCompleted"))) Completed->BreakAllPinLinks();
				}
				UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
				if (!Get || Get->GetVarName() != TEXT("FireDelayTimeSecs")) continue;
				UEdGraphPin* Output = Get->FindPin(TEXT("FireDelayTimeSecs"));
				if (!Output || Output->LinkedTo.IsEmpty()) return 1;
				if (Output->LinkedTo.Num() == 1)
				{
					const UK2Node_CallFunction* Existing = Cast<UK2Node_CallFunction>(Output->LinkedTo[0]->GetOwningNode());
					if (Existing && Existing->FunctionReference.GetMemberName() == Function->GetFName()) { ++Patched; continue; }
				}
				UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
				Graph->AddNode(Call, false, false);
				Call->CreateNewGuid();
				Call->SetFromFunction(Function);
				Call->AllocateDefaultPins();
				Call->NodePosX = Node->NodePosX + 230;
				Call->NodePosY = Node->NodePosY + 80;
				const auto Links = Output->LinkedTo;
				Output->BreakAllPinLinks();
				if (!Graph->GetSchema()->TryCreateConnection(Output, Call->FindPinChecked(TEXT("DefaultInterval")))) return 1;
				for (UEdGraphPin* Link : Links)
					if (!Graph->GetSchema()->TryCreateConnection(Call->FindPinChecked(TEXT("ReturnValue")), Link)) return 1;
				++Patched;
			}
		}
		if (Patched != 1 || !CompileAndSave(Fire)) return 1;
		UE_LOG(LogWeaponConfig, Display, TEXT("Weapon configuration migration complete; fire timer reads the equipped weapon profile."));
		return 0;
	}

	int32 ClearWeaponItemReferences()
	{
		for (const TCHAR* Name : {TEXT("Rifle"), TEXT("Rifle_S"), TEXT("ak47"), TEXT("Pistol"), TEXT("Shotgun")})
		{
			const FString Path = FString::Printf(TEXT("/Game/Weapons/Recoil/DA_Recoil_%s.DA_Recoil_%s"), Name, Name);
			ULyraRecoilProfile* Profile = LoadObject<ULyraRecoilProfile>(nullptr, *Path);
			if (!Profile)
			{
				UE_LOG(LogWeaponConfig, Error, TEXT("Unable to load recoil profile %s"), *Path);
				return 1;
			}

			if (!Profile->WeaponItemDefinition.IsNull())
			{
				Profile->WeaponItemDefinition.Reset();
				if (!SaveAsset(Profile)) return 1;
				UE_LOG(LogWeaponConfig, Display, TEXT("Cleared cross-plugin weapon index from %s"), *Path);
			}
		}
		return 0;
	}

	int32 MigrateReticles()
	{
		for (const TCHAR* Name : {TEXT("Rifle"), TEXT("Pistol"), TEXT("Shotgun")})
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("/ShooterCore/Weapons/%s/W_Reticle_%s.W_Reticle_%s"), Name, Name, Name));
			if (!BP) return 1;
			UFunction* RadiusFunction = BP->ParentClass->FindFunctionByName(TEXT("ComputeSpreadRadiusInWidgetUnits"));
			if (!RadiusFunction) return 1;
			TArray<UEdGraph*> Graphs;
			BP->GetAllGraphs(Graphs);
			int32 RadiusLinks = 0, SizeLinks = 0;
			for (UEdGraph* Graph : Graphs)
			{
				UK2Node_CallFunction* Radius = nullptr;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
					if (Call && (Call->FunctionReference.GetMemberName() == TEXT("ComputeMaxScreenspaceSpreadRadius") || Call->FunctionReference.GetMemberName() == RadiusFunction->GetFName()))
					{
						Call->SetFromFunction(RadiusFunction);
						Call->ReconstructNode();
						Radius = Call;
						break;
					}
				}
				if (!Radius) continue;
				UK2Node_CallFunction* Diameter = nullptr;
				for (UEdGraphNode* Node : Graph->Nodes)
					if (Node->NodeComment == TEXT("Exact spread diameter in UMG units")) Diameter = Cast<UK2Node_CallFunction>(Node);
				if (!Diameter)
				{
					Diameter = NewObject<UK2Node_CallFunction>(Graph);
					Graph->AddNode(Diameter, false, false);
					Diameter->CreateNewGuid();
					Diameter->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Multiply_DoubleDouble")));
					Diameter->AllocateDefaultPins();
					Diameter->NodePosX = Radius->NodePosX + 260;
					Diameter->NodePosY = Radius->NodePosY + 120;
					Diameter->NodeComment = TEXT("Exact spread diameter in UMG units");
					Graph->GetSchema()->TrySetDefaultValue(*Diameter->FindPinChecked(TEXT("B")), TEXT("2.0"));
					if (!Graph->GetSchema()->TryCreateConnection(Radius->GetReturnValuePin(), Diameter->FindPinChecked(TEXT("A")))) return 1;
				}
				const auto Nodes = Graph->Nodes;
				for (UEdGraphNode* Node : Nodes)
				{
					UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
					if (!Call) continue;
					const FName FunctionName = Call->FunctionReference.GetMemberName();
					if (FunctionName == TEXT("SetRadius"))
					{
						UEdGraphPin* Input = Call->FindPinChecked(TEXT("InRadius"));
						Input->BreakAllPinLinks();
						if (!Graph->GetSchema()->TryCreateConnection(Radius->GetReturnValuePin(), Input)) return 1;
						++RadiusLinks;
					}
					else if (FunctionName == TEXT("SetWidthOverride") || FunctionName == TEXT("SetHeightOverride"))
					{
						UEdGraphPin* Input = Call->FindPinChecked(FunctionName == TEXT("SetWidthOverride") ? TEXT("InWidthOverride") : TEXT("InHeightOverride"));
						Input->BreakAllPinLinks();
						if (!Graph->GetSchema()->TryCreateConnection(Diameter->GetReturnValuePin(), Input)) return 1;
						++SizeLinks;
					}
				}
			}
			if (RadiusLinks != 1 || SizeLinks != 2 || !CompileAndSave(BP)) return 1;
			UE_LOG(LogWeaponConfig, Display, TEXT("%s reticle: DPI-correct radius, diameter=2r, no visual clamp"), Name);
		}
		return 0;
	}
}

int32 ULyraWeaponConfigCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("ak47sockets")))
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Weapons/ak47/Mesh/SK_ak47"));
		if (!Mesh) return 1;
		const TPair<FName, FVector> Sockets[] = {
			{TEXT("Muzzle"), FVector(64.2, 0.0, 9.9543)},
			{TEXT("ShellEject"), FVector(17.2, 2.3, 11.4)},
			{TEXT("ShellEject_0"), FVector(17.2, 1.9, 11.4)}
		};
		for (const auto& Spec : Sockets)
		{
			USkeletalMeshSocket* Socket = Mesh->FindSocket(Spec.Key);
			if (!Socket || Socket->GetOuter() != Mesh)
			{
				Socket = NewObject<USkeletalMeshSocket>(Mesh);
				Socket->SocketName = Spec.Key;
				Mesh->AddSocket(Socket, false);
			}
			Socket->BoneName = TEXT("Grip");
			Socket->RelativeLocation = Spec.Value;
			Socket->RelativeRotation = FRotator::ZeroRotator;
			Socket->RelativeScale = FVector::OneVector;
		}
		return SaveAsset(Mesh) ? 0 : 1;
	}
	if (FParse::Param(*Params, TEXT("fixakredirectors")))
	{
		TArray<UObjectRedirector*> Redirectors;
		for (const TCHAR* Path : {TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle_7"),
			TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle_7"), TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle_7"),
			TEXT("/ShooterCore/Weapons/Rifle/B_WeaponInstance_Rifle_7")})
		{
			if (!FPackageName::DoesPackageExist(Path)) continue;
			UPackage* Package = LoadPackage(nullptr, Path, LOAD_NoWarn);
			if (Package)
				if (auto* Redirector = FindObject<UObjectRedirector>(Package, *FPackageName::GetLongPackageAssetName(Path))) Redirectors.Add(Redirector);
		}
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().FixupReferencers(Redirectors, false);
		UE_LOG(LogWeaponConfig, Display, TEXT("Fixed %d ak47 migration redirectors."), Redirectors.Num());
		return 0;
	}

	if (FParse::Param(*Params, TEXT("loadout")) || FParse::Param(*Params, TEXT("addpistol")))
	{
		UBlueprint* Hero = LoadObject<UBlueprint>(nullptr, TEXT("/ShooterCore/Game/B_Hero_ShooterMannequin.B_Hero_ShooterMannequin"));
		if (!Hero || !Hero->GeneratedClass) return 1;
		FArrayProperty* Inventory = FindFProperty<FArrayProperty>(Hero->GeneratedClass, TEXT("InitialInventoryItems"));
		FObjectPropertyBase* ItemProperty = Inventory ? CastField<FObjectPropertyBase>(Inventory->Inner) : nullptr;
		if (!ItemProperty) return 1;
		FScriptArrayHelper Items(Inventory, Inventory->ContainerPtrToValuePtr<void>(Hero->GeneratedClass->GetDefaultObject()));
		if (FParse::Param(*Params, TEXT("addpistol")))
		{
			UClass* Pistol = LoadClass<UObject>(nullptr, TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"));
			if (!Pistol) return 1;
			if (Items.Num() == 3 && ItemProperty->GetObjectPropertyValue(Items.GetRawPtr(2)) == Pistol)
			{
				UE_LOG(LogWeaponConfig, Display, TEXT("Third slot already contains the pistol."));
			}
			else
			{
				if (Items.Num() != 2)
				{
					UE_LOG(LogWeaponConfig, Error, TEXT("Expected two existing starting weapons, found %d; no changes saved."), Items.Num());
					return 1;
				}
				ItemProperty->SetObjectPropertyValue(Items.GetRawPtr(Items.AddValue()), Pistol);
				if (!CompileAndSave(Hero)) return 1;
			}
		}
		// Reacquire the CDO after Blueprint compilation, which can replace it.
		Inventory = FindFProperty<FArrayProperty>(Hero->GeneratedClass, TEXT("InitialInventoryItems"));
		ItemProperty = CastFieldChecked<FObjectPropertyBase>(Inventory->Inner);
		FScriptArrayHelper SavedItems(Inventory, Inventory->ContainerPtrToValuePtr<void>(Hero->GeneratedClass->GetDefaultObject()));
		for (int32 Index = 0; Index < SavedItems.Num(); ++Index)
		{
			UE_LOG(LogWeaponConfig, Display, TEXT("Starting slot %d: %s"), Index + 1, *GetPathNameSafe(ItemProperty->GetObjectPropertyValue(SavedItems.GetRawPtr(Index))));
		}
		return 0;
	}
	if (FParse::Param(*Params, TEXT("migrate"))) return MigrateWeaponConfig();
	if (FParse::Param(*Params, TEXT("clearweaponrefs"))) return ClearWeaponItemReferences();
	if (FParse::Param(*Params, TEXT("reticles"))) return MigrateReticles();
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(FPaths::ProjectPluginsDir() / TEXT("GameFeatures/ShooterCore/Content/Weapons")), TEXT("*.uasset"), true, false);
	TArray<FString> GameFiles;
	IFileManager::Get().FindFilesRecursive(GameFiles, *(FPaths::ProjectContentDir() / TEXT("Weapons")), TEXT("GA_*.uasset"), true, false);
	Files.Append(GameFiles);
	FString Report;
	for (const FString& File : Files)
	{
		const FString Name = FPaths::GetBaseFilename(File);
		if (!Name.StartsWith(TEXT("GA_")) && !Name.StartsWith(TEXT("ID_")) && !Name.StartsWith(TEXT("B_WeaponInstance")) && !Name.StartsWith(TEXT("W_Reticle"))) continue;
		FString Relative = File;
		const bool bGame = File.StartsWith(FPaths::ProjectContentDir());
		FPaths::MakePathRelativeTo(Relative, *(bGame ? FPaths::ProjectContentDir() : FPaths::ProjectPluginsDir() / TEXT("GameFeatures/ShooterCore/Content/")));
		const FString Path = (bGame ? TEXT("/Game/") : TEXT("/ShooterCore/")) + FPaths::ChangeExtension(Relative, TEXT("")) + TEXT(".") + Name;
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
		if (!BP || !BP->GeneratedClass) return 1;
		Report += TEXT("\nASSET ") + Path + TEXT(" PARENT ") + BP->ParentClass->GetPathName() + TEXT("\n");
		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		for (TFieldIterator<FProperty> It(CDO->GetClass()); It; ++It)
		{
			const FString PropName = It->GetName();
			if (It->GetOwnerClass() == BP->GeneratedClass || PropName.Contains(TEXT("Fire")) || PropName.Contains(TEXT("Auto")) || PropName.Contains(TEXT("Spread")) || PropName.Contains(TEXT("Heat")) || PropName.Contains(TEXT("Recoil")) || PropName == TEXT("Fragments"))
			{
				FString Value;
				It->ExportText_InContainer(0, Value, CDO, CDO, CDO, PPF_None);
				Report += PropName + TEXT(" = ") + Value + TEXT("\n");
			}
		}
		TArray<UObject*> Subobjects;
		GetObjectsWithOuter(CDO, Subobjects, EGetObjectsFlags::IncludeNestedObjects);
		if (Name.StartsWith(TEXT("W_Reticle")))
		{
			TArray<UObject*> Widgets;
			GetObjectsWithOuter(BP, Widgets, EGetObjectsFlags::IncludeNestedObjects);
			for (UObject* Widget : Widgets)
			{
				if (Widget->GetClass()->GetName() == TEXT("CircumferenceMarkerWidget") || Widget->GetClass()->GetName() == TEXT("Image")) Subobjects.Add(Widget);
			}
		}
		for (UObject* Subobject : Subobjects)
		{
			Report += TEXT("SUBOBJECT ") + Subobject->GetName() + TEXT("\n");
			for (TFieldIterator<FProperty> It(Subobject->GetClass()); It; ++It)
			{
				FString Value;
				It->ExportText_InContainer(0, Value, Subobject, Subobject, Subobject, PPF_None);
				Report += It->GetName() + TEXT(" = ") + Value + TEXT("\n");
			}
		}
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			Report += TEXT("GRAPH ") + Graph->GetName() + TEXT("\n");
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				Report += TEXT("NODE ") + Node->GetName() + TEXT(" ") + Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString() + TEXT("\n");
				for (UEdGraphPin* Pin : Node->Pins)
				{
					Report += FString::Printf(TEXT("  %s %s = %s"), Pin->Direction == EGPD_Input ? TEXT("IN") : TEXT("OUT"), *Pin->PinName.ToString(), *Pin->DefaultValue);
					for (UEdGraphPin* Link : Pin->LinkedTo) Report += TEXT(" -> ") + Link->GetOwningNode()->GetName() + TEXT(".") + Link->PinName.ToString();
					Report += TEXT("\n");
				}
			}
		}
	}
	for (const TCHAR* Name : {TEXT("Rifle"), TEXT("Rifle_S"), TEXT("ak47"), TEXT("Pistol"), TEXT("Shotgun")})
	{
		const FString Path = FString::Printf(TEXT("/Game/Weapons/Recoil/DA_Recoil_%s.DA_Recoil_%s"), Name, Name);
		ULyraRecoilProfile* Profile = LoadObject<ULyraRecoilProfile>(nullptr, *Path);
		if (!Profile) return 1;
		Report += FString::Printf(TEXT("PROFILE %s enabled=%d standing=%g max=%g\n"), Name, Profile->bEnableProfileSpread, Profile->SpreadAngle_Standing, Profile->MaxSpreadAngle_Standing);
	}
	for (const TCHAR* Name : {TEXT("Rifle"), TEXT("Pistol"), TEXT("Shotgun")})
	{
		UObject* Material = LoadObject<UObject>(nullptr, *FString::Printf(TEXT("/Game/UI/Hud/Art/MI_UI_Reticles_Outer_%s.MI_UI_Reticles_Outer_%s"), Name, Name));
		if (!Material) return 1;
		Report += TEXT("MATERIAL ") + Material->GetPathName() + TEXT("\n");
		for (const TCHAR* Prop : {TEXT("ScalarParameterValues"), TEXT("VectorParameterValues"), TEXT("Parent")})
		{
			FProperty* Property = FindFProperty<FProperty>(Material->GetClass(), Prop);
			FString Value;
			Property->ExportText_InContainer(0, Value, Material, Material, Material, PPF_None);
			Report += FString(Prop) + TEXT(" = ") + Value + TEXT("\n");
		}
	}
	return FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("WeaponConfigInspection.txt"))) ? 0 : 1;
}
