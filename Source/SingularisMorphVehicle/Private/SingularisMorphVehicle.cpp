#include "SingularisMorphVehicle.h"

#include <PhysicsPublic.h>

#include "Core/SingularisMorphSimModuleManager.h"

#define LOCTEXT_NAMESPACE "FSingularisMorphVehicleModule"

void FSingularisMorphVehicleModule::StartupModule()
{
	// 1) 验证配置文件可用
	check(GConfig);

	// 2) 注册物理场景初始化与终止回调
	OnPhysSceneInitHandle = FPhysicsDelegates::OnPhysSceneInit.AddRaw(
		this,
		&FSingularisMorphVehicleModule::PhysSceneInit
	);
	OnPhysSceneTermHandle = FPhysicsDelegates::OnPhysSceneTerm.AddRaw(
		this,
		&FSingularisMorphVehicleModule::PhysSceneTerm
	);
}

void FSingularisMorphVehicleModule::ShutdownModule()
{
	FPhysicsDelegates::OnPhysSceneInit.Remove(OnPhysSceneInitHandle);
	FPhysicsDelegates::OnPhysSceneTerm.Remove(OnPhysSceneTermHandle);
}

// ReSharper disable CppMemberFunctionMayBeStatic

void FSingularisMorphVehicleModule::PhysSceneInit(FPhysScene* PhysScene)
{
	new FSingularisMorphSimModuleManager(PhysScene);
}

void FSingularisMorphVehicleModule::PhysSceneTerm(FPhysScene* PhysScene)
{
	// 1) 获取物理场景关联的仿真模块管理器
	FSingularisMorphSimModuleManager* VehicleManager = FSingularisMorphSimModuleManager::GetManagerFromScene(PhysScene);
	if (VehicleManager != nullptr)
	{
		// 2) 解除绑定并销毁管理器
		VehicleManager->DetachFromPhysScene(PhysScene);
		delete VehicleManager;
		VehicleManager = nullptr;
	}
}

// ReSharper restore CppMemberFunctionMayBeStatic

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSingularisMorphVehicleModule, SingularisMorphVehicle)
