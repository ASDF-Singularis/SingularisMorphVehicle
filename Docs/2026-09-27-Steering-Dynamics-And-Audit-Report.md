# 转向动力学、双插件取舍与缺陷审查报告

日期：2026-09-27
范围：`SingularisMorphVehicle` 转向动力学补齐、ChaosVehiclesPlugin / ChaosModularVehicle 双向对照取舍、插件自身缺陷审查与修复。
验证状态：未执行编译与运行验证（按要求不编译、无 LSP），结论均基于与 `ChaosVehicles` / `ChaosModularVehicle` / `ChaosVehiclesPlugin` 源码逐行对照的静态核对。

---

## 1. 转向动力学（任务 1）

### 1.1 根因

ChaosVehicles 的车轮模块把转向角作为**无状态代数映射**求出：

- `ChaosVehiclesCore/Private/SimModule/WheelModule.cpp:41`
  `SteerAngleDegrees = Setup().SteeringEnabled ? Inputs.GetControls().GetMagnitude(SteeringControlName) * Setup().MaxSteeringAngle : 0.0f;`

输入阶跃等价于转角阶跃，且不存在车速衰减、内外轮几何与回正力矩。经典插件的等价行（`ChaosVehiclesPlugin/.../ChaosWheeledVehicleMovementComponent.cpp` `ProcessSteering`）同样是瞬时赋值，其"有过程"的观感来自 **游戏线程的输入速率平滑 + 速度敏感转向曲线**：

- `ChaosVehicleMovementComponent.cpp:633-639`：`SteeringInputRate.RiseRate = 2.5 / FallRate = 5.0 / SquaredFunction`
- `ChaosWheeledVehicleMovementComponent.h:473-483`：`SteeringCurve` 关键帧 `0/20/60/120 mph → 1.0/0.8/0.4/0.3`
- `ChaosWheeledVehicleMovementComponent.h:401-413`：`FVehicleInputRateConfig::InterpInputValue`（上升/回落分流限幅）
- `SteeringSystem.h:92-127`：`SingleAngle` / `AngleRatio` / `Ackermann` 三种几何

SingularisMorphVehicle 既没有物理线程侧的转角状态，也不依赖速度曲线；平滑仅由可选的 `UInputModifier_SingularisMorphVehicleSmooth` 提供，且该修饰器在无输入时把值**直接归零**（`InputModifier_SingularisMorphVehicleSmooth.cpp` 旧实现），并默认 `RiseRate = 10`、无回落速率——既不完整，也不属于载具系统（未接线即完全失效）。

### 1.2 方案

转向角改为**被积分的状态量**，动力学位于物理线程的车轮模块内（不依赖输入接线，网络重演下两端一致）：

1. **目标角** = 控制输入 × 最大转角 × 车速衰减倍率，再按几何类型分配内外轮。
2. **实际角** 以有限角速度逼近目标角：上升用 `SteeringRiseRate`，离开目标角用 `SteeringFallRate`。
3. 可选**轮胎回正力矩**：以上一步轮胎侧向力按增益削减转角（附着突变时产生转向反馈）。
4. 机械限位：结果钳制到 ± 最大转角。

实现不复制任何轮胎公式：算出的实际转向角以归一化形式写入输入容器的 `Steering` 项，调用官方 `FWheelSimModule::Simulate` 后**立即还原**原值，因此同帧其余车轮与模块仍读到原始控制输入，轮胎力学路径完全复用官方实现。

前置条件是 `FWheelSettings::SteeringEnabled` 为真且 `MaxSteeringAngle > 0`；关闭转向时完全走官方路径（转角恒 0）。保持 `GetSimType()` 为 `"FWheelSimModule"`，扭矩传递、树构建与网络复制结构与官方车轮模块一致。

### 1.3 文件

