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

	// 2) 悬挂↔车轮交叉引用：按父子邻接关系建立双向链接。
	//    车轮的树内位置由游戏线程的 RebuildFromSnapshot 权威决定（挂载立轴槽位的
	//    车轮在动力链末端，自由轮挂底盘之下），此处不做任何扭矩链重挂——重挂会
	//    覆盖游戏线程的自由轮语义（自由轮被强行驱动）。
	//    配对唯一时写入；拓扑歧义（一个悬挂挂有多个车轮，或悬挂既是车轮的子节点
	//    又挂车轮）时保留首个配对并告警，避免后写入的配对覆盖先前配对，
	//    使两侧车轮受力不一致
	auto LinkWheelSuspension = [](
		FSuspensionBaseInterface& Suspension,
		const int32 SuspensionIndex,
		FWheelBaseInterface& Wheel,
		const int32 WheelIndex
	)
	{
		const int32 ExistingWheel = Suspension.GetWheelSimTreeIndex();
		const int32 ExistingSuspension = Wheel.GetSuspensionSimTreeIndex();

		if (ExistingWheel != ISimulationModuleBase::INVALID_IDX
			|| ExistingSuspension != ISimulationModuleBase::INVALID_IDX)
		{
			UE_LOG(
				LogSingularisMorphVehicle,
				Warning,
				TEXT(
					"[FixupTreeLinks] Ambiguous suspension-wheel pairing skipped: Suspension %d (paired wheel %d), Wheel %d (paired suspension %d)"
				),
				SuspensionIndex,
				ExistingWheel,
				WheelIndex,
				ExistingSuspension
			);
			return;
		}

		Suspension.SetWheelSimTreeIndex(WheelIndex);
		Wheel.SetSuspensionSimTreeIndex(SuspensionIndex);
	};

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
					LinkWheelSuspension(*Suspension, I, *Wheel, SuspensionNode.Parent);
			}
		}

		// 悬挂的子节点是车轮（Suspension → Wheel 结构），交叉链接同样双向建立
		for (const int32 Child : SuspensionNode.Children)
		{
			ISimulationModuleBase* ChildModule = SimModuleTree->AccessSimModule(Child);
			if (!ChildModule) continue;

			if (FWheelBaseInterface* ChildWheel = ChildModule->Cast<FWheelBaseInterface>())
				LinkWheelSuspension(*Suspension, I, *ChildWheel, Child);
		}
	}
}
