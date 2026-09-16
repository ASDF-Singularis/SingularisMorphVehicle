#pragma once

#include <CoreMinimal.h>
#include <Modules/ModuleManager.h>

DECLARE_LOG_CATEGORY_EXTERN(LogSingularisMorphVehicle, Log, All);

class AHUD;
class UCanvas;
class FDebugDisplayInfo;

class FSingularisMorphVehicleModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** HUD 调试显示入口（showdebug SingularisMorphVehicle） */
	static void OnShowDebugInfo(
		AHUD* HUD,
		UCanvas* Canvas,
		const FDebugDisplayInfo& DisplayInfo,
		float& YL,
		float& YPos
	);

	FDelegateHandle OnShowDebugInfoHandle;
};
