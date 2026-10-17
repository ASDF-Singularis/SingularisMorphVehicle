# ChaosVehicles 模拟树单刚体动力链机制分析

> [!IMPORTANT]
> 在街机路线的**单刚体**（Single Rigid Body）模式下，`ChaosVehicles` 的**模拟树**（Simulation Tree，`FSimModuleTree`）对任何模块类型均无数量约束：底盘唯一是语义约定，**引擎**（Engine）、**变速箱**（Transmission）、**离合器**（Clutch）、**轮轴**（Axle）在结构上均可存在多个实例，动力链连接关系完全由树的**父子边**（Parent-Child Edge）定义，且一棵模拟树对应且仅对应一个根粒子，即一辆载具。

---

## 一、问题定义

`ChaosVehicles` 的**模块化体系**使用**模拟树**（`FSimModuleTree`）组织载具的模拟单元。在街机路线下，载具被视作单一刚体进行模拟，此时需要确认模拟树对**动力链**（Powertrain）各组成模块的数量约束机制。候选机制有三：

1. 载具只能有一个底盘、一个变速箱、一个离合、一个轮轴、一个引擎；
2. 载具只有一个底盘，可以有多个变速箱、多个离合、多个轮轴、多个引擎，通过显式配置 `引擎-变速箱-轮轴-多个轮胎` 的链路，在一个单刚体载具上支持多套动力；
3. 载具可以有多个底盘及多套动力总成，一棵模拟树能模拟多个载具。

## 二、结论

实际机制为候选 2，且比候选 2 的表述更宽松：

- 模拟树对**任何模块类型**均无数量约束，不存在"引擎只能有一个"的结构限制或断言；
- "底盘唯一"是语义约定而非结构强制——底盘对应单刚体本身，其数量约束来源于 `Simulate()` 的入口签名（单一**根粒子**），而非树结构；
- 一棵模拟树对应一个根粒子（`Chaos::FPBDRigidParticleHandle`），即一棵树等价于一辆载具，不存在一棵树模拟多个独立载具的机制。

## 三、机制证据

### 1. 树结构层面：无单例约束

`SimModuleTree.h` 与 `SimModuleTree.cpp` 中的关键事实：

- 节点为通用结构 `FSimModuleNode { SimModule, Parent, TSet<int> Children }`，类型系统上不存在针对特定模块类型的实例数限制，源码中亦无 `check(NumEngines == 1)` 类断言；
- `AddRoot()` 可被多次调用，`GetRootNodes()` 返回数组并遍历收集所有 `Parent == INVALID_IDX` 的节点，源码注释明确说明：

```cpp
// never assume the root bone is always index 0
for (int i = 0; i < SimulationModuleTree.Num(); i++)
```

- `Simulate()` 对所有根节点逐一执行 `SimulateNode()`；
- 唯一具有单例色彩的是 `LocateNodeByType<T>()`，其返回第一个类型匹配的节点，但该函数仅为查询便利接口，不构成结构限制。

### 2. 扭矩传递：由父子边驱动

`TorqueSimModule.cpp` 中的 `FTorqueSimModule::TransmitTorque()` 是动力系统的核心机制：

```cpp
Interface->SetDriveTorque(PushedTorque * GearingRatio * ClutchSlip / Children.Num());
```

其行为为：

- **驱动扭矩**（Drive Torque）下推至当前节点自己的**子节点**（Child Node），并按子节点数量均分（等效于硬编码的**开放式差速器**，Open Differential）；
- **制动扭矩**（Braking Torque）由子节点向上求和回传至父节点；
- **转速耦合**（Angular Velocity Coupling）：子节点平均转速反馈至当前节点（差速器行为），并按**离合打滑系数**（Clutch Slip）加权。

每个引擎模块的 `Simulate()` 仅调用 `TransmitTorque` 将扭矩推给自己的子节点。因此 `引擎 → (离合) → 变速箱(齿比) → 轮轴 → 多个轮子` 这条链完全由树的父子边定义，两套独立的 `引擎→变速箱→轮轴→轮` 子树之间不存在互相干扰，多套动力链在机制层面天然支持。

### 3. 单刚体边界：单根粒子

```cpp
void FSimModuleTree::Simulate(float DeltaTime, FAllInputs& Inputs,
    IPhysicsProxyBase* PhysicsProxy, Chaos::FPBDRigidParticleHandle* RootParticle)
```

`Simulate()` 仅接收一个 `RootParticle`：

- `UpdateVehicleState(RootParticle)` 从该单一粒子读取位姿与速度；
- 所有模块产出的力进入 `FDeferredForcesModular`，最终应用到这一个**物理代理**（Physics Proxy）与根粒子。

因此一棵树等价于一个刚体、一辆载具。模拟树支持的**集群联合**（Cluster Union）是"多个粒子聚合为一个刚体求解"的机制，仍属单载具语义，不构成多载具模拟。