| 文件 | 内容 |
| --- | --- |
| `Public/Types/SingularisMorphVehicleType.h` | 新增 `ESingularisMorphVehicleSteeringType`（SingleAngle / AngleRatio / Ackermann） |
| `Public/Core/SingularisMorphVehicleWheelSimModule.h` | 新增 `FSingularisMorphWheelSteeringSettings`（物理线程端设置）与 `FSingularisMorphWheelSimModule` |
| `Private/Core/SingularisMorphVehicleWheelSimModule.cpp` | 目标角解算、角速率限幅、回正力矩、机械限位、输入容器注入与还原 |
| `Public/Components/SingularisWheelSUComponent.h` | 新增 `FSingularisMorphVehicleSteeringSetup` 与 `SteeringSetup` 属性；新增实际转角缓存 |
| `Private/Components/SingularisWheelSUComponent.cpp` | 曲线烘培、转向设置装配、模块创建、输出转角缓存 |
| `Public/Types/InputModifier_SingularisMorphVehicleSmooth.h` / `Private/...cpp` | 输入修饰器对齐经典插件：上升/回落速率 + 线性/平方/自定义曲线；移除"无输入即归零" |

### 1.4 参数与默认值

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| 转向几何类型 | SingleAngle | 内外轮同角；`AngleRatio` 放大内侧轮，`Ackermann` 按轴距/轮距解算 |
| 转向上升速率 | 120 °/s | 35° 满舵约 0.29 s |
| 转向回落速率 | 240 °/s | 回正快于建角，符合实车转向机特性 |
| 角度比例 | 0.7 | `内侧轮 = 参考角 / 比例`，与经典插件默认一致（外侧轮保持参考角） |
| 轴距 / 轮距 | 280 / 160 cm | 仅 `Ackermann` 使用 |
| 回正力矩增益 | 0（关闭） | 单位 度/(牛顿·秒) |
| 车速敏感曲线 | `0/32/97/193 km/h → 1.0/0.8/0.4/0.3` | 由经典插件 20/60/120 mph 换算；曲线为空表示不随车速衰减 |

曲线在模块创建时按峰值归一化后等距采样 20 段烘培为 `Chaos::FGraph`（物理线程不访问 UObject）。

### 1.5 已知边界

- 转向角属于积分状态，未进入网络复制数据（`FWheelSimModuleData` 不含该字段，且不得改动其序列化布局）。拓扑重建时以最近一次输出的实际转角为初值（`USingularisWheelSUComponent::CachedSteeringAngleDegrees`），联机重演下不随回滚复位。
- 内外轮几何在角速率限幅**之后**生效，稳态精确、过渡期内外轮同步变化；内侧轮受机械限位钳制（与实车满舵顶死一致）。
- 符号约定：正转向角使车头偏向 +Y（UE 左手系），故内侧轮为 `sign(转向角) × sign(轮位 Y) > 0` 侧。经典插件的 `FSimpleSteeringSim::GetSteeringAngle` 将 `InNormSteering * WheelSide > 0` 判为外侧轮，与本实现相反，且 `Ackermann` 分支对输入取负（`CalculateAkermannAngle(-InNormSteering, ...)`），两分支符号不一致，属上游缺陷，未沿用。

---

## 2. 双插件对照与取舍（任务 2）

### 2.1 已采用（取其精华）

| 来源 | 内容 | 落点 |
| --- | --- | --- |
| ChaosVehiclesPlugin | 速度敏感转向曲线 | 烘培进 `FSingularisMorphWheelSteeringSettings::SpeedSteeringCurve` |
| ChaosVehiclesPlugin | 内外轮几何（AngleRatio / Ackermann） | `ComputeTargetSteeringAngle` |
| ChaosVehiclesPlugin | `FVehicleInputRateConfig` 的上升/回落分流限幅与输入曲线 | `UInputModifier_SingularisMorphVehicleSmooth` |
| ChaosVehiclesPlugin | `FNetworkVehicleInputs::InterpolateData` 的帧差零守卫（`FrameSpan > 0 ? ... : 1.0f`） | 移植到输入与状态插值 |
| ChaosVehiclesPlugin | 弹簧-阻尼模型、制动力一步停稳钳制、ABS/TC 0.98 折减、摩擦圆 | 官方模块已具备，本轮仅确认并保留 |
| ChaosModularVehicle | 模块树、延迟力、集群联合粒子、网络状态历史 | 体系结构保留不变 |

