#include "Subsystems/SingularisMorphVehicleSchedulerSubsystem.h"

#include <PBDRigidsSolver.h>
#include <Engine/World.h>

#include "Components/SingularisMorphVehicleSimulationComponent.h"
#include "Types/SingularisMorphModuleInputTokenStore.h"

/**
 * 构造函数。
 *
 * bGInitialized 充当"全局一次初始化"标记：
 * 由于 WorldSubsystem 在每个 World 中各自实例化（PIE 多窗口、关卡流送等），
 * 构造函数可能被多次调用，此处通过标记确保全局副作用只执行一次。
 *
 * 原本在此处注册的 OnPostWorldInitialization / OnWorldCleanup 全局委托
 * 已迁移至 WorldSubsystem 的 PostInitialize() / Deinitialize() 生命周期管理，
 * 以避免多 World 场景下的委托冲突。
 */
USingularisMorphVehicleSchedulerSubsystem::USingularisMorphVehicleSchedulerSubsystem()
{
	if (bGInitialized) return;
	bGInitialized = true;

	// 由 WorldSubsystem 生命周期代替
	// OnPostWorldInitializationHandle = FWorldDelegates::OnPostWorldInitialization.AddUObject(
	// 	this,
	// 	&USingularisMorphVehicleSchedulerSubsystem::OnPostWorldInitialization
	// );
	//
	// OnWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(
	// 	this,
	// 	&USingularisMorphVehicleSchedulerSubsystem::OnWorldCleanup
	// );
}

/**
 * WorldSubsystem 初始化入口。
 *
 * 仅执行父类初始化，实际的物理场景绑定与事件注册延迟到 PostInitialize()——
 * 此时 UWorld 及其物理场景已完全就绪。
 */
void USingularisMorphVehicleSchedulerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

/**
 * WorldSubsystem 后初始化。
 *
 * 此时 UWorld 及物理场景已完全就绪，执行两个关键步骤：
 * 1) 获取并校验物理场景指针——若为空则断言失败，表明项目物理设置不正确。
 * 2) 调用 BindEvent() 注册所有回调与异步回调对象，将子系统接入物理管线。
 */
void USingularisMorphVehicleSchedulerSubsystem::PostInitialize()
{
	Super::PostInitialize();

	// 1) 获取当前 World 的 Chaos 物理场景，断言确保物理系统已正确初始化
	PhysicsScene = GetWorld()->GetPhysicsScene();
	checkf(
		PhysicsScene,
		TEXT(
			"USingularisMorphVehicleSchedulerSubsystem: PhysicsScene is null, please ensure that the physics system is initialized before using this subsystem."
		)
	);

	// 2) 注册所有回调（物理 Tick、网络复制、异步回调对象）
	BindEvent();
}

/**
 * WorldSubsystem 销毁清理。
 *
 * 清理顺序不可逆：
 * 1) OutputRecord.Clear() —— 先释放双缓冲中的 TSimCallbackOutputHandle。
 *    这些句柄内部持有对 Chaos Solver 的回调引用，必须在物理回调解绑前释放，
 *    否则析构时会触发"回调句柄仍被持有"的断言。
 * 2) UnbindEvent() —— 移除所有委托、注销 AsyncCallback。
 * 3) Super::Deinitialize() —— 最后调用父类清理。
 */
void USingularisMorphVehicleSchedulerSubsystem::Deinitialize()
{
	// 1) 清理异步输出记录（防止析构时悬空物理回调句柄）
	OutputRecord.Clear();

	// 2) 解绑事件
	UnbindEvent();

	Super::Deinitialize();
}

/**
 * 注册载具组件到调度器。
 *
 * 使用 AddUnique 保证幂等：同一组件多次注册只保留一份。
 * 存储为弱指针（TWeakObjectPtr），不阻止组件被 GC 回收——
 * 所有消费端通过 Pin() 升级为强引用后判空来安全跳过已销毁的组件。
 */
