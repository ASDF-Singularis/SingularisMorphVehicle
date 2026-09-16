# SingularisMorphVehicle 实现状态报告

日期：2026-09-14
目标：将 SingularisMorphVehicle 从"架构成型但主干断裂"补全至生产就绪状态。
配套文档：`2026-09-14-Deferred-Features-Report.md`（本次明确不实现的事项与后续实现路径）。

## 1. 实现范围

### 阶段 1 输入管线（补全前完全断裂）
- SimulationComponent 新增输入生产者接入：`InputProducerClass`（默认 `USingularisMorphVehicleDefaultInputProducer`）、`InputProducer` 实例（`UPROPERTY(Transient)`）。
- 新增输入数据：`InputNameMap`、`StateNameMap`、`InputsContainer`、`StateInputContainer`、组件级 `InputConfig`。
- `InputConfig` 构造函数默认填入 12 个标准控制输入：Throttle、Steering、Brake、Handbrake、Clutch、Boost、Pitch、Roll、Yaw、Reverse、ChangeUp、ChangeDown。缺少任一被模块读取的输入名会导致该模块读取失败。
- `SetupInputConfiguration()`：聚合组件级 InputConfig 与 Owner 上全部 SU 组件的 InputConfig（按名称去重），实例化输入生产者，初始化控制/状态输入容器，同步物理线程端输入映射与量化类型；配置未变更时为空操作。
- `AssimilateComponentInputs()`：以组件级 InputConfig 为基准合并 SU 配置。
- `AddInput()`：运行时追加输入配置并重建。
- `ProduceInput()` 现在填充：`PhysicsInputs.NetworkInputs.VehicleInputs.Container`、`StateInputs.StateInputContainer`、`bIsLocallyControlled`、`KeepAwake`、`CurrentTimeDilation`、碰撞通道/射线参数/射线类型。
- 蓝图输入 API：`SetInputBool`、`SetInputInteger`、`SetInputAxis1D/2D/3D`；C++ 重载 `SetInput`/`SetState`；`SetGearInput`（与缓存挡位比较后写入 ChangeUp/ChangeDown）。
- 输入生产者修复：Random 生产者改用实例成员随机流（原为函数局部 static，多载具共享）；Playback 生产者使用局部固定种子流保证回放确定性。

### 阶段 2 输出与视觉反馈
- `ParallelUpdate()`：修正插值分支（下一帧无同 GUID 输出时保持当前帧数据，原实现会留下未初始化的输出）；新增挡位缓存（`CurrentGear`）、引擎状态缓存（`EngineRPM`/`EngineTorque`）；末尾调用 `UpdateNonSkeletalAnimations()`。
- `UpdateNonSkeletalAnimations()`：按 GUID 定位模块对应可视化组件，应用旋转偏移与位置偏移（`TeleportPhysics`），实现悬挂压缩与车轮滚动的非骨骼表现。
- `PostUpdate()`（原整体被注释）：广播模块新增事件；按 `bPendingReplicationStructureRebuild` 标志为网络状态历史全部帧重建复制结构。
- 模块生命周期接通：`OnCreatePhysicsState` 自订阅 native 事件，`OnSimulationModuleInitialized` 分发 `OnAdded()` 并写入真实树索引，`OnSimulationModuleRemovedCallback` 分发 `OnRemoved()`；`RemoveSimulationModule` 广播移除事件并清空 SU 的 Guid/TreeIndex。
- 动画实例新增 `NativeUpdateAnimation` 重同步：模块数量或骨骼名变化时重建模块实例列表（运行时变形会重建模块集合）。
- `RemoveSimulationModule` 删除动画条目后重排 `AnimationSetupIndex`，保持模块与动画条目索引一致。

### 阶段 3 旧调度体系退役
- 删除 `FSingularisMorphVehicleSimModuleManager`（模块 Startup 不再为每个物理场景创建第二套异步回调与 PreTick/PostTick）。
- `FSingularisMorphSimModuleOutputRecord` 迁移至 `SingularisMorphSimModuleManagerAsyncCallback.h/.cpp`（实现逐条等价迁移）。
- 模块 `FSingularisMorphVehicleModule` 简化为：注册 `AHUD::OnShowDebugInfo` HUD 调试钩子。
- 删除死代码：`SingularisMorphVehicleSimCollection`、`SingularisMorphVehicleAsset`（GeometryCollection 资产链路空壳）、`SingularisMorphVehicleDebug.h`（硬编码 `D:\Server.txt`/`D:\Client.txt` 写入）、`SingularisMorphVehicleDefaultAsyncInput.h`（声明了 override 但无实现，引用即链接失败）、`SingularisMorphVehicleSchedulerComponent`（与 SimulationComponent 自注册重复）。