### 2.2 已剔除（去其糟粕）

| 缺陷 | 证据 | 处理 |
| --- | --- | --- |
| 转向为瞬时映射 | `WheelModule.cpp:41` | 以角速率限幅的积分状态替代 |
| 侧滑角上限声明后未使用 | `WheelModule.cpp:132-134`（`ClippedAngle` 求而不用）；`WheelSystem.cpp:155-157` 同 | 未复制该写法；`SlipAngleLimit` 在上游仍为死配置（见 4.1） |
| 内外轮符号自相矛盾 | `SteeringSystem.h:100,110` | 采用几何自洽的新实现 |
| 帧差为零时插值除零 | `ChaosSimModuleManagerAsyncCallback` 等价实现与 `SingularisMorphSimModuleManagerAsyncCallback.cpp:187/586` | 加帧差守卫 |
| `SetGearInput` 单次脉冲语义与文档不符 | `ModularVehicleBaseComponent.cpp:1935` 仅存目标值，本插件旧实现按缓存挡位发一次脉冲 | 改为"记录目标 + 逐帧产出脉冲" |
| 输入容器重建即清零 | 官方仅在建物理状态时初始化一次；本插件每次重建都会调用 | 重建时按输入名回迁已缓冲值 |
| 模拟树跨线程无锁访问 | 官方 `AccessSimComponentTree` 无锁 | 网络状态的写入取树写锁、写出取树读锁，与物理线程的树增删互斥 |

---

## 3. 缺陷审查与修复（任务 3）

### 3.1 阻断 / 严重

| # | 问题 | 位置 | 修复 |
| --- | --- | --- | --- |
| 1 | `FAllInputs` 成员持有指向栈上 `FInputInterface` 的悬垂指针，`ManualOverride` 回调下游读取即 UAF | `SingularisMorphVehicleSimulationCU.h:161`、`.cpp:340-346` | `SimInputData` 改为 `SimulateModuleTree` 内局部对象 |
| 2 | 模拟树同时被游戏线程读写（`SetSimState` / `SetNetState`）与物理线程增删，无互斥 | `SingularisMorphSimModuleManagerAsyncCallback.cpp:251-271`、`SimulationCU.cpp:104` | 新增 `ApplySimStateFromNetwork`（树**写**锁）/ `BuildNetStateForNetwork`（树读锁），网络路径统一走带锁入口 |
| 3 | 网络反序列化的模块数直接驱动 `FNetBitArray` 与数组扩容，损坏包可致巨量分配 | 同上 `.cpp:283-290`、`:448`、输入令牌路径 | 三处加 `MaxNetModulesPerVehicle = 4096` 上限，超限整包判失败 |
| 4 | 悬停射线命中面缺材质时沿用上一次命中的摩擦（粘滞抓地力） | `SimulationCU.cpp:709-724` | 命中面有材质取材质，无材质回退到组件新增的 `DefaultSurfaceFriction`（默认 0.7，与 `UPhysicalMaterial` 一致） |
| 5 | 地面体即载具自身根粒子时记录为 GroundBody，车轮反力重复施加到自身 | `SimulationCU.cpp:727-746` | 增加 `GroundParticle != RootParticle` 判定 |
| 6 | `FixupTreeLinks` 在建交叉链接**之后**重挂车轮，重挂后悬挂↔车轮索引陈旧 | `SingularisMorphVehicleBuilder.cpp:92-128`（旧顺序） | 交叉链接重建移到全部重挂之后，并加环保护步数上限 |
| 7 | 底盘守卫以"组件是否存在"为判据，底盘件离簇后仍以无效粒子索引重建整棵树 | `SimulationComponent.cpp:398-420`（旧） | 判据改为"驱动组件位于本次快照中或为纯仿真底盘"，并统计多底盘告警 |
| 8 | `RebuildFromSnapshot` 在 `ConsumeSnapshot` 之后静默 return，脏标记已消费而拓扑变更丢失 | `SimulationComponent.cpp:323-344` | 前置条件（Owner / 模拟对象 / 映射子系统）校验上移到 `PreTickGT` |
| 9 | 运行时总线上限/死循环：`for (X = 0; X <= MaxRPM; X += MaxRPM / 20)` 在 `MaxRPM = 0` 时步长为零，游戏线程死循环 | `SingularisEngineSUComponent.cpp:42` | 改为固定段数采样 + `Clamp(MaxRPM, 1, 65535)` |
| 10 | 引擎扭矩曲线被追加在设置结构默认曲线之后，归一化横轴与实际曲线错位 | 同上 | 采样前 `TorqueCurve.Empty()` |

