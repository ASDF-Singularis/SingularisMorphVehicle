#pragma once

#include <CoreMinimal.h>
#include <Components/PrimitiveComponent.h>
#include <Components/SceneComponent.h>
#include <UObject/ObjectPtr.h>

#include "SingularisMorphSimModuleManagerAsyncCallback.h"
#include "SingularisMorphVehicleSimType.generated.h"

/**
 * 引力奇点变型载具组件数据。
 * 将场景组件映射至模拟 GUID 与可选的动画视觉组件。
 */
USTRUCT()
struct FSingularisMorphVehicleComponentData
{
	GENERATED_BODY()

	/** 模拟模块唯一标识符 */
	int32 Guid = -1;

	/** 动画驱动的代理组件缓存（可能与模拟组件不同） */
	UPROPERTY()
	TObjectPtr<USceneComponent> ProxyComponentToAnimate = nullptr;
};

/**
 * 引力奇点变型载具复制状态。
 * 继承自 FSingularisMorphVehicleInputs，附加反向与保持唤醒标志。
 */
USTRUCT()
struct FSingularisMorphReplicatedState : public FSingularisMorphVehicleInputs
{
	GENERATED_USTRUCT_BODY()

	FSingularisMorphReplicatedState() {}
};

/**
 * 引力奇点变型载具构建数据。
 * 关联构建中的场景组件与其构建索引。
 */
USTRUCT()
struct FSingularisMorphConstructionData
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	TObjectPtr<USceneComponent> Component = nullptr;

	UPROPERTY()
	int32 ConstructionIndex = INDEX_NONE;
};

/**
 * 引力奇点变型模块动画配置。
 * 定义单个模拟模块与骨骼（或非骨骼变换）之间的动画绑定参数。
 */
USTRUCT()
struct FSingularisMorphModuleAnimationSetup
{
	GENERATED_USTRUCT_BODY()

	FSingularisMorphModuleAnimationSetup(
		const FName BoneNameIn,
		const int TransformIndexIn,
		const int GuidIn = INDEX_NONE
	)
		: BoneName(BoneNameIn),
		  RotOffset(FRotator::ZeroRotator),
		  LocOffset(FVector::ZeroVector),
		  CombinedRotation(FQuat::Identity),
		  AnimFlags(0),
		  TransformIndex(TransformIndexIn),
		  ModuleGUID(GuidIn),
		  InitialRotOffset(FQuat::Identity),
		  InitialLocOffset(FVector::ZeroVector) {}

	FSingularisMorphModuleAnimationSetup()
		: BoneName(NAME_None),
		  RotOffset(FRotator::ZeroRotator),
		  LocOffset(FVector::ZeroVector),
		  CombinedRotation(FQuat::Identity),
		  AnimFlags(0),
		  TransformIndex(INDEX_NONE),
		  ModuleGUID(INDEX_NONE),
		  InitialRotOffset(FQuat::Identity),
		  InitialLocOffset(FVector::ZeroVector) {}

	/** 骨骼名称（骨骼网格体动画必需） */
	FName BoneName;

	/** 旋转偏移 */
	FRotator RotOffset;

	/** 位移偏移 */
	FVector LocOffset;

	/** 组合旋转四元数 */
	FQuat CombinedRotation;

	/** 动画标志位 */
	uint16 AnimFlags;

	/** 变换索引（非骨骼网格体动画必需） */
	int32 TransformIndex;

	/** 模块 GUID */
	int32 ModuleGUID;

	/** 初始旋转偏移 */
	FQuat InitialRotOffset;

	/** 初始位移偏移 */
	FVector InitialLocOffset;
};

/**
 * 引力奇点变型模块静止基准。
 *
 * 记录模块首次注册时捕获的静止位姿，供后续拓扑重建复用。
 * 引擎在动画阶段会把「静止基准 + 动画位移」写回集群子粒子的 ChildToParent
 * （SimModuleTree 的 UpdateClusterUnionTransformsIfRequired），因此 ChildToParent
 * 是「静止基准 + 当帧动画位移」的合成值而非静止值；拓扑重建若重新读取该实时值
 * 作为新基准，就会把当帧的悬挂压缩量与车轮转角固化为静止基准，
 * 逐次重建累积为可视轮位漂移与悬挂行程漂移（约束不断把载具抬离地面）。
 * 故基准只在首次注册时捕获，此后一切重建复用同一基准。
 *
 * 部件真实位姿变化的表达方式：真实变形须经离簇/入簇（粒子重建，键不匹配而自动重新捕获）；
 * 同一粒子上的位姿变化一律视为动画位移，不重新基准。
 */
struct FSingularisMorphModuleRestPose
{
	/** 静止基准：模块在集群父粒子坐标系中的位姿（未叠加动画位移） */
	FTransform RestTransform = FTransform::Identity;

	/**
	 * 驱动组件相对参考变换的位姿（未叠加动画位移，未叠加 SU 的 TransformOffset）。
	 * 供引擎换算动画位移方向与延迟力方向使用，SU 的 TransformOffset 在注册时另行叠加。
	 */
	FTransform ComponentTransform = FTransform::Identity;
};

/**
 * 引力奇点变型模块静止基准的查表键。
 *
 * 键为「驱动组件 + 集群子粒子唯一索引」：以粒子索引区分「同一组件重新入簇」
 * （新粒子携带新的部件姿态，须重新捕获）与「同一粒子上的多个模块」
 * （车轮与悬挂共用同一驱动组件，复用同一基准）。
 */
struct FSingularisMorphModuleRestPoseKey
{
	TWeakObjectPtr<UPrimitiveComponent> Component = nullptr;
	int32 ParticleIndex = INDEX_NONE;

	bool operator==(const FSingularisMorphModuleRestPoseKey& Other) const
	{
		return Component == Other.Component && ParticleIndex == Other.ParticleIndex;
	}
};

/** 静止基准查表键的哈希（弱引用按当前对象指针取值，与相等判定语义一致） */
inline uint32 GetTypeHash(const FSingularisMorphModuleRestPoseKey& Key)
{
	return HashCombine(GetTypeHash(Key.Component.Get()), GetTypeHash(Key.ParticleIndex));
}