### 阶段 4 网络物理预测集成
- 构造函数在 `Chaos::FPhysicsSolverBase::IsNetworkPhysicsPredictionEnabled()` 为真时创建 `UNetworkPhysicsComponent` 子对象。
- `OnCreatePhysicsState`：`FScopedModuleInputInitializer` + `CreateDataHistory<FPhysicsSingularisMorphVehicleTraits>()`，本地控制时 `SetIsRelayingLocalInputs(true)`。
- `OnDestroyPhysicsState`：`RemoveDataHistory()`。
- 模块增删后（`bPendingReplicationStructureRebuild`）为 StateHistory 全部历史帧重跑 `GenerateReplicationStructure`。
- `SetLocallyControlled`/`IsLocallyControlled`：网络预测开启时以 NetworkPhysicsComponent 为唯一权威，否则回退到 Owner Pawn 控制器判断。

### 阶段 5 模块类型与拓扑
- 新增 `USingularisMotorSUComponent`（`Chaos::FMotorSimModule`，参数 MaxRPM/MaxTorque/EngineInertia，扭矩按 `TorqueMToCm` 换算）与 `USingularisAxleSUComponent`（`Chaos::FAxleSimModule`，参数 AxleInertia，含 `LinkedTransmission`）。
- `USingularisWheelSUComponent` 补齐 `FWheelSettings` 全部字段：MaxRotationVel、LateralSlipGraphMultiplier、LateralSlipGraph（`TArray<FVector2D>` → `Chaos::FGraph`）、SlipModifier、AutoHandbrakeEnabled、AutoHandbrakeVelocityThreshold；新增 `LinkedAxle`。
- `USingularisClutchSUComponent` 的 `LinkedTransmission` AllowedClasses 修正为真实类名。
- `RebuildFromSnapshot` 重构：
  - SU 来源统一为"快照实体映射结果 + Owner 上其余 SU"，避免动力链模块（未入簇）被静默丢弃；
  - 空快照：无既有模块时无操作（幂等），有既有模块时清除全部模块（载具解体）；
  - 无 Chassis 时清除全部模块；
  - Pass 顺序：Chassis → Engine/Clutch/Transmission → Axle → Wheel → Suspension（挂配对车轮之下）→ 其余；
  - 悬挂配对优先级：同一物理组件上的车轮 → 车轮 `LinkedSuspension` 显式引用 → Chassis（并告警）；
- `FSingularisMorphVehicleBuilder::FixupTreeLinks` 重写：遍历全部树槽位；先重置全部悬挂/车轮交叉索引；再按父子关系建立双向链接；扭矩链修复（车轮祖先链无轮轴/变速箱时挂到首个扭矩源，防环校验）；轮轴链修复（挂到首个变速箱）。删除无调用者的 `GenerateSimTree`。
- 状态查询 API：`GetCurrentGear`、`IsReversing`、`GetEngineRPM`、`GetEngineTorque`、`GetVehicleSpeed`（cm/s → km/h，系数 0.036）。

### 阶段 6 调试与日志
- `ShowDebugInfo` 实现：控制输入实时值、挡位/RPM/扭矩/车速摘要、逐模块输出（`ToString`，非 Shipping 构型）；`showdebug SingularisMorphVehicle` 遍历当前 World 全部载具。
- 每帧刷屏日志降为 Verbose（SimulationComponent、SimulationCU 树转储、ClusterUnionAdapter）。
- 无输入消费者误报的悬挂链接告警降为 Verbose + 悬挂配对失败告警。