### 3.2 一般

| # | 问题 | 位置 | 修复 |
| --- | --- | --- | --- |
| 11 | 动画槽位标志只增不减，已停止动画的通道把部件钉在旧位姿 | `SimulationComponent.cpp` `ParallelUpdate`、`SingularisMorphVehicleAnimationInstance.cpp:101` | 帧首清零全部槽位，按驱动通道累加；动画实例改为直接赋值 |
| 12 | 槽位被接管时保留旧模块的动画残值 | `RemoveSimulationModule` | 接管时清零标志与偏移 |
| 13 | 输入配置比较仅比名称（`operator==` 只比 `Name`），类型与标志变更静默失效 | `SetupInputConfiguration` | 新增 `IsInputConfigurationEqual`（名称+类型+三个标志） |
| 14 | 同名不同类型的输入被静默丢弃 | `AssimilateComponentInputs` | 保留首条并告警 |
| 15 | `SetInputProducerClass` 换类不生效（生产者已存在且配置未变即早退） | `SetupInputConfiguration` | 类不匹配时重建实例 |
| 16 | `ProduceInput` 在代理未解析时提前返回，输入缓冲不被消费，恢复后脉冲叠加 | `ProduceInput` | 生产者调用移到代理守卫之前 |
| 17 | 模块创建成功但入树失败时泄漏模块对象与悬挂约束 | `RebuildFromSnapshot` 的 `AddEntity` | 失败路径终止并释放模块，附日志 |
| 18 | 幂等守卫忽略动画开关，运行期切换动画不重建 | 同上 | 判据纳入 `IsAnimationEnabled()` 比较 |
| 19 | 换挡请求为单次脉冲，变速箱一次只接受一挡（目标挡位 ±1），跨挡请求失效；且未收到变速箱输出时挡位未知，发脉冲会把初始挡位误判为需升降挡 | `SimulationComponent.cpp` `SetGearInput` / `Update` | 改为「记录目标挡位 + `Update` 逐帧产出脉冲」，请求按变速箱实际挡数钳制（见 3.3 第 45 条）；新增 `bHasGearData` 判据，挡位未知时不产出脉冲；`CurrentGear` 缓存初值保持 0（0 同时表示换挡过渡） |
| 20 | 物理状态销毁后跨世代残留挡位/RPM/本地控制/待重建标记/在飞异步数据 | `OnDestroyPhysicsState` | 逐项复位 |
| 21 | 回放输入缓冲从不清空，运行时重建会无界增长且物理线程取到陈旧条目 | `SingularisMorphVehicleInputProducer.cpp:89` | 填充前 `PlaybackBuffer.Reset()`；新增 `PlaybackSeed` |
| 22 | 随机输入生产者以可变随机流推进，网络重演下不可复现 | 同上 | 由「种子 + 求解器帧号分组」派生，参数化 `Seed` |
| 23 | `AddEntity` 之外的默认输入生产者容器重建同样丢失已捕获输入 | `SingularisMorphVehicleInputProducer.cpp` | 生产者自持名映射副本，重建后按名回迁（`SingularisMorphVehicleInputUtils::CarryOverValues`） |
| 24 | 网络驱动委托只绑不解绑；`OnNetDriverCreated` 收 `World` 却不过滤 | `SingularisMorphVehicleSchedulerSubsystem.cpp:106-136` | 记录句柄并在 `Deinitialize` 解绑；按 `World == GetWorld()` 过滤 |
| 25 | 调度器按数组下标把物理线程输出的"新建模块事件"配给当前载具数组，帧内注册/注销即错配 | `ParallelUpdateVehicles` | 改为按载具指针配对（`TMap<Component*, TArray<FCreatedModule>>`） |
| 26 | `checkf(PhysicsScene)` 在编辑器预览等无物理场景的 World 上崩进程；缓存场景指针另有空解引用 | `PostInitialize:35`、`ParallelUpdateVehicles:499` | 降级为告警并跳过绑定；世界取 `GetWorld()` |
| 27 | 集群联合引用解析失败完全静默，载具永不模拟 | `SingularisMorphVehicleClusterUnionAdapter.cpp:14-17,133-138` | 解析失败/类型不符分别 `Error` 告警并打印引用路径 |
| 28 | 脏标记在"防御性返回"之前清除，代理暂时不可用时该次变更永久丢失 | 同上 `ConsumeSnapshot` | 仅在真正产出快照后清脏 |
| 29 | 骨骼网格体多物理体组件被当作单模块装配，仅有一条 Warning | `SingularisMorphVehicleClusterUnionComponent.cpp:124-134` | 显式告警并说明需专用骨骼网格体适配器 |
| 30 | 动画线程直接读取游戏线程会被重建/重排的槽位表（并行动画求值下读写竞争） | `SingularisMorphVehicleAnimationInstance.cpp:13,61,90` | 组件新增 `ModuleAnimationSetupsLock`，游戏线程全部写入点与动画线程读取点持锁；`ParallelUpdate` 本地累积后不持锁执行模块回调（提交方式见 3.3 第 53 条） |
| 31 | AnimNode 仅按数量判断是否需要重建骨骼引用，等量替换/重排后骨骼永久错位 | `AnimNode_SingularisMorphVehicleController.cpp:166` | 改为「数量 + 骨骼名」签名比较 |
| 32 | AnimNode 无类型校验的 `static_cast`、调试路径空解引用 | 同上 `:26,185` | 按动画实例类型校验代理，空代理时直通 |
| 33 | SU 组件 `EndPlay` 重新解析驱动组件，驱动组件先销毁时映射表条目永久残留 | `SingularisMorphVehicleSUComponent.cpp:42-63` | 记录 BeginPlay 解析结果并在 EndPlay 复用 |
| 34 | `GetInputConfig()` 按值返回数组（每次重建全表拷贝） | 接口与基类 | 改返回 `const TArray&` |
| 35 | 移除忽略 Actor 时不做校验，可移除自忽略的 Owner（悬挂射线命中自身） | `RemoveActorsToIgnore` | 过滤空指针与 Owner |
| 36 | 调试串在 Verbose 被抑制时仍逐节点拼接；新增模块上报为 O(待建数 × 节点数) | `ActionTreeUpdates` | 以 `IsSuppressed(Verbose)` 守卫；一次遍历建 GUID→树索引表 |
| 37 | `Simulate` 中求解器帧号计算为死代码 | `SimulationCU.cpp:285-292` | 删除 |
| 38 | 空 Tick 的 Actor 仍开启 Tick | 集群 Actor / Pawn | 关闭 Tick 并删除空重写 |
| 39 | 过时或误导注释（单线程原因、冻结字段） | 多处 | 按实际约束改写 |

