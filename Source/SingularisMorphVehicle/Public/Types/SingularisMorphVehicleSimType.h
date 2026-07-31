#pragma once

#include <CoreMinimal.h>
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
