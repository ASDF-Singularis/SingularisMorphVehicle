#pragma once

#include <CoreMinimal.h>

#include "BoneContainer.h"
#include "BonePose.h"
#include "Animations/SingularisMorphVehicleAnimationInstance.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "AnimNode_SingularisMorphVehicleController.generated.h"

/**
 * 引力奇点变型载具动画控制器节点。
 *
 * 在动画蓝图 AnyThread 上下文中运行，将模拟模块的输出数据（位移、旋转）
 * 应用到对应骨骼或变换节点上，实现悬挂压缩/释放、车轮转动等动画效果。
 */
USTRUCT()
struct SINGULARISMORPHVEHICLE_API FAnimNode_SingularisMorphVehicleController : public FAnimNode_SkeletalControlBase
{
	GENERATED_USTRUCT_BODY()

	FAnimNode_SingularisMorphVehicleController();

	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual void EvaluateSkeletalControl_AnyThread(
		FComponentSpacePoseContext& Output,
		TArray<FBoneTransform>& OutBoneTransforms
	) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;

private:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;

	struct FSingularisMorphModuleLookupData
	{
		int32 ModuleIndex;
		FBoneReference BoneReference;
	};

	TArray<FSingularisMorphModuleLookupData> Modules;
	const FSingularisMorphVehicleAnimationInstanceProxy* AnimInstanceProxy = nullptr;
};