void USingularisMorphVehicleSchedulerSubsystem::RegisterVehicleComponent(
	USingularisMorphVehicleSimulationComponent* Vehicle
)
{
	VehicleSimulationComponents.AddUnique(Vehicle);
}

/**
 * 从调度器注销载具组件。
 *
 * 注意：Remove 只从 TArray 中移除指针，不销毁组件对象本身。
 * 调用方负责在合适的时机（如 EndPlay）调用此函数。
 */
void USingularisMorphVehicleSchedulerSubsystem::UnregisterVehicleComponent(
	USingularisMorphVehicleSimulationComponent* Vehicle
)
{
	VehicleSimulationComponents.Remove(Vehicle);
}

/**
 * World 初始化完成回调（全局委托，当前未被使用）。
 *
 * 原本用于在 World 就绪后绑定事件，现已由 WorldSubsystem::PostInitialize() 替代。
 * 保留此函数作为后备入口：若未来需要从全局委托而非子系统生命周期触发初始化。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnPostWorldInitialization(
	UWorld* World,
	FWorldInitializationValues WorldInitializationValues
)
{
	BindEvent();
}

/**
 * World 清理回调（全局委托，当前未被使用）。
 *
 * 原本用于在 World 销毁时解绑事件，现已由 WorldSubsystem::Deinitialize() 替代。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnWorldCleanup(UWorld* World, bool bArg, bool bCond)
{
	UnbindEvent();
}

/**
 * 网络驱动创建回调。
 *
 * 在 NetDriver（客户端或服务器网络驱动）创建时被调用。
 * 需要向 NetDriver 的 NetTokenStore 注册载具专用的网络同步数据类型，
 * 否则网络复制管线会因找不到对应令牌数据存储而断言失败。
 *
 * 采用防御性时序处理：
 * - 若调用时 NetTokenStore 已就绪：直接注册。
 * - 若 NetTokenStore 尚未就绪：通过 OnNetTokenStoreReady 委托延迟注册。
 *   这是因为 NetDriver 的令牌存储可能在驱动创建后的某个异步点才完成初始化。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnNetDriverCreated(UWorld* World, UNetDriver* NetDriver)
{
	// 1) 守卫
	if (!IsValid(NetDriver)) return;

	// 2) 注册网络令牌数据存储，防御性编程，不信任时序
	if (NetDriver->GetNetTokenStore())
		RegisterNetTokenDataStores(NetDriver);
	else
	{
		NetDriver->OnNetTokenStoreReady().AddUObject(
			this,
			&USingularisMorphVehicleSchedulerSubsystem::OnNetTokenStoreReady
		);
	}
}

/**
 * NetTokenStore 就绪的延迟回调。
 *
 * 当 NetDriver 在创建时 NetTokenStore 尚未初始化时，此回调将被触发。
 * 由于委托可能在对象已销毁后触发，需要先校验 NetDriver 有效性。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnNetTokenStoreReady(UNetDriver* NetDriver) const
{
	// 1) 守卫
	if (!IsValid(NetDriver)) return;

	// 2) 注册网络令牌数据存储
	RegisterNetTokenDataStores(NetDriver);
}

/**
 * 物理场景 PreTick 回调（每帧物理步进前，游戏线程）。
 *
 * 分三个阶段执行：
 * 1) PreTickGT —— 各载具在物理更新前刷新游戏线程专属状态：
 *    - 从玩家控制器读取最新输入（油门、转向、刹车等）。
 *    - 更新动画蓝图参数。
 *    - 计算本地预测数据。
 *
 * 2) Update —— 各载具执行逻辑 Tick：
 *    - 处理模块增删请求。
 *    - 更新内部状态机。
 *    - 准备输入数据。
 *
 * 3) FinalizeSimCallbackData —— 将本帧的控制输入序列化到 AsyncInput：
 *    - 写入物理代理引用（Proxy），物理线程通过它定位 Chaos 粒子。
 *    - 写入控制参数（PhysicsInputs），供物理线程 Simulate 使用。
 *
 * 注意：SubStepCount 在此处归零，由物理线程回调中递增。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnPhysScenePreTick(
	FPhysScene_Chaos* PhysScene_Chaos,
	const float DeltaTime
)
{
	// 1) 守卫
	const UWorld* World = GetWorld();
	if (!IsValid(World)) return;

	// 重置子步计数（将在物理线程回调中随子步递增）
	SubStepCount = 0;

	// 2) 阶段一：PreTickGT —— 刷新各载具的游戏线程状态
	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : VehicleSimulationComponents)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
			StrongPtr->PreTickGT(DeltaTime);
	}

	// 3) 阶段二 & 三：Update + FinalizeSimCallbackData
	// 获取共享的异步输入缓冲区（将在物理线程的 OnPreSimulate_Internal 中被消费）
	FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();

	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : VehicleSimulationComponents)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (!StrongPtr.IsValid())
			continue;

		// Update：逻辑更新（模块管理、状态机等）
		StrongPtr->Update(DeltaTime);
		// FinalizeSimCallbackData：将控制输入写入 AsyncInput（Proxy + PhysicsInputs）
		StrongPtr->FinalizeSimCallbackData(*AsyncInput);
	}
}

/**
 * 物理场景 PostTick 回调（每帧物理步进后，游戏线程）。
 *
 * 分两步执行：
 * 1) ParallelUpdateVehicles() —— 从物理线程拉取模拟结果、插值后分发至各载具组件。
 * 2) PostUpdate() —— 各载具完成本帧的渲染状态更新：
 *    - 将物理模拟结果应用到渲染变换（位置、旋转）。
 *    - 更新悬挂、车轮等视觉效果。
 *    - 触发音效、粒子等反馈。
 */