## 四、动力链各模块职能

| 模块 | 职能 |
|---|---|
| `FChassisSimModule` | 仅施加**拖拽力**（Drag Force）与**角阻尼**（Angular Damping），通过 `AddLocalForce` / `AddLocalTorque` 写入延迟力容器；该模块并非必需，可以不放置 |
| `FClutchSimModule` | 动力链上的插入节点，通过 `GetParent()` / `GetFirstChild()` 直接获取上下游模块，对两侧转速差施加 0.1 系数的耦合（`ClutchModule.cpp`），离合值由命名输入计算 |
| `FTransmissionSimModule` | 承载挡位状态机与齿比，调用 `TransmitTorque` 时以 `GearRatio` 作为传动比下推扭矩 |
| `FAxleSimModule` | 纯扭矩中继：调用 `TransmitTorque` 后按 `AxleInertia` 自积分 |
| `FWheelSimModule` / `FWheelBaseInterface` | 通过**显式树索引** `SuspensionSimTreeIndex` 跨子树引用悬挂模块，表明模块间连接本身即为显式配置风格 |
| `FMotorSimModule` | 简化的单模块动力源（无变速箱、无挡位），是构建动力链的另一种粒度选项 |

## 五、候选机制排除分析

**候选 1 不成立**：树结构与扭矩传递机制均不依赖任何模块的唯一性。多个引擎各自向自己的子树传递扭矩，`TransmitTorque` 的实现仅遍历调用者自身的 `Children` 集合，与其它动力子树无交集。

**候选 3 不成立**：`Simulate()` 的签名限定单一 `RootParticle` 与单一 `PhysicsProxy`，`UpdateVehicleState` 与 `FDeferredForcesModular` 的应用目标均锚定于该粒子。多根节点（`GetRootNodes` 返回数组）支持的是同一载具内多个顶层模块（例如底盘子树与独立动力子树并列），而非多个载具。

## 六、多动力链的机制限制

在单刚体载具上配置多套动力链时，存在以下源码级限制：

1. **差速均分硬编码**：扭矩按 `/ Children.Num()` 均分，无分配比例配置。源码 TODO 原文："We are performing equal torque splitting here is that correct/desired?"。实现**偏置式限滑差速**（Biased Limited-Slip Differential）需要扩展 `TransmitTorque` 或在上层适配。
2. **同类型模块不能串接**：`TransmitTorque` 跳过与自身同 `SimType` 的子节点，源码注释："hierarchy is 1 deep at present"。即两个变速箱串联时扭矩流会中断——一条动力链上每种模块类型只能存在一层。
3. **输入按名称全树共享**：所有引擎读取同一个**命名输入**（Named Input）`ThrottleControlName`，多个变速箱会响应同一个换挡输入。多动力链需要独立控制时，须使用 `FInputInterface` / `FModuleInputContainer` 提供的自定义命名输入机制（该容器支持任意命名的输入条目）。
4. **离合仅作用于第一个子节点**：`FClutchSimModule` 通过 `GetFirstChild()` 获取下游，链的形态假设为线性链路。

## 七、源码索引

| 主题 | 文件 | 关键符号 |
|---|---|---|
| 模拟树结构 | `ChaosVehiclesCore/Public/SimModule/SimModuleTree.h` | `FSimModuleTree`、`FSimModuleNode`、`AddRoot`、`GetRootNodes`、`LocateNodeByType` |
| 树遍历与模拟 | `ChaosVehiclesCore/Private/SimModule/SimModuleTree.cpp` | `Simulate`、`SimulateNode`、`UpdateVehicleState`、`UpdateModuleVelocites` |
| 扭矩传递 | `ChaosVehiclesCore/Private/SimModule/TorqueSimModule.cpp` | `TransmitTorque`、`IntegrateAngularVelocity` |
| 引擎 | `ChaosVehiclesCore/Private/SimModule/EngineModule.cpp` | `FEngineSimModule::Simulate` |
| 变速箱 | `ChaosVehiclesCore/Private/SimModule/TransmissionModule.cpp` | `FTransmissionSimModule::Simulate`、`GetGearRatio` |
| 离合器 | `ChaosVehiclesCore/Private/SimModule/ClutchModule.cpp` | `FClutchSimModule::Simulate` |
| 轮轴 | `ChaosVehiclesCore/Private/SimModule/AxleModule.cpp` | `FAxleSimModule::Simulate` |
| 底盘 | `ChaosVehiclesCore/Private/SimModule/ChassisModule.cpp` | `FChassisSimModule::Simulate` |
| 输入系统 | `ChaosVehiclesCore/Public/SimModule/ModuleInput.h` | `FInputInterface`、`FModuleInputContainer`、`FModuleInputSetup` |
| 模块间通信 | `ChaosVehiclesCore/Public/SimModule/VehicleBlackboard.h` | `FVehicleBlackboard` |