### 3.3 第二轮全局复核修正

对全部改动文件做逐行复核，同时通过机械扫描（花括号平衡、同签名重复定义、相邻重复行、新旧符号残留）。下表为复核中额外发现并已落到代码的问题。

| # | 问题 | 位置 | 修复 |
| --- | --- | --- | --- |
| 40 | 动画节点经 `FAnimInstanceProxy::GetAnimInstance()` 取动画实例，引擎无此接口，必然编译失败 | `AnimNode_SingularisMorphVehicleController.cpp:212` | 改用 `GetAnimInstanceObject()` 并按类型校验 |
| 41 | 集群联合引用的 `PathToComponent` 在 UE5 起已是 `FString`，仍调用 `ToString()` | 集群 Actor / Pawn 构造函数 | 直接使用该字段 |
| 42 | `SetSimState` 写回模块状态却只取树读锁，与同样持读锁的 `GenerateReplicationStructure` 互不排斥，同一模块被并发读写 | `SingularisMorphVehicleSimulationCU.cpp` `ApplySimStateFromNetwork` | 改为 `TWriteScopeLock` |
| 43 | 输入令牌的三组反量化数组未校验等长，短包会导致按下标越界读 | `SingularisMorphSimModuleManagerAsyncCallback.cpp:176` | `Types` / `DecayValues` / `InputValues` 长度一致性校验，不一致整包判失败 |
| 44 | 反序列化出的树索引可为负（编码 -1 表示空槽），被引擎的「上界」检查放行后越界访问 | 同上 `NetSerialize`、`DeltaNetSerialize` | 两处 `SimArrayIndex < 0` 拒绝整包 |
| 45 | 换挡请求不按变速箱实际挡数钳制，越界请求使「当前挡位 ≠ 目标挡位」恒成立而逐帧持续发脉冲 | `SingularisMorphVehicleSimulationComponent.cpp` `SetGearInput` | 按 `ForwardRatios` / `ReverseRatios` 数量钳制 |
| 46 | 负的最大转角（编辑器 `ClampMin` 仅约束 UI）经归一化写回后得到反向转向角 | `SingularisWheelSUComponent.cpp` `CreateNewCoreModule` | 钳制到非负；转向动力学与轮胎设置共用该值，保证写回与还原严格互逆 |
| 47 | 输入修饰器对非标量输入动作调用 `Get<float>()` 会断言；`DeltaTime <= 0` 分支返回未平滑的原值（形参遮蔽成员） | `InputModifier_SingularisMorphVehicleSmooth.cpp` | 仅处理 `Axis1D`；删除该分支，让速率限幅在零步长下自然退化 |
| 48 | 阿克曼内侧轮轨迹半径按半轮距计算，内外轮转角差偏大 | `SingularisMorphVehicleWheelSimModule.cpp` `ComputeTargetSteeringAngle` | 改为 `R − 轮距`（两轮轨迹半径之差即整条轮距） |
| 49 | 轮位横向符号取自相对父粒子的位姿，簇质心横向偏移时同轴两轮判为同侧，内侧轮分配反向 | 同上 `GetLateralSideSign` | 改用模块组件变换（构造期写入，不随运动变化） |
| 50 | 车轮触地变化只有原生委托广播，蓝图委托无人广播 | `SingularisWheelSUComponent.cpp` `OnOutputReady` | 补 `OnWheelTouchChangeEvent` 广播 |
| 51 | `FCriticalSection` / `FScopeLock` 依赖传递包含，包含关系变动即编译失败 | 多处 | 显式包含 `HAL/CriticalSection.h` / `Misc/ScopeLock.h` |
| 52 | 悬挂↔车轮交叉链接在歧义拓扑（一个悬挂挂多个车轮，或悬挂既是车轮的子节点又挂车轮）下后写覆盖先写，同轴两轮受力不一致 | `SingularisMorphVehicleBuilder.cpp` `FixupTreeLinks` | 配对唯一时写入，歧义时保留首个配对并告警（本插件自身拓扑为 `Wheel → Suspension`，不受影响） |
| 53 | 动画槽位表按整表覆盖提交，回调期间发生的槽位增删被丢弃，已移除的槽位还会被复制品复活 | `SingularisMorphVehicleSimulationComponent.cpp` `ParallelUpdate` | 改为按 `ModuleGUID` 逐槽回写；GUID 全局唯一且永不复用，重建后的新模块不会命中旧槽位 |
| 54 | 局部量与形参遮蔽基类/自身成员（MSVC C4458 在本工程中为错误）：`ForceFromFriction` 遮蔽 `Chaos::FWheelSimModule` 同名成员、`CurrentValue` 遮蔽修饰器自身成员 | `SingularisMorphVehicleWheelSimModule.cpp` `Simulate`、`InputModifier_SingularisMorphVehicleSmooth.cpp` `ModifyRaw_Implementation` | 分别重命名为 `FrictionForce` 与 `InCurrentValue`；并对全插件重扫确认无其他遮蔽（包括对引擎基类成员的遮蔽） |

