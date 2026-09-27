#include "Animations/AnimNode_SingularisMorphVehicleController.h"

#include "AnimationRuntime.h"
#include "Animation/AnimStats.h"
#include "Animation/AnimTrace.h"
#include "Animations/SingularisMorphVehicleAnimationInstance.h"
#include "SimModule/SimulationModuleBase.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AnimNode_SingularisMorphVehicleController)

namespace
{
	/** 模块动画集合签名：模块数量与骨骼名共同决定骨骼引用是否仍然有效 */
	uint32 ComputeModuleAnimDataSignature(const TArray<FSingularisMorphModuleAnimationData>& ModuleAnimData)
	{
		auto Signature = GetTypeHash(ModuleAnimData.Num());
		for (const FSingularisMorphModuleAnimationData& Module : ModuleAnimData)
			Signature = HashCombine(Signature, GetTypeHash(Module.BoneName));

		return Signature;
	}
}


FAnimNode_SingularisMorphVehicleController::FAnimNode_SingularisMorphVehicleController()
{
	AnimInstanceProxy = nullptr;
}

void FAnimNode_SingularisMorphVehicleController::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);

	DebugLine += "(";
	AddDebugNodeData(DebugLine);
	DebugLine += ")";

	DebugData.AddDebugItem(DebugLine);

	if (!AnimInstanceProxy)
	{
		ComponentPose.GatherDebugData(DebugData);
		return;
	}

	const TArray<FSingularisMorphModuleAnimationData>& AnimData = AnimInstanceProxy->GetModuleAnimData();
	for (const FSingularisMorphModuleLookupData& Module : Modules)
	{
		// 模块集合变化后 Modules 可能先于 AnimData 刷新，越界时不输出该模块
		if (!AnimData.IsValidIndex(Module.ModuleIndex))
		{
			DebugData.AddDebugItem(
				FString::Printf(TEXT(" [Module Index : %d] (stale lookup data)"), Module.ModuleIndex)
			);
			continue;
		}

		if (Module.BoneReference.BoneIndex != INDEX_NONE)
		{
			DebugLine = FString::Printf(
				TEXT(" [Module Index : %d] Bone: %s , Rotation Offset : %s, Location Offset : %s"),
				Module.ModuleIndex,
				*Module.BoneReference.BoneName.ToString(),
				*AnimData[Module.ModuleIndex].RotOffset.ToString(),
				*AnimData[Module.ModuleIndex].LocOffset.ToString()
			);
		}
		else
		{
			DebugLine = FString::Printf(
				TEXT(" [Module Index : %d] Bone: %s (invalid bone)"),
				Module.ModuleIndex,
				*Module.BoneReference.BoneName.ToString()
			);
		}

		DebugData.AddDebugItem(DebugLine);
	}

	ComponentPose.GatherDebugData(DebugData);
}

void FAnimNode_SingularisMorphVehicleController::EvaluateSkeletalControl_AnyThread(
	FComponentSpacePoseContext& Output,
	TArray<FBoneTransform>& OutBoneTransforms
)
{
	check(OutBoneTransforms.Num() == 0);

	ANIM_MT_SCOPE_CYCLE_COUNTER_VERBOSE(SingularisMorphVehicleController, !IsInGameThread());

	// 代理类型不匹配（节点被放入非载具动画蓝图）时无数据可施加，保持输入姿态
	if (!AnimInstanceProxy) return;

	const TArray<FSingularisMorphModuleAnimationData>& ModuleAnimData = AnimInstanceProxy->GetModuleAnimData();

	const FBoneContainer& BoneContainer = Output.Pose.GetPose().GetBoneContainer();
	for (const FSingularisMorphModuleLookupData& Module : Modules)
	{
		if (Module.BoneReference.IsValidToEvaluate(BoneContainer))
		{
			if (Module.ModuleIndex < ModuleAnimData.Num())
			{
				FCompactPoseBoneIndex ModuleSimBoneIndex = Module.BoneReference.GetCompactPoseIndex(BoneContainer);

				// the way we apply transform is same as FMatrix or FTransform
				// we apply scale first, and rotation, and translation
				// if you'd like to translate first, you'll need two nodes that first node does translate and second nodes to rotate. 
				FTransform NewBoneTM = Output.Pose.GetComponentSpaceTransform(ModuleSimBoneIndex);

				FAnimationRuntime::ConvertCSTransformToBoneSpace(
					Output.AnimInstanceProxy->GetComponentTransform(),
					Output.Pose,
					NewBoneTM,
					ModuleSimBoneIndex,
					BCS_ComponentSpace
				);

				if (ModuleAnimData[Module.ModuleIndex].Flags & Chaos::EAnimationFlags::AnimateRotation)
				{
					// Apply rotation offset
					const FQuat BoneQuat(ModuleAnimData[Module.ModuleIndex].RotOffset);
					NewBoneTM.SetRotation(BoneQuat * NewBoneTM.GetRotation());
				}

				if (ModuleAnimData[Module.ModuleIndex].Flags & Chaos::EAnimationFlags::AnimatePosition)
				{
					// Apply loc offset
					NewBoneTM.AddToTranslation(ModuleAnimData[Module.ModuleIndex].LocOffset);
				}

				// Convert back to Component Space.
				FAnimationRuntime::ConvertBoneSpaceTransformToCS(
					Output.AnimInstanceProxy->GetComponentTransform(),
					Output.Pose,
					NewBoneTM,
					ModuleSimBoneIndex,
					BCS_ComponentSpace
				);

				// add back to it
				OutBoneTransforms.Add(FBoneTransform(ModuleSimBoneIndex, NewBoneTM));
			}
		}
	}

#if ANIM_TRACE_ENABLED
	for (const FSingularisMorphModuleLookupData& Module : Modules)
	{
		if (Module.BoneReference.BoneIndex != INDEX_NONE && Module.ModuleIndex < ModuleAnimData.Num())
		{
			TRACE_ANIM_NODE_VALUE(
				Output,
				*FString::Printf(TEXT("Module %d Name"), Module.ModuleIndex),
				*Module.BoneReference.BoneName.ToString()
			);
			TRACE_ANIM_NODE_VALUE(
				Output,
				*FString::Printf(TEXT("Module %d Rotation Offset"), Module.ModuleIndex),
				ModuleAnimData[Module.ModuleIndex].RotOffset
			);
			TRACE_ANIM_NODE_VALUE(
				Output,
				*FString::Printf(TEXT("Module %d Location Offset"), Module.ModuleIndex),
				ModuleAnimData[Module.ModuleIndex].LocOffset
			);
		}
		else
		{
			TRACE_ANIM_NODE_VALUE(
				Output,
				*FString::Printf(TEXT("Module %d Name"), Module.ModuleIndex),
				*FString::Printf(TEXT("%s (invalid)"), *Module.BoneReference.BoneName.ToString())
			);
		}
	}
#endif
}

