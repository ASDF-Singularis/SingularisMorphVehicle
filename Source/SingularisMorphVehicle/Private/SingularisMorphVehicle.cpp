#include "SingularisMorphVehicle.h"

#include <Engine/Canvas.h>
#include <Engine/World.h>
#include <GameFramework/HUD.h>

#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"

DEFINE_LOG_CATEGORY(LogSingularisMorphVehicle);

#define LOCTEXT_NAMESPACE "FSingularisMorphVehicleModule"

void FSingularisMorphVehicleModule::StartupModule()
{
	// 1) 验证配置文件可用
	check(GConfig);

	// 2) 注册 HUD 调试显示回调
	OnShowDebugInfoHandle = AHUD::OnShowDebugInfo.AddStatic(
		&FSingularisMorphVehicleModule::OnShowDebugInfo
	);
}

void FSingularisMorphVehicleModule::ShutdownModule()
{
	AHUD::OnShowDebugInfo.Remove(OnShowDebugInfoHandle);
}

void FSingularisMorphVehicleModule::OnShowDebugInfo(
	AHUD* HUD,
	UCanvas* Canvas,
	const FDebugDisplayInfo& DisplayInfo,
	float& YL,
	float& YPos
)
{
	// 1) 守卫：仅响应 showdebug SingularisMorphVehicle
	static const FName NAME_SingularisMorphVehicle("SingularisMorphVehicle");
	if (!HUD || !Canvas || !HUD->ShouldDisplayDebug(NAME_SingularisMorphVehicle)) return;

	// 2) 定位当前 World 的调度器子系统
	UWorld* World = HUD->GetWorld();
	if (!IsValid(World)) return;

	USingularisMorphVehicleSchedulerSubsystem* Subsystem =
		World->GetSubsystem<USingularisMorphVehicleSchedulerSubsystem>();
	if (!IsValid(Subsystem)) return;

	// 3) 逐个载具输出调试信息
	for (const TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle :
	     Subsystem->GetVehicleSimulationComponents())
	{
		const TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
			StrongPtr->ShowDebugInfo(HUD, Canvas, DisplayInfo, YL, YPos);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSingularisMorphVehicleModule, SingularisMorphVehicle)