### 3.4 经复核判定不改动

- **载具级 `ParallelFor(..., bForceSingleThread = true)`**：`ParallelUpdate` 会写入组件变换（`SetRelativeTransform` + `TeleportPhysics`）与动画槽位，必须在游戏线程执行，强制单线程使该循环留在调用线程；`EnableMultithreading` 需要先把渲染写出移出该循环，属独立改造。
- **`Tooltip`/元数据补齐、SU 组件样板上移基类**：纯整洁性改动，收益低且触及 10 个文件。
- **`AccessModuleAnimationSetups()` 可变访问器**：当前无调用方，保留但已在注释中标注加锁约定。
- **`FThrusterSettings::MaxSpeed`**：上游声明后未在任何模块中读取，未为其新增组件属性。

---

## 4. 未修复项与理由

以下各项均为已知缺口，本轮明确暂不实施（先收敛改动范围，避免未经验证的物理与线程改造）：防倾杆（4.2）、车轮侧向力的侧滑角上限截断（4.1）、载具级多线程（3.4 首条）、骨骼网格体专用适配器（4.4 末条）。4.3 为契约性风险，需后续改造才能彻底消除。

### 4.1 车轮侧向力未按侧滑角上限截断（上游缺陷，未复制亦未修复）

`WheelModule.cpp:132-134` 求出 `ClippedAngle` 后未使用，`FinalLateralForce = |SlipAngle| × CorneringStiffness` 随侧滑角无界线性增长（仅受摩擦圆与"一步停稳"钳制）。修复需要在官方车轮模块内部改写力计算，等价于复制其轮胎公式，收益（大侧滑角下横向力偏大）与风险（无编译验证的物理回归）不成比例。建议后续以自有车轮模块整体接管力计算时一并修正。