void USingularisMorphVehicleSchedulerSubsystem::OnPhysScenePostTick(FChaosScene* ChaosScene)
{
	// 物理结果拉取与分发（详见 ParallelUpdateVehicles 文档注释）
	ParallelUpdateVehicles();

	// 各载具的渲染状态更新
	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : VehicleSimulationComponents)
	{
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (StrongPtr.IsValid())
			StrongPtr->PostUpdate();
	}
}


/**
 * 游戏线程注入输入：为物理线程准备本帧各载具的模拟输入数据。
 *
 * 架构背景：
 * - 这是游戏线程 → 物理线程的数据通路，与 ParallelUpdateVehicles()（物理线程 → 游戏线程）对称。
 * - 通过 SolverRewindCallback->InjectInputsExternal 注册，因此在两种场景下都会被调用：
 *   a) 正常物理 Tick：每帧在物理步进前注入最新输入。
 *   b) 网络物理回放（Rewind）：服务器需要重新注入历史输入以复现过去的模拟步骤。
 * - 由于回放场景的存在，本函数必须是纯数据驱动的——不能依赖或修改游戏线程的可变状态。
 * - AsyncInput 是游戏线程与物理线程共享的缓冲区：游戏线程写入完毕后，物理线程在回调中读取。
 *
 * 参数：
 * - PhysicsStep：当前子步索引（Chaos Solver 在一次游戏帧内可能执行多个物理子步）。
 * - NumSteps：本帧总共需要模拟的物理子步数，供载具层面的运动预测与子步插值使用。
 *
 * 调用时机：由 Chaos Solver 在每次物理步进前回调（通过 FNetworkPhysicsCallback）。
 */