bool FAnimNode_SingularisMorphVehicleController::IsValidToEvaluate(
	const USkeleton* Skeleton,
	const FBoneContainer& RequiredBones
)
{
	if (AnimInstanceProxy)
	{
		// 骨骼引用必须与当前模块集合一一对应：仅比较数量会漏判“等量替换/重排”
		// （删一个再加一个），使动画被施加到已错位的骨骼上且无法自愈
		const TArray<FSingularisMorphModuleAnimationData>& ModuleAnimData = AnimInstanceProxy->GetModuleAnimData();
		if (ComputeModuleAnimDataSignature(ModuleAnimData) != BoneReferenceSignature)
			InitializeBoneReferences(RequiredBones);
	}

	// if both bones are valid
	for (const FSingularisMorphModuleLookupData& Module : Modules)
	{
		// if one of them is valid
		if (Module.BoneReference.IsValidToEvaluate(RequiredBones) == true)
			return true;
	}

	return false;
}

void FAnimNode_SingularisMorphVehicleController::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_SkeletalControlBase::Initialize_AnyThread(Context);

	// 类型校验：节点被放入非载具动画蓝图时代理类型不匹配，直接 static_cast 后使用即为未定义行为。
	// 代理由 UAnimInstance 创建，故以动画实例类型为判据（FAnimInstanceProxy 不提供代理自身的类型查询）
	AnimInstanceProxy = Context.AnimInstanceProxy &&
	                    Cast<USingularisMorphVehicleAnimationInstance>(
		                    Context.AnimInstanceProxy->GetAnimInstanceObject()
	                    )
		                    ? static_cast<FSingularisMorphVehicleAnimationInstanceProxy*>(Context.AnimInstanceProxy)
		                    : nullptr;

	BoneReferenceSignature = 0;
}

void FAnimNode_SingularisMorphVehicleController::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	if (!AnimInstanceProxy) return;

	const TArray<FSingularisMorphModuleAnimationData>& ModuleAnimData = AnimInstanceProxy->GetModuleAnimData();
	const int32 NumModules = ModuleAnimData.Num();
	Modules.Empty(NumModules);

	for (auto ModuleIndex = 0; ModuleIndex < NumModules; ++ModuleIndex)
	{
		auto Module = new(Modules)FSingularisMorphModuleLookupData();
		Module->ModuleIndex = ModuleIndex;
		Module->BoneReference.BoneName = ModuleAnimData[ModuleIndex].BoneName;
		Module->BoneReference.Initialize(RequiredBones);
	}

	// sort by bone indices
	Modules.Sort(
		[](const FSingularisMorphModuleLookupData& L, const FSingularisMorphModuleLookupData& R)
		{
			return L.BoneReference.BoneIndex < R.BoneReference.BoneIndex;
		}
	);

	BoneReferenceSignature = ComputeModuleAnimDataSignature(ModuleAnimData);
}