### 4.2 防倾杆（经典插件 `FSimpleSuspensionSim` 具备，模块化插件缺失）

经典插件提供同轴两侧弹簧压缩量之差产生的反向力（`RollbarScaling`）与载荷传递控制（`WheelLoadRatio`），模块化插件两者皆无，代价是车身侧倾抑制能力偏弱。实现路径：在 `FSingularisMorphSuspensionSettings` 增加 `AntiRollScaling`，模块 `Simulate` 中按对侧弹簧压缩量差施加位置力；配对索引在 `FixupTreeLinks` 的交叉链接阶段完成（同轴判据：两侧悬挂所配对的轮节点共享同一父节点且横向位置符号相反）。本轮未实施：属新增物理特性而非缺陷修复，且需实机调参。

### 4.3 线程模型残留

- `FSingularisMorphVehicleAsyncInput::Vehicle` 与 `SignularisMorphVehicleAsyncOutput::Vehicle` 为非 UPROPERTY 裸指针，游戏线程销毁与物理线程解引用之间仅靠求解器命令队列兜底（与官方同构）。彻底修复需改为弱指针并在销毁前清空在飞异步数据。
- `GuidToCoreModule` 持有物理线程所拥有模块的裸指针，契约是"仅用于终止回调与事件广播"；外部经公有成员绕过 API 直接操作 `StoredTreeUpdates` 仍可破坏该契约。
- 动画槽位的加锁方案为互斥锁而非双缓冲，极端情况下动画线程会短暂等待游戏线程；若后续出现抖动热点，可改为双缓冲快照。

### 4.4 功能缺口（维持既有决策）

- 螺旋桨 / 气球模块未实现（见 `2026-09-14-Deferred-Features-Report.md`）。
- 非预测联机路径（传统 RPC 复制）未实现。
- 骨骼网格体载具需专用适配器：本轮仅把静默降级改为显式告警（3.2 第 29 条）。

---

## 5. 文件变更清单

### 新增（4）

- `Source/.../Public/Core/SingularisMorphVehicleWheelSimModule.h`
- `Source/.../Private/Core/SingularisMorphVehicleWheelSimModule.cpp`
- `Source/.../Public/Types/SingularisMorphVehicleInputUtils.h`
- `Docs/2026-09-27-Steering-Dynamics-And-Audit-Report.md`