void USingularisMorphVehicleSchedulerSubsystem::InjectInputs(const int PhysicsStep, const int NumSteps)
{
	// 1) 守卫：World 不存在或正在被销毁时无法注入输入
	UWorld* World = GetWorld();
	if (!IsValid(World)) return;

	// 2) 获取异步输入缓冲区（游戏线程与物理线程共享的编组容器）
	//
	// GetProducerInputData_External() 返回的指针在回调生命周期内始终有效，
	// 物理线程在 OnPreSimulate_Internal 中读取该缓冲区后由 Solver 自动管理切换。
	FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();
	check(AsyncInput);

	// 3) 重置缓冲区并写入本帧元数据
	//
	// Reset() 清空上一帧残留的载具输入列表（"仅保留最新帧数据"），避免物理线程读到过期数据。
	AsyncInput->Reset();
	// Reserve 预分配避免循环 Add 时动态扩容带来的堆分配抖动。
	AsyncInput->VehicleInputs.Reserve(VehicleSimulationComponents.Num());
	// World 引用供物理线程在 Simulate 回调中使用（如射线检测需要 World 上下文）。
	AsyncInput->World = World;
	// Timestamp 是单调递增的帧序号，物理线程用于判断输入是否过期。
	AsyncInput->Timestamp = Timestamp;
	++Timestamp;

	// 4) 为每个已注册的载具构建独立的异步输入结构体
	//
	// 每个载具拥有独立的 FSingularisMorphVehicleAsyncInput，包含：
	// - 物理代理指针（Proxy）：物理线程通过它定位 Chaos 粒子。
	// - 控制标志（bIsLocallyControlled）：影响网络预测策略。
	// - 物理输入数据（PhysicsInputs）：油门/转向/刹车等。
	for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : VehicleSimulationComponents)
	{
		// 弱指针升级为强引用，防止迭代期间对象被 GC 回收
		TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
		if (!StrongPtr.IsValid())
			continue;

		// MakeUnique 创建智能指针，MoveTemp 转移所有权到 TArray<TUniquePtr<>> 中。
		// 物理线程通过该数组以索引访问各载具输入。
		auto CurInput = MakeUnique<FSingularisMorphVehicleAsyncInput>();
		AsyncInput->VehicleInputs.Add(MoveTemp(CurInput));

		// ProduceInput 在刚添加的输入结构体上填充载具专属数据
		// （PhysicsStep/NumSteps 供载具计算该子步内的运动增量）。
		StrongPtr->ProduceInput(PhysicsStep, NumSteps, AsyncInput->VehicleInputs.Last().Get());
	}
}

/**
 * 注册所有事件回调与异步回调对象，将子系统接入整个物理与网络管线。
 *
 * 注册项分为四层，按依赖顺序执行：
 *
 * 1) 网络层 —— 注册 OnNetDriverCreated 全局委托。
 *    每当新 NetDriver 创建时触发，为网络复制管线注册载具专用的令牌数据存储。
 *
 * 2) 物理 Tick 层 —— 注册 PreTick 和 PostTick 回调到当前 World 的 FPhysScene。
 *    PreTick：每帧物理步进前，执行载具的 PreTickGT + Update + FinalizeSimCallbackData。
 *    PostTick：每帧物理步进后，执行 ParallelUpdateVehicles + PostUpdate。
 *
 * 3) 异步回调层 —— 在 Chaos Solver 上创建并注册 TSimCallbackObject。
 *    AsyncCallback 负责物理线程侧的 PreSimulate / Rewind / ContactModification 回调，
 *    是游戏线程与物理线程之间 AsyncInput / AsyncOutput 数据编组的中枢。
 *
 * 4) 网络回放层 —— 将 InjectInputs 注册到 SolverRewindCallback。
 *    网络物理回放时，服务器需要重新注入历史输入以复现过去的模拟步骤。
 *    若 GetRewindCallback() 返回空（单机模式无网络回放），则跳过此步骤。
 *
 * 调用时机：PostInitialize() 中，物理场景已完全就绪后。
 */