### 阶段 7 一般级修复
- GT 侧不再遍历物理线程拥有的模拟树：新增 `GuidToCoreModule` 映射（创建时登记、移除时摘除、销毁时清空），`RemoveSimulationModule` 只查映射表。
- `DestroyVehicleSimulation`：未提交模块在 GT 终止并释放；已提交模块只调用终止（对象归模拟树所有）；PT 侧对象销毁推迟到 `Solver->EnqueueCommandImmediate`，Solver 不可用时告警并在 GT 兜底。
- `PreTickGT`：缺失 Owner/映射子系统时不消费适配器脏标记，留待下一帧重试。
- SchedulerSubsystem：`BindEvent`/`UnbindEvent`/`InjectInputs`/`OnPhysScenePreTick`/`ParallelUpdateVehicles` 全链路空指针守卫；`UnbindEvent` 新增 `InjectInputsExternal.RemoveAll` 与 `AsyncCallback` 无条件置空；删除死代码（SubStepCount、未使用的回调声明、注释掉的委托、bGInitialized）。
- `GenerateNewGuid` 改用 `std::atomic` 计数器。
- 删除 `SetUpdatedComponent`（空实现）及其在 Pawn 构造中的调用；删除空 `TickComponent`/`BeginPlay` 重写并关闭组件 Tick。
- 删除 `USingularisMorphVehicleClusterUnionComponent` 中注释掉的子组件 Socket 聚合死代码。
- `FSingularisMorphVehicleSocket` 删除冗余构造函数（字段已有类内默认值）。
- `USingularisMorphVehicleSuspensionSimModule` 补充模块导出宏。
- 死亡成员清理：`NextTransformIndex`、`bSimTreeBuilt`、`TypeToTreeIndex`、`ModuleData`、动画实例空构造函数。

## 2. 文件变更清单

### 新增（4）
- `Source/.../Public/Components/SingularisMotorSUComponent.h`
- `Source/.../Private/Components/SingularisMotorSUComponent.cpp`
- `Source/.../Public/Components/SingularisAxleSUComponent.h`
- `Source/.../Private/Components/SingularisAxleSUComponent.cpp`

### 删除（10）
- `Public/Core/SingularisMorphVehicleSimModuleManager.h`、`Private/Core/SingularisMorphVehicleSimModuleManager.cpp`
- `Public/Core/SingularisMorphVehicleSimCollection.h`、`Private/Core/SingularisMorphVehicleSimCollection.cpp`
- `Public/Core/SingularisMorphVehicleAsset.h`、`Private/Core/SingularisMorphVehicleAsset.cpp`
- `Public/Core/SingularisMorphVehicleDebug.h`
- `Public/Types/SingularisMorphVehicleDefaultAsyncInput.h`
- `Public/Components/SingularisMorphVehicleSchedulerComponent.h`、`Private/Components/SingularisMorphVehicleSchedulerComponent.cpp`

### 修改（26）
| 文件 | 修改内容 |
|---|---|
| `Public/Components/SingularisMorphVehicleSimulationComponent.h` | 新增输入属性/API/状态 getter/映射表/缓存成员；删除死方法 |
| `Private/Components/SingularisMorphVehicleSimulationComponent.cpp` | 输入管线、输出分发、网络集成、拓扑重构、销毁路径、调试显示等主体改造 |
| `Public/Subsystems/SingularisMorphVehicleSchedulerSubsystem.h` | 删除旧管理器 include 与死成员；新增载具列表只读访问器 |
| `Private/Subsystems/SingularisMorphVehicleSchedulerSubsystem.cpp` | 空指针守卫、对称解绑、死代码清理 |
| `Public/Types/SingularisMorphSimModuleManagerAsyncCallback.h` | 迁入 `FSingularisMorphSimModuleOutputRecord` |
| `Private/Types/SingularisMorphSimModuleManagerAsyncCallback.cpp` | 迁入 OutputRecord 实现（逐条等价） |
| `Public/Core/SingularisMorphVehicleBuilder.h` | 删除 `GenerateSimTree` 声明 |
| `Private/Core/SingularisMorphVehicleBuilder.cpp` | `FixupTreeLinks` 重写（全槽位、交叉链接重置、扭矩链修复、防环） |
| `Public/SingularisMorphVehicle.h` / `Private/SingularisMorphVehicle.cpp` | 模块简化为 HUD 调试钩子注册 |
| `Private/Core/SingularisMorphVehicleSimulationCU.cpp` | 索引访问改 `Cast` + 范围校验；诊断日志降级；删除注释掉的调试写入 |
| `Public/Core/SingularisMorphVehicleSuspensionSimModule.h` | 补充模块导出宏 |
| `Public/Animations/SingularisMorphVehicleAnimationInstance.h` / `Private/...cpp` | 新增重同步逻辑与代理访问器；删除死成员与空构造函数 |
| `Public/Types/SingularisMorphVehicleInputProducer.h` / `Private/...cpp` | 随机流实例化；删除死变量 |
| `Public/Components/SingularisWheelSUComponent.h` / `Private/...cpp` | 补齐 6 项缺失属性；新增 `LinkedAxle` |
| `Public/Components/SingularisClutchSUComponent.h` | AllowedClasses 修正 |
| `Public/Types/SingularisMorphVehicleSimComponentsInclude.h` | 补充 Axle/Motor/Thruster include |
| `Private/Components/SingularisMorphVehicleClusterUnionComponent.cpp` | 删除注释掉的死代码，Socket 查询路径整理 |
| `Private/Objects/SingularisMorphVehicleClusterUnionAdapter.cpp` | 日志降级 |
| `Private/Actors/SingularisMorphVehiclePawn.cpp` | 删除对已移除空方法的调用 |
| `Public/Types/SingularisMorphVehicleSocket.h` / `Private/...cpp` | 删除冗余构造函数 |
| `Public/Components/SingularisMorphVehicleSUComponent.h` | `TransformOffset` 注释与实现对齐 |