### 修改（30）

`Public/Types/SingularisMorphVehicleType.h`、`Public/Types/InputModifier_SingularisMorphVehicleSmooth.h`、`Private/Types/InputModifier_SingularisMorphVehicleSmooth.cpp`、`Public/Types/SingularisMorphSimModuleManagerAsyncCallback.h`、`Private/Types/SingularisMorphSimModuleManagerAsyncCallback.cpp`、`Public/Types/SingularisMorphVehicleInputProducer.h`、`Private/Types/SingularisMorphVehicleInputProducer.cpp`、`Public/Interfaces/SingularisMorphVehicleSUInterface.h`、`Public/Components/SingularisMorphVehicleSUComponent.h`、`Private/Components/SingularisMorphVehicleSUComponent.cpp`、`Public/Components/SingularisWheelSUComponent.h`、`Private/Components/SingularisWheelSUComponent.cpp`、`Public/Components/SingularisEngineSUComponent.h`、`Private/Components/SingularisEngineSUComponent.cpp`、`Public/Components/SingularisMorphVehicleSimulationComponent.h`、`Private/Components/SingularisMorphVehicleSimulationComponent.cpp`、`Public/Core/SingularisMorphVehicleSimulationCU.h`、`Private/Core/SingularisMorphVehicleSimulationCU.cpp`、`Private/Core/SingularisMorphVehicleBuilder.cpp`、`Private/Subsystems/SingularisMorphVehicleSchedulerSubsystem.cpp`（含头文件）、`Public/Subsystems/SingularisMorphVehicleSchedulerSubsystem.h`、`Private/Animations/SingularisMorphVehicleAnimationInstance.cpp`、`Public/Animations/AnimNode_SingularisMorphVehicleController.h`、`Private/Animations/AnimNode_SingularisMorphVehicleController.cpp`、`Public/Actors/SingularisMorphVehicleClusterActor.h`、`Private/Actors/SingularisMorphVehicleClusterActor.cpp`、`Public/Actors/SingularisMorphVehicleClusterPawn.h`、`Private/Actors/SingularisMorphVehicleClusterPawn.cpp`、`Private/Objects/SingularisMorphVehicleClusterUnionAdapter.cpp`、`Private/Components/SingularisMorphVehicleClusterUnionComponent.cpp`。

## 6. 待验证清单

| 优先级 | 验证项 |
| --- | --- |
| 高 | 编译（本轮改动量大，UHT 与头文件包含关系需实机确认） |
| 高 | 转向手感：默认曲线下述评（0 → 满舵 ≈ 0.29 s）、高速转向衰减、满舵回正 |
| 高 | 转向几何：切换 AngleRatio / Ackermann 时内外轮角差异与转弯半径变化 |
| 高 | 变形帧输入连续性：增删部件当帧油门/转向不中断（第 16、23 条） |
| 高 | 摩擦回退：命中面无物理材质时抓地力由引擎默认 1.0 变为 `DefaultSurfaceFriction`（0.7），确认既有地图中的手感差异可接受（第 4 条） |
| 中 | 网络预测双端：转向状态在重演下的可复现性（转向角未入复制数据，见 1.5）；反量化数组不一致与负树索引的拒包日志（第 43、44 条） |
| 中 | 动画：变形后骨骼绑定正确性（第 31 条）、停止动画后部件位姿复位（第 11、12 条）、帧内槽位增删时动画不中断（第 53 条） |
| 中 | 物理悬挂约束与防倾杆缺失下的侧倾表现（若有需求见 4.2） |
| 中 | 载具解体 → 重组：底盘离簇时的清除与重新装配（第 7、8、28 条） |
| 低 | 悬挂↔车轮配对告警：非常规手动拓扑下应出现告警而非静默错配（第 52 条） |
| 低 | 引擎扭矩曲线：新采样路径与旧行为的差异（第 9、10 条） |
| 低 | 换挡：`SetGearInput` 目标语义在多挡跳转下的收敛（第 19、45 条） |