void USingularisMorphVehicleSchedulerSubsystem::BindEvent()
{
	// 1) 注册网络驱动创建回调
	OnNetDriverCreatedHandle = FWorldDelegates::OnNetDriverCreated.AddUObject(
		this,
		&USingularisMorphVehicleSchedulerSubsystem::OnNetDriverCreated
	);

	// 2) 注册物理场景前后Tick回调
	OnPhysScenePreTickHandle = PhysicsScene->OnPhysScenePreTick.AddUObject(
		this,
		&USingularisMorphVehicleSchedulerSubsystem::OnPhysScenePreTick
	);
	OnPhysScenePostTickHandle = PhysicsScene->OnPhysScenePostTick.AddUObject(
		this,
		&USingularisMorphVehicleSchedulerSubsystem::OnPhysScenePostTick
	);

	// 3) 创建异步回调对象以管理异步 Ticking 与数据编组
	AsyncCallback = PhysicsScene->GetSolver()->CreateAndRegisterSimCallbackObject_External<
		FSingularisMorphSimModuleManagerAsyncCallback
	>();

	// 4) 将输入注入函数注册至网络物理回放回调
	if (const auto SolverRewindCallback = static_cast<FNetworkPhysicsCallback*>(
		PhysicsScene->GetSolver()->GetRewindCallback()
	))
	{
		SolverRewindCallback->InjectInputsExternal.AddUObject(
			this,
			&USingularisMorphVehicleSchedulerSubsystem::InjectInputs
		);
	}
}

/**
 * 解绑所有事件回调并注销异步回调对象，执行 BindEvent() 的逆向清理。
 *
 * 清理顺序与注册顺序相反：
 * 1) 先移除普通委托（Remove），这些操作是幂等且安全的。
 * 2) 再注销异步回调对象（UnregisterAndFreeSimCallbackObject_External），
 *    这会将 AsyncCallback 从 Solver 的回调列表中移除并释放内存。
 *
 * 守卫条件说明：
 * - PhysicsScene 空指针守卫：编辑器关闭时，物理场景可能在子系统 Deinitialize 前
 *   已被引擎销毁，此时 PhysicsScene 指针置空。跳过清理可避免空指针访问崩溃。
 * - AsyncCallback + Solver 双重校验：Solver 可能在物理场景销毁时已被释放。
 *
 * 调用时机：Deinitialize() 中，子系统销毁前。
 */
void USingularisMorphVehicleSchedulerSubsystem::UnbindEvent()
{
	// 1) 守卫：编辑器关闭时物理场景和求解器可能在子系统 Deinitialize 前已释放
	if (!PhysicsScene) return;

	auto* Solver = PhysicsScene->GetSolver();

	// 2) 移除物理场景回调（委托解绑是幂等的，重复调用无害）
	PhysicsScene->OnPhysScenePreTick.Remove(OnPhysScenePreTickHandle);
	PhysicsScene->OnPhysScenePostTick.Remove(OnPhysScenePostTickHandle);
	FWorldDelegates::OnNetDriverCreated.Remove(OnNetDriverCreatedHandle);

	// 3) 注销异步回调对象（从 Solver 移除并释放内存）
	if (AsyncCallback && Solver)
	{
		Solver->UnregisterAndFreeSimCallbackObject_External(AsyncCallback);
		AsyncCallback = nullptr;
	}
}

/**
 * 向 NetDriver 的 NetTokenStore 注册载具相关的网络令牌数据存储。
 *
 * 注册两种数据类型：
 * - FModuleInputNetTokenStore：模块输入的令牌存储。
 *   用于在网络同步中将模块输入数据（油门/转向等）打包为网络令牌传输，
 *   避免每帧同步完整的结构体数据，显著降低网络带宽消耗。
 * - FNetworkSingularisMorphVehicleStateNetTokenStore：载具状态的令牌存储。
 *   用于同步载具的物理状态（位置/速度/姿态）等关键网络数据。
 *
 * 采用幂等注册策略：
 * - 先通过 GetDataStore<T>() 查询是否已注册，避免重复注册。
 * - 若不存在则通过 CreateAndRegisterDataStore<T>() 创建并注册。
 *
 * NetTokenStore 机制说明：
 * NetTokenStore 是 UE5 Iris 网络复制系统的令牌管理容器。
 * 每种需要网络同步的数据类型必须在 TokenStore 中预先注册对应的 DataStore，
 * 网络复制管线在序列化/反序列化时通过 DataStore 进行令牌 ↔ 数据的转换。
 */
