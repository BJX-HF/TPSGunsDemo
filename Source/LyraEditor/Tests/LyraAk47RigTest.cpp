#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "SkeletalMeshAttributes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraAk47RigTest,
    "Lyra.Recoil.WeaponConfig.Ak47Rig", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraAk47RigTest::RunTest(const FString& Parameters)
{
    const auto* Original = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Weapons/Rifle/Mesh/SK_Rifle"));
    const auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Weapons/ak47/Mesh/SK_ak47"));
    if (!TestNotNull(TEXT("Original rifle"), Original) || !TestNotNull(TEXT("ak47 mesh"), Mesh)) return false;
    TestEqual(TEXT("Uses compatible animation skeleton"), Mesh->GetSkeleton(), Original->GetSkeleton());
    const auto& Ref = Mesh->GetRefSkeleton();
    const auto& OriginalRef = Original->GetRefSkeleton();
    TestEqual(TEXT("Same bone count"), Ref.GetNum(), OriginalRef.GetNum());
    for (int32 Index = 0; Index < OriginalRef.GetNum(); ++Index)
    {
        const FName Name = OriginalRef.GetBoneName(Index);
        const int32 NewIndex = Ref.FindBoneIndex(Name);
        if (!TestTrue(*FString::Printf(TEXT("Bone %s exists"), *Name.ToString()), NewIndex != INDEX_NONE)) return false;
        TestTrue(*FString::Printf(TEXT("%s retains the rifle bind pose"), *Name.ToString()),
            Ref.GetRefBonePose()[NewIndex].Equals(OriginalRef.GetRefBonePose()[Index], 0.001));
    }
    const auto* Muzzle = Mesh->FindSocket(TEXT("Muzzle"));
    if (!TestNotNull(TEXT("Muzzle socket"), Muzzle)) return false;
    TestTrue(TEXT("Muzzle override is mesh-specific"), Muzzle->GetOuter() == Mesh);
    TestEqual(TEXT("Muzzle follows gun body"), Muzzle->BoneName, FName(TEXT("Grip")));
    TestTrue(TEXT("Muzzle is at the end of the AK barrel"), Muzzle->RelativeLocation.Equals(FVector(64.2, 0, 9.9543), 0.001));
    TestEqual(TEXT("Original rifle muzzle stays on Barrel"), Original->FindSocket(TEXT("Muzzle"))->BoneName, FName(TEXT("Barrel")));
    const FMeshDescription* Description = Mesh->GetMeshDescription(0);
    if (!TestNotNull(TEXT("Source geometry available"), Description)) return false;
    TestTrue(TEXT("Source contains vertices"), Description->Vertices().Num() > 0);
    const FSkeletalMeshConstAttributes Attributes(*Description);
    const auto Weights = Attributes.GetVertexSkinWeights();
    TMap<FName,int32> WeightedVertices;
    for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
    {
        const auto Influences = Weights.Get(Vertex);
        if (!TestEqual(TEXT("Mechanical parts are rigidly weighted"), Influences.Num(), 1)) return false;
        for (const auto Weight : Influences)
            ++WeightedVertices.FindOrAdd(Ref.GetBoneName(Weight.GetBoneIndex()));
    }
    for (const auto& Pair : WeightedVertices) AddInfo(FString::Printf(TEXT("Rigid vertices: %s=%d"), *Pair.Key.ToString(), Pair.Value));
    for (const FName Bone : {FName(TEXT("Grip")),FName(TEXT("Magazine")),FName(TEXT("Bolt")),FName(TEXT("Trigger"))})
        TestTrue(*FString::Printf(TEXT("%s has weighted geometry"), *Bone.ToString()), WeightedVertices.FindRef(Bone) > 0);
    return true;
}
#endif
