#include "Core/SingularisMorphVehicleBuilder.h"

#include "SingularisMorphVehicle.h"
#include "Core/SingularisMorphVehicleSimulationCU.h"
#include "SimModule/SimModulesInclude.h"

namespace
{
	/** 树槽位索引与数组下标的有效范围校验 */
	bool IsValidTreeIndex(const Chaos::FSimModuleTree& SimModuleTree, const int32 Index)
	{
		return Index >= 0 && Index < SimModuleTree.GetNumNodes();
	}

	/** 判断节点的祖先链中是否存在满足条件的模块 */
	bool HasAncestorMatching(
		Chaos::FSimModuleTree& SimModuleTree,
		const int32 StartIndex,
		const TFunctionRef<bool(const Chaos::ISimulationModuleBase&)>& Predicate
	)
	{
		if (!IsValidTreeIndex(SimModuleTree, StartIndex)) return false;

		auto CurrentIndex = SimModuleTree.GetNode(StartIndex).Parent;
		while (IsValidTreeIndex(SimModuleTree, CurrentIndex))
		{
			if (const Chaos::ISimulationModuleBase* Module = SimModuleTree.GetNode(CurrentIndex).SimModule)
			{
				if (Predicate(*Module))
					return true;
			}
			CurrentIndex = SimModuleTree.GetNode(CurrentIndex).Parent;
		}
		return false;
	}

	/** 判断 CandidateIndex 是否位于 NodeIndex 的子树内（防止重挂形成环） */
	bool IsDescendantOf(
		Chaos::FSimModuleTree& SimModuleTree,
		const int32 CandidateIndex,
		const int32 NodeIndex
	)
	{
		auto CurrentIndex = CandidateIndex;
		while (IsValidTreeIndex(SimModuleTree, CurrentIndex))
		{
			if (CurrentIndex == NodeIndex)
				return true;
			CurrentIndex = SimModuleTree.GetNode(CurrentIndex).Parent;
		}
		return false;
	}

	bool IsTorqueSourceModule(const Chaos::ISimulationModuleBase& Module)
	{
		return Module.IsSimType<Chaos::FTransmissionSimModule>() || Module.IsSimType<Chaos::FAxleSimModule>();
	}

	bool IsTransmissionModule(const Chaos::ISimulationModuleBase& Module)
	{
		return Module.IsSimType<Chaos::FTransmissionSimModule>();
	}
}


void FSingularisMorphVehicleBuilder::FixupTreeLinks(TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree)
{
	using namespace Chaos;

	if (!SimModuleTree.IsValid())
		return;

	// 遍历全部树槽位（含删除后遗留的空槽），而不是按 NumActiveNodes 计数取前 N 个索引：
	// 全量重建时新节点的树索引不从 0 开始（旧节点删除、槽位由 FreeList 复用或追加），
	// 按计数取前 N 个索引会访问到已删除的空槽（nullptr 全部被跳过），
	// 导致悬挂↔车轮链接全部失败（WheelSimTreeIdx=-1），悬挂失去弹簧力、轮子下坠。
	const int32 NumNodes = SimModuleTree->GetNumNodes();

	// 1) 重置全部交叉链接，避免节点删除后残留指向已释放槽位的陈旧索引
	for (auto I = 0; I < NumNodes; I++)
	{
		ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
		if (!Module) continue;

		if (FSuspensionBaseInterface* Suspension = Module->Cast<FSuspensionBaseInterface>())
			Suspension->SetWheelSimTreeIndex(ISimulationModuleBase::INVALID_IDX);
		if (FWheelBaseInterface* Wheel = Module->Cast<FWheelBaseInterface>())
			Wheel->SetSuspensionSimTreeIndex(ISimulationModuleBase::INVALID_IDX);
	}

	// 2) 悬挂↔车轮交叉引用：由树形父子关系推导
	for (auto I = 0; I < NumNodes; I++)
	{
		ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
		if (!Module) continue;

		FSuspensionBaseInterface* Suspension = Module->Cast<FSuspensionBaseInterface>();
		if (!Suspension) continue;

		const FSimModuleTree::FSimModuleNode& SuspensionNode = SimModuleTree->GetNode(I);

		// 悬挂的父节点是车轮（Wheel → Suspension 结构）。
		// 父索引可能为 INVALID_IDX（悬挂为根节点），须先校验范围
		if (IsValidTreeIndex(*SimModuleTree, SuspensionNode.Parent))
		{
			if (ISimulationModuleBase* ParentModule = SimModuleTree->AccessSimModule(SuspensionNode.Parent))
			{
				if (FWheelBaseInterface* Wheel = ParentModule->Cast<FWheelBaseInterface>())
				{
					Wheel->SetSuspensionSimTreeIndex(I);
					Suspension->SetWheelSimTreeIndex(SuspensionNode.Parent);
				}
			}
		}

		// 悬挂的子节点是车轮（Suspension → Wheel 结构），交叉链接同样双向建立
		for (const int32 Child : SuspensionNode.Children)
		{
			ISimulationModuleBase* ChildModule = SimModuleTree->AccessSimModule(Child);
			if (!ChildModule) continue;

			if (FWheelBaseInterface* ChildWheel = ChildModule->Cast<FWheelBaseInterface>())
			{
				Suspension->SetWheelSimTreeIndex(Child);
				ChildWheel->SetSuspensionSimTreeIndex(I);
			}
		}
	}

	// 3) 收集扭矩源槽位：优先挂轮轴，其次挂变速箱
	int32 FirstAxleIndex = INDEX_NONE;
	int32 FirstTransmissionIndex = INDEX_NONE;
	for (auto I = 0; I < NumNodes; I++)
	{
		const ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
		if (!Module) continue;

		if (FirstAxleIndex == INDEX_NONE && Module->IsSimType<FAxleSimModule>())
			FirstAxleIndex = I;
		if (FirstTransmissionIndex == INDEX_NONE && Module->IsSimType<FTransmissionSimModule>())
			FirstTransmissionIndex = I;
	}

	// 4) 扭矩链修复：车轮必须位于扭矩源（轮轴/变速箱）之下，否则回退挂到首个扭矩源。
	//    已显式声明扭矩父节点的车轮（祖先链含轮轴或变速箱）保持不变，支持多变速箱/多轮轴拓扑。
	for (auto I = 0; I < NumNodes; I++)
	{
		ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
		if (!Module || !Module->IsSimType<FWheelBaseInterface>()) continue;

		if (HasAncestorMatching(*SimModuleTree, I, IsTorqueSourceModule))
			continue;

		const int32 FallbackParent = FirstAxleIndex != INDEX_NONE ? FirstAxleIndex : FirstTransmissionIndex;
		if (FallbackParent == INDEX_NONE) continue;

		// 防止重挂形成环：回退父节点不得位于车轮子树内
		if (IsDescendantOf(*SimModuleTree, FallbackParent, I))
			continue;

		SimModuleTree->Reparent(I, FallbackParent);
	}

	// 5) 轮轴链修复：轮轴必须位于变速箱之下（若存在变速箱）
	if (FirstTransmissionIndex != INDEX_NONE)
	{
		for (auto I = 0; I < NumNodes; I++)
		{
			ISimulationModuleBase* Module = SimModuleTree->AccessSimModule(I);
			if (!Module || !Module->IsSimType<FAxleSimModule>()) continue;

			if (HasAncestorMatching(*SimModuleTree, I, IsTransmissionModule))
				continue;

			if (IsDescendantOf(*SimModuleTree, FirstTransmissionIndex, I))
				continue;

			SimModuleTree->Reparent(I, FirstTransmissionIndex);
		}
	}
}