void USingularisMorphVehicleSchedulerSubsystem::RegisterNetTokenDataStores(UNetDriver* NetDriver) const
{
	// 1) 守卫：多层空指针校验
	const UWorld* World = GetWorld();
	if (!IsValid(World)) return;
	if (!IsValid(NetDriver)) return;

	UE::Net::FNetTokenStore* TokenStore = NetDriver->GetNetTokenStore();
	if (!TokenStore) return;

	// 类型别名，提升可读性
	using FModuleInputNetTokenStore = UE::Net::TStructNetTokenDataStore<FSingularisMorphModuleInputNetTokenData>;
	using FNetworkSingularisMorphVehicleStateNetTokenStore =
		UE::Net::TStructNetTokenDataStore<FNetworkSingularisMorphVehicleStateNetTokenData>;

	// 2) 幂等注册：若已存在则跳过，避免重复创建
	if (!TokenStore->GetDataStore<FModuleInputNetTokenStore>())
		TokenStore->CreateAndRegisterDataStore<FModuleInputNetTokenStore>();
	if (!TokenStore->GetDataStore<FNetworkSingularisMorphVehicleStateNetTokenStore>())
		TokenStore->CreateAndRegisterDataStore<FNetworkSingularisMorphVehicleStateNetTokenStore>();
}

/**
 * 游戏线程后处理：从物理线程拉取异步模拟结果，插值后分发给各载具组件。
 *
 * 架构背景：
 * - Chaos 物理引擎以固定步长异步运行，游戏线程以可变帧率渲染。
 * - AsyncCallback 是注册在 Chaos Solver 上的回调对象，负责双向数据编组：
 *   游戏线程 → 物理线程（输入）：InjectInputs() 写入，物理线程读取。
 *   物理线程 → 游戏线程（输出）：物理线程写入，本函数通过 PopOutputData_External() 取出。
 * - OutputRecord 是一个双槽位环形缓冲，缓存最近两帧异步输出，用于时间插值。
 * - CombinedNewlyCreatedModuleGuids 收集模拟过程中动态创建的模块 GUID，
 *   供各载具组件的 ParallelUpdate() 消费以初始化新模块。
 *
 * 调用时机：OnPhysScenePostTick（每帧物理步进后，游戏线程）
 */
