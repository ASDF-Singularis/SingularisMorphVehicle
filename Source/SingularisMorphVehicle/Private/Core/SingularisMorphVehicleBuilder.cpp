#include "Core/SingularisMorphVehicleBuilder.h"

#include "Components/SingularisMorphVehicleSUComponent.h"
#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "SimModule/SimModulesInclude.h"


void FSingularisMorphVehicleBuilder::GenerateSimTree(USingularisMorphVehicleSimulationComponent* ModularVehicle)
{
	// 1) 卫语句：空载具直接返回
	if (!ModularVehicle) return;

	// 2) 根据网络模式决定是否启用动画
	bool RequiresAnimation = true;
	if (ModularVehicle->GetOwner())
		RequiresAnimation = ModularVehicle->GetOwner()->GetNetMode() != NM_DedicatedServer;

	// 3) 将游戏线程组件打包为紧凑的模拟树结构，转交物理线程
	if (auto SimModuleTree = MakeUnique<Chaos::FSimModuleTree>())
	{
		SimModuleTree->SetAnimationEnabled(RequiresAnimation);
		SimModuleTree->SetSimTreeProcessingOrder(ModularVehicle->GetTreeProcessingOrder());
		ModularVehicle->VehicleSimulationPT->Initialize(SimModuleTree);
	}
}


void FSingularisMorphVehicleBuilder::FixupTreeLinks(TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree)
{
	using namespace Chaos;

	if (!SimModuleTree.IsValid())
		return;

	const FSimModuleTree::FSimModuleNode* TransmissionNode =
		SimModuleTree->LocateNodeByType<FTransmissionSimModule>();

	// 遍历全部树槽位（含删除后遗留的空槽），而不是按 NumActiveNodes 计数取前 N 个索引：
	// 全量重建时新节点的树索引不从 0 开始（旧节点删除、槽位由 FreeList 复用或追加），
	// 按计数取前 N 个索引会访问到已删除的空槽（nullptr 全部被跳过），
	// 导致悬挂↔车轮链接全部失败（WheelSimTreeIdx=-1），悬挂失去弹簧力、轮子下坠。
	const int32 NumNodes = SimModuleTree->GetNumNodes();
	UE_LOG(
		LogSingularisMorphBase,
		Log,
		TEXT("=== FixupTreeLinks: %d tree slots, TransmissionNode=%s ==="),
		NumNodes,
		TransmissionNode ? TEXT("found") : TEXT("NOT found")
	);

	for (int32 I = 0; I < NumNodes; I++)
	{
		ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
		if (!Module)
			continue;

		// ① 悬挂-车轮交叉引用：通过树形父子关系推导
		if (Module->IsSimType<FSuspensionBaseInterface>())
		{
			FSimModuleTree::FSimModuleNode& SuspensionNode = SimModuleTree->GetNode(I);
			FSuspensionBaseInterface* Suspension = Module->Cast<FSuspensionBaseInterface>();

			// 检查悬挂的父节点是否是车轮（Wheel → Suspension 结构）
			if (SuspensionNode.Parent != FSimModuleTree::FSimModuleNode::INVALID_IDX)
			{
				FSimModuleTree::FSimModuleNode& ParentNode = SimModuleTree->GetNode(SuspensionNode.Parent);
				ISimulationModuleBase* ParentModule = SimModuleTree->AccessSimModule(
					ParentNode.SimModule->GetTreeIndex()
				);

				if (FWheelBaseInterface* Wheel = ParentModule->Cast<FWheelBaseInterface>())
				{
					Wheel->SetSuspensionSimTreeIndex(Suspension->GetTreeIndex());
					Suspension->SetWheelSimTreeIndex(Wheel->GetTreeIndex());
					UE_LOG(
						LogSingularisMorphBase,
						Log,
						TEXT("  Fixup: Suspension[%d] <-> Wheel[%d] (parent link) | SuspGUID=%d, WheelGUID=%d"),
						Suspension->GetTreeIndex(),
						Wheel->GetTreeIndex(),
						Suspension->GetGuid(),
						Wheel->GetGuid()
					);
				}
			}

			// 检查悬挂的子节点是否是车轮（Suspension → Wheel 结构）
			for (int32 Child : SuspensionNode.Children)
			{
				if (Child == FSimModuleTree::FSimModuleNode::INVALID_IDX)
					continue;

				FSimModuleTree::FSimModuleNode& ChildNode = SimModuleTree->GetNode(Child);
				ISimulationModuleBase* ChildModule = SimModuleTree->AccessSimModule(
					ChildNode.SimModule->GetTreeIndex()
				);

				if (FWheelBaseInterface* Wheel = ChildModule->Cast<FWheelBaseInterface>())
				{
					Wheel->SetSuspensionSimTreeIndex(Suspension->GetTreeIndex());
					Suspension->SetWheelSimTreeIndex(Wheel->GetTreeIndex());
					UE_LOG(
						LogSingularisMorphBase,
						Log,
						TEXT("  Fixup: Suspension[%d] <-> Wheel[%d] (child link) | SuspGUID=%d, WheelGUID=%d"),
						Suspension->GetTreeIndex(),
						Wheel->GetTreeIndex(),
						Suspension->GetGuid(),
						Wheel->GetGuid()
					);
				}
			}
		}

		// ② 车轮重挂到变速箱下
		if (TransmissionNode && Module->IsSimType<class FWheelSimModule>())
		{
			const int32 WheelIdx = Module->GetTreeIndex();
			const int32 TransIdx = TransmissionNode->SimModule->GetTreeIndex();
			SimModuleTree->Reparent(WheelIdx, TransIdx);
			UE_LOG(
				LogSingularisMorphBase,
				Log,
				TEXT("  Fixup: Wheel[%d] reparented to Transmission[%d]"),
				WheelIdx,
				TransIdx
			);
		}
	}

	UE_LOG(LogSingularisMorphBase, Log, TEXT("=== FixupTreeLinks complete ==="));
}
