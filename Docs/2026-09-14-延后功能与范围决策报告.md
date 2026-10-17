# SingularisMorphVehicle 延后功能与范围决策报告

日期：2026-09-14
背景：本次开发将 SingularisMorphVehicle 补全至生产就绪状态。本报告记录本次明确不实现或延后的事项及依据，供后续实现时快速预览。

## 1. SimCollection 资产链路

### 定义
数据资产驱动的载具编排工作流，与现有组件驱动工作流相对：

- 组件驱动（现有架构，运行时主路径）：在 Actor/Pawn 上挂 SU 组件，PhysicsAdapter 按物理粒子快照自动构建模拟树。
- 资产驱动（SimCollection 路线）：将载具建模为 GeometryCollection，在数据资产中为每个几何元素标注模块类型，由 GenerateSimTree() 从资产生成模拟树。

### 当前状态
死代码。具体事实：

- `SingularisMorphVehicleSimCollection.cpp` 中 `Init()`/`Construct()`/`GenerateSimTree()` 均为空实现。
- `SimModuleIndex` TManagedArray 从未注册进 TransformGroup。
- `FSingularisMorphVehicleAssetEdit` 析构函数为空实现。
- 全项目无运行时代码依赖此链路。

### 本次处理
删除空壳代码。

### 后续实现方案
- 触发条件：需要载具预设资产或工具管线批量编排时实现。
- 实现路径：基于现有组件+快照架构设计，将资产导出为 `FSingularisMorphVehiclePhysicsAdapterSnapshot`，调用现有 `RebuildFromSnapshot()` 构建模拟树。
- 不采用原版 ChaosModularVehicle 的 GeometryCollection 方案。

## 2. Propeller（螺旋桨）

### 定义
螺旋桨推进模块。ChaosVehicles 底层无现成模块，需自定义实现。

### 缺失影响
- 不影响系统可用性。
- 螺旋桨飞机可用 Thruster（推力）+ Aerofoil（升力/舵面）近似构建。
- 缺失项：与转速联动的推力曲线、桨扭矩反作用等拟真细节。

### 后续实现方案
- 遵循仓库内 `FSingularisMorphVehicleSuspensionSimModule` 的四件套模板：SimModule + ModuleNetData + OutputData + 自动注册 Factory（`TSimFactoryAutoRegister`）。
- 枚举位已保留（`ESingularisMorphVehicleModuleType::Propeller`），加入时不破坏 API。

## 3. Balloon（气球/浮力）

### 定义
浮力模块。ChaosVehicles 底层无现成模块，需自定义实现。

### 缺失影响
- 无替代方案。缺失则无法构建飞艇类载具。
- 若玩法包含"变形为飞艇"，则为功能缺口。

### 后续实现方案
- 同 Propeller 四件套模板。
- 优先级：Balloon > Propeller（若飞艇在玩法规划内）。

## 4. 网络复制路径决策

- 只实现 NetworkPhysics 预测路径（`UNetworkPhysicsComponent` + `CreateDataHistory` + Iris NetToken）。
- 不实现传统 RPC 复制路径（ReplicatedState + ServerUpdateState）。

## 5. 本次执行范围（阶段 1-7）

1. 打通输入管线：InputProducer 接入、ProduceInput 填充 VehicleInputs.Container、SetLocallyControlled、ClearConsumedInputs。
2. 恢复输出与视觉反馈：PostUpdate 恢复、模块增删事件广播、非骨骼动画（悬挂压缩/车轮滚动驱动 StaticMesh）、输出插值。
3. 退役旧调度体系：删除 FSingularisMorphVehicleSimModuleManager 注册，调度统一走 SchedulerSubsystem。
4. 网络物理预测集成：UNetworkPhysicsComponent、CreateDataHistory、SetIsRelayingLocalInputs、GenerateReplicationStructure 历史帧补齐、ConstructionDatas 构造顺序复制。
5. 模块类型补全：Motor/Axle SU 组件（ChaosVehicles 有现成 FMotorSimModule/FAxleSimModule）、Wheel 属性补全（LateralSlipGraph/SlipModifier/AutoHandbrake/MaxRotationVel 等）、引擎状态查询 API（RPM/挡位/车速）。
6. 调试体系与日志治理：ShowDebugInfo HUD 输出、PT 侧 FDebugDrawQueue 绘制 + CVar 开关、每帧日志降 Verbose、硬编码路径清理。
7. 一般级修复 + 多轮审查：映射缓存泄漏、随机流隔离、空指针防护、FixupTreeLinks 多变速箱问题等。
