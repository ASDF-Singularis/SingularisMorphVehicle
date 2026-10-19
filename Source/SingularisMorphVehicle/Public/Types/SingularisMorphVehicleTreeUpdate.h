#pragma once

#include <CoreMinimal.h>
#include <SimModule/SimModuleTree.h>

/**
 * 模拟树父节点重挂请求。
 *
 * 以模块 GUID 表达，物理线程在全部增删完成后解析为真实树索引。
 * 父/子任一方不存在时请求被跳过并记录告警，不影响同批其它变更。
 */
struct FSingularisMorphModuleReparent
{
	int32 ChildGuid = INDEX_NONE;
	int32 ParentGuid = INDEX_NONE;
};

/**
 * 已存在车轮模块的运行期转速上限更新。
 *
 * 动力链变化会改变引擎最高转速折算的轮速上限。保留车轮模块时不能重建模块，
 * 必须在物理线程写锁内原位更新设置，才能同时保留角位置、角速度与动画槽位。
 */
struct FSingularisMorphWheelSpeedLimit
{
	int32 ModuleGuid = INDEX_NONE;
	float MaxRotationVel = 0.0f;
};

/**
 * 一次原子的模拟树拓扑更新。
 *
 * 新增/删除模块、父节点重挂与车轮限速更新必须同批提交：重挂引用的新父模块可能在
 * 本批新增，物理线程先应用完全部增删，再统一解析重挂关系并更新运行期设置。
 */
struct FSingularisMorphTreeUpdate
{
	Chaos::FSimTreeUpdates TreeUpdates;
	TArray<FSingularisMorphModuleReparent> Reparents;
	TArray<FSingularisMorphWheelSpeedLimit> WheelSpeedLimits;
};
