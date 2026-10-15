#include "LyraAk47PreviewCommandlet.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BufferArchive.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "AssetCompilingManager.h"
#include "ContentStreaming.h"

int32 ULyraAk47PreviewCommandlet::Main(const FString& Params)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
    Context.SetCurrentWorld(World);
    const auto LoadMesh = [](const TCHAR* Path) { return LoadObject<USkeletalMesh>(nullptr, Path); };
    auto* Character = World->SpawnActor<ASkeletalMeshActor>();
    auto* Body = Character->GetSkeletalMeshComponent();
    Body->SetSkeletalMesh(LoadMesh(TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Manny")));
    Body->SetForcedLOD(1);
    Body->PrestreamTextures(30.0f, true);
    Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    auto* GunActor = World->SpawnActor<ASkeletalMeshActor>();
    auto* Gun = GunActor->GetSkeletalMeshComponent();
    Gun->SetSkeletalMesh(LoadMesh(TEXT("/Game/Weapons/ak47/Mesh/SK_ak47")));
    Gun->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Gun->AttachToComponent(Body, FAttachmentTransformRules::KeepRelativeTransform, TEXT("weapon_r"));
    Gun->SetRelativeRotation(FRotator(0, -90, 0));
    for (const FRotator Rotation : {FRotator(-30, -45, 0), FRotator(-35, 140, 0), FRotator(-60, 25, 0)})
    {
        auto* Light = World->SpawnActor<ADirectionalLight>();
        Light->SetActorRotation(Rotation);
        Light->GetLightComponent()->SetIntensity(3.0f);
    }
    auto* CaptureActor = World->SpawnActor<ASceneCapture2D>();
    auto* Capture = CaptureActor->GetCaptureComponent2D();
    auto* Target = NewObject<UTextureRenderTarget2D>();
    Target->RenderTargetFormat = RTF_RGBA8;
    Target->ClearColor = FLinearColor(0.04f, 0.04f, 0.05f);
    Target->InitAutoFormat(1400, 1000);
    Target->UpdateResourceImmediate(true);
    Capture->TextureTarget = Target;
    Capture->CaptureSource = SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->FOVAngle = 45;
    Capture->ShowFlags.SetLighting(false);
    Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    Capture->PostProcessSettings.AutoExposureMethod = AEM_Manual;
    Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
    Capture->PostProcessSettings.AutoExposureBias = 1;
    const auto LoadAnim = [](const TCHAR* Path) { return LoadObject<UAnimSequence>(nullptr, Path); };
    UAnimSequence* Idle = LoadAnim(TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/MM_Rifle_Idle_Hipfire"));
    UAnimSequence* Reload = LoadAnim(TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/MM_Rifle_Reload"));
    UAnimSequence* GunReload = LoadAnim(TEXT("/Game/Weapons/Rifle/Animations/Weap_Rifle_Reload"));
    if (!Body->GetSkeletalMeshAsset() || !Gun->GetSkeletalMeshAsset() || !Idle || !Reload || !GunReload) return 1;
    FAssetCompilingManager::Get().FinishAllCompilation();
    GShaderCompilingManager->FinishAllCompilation();
    World->UpdateWorldComponents(true, false);
    IStreamingManager::Get().StreamAllResources(5.0f);
    const auto Pose = [](USkeletalMeshComponent* Component, UAnimSequence* Animation, float Time)
    {
        Component->SetAnimation(Animation);
        Component->SetPosition(Time, false);
        Component->TickAnimation(0, false);
        Component->RefreshBoneTransforms();
        Component->UpdateComponentToWorld();
        Component->MarkRenderDynamicDataDirty();
    };
    for (int32 Shot = 0; Shot < 5; ++Shot)
    {
        const float Time = Shot < 2 ? 0.0f : (Shot - 1) * 0.5f;
        Pose(Body, Shot < 2 ? Idle : Reload, Time);
        Pose(Gun, GunReload, Time);
        const FVector Camera = Shot == 1 ? FVector(-170, 30, 155) : FVector(150, 90, 155);
        const FVector Aim(-15, 35, 132);
        CaptureActor->SetActorLocationAndRotation(Camera, (Aim-Camera).Rotation());
        World->SendAllEndOfFrameUpdates();
        FlushRenderingCommands();
        Capture->CaptureScene();
        FlushRenderingCommands();
        FBufferArchive PNG;
        if (!FImageUtils::ExportRenderTarget2DAsPNG(Target, PNG)) return 1;
        if (!FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir() / FString::Printf(TEXT("AK47Work/engine_pose_%d.png"), Shot)))) return 1;
        UE_LOG(LogTemp, Display, TEXT("AK47_PREVIEW pose=%d muzzle=%s right=%s left=%s"), Shot,
            *Gun->GetSocketLocation(TEXT("Muzzle")).ToString(), *Body->GetSocketLocation(TEXT("hand_r")).ToString(), *Body->GetSocketLocation(TEXT("hand_l")).ToString());
    }
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return 0;
}