## 3. 审查记录

| 轮次 | 方式 | 结果 |
|---|---|---|
| 1 | 2 个独立代理（现状诊断 + 参考实现对照） | 认定原完成度约 60%；定位三大断裂主干（输入、视觉反馈、网络集成）与 28 项缺口 |
| 2 | 2 个独立代理交叉复审 | 阻断 2 项（未声明状态成员、输入生产者随机流）、严重 4 项（PT 对象跨线程释放、GT 读 PT 树、悬挂配对拓扑、空快照契约）、一般/建议多项；全部修复 |
| 3 | 代理复审（部分取消）+ 自查 | 修复 1 项编译阻断（重复成员声明）、2 项高危（析构泄漏路径、悬挂父索引越界）、若干中低项；完成机械校验 |

机械校验项（全部通过）：相邻重复行扫描、代码括号平衡、头文件与实现双向对应（SimulationComponent 47 项定义、SchedulerSubsystem 14 项、AnimationInstance）、已删除类的残留引用、Log 级日志残留。

## 4. 验证状态

- 未执行编译与运行验证（环境无 LSP/构建链，且按要求不编译验证）。
- 已执行静态验证：与本仓库内 `ChaosVehicles` 头文件逐项核对 API 签名；与 `ChaosModularVehicle` 参考实现对照关键流程；机械化语法校验（括号/重复行/声明对应）。
- 未验证项：物理线程并发行为、网络物理预测（Rewind/NetToken/增量序列化）双端表现、实际驾驶手感、动画表现。

## 5. 遗留风险与待办

1. **编译验证**（最高优先）：UHT 与编译未运行，首次编译可能暴露头文件包含顺序、UHT 宏约束等静态检查无法覆盖的问题。
2. **网络预测路径实机验证**：DataHistory 注册、`SetIsRelayingLocalInputs` 时序、历史帧复制结构重建、Delta 序列化均需双端 PIE 验证。
3. **拓扑依赖编辑器配置**：`DrivenComponent`、`LinkedSuspension`、`LinkedAxle`、`LinkedTransmission` 决定模块拓扑与配对；仓库内无默认资产，功能可用性取决于配置正确性（悬挂配对失败与无 Chassis 场景已有告警）。
4. **输入容器重建的 1 帧输入丢失**：输入配置变更（运行时增删模块）时输入容器重新初始化，当帧已缓冲输入被清零；配置未变更时不触发。
5. **动画重同步为启发式**：以"模块数量 + 骨骼名"判断是否需要重建模块实例列表；骨骼名重复或仅顺序变化的场景可能漏检。
6. **Epic 原版一致性遗留（未改动）**：
   - Spherecast 模式下 `Offset` 与 `HitPoint` 的半径口径与 Raycast 分支不一致（默认使用 Raycast 不触发）；
   - `FrictionOverride` 默认 1.0 且在非 `CHAOS_DEBUG_DRAW` 构型下 CVar 不注册，导致物理材质摩擦路径不生效。
   两项均与原版行为一致，建议实机 A/B 后再决定是否偏离。
7. **集群联合子组件 Socket 聚合未实现**：Socket 查询仅覆盖组件自身的 `Sockets` 数组（原实现为注释掉的死代码，本次清理而非实现）。
8. **传统 RPC 复制路径未实现**（按既定决策只走 NetworkPhysics 预测路径）：组件仍 `SetIsReplicatedByDefault(true)`，非预测联机场景下远端输入不会同步。
9. **未提交模块的销毁路径**：`DestroyVehicleSimulation` 对仍在 `StoredTreeUpdates` 中的模块执行"终止 + delete"，依赖 `AddModuleToTree` 的登记不变量；若未来新增绕过该函数的模块创建路径，需同步维护。