void USingularisMorphVehicleSchedulerSubsystem::ParallelUpdateVehicles()
{
	// 物理线程当前已推进到的时刻（即异步步骤的结束时刻）
	const double ResultsTime = AsyncCallback->GetSolver()->GetPhysicsResultsTime_External();
	// 异步步骤的单步时长（物理子步的累计时间）
	const double AsyncDeltaTime = AsyncCallback->GetSolver()->GetAsyncDeltaTime();

	// 1) 弹出异步输出并累积新创建的模块事件
	//
	// 为每个载具槽位预分配一个空的事件容器，后续从异步输出中填充。
	TArray<Chaos::FCreatedModules> CombinedNewlyCreatedModuleGuids;
	CombinedNewlyCreatedModuleGuids.Init(Chaos::FCreatedModules{}, VehicleSimulationComponents.Num());

	// 判断是否有新的异步输出可用：
	// 若物理线程推进到了比当前缓存的最新输出更远的时刻，则需要弹出新数据。
	const double LatestOutputResultsTime = OutputRecord.GetLatestOutputStartTime() + AsyncDeltaTime;
	if (ResultsTime > LatestOutputResultsTime)
	{
		// PopOutputData_External() 一次性取出物理线程已积压的全部输出帧
		// （物理线程可能一次产生多帧，必须全部消费以保持同步）。
		Chaos::TSimCallbackOutputHandle<FSingularisMorphChaosSimModuleManagerAsyncOutput> NextAsyncOutput;
		while ((NextAsyncOutput = AsyncCallback->PopOutputData_External()))
		{
			// 遍历当前输出帧中每个载具的异步结果
			for (int32 VehicleIdx = 0; VehicleIdx < NextAsyncOutput->VehicleOutputs.Num(); ++VehicleIdx)
			{
				if (!NextAsyncOutput->VehicleOutputs[VehicleIdx])
					continue;

				// 从该载具的模拟输出中取出本轮新创建的模块 GUID 列表
				TArray<Chaos::FCreatedModule>& NewlyCreatedModuleGuids =
					NextAsyncOutput->VehicleOutputs[VehicleIdx]->VehicleSimOutput.NewlyCreatedModuleGuids;
				// 追加到累积容器中（跨帧去重由消费端处理）
				if (NewlyCreatedModuleGuids.Num() > 0 && VehicleIdx < CombinedNewlyCreatedModuleGuids.Num())
					CombinedNewlyCreatedModuleGuids[VehicleIdx].ModuleEvents.Append(NewlyCreatedModuleGuids);
			}
			// 将当前输出帧移交 OutputRecord 双缓冲（自动管理先前/后续帧的角色切换）
			OutputRecord.ConsumeOutput(MoveTemp(NextAsyncOutput));
		}
	}

	// 2) 计算插值因子，为每个载具组件设置当前帧的异步数据引用
	//
	// OutputRecord 持有前后两帧异步输出，本步骤根据当前游戏时间在两者之间插值。
	FSingularisMorphChaosSimModuleManagerAsyncOutput* PreviousOutput = OutputRecord.GetPreviousOutput();
	FSingularisMorphChaosSimModuleManagerAsyncOutput* NextOutput = OutputRecord.GetNextOutput();
	const UWorld* World = PhysicsScene->GetOwningWorld();
	const FSingularisMorphChaosSimModuleManagerAsyncInput* AsyncInput = AsyncCallback->GetProducerInputData_External();

	// 三者缺一不可：World 为载具提供物理场景上下文，PreviousOutput 保证有数据可插值，
	// AsyncInput 提供当前帧的时间戳等元信息。
	if (World && PreviousOutput && AsyncInput)
	{
		// ResultsTime 是步骤结束时刻，InternalTime 是步骤开始时刻。
		// GetInterpolationFactor 按 InternalTime 时间线计算插值，因此需将基准时间回退一个步长。
		const double InterpolationBaseTime = ResultsTime - AsyncDeltaTime;
		const float Alpha = OutputRecord.GetInterpolationFactor(InterpolationBaseTime);

		// 将插值所需的引用（前一帧/后一帧/插值因子/时间戳）注入每个载具组件，
		// 组件内部的 ParallelUpdate 将据此选取对应帧的数据进行混合。
		for (TWeakObjectPtr<USingularisMorphVehicleSimulationComponent>& Vehicle : VehicleSimulationComponents)
		{
			TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr = Vehicle.Pin();
			if (StrongPtr.IsValid())
				StrongPtr->SetCurrentAsyncData(PreviousOutput, NextOutput, Alpha, Timestamp);
		}
	}

	// 3) 对所有载具组件执行并行更新（读取异步输出并分发给各模拟模块）
	//
	// 注意：此处 bForceSingleThread = true，并行更新在单线程上串行执行。
	// 这是为了规避 ParallelFor 多线程下弱指针 Pin() 导致的竞态风险。
	auto UpdateVehicleParallel = [&CombinedNewlyCreatedModuleGuids, this](const int32 Idx)
	{
		// 弱指针升级为强引用，防止迭代期间对象被 GC 回收
		const TStrongObjectPtr<USingularisMorphVehicleSimulationComponent> StrongPtr =
			VehicleSimulationComponents[Idx].Pin();
		if (StrongPtr.IsValid())
			// 将对应槽位的新建模块事件传给组件，组件内部完成：
			// a) 清理上帧输出；b) 读取异步输出数据；c) 初始化新模块
			StrongPtr->ParallelUpdate(CombinedNewlyCreatedModuleGuids[Idx]);
	};

	ParallelFor(VehicleSimulationComponents.Num(), UpdateVehicleParallel, true);
}
