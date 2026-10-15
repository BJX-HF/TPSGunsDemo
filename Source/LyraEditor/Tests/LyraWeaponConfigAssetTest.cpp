#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_CallFunction.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraWeaponConfigAssetTest,
	"Lyra.Recoil.WeaponConfig.BlueprintWiring", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraWeaponConfigAssetTest::RunTest(const FString& Parameters)
{
	UBlueprint* Fire = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Weapons/GA_Weapon_Fire.GA_Weapon_Fire"));
	if (!TestNotNull(TEXT("Base fire Blueprint"), Fire)) return false;
	TArray<UEdGraph*> Graphs;
	Fire->GetAllGraphs(Graphs);
	int32 Timers = 0;
	for (UEdGraph* Graph : Graphs)
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call && Call->FunctionReference.GetMemberName() == TEXT("GetWeaponFireInterval"))
		{
			UEdGraphPin* Return = Call->GetReturnValuePin();
			TestEqual(TEXT("Interval drives exactly one timer"), Return->LinkedTo.Num(), 1);
			if (Return->LinkedTo.Num() == 1) TestEqual(TEXT("Interval connected to Time"), Return->LinkedTo[0]->PinName, FName(TEXT("Time")));
			++Timers;
		}
		if (Node->FindPin(TEXT("MontageToPlay")))
		{
			if (UEdGraphPin* Completed = Node->FindPin(TEXT("OnCompleted")))
				TestEqual(TEXT("Montage completion cannot shorten the configured interval"), Completed->LinkedTo.Num(), 0);
		}
	}
	TestEqual(TEXT("One profile-driven fire timer"), Timers, 1);
	for (const TCHAR* Name : {TEXT("Rifle"), TEXT("Pistol"), TEXT("Shotgun")})
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("/ShooterCore/Weapons/%s/W_Reticle_%s.W_Reticle_%s"), Name, Name, Name));
		if (!TestNotNull(Name, BP)) return false;
		Graphs.Reset();
		BP->GetAllGraphs(Graphs);
		int32 Radii = 0, Dimensions = 0;
		for (UEdGraph* Graph : Graphs)
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			if (!Call) continue;
			const FName Function = Call->FunctionReference.GetMemberName();
			if (Function == TEXT("SetRadius"))
			{
				UEdGraphPin* Input = Call->FindPinChecked(TEXT("InRadius"));
				if (!TestEqual(TEXT("Radius has a source"), Input->LinkedTo.Num(), 1)) return false;
				const auto* Source = Cast<UK2Node_CallFunction>(Input->LinkedTo[0]->GetOwningNode());
				TestTrue(TEXT("Marker directly uses DPI-correct radius with no clamp"), Source && Source->FunctionReference.GetMemberName() == TEXT("ComputeSpreadRadiusInWidgetUnits"));
				++Radii;
			}
			if (Function == TEXT("SetWidthOverride") || Function == TEXT("SetHeightOverride"))
			{
				UEdGraphPin* Input = Call->FindPinChecked(Function == TEXT("SetWidthOverride") ? TEXT("InWidthOverride") : TEXT("InHeightOverride"));
				if (!TestEqual(TEXT("Dimension has a source"), Input->LinkedTo.Num(), 1)) return false;
				const auto* Source = Cast<UK2Node_CallFunction>(Input->LinkedTo[0]->GetOwningNode());
				if (!TestNotNull(TEXT("Diameter expression"), Source)) return false;
				TestEqual(TEXT("Outer dimensions use diameter, no fixed min/max"), Source->FunctionReference.GetMemberName(), FName(TEXT("Multiply_DoubleDouble")));
				TestEqual(TEXT("Diameter multiplier is two"), FCString::Atof(*Source->FindPinChecked(TEXT("B"))->DefaultValue), 2.0f);
				++Dimensions;
			}
		}
		TestEqual(TEXT("One marker radius updated"), Radii, 1);
		TestEqual(TEXT("Both outer dimensions updated"), Dimensions, 2);
	}
	return true;
}
#endif
