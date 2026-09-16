# SingularisMorphVehicle 编译修复与决策落地报告

日期：2026-09-16
背景：首次编译暴露的错误修复，以及上一轮审查提出的待拍板事项的落地结果。本报告记录本轮全部变更、明确不改动的项与待验证清单。

## 1. 编译错误修复（7 项）

| 位置 | 错误 | 根因 | 修复 |
| --- | --- | --- | --- |
| `Private/Components/SingularisAxleSUComponent.cpp:23` | C2259 无法实例化抽象类 `Chaos::FAxleSimModule`（2 条） | 上游未覆写 `GenerateNetData` | 改为 `FSingularisMorphAxleSimModule` |
| `Private/Components/SingularisMotorSUComponent.cpp:26` | C2259 无法实例化抽象类 `Chaos::FMotorSimModule`（2 条） | 同上 | 改为 `FSingularisMorphMotorSimModule` |
| `Private/Components/SingularisMorphVehicleSimulationComponent.cpp:192` | C2440 转换丢失限定符 | `TMap::Find` 结果绑定的指针层级带 const，解引用后无法转为非 const | `FoundModule` 声明为 `Chaos::ISimulationModuleBase**`，与 `TMap` 非 const `Find` 的返回类型精确匹配 |
| `Private/Components/SingularisMorphVehicleSimulationComponent.cpp:852` | C2662 无法将 `this` 从 const 转为非 const | 在 `const FSimOutputData*` 上调用非 const 的 `MakeNewData()` | `CurrentSimData` 去掉 const（输出容器本身为 const，元素是非 const 指针） |
| `Private/Components/SingularisMorphVehicleSimulationComponent.cpp:1514` | C2440 无法从 int 转换为 `<unnamed-enum-INDEX_NONE>` | `auto` 推导到了引擎中 `INDEX_NONE` 的匿名枚举类型 | 显式声明 `int32 SetupIndex` |
| `Public/Components/SingularisMorphVehicleSimulationComponent.h:250` | UHT：找不到名为 `FOnSimulationModuleRemovedEvent` 的类型 | 把委托成员名当成了类型名 | 类型改为第 66 行声明的 `FOnSimulationModuleRemoved` |

## 2. 上游扭矩模块抽象化的处理

`ChaosVehicles` 的 `FAxleSimModule` 与 `FMotorSimModule` 未覆写 `ISimulationModuleBase::GenerateNetData`，因此在引擎侧即为抽象类；两者的 `Simulate()` 实现完整（轮轴扭矩传递、电机转速平方扭矩曲线），上游自身也从未实例化过这两个类。

新增 `Public/Core/SingularisMorphVehicleTorqueSimModules.h`，按 `FAerofoilSimModule` 的既有做法把 `GenerateNetData` 补为返回空网络数据（`FSimModuleTree::GenerateReplicationStructure` 声明 `nullptr is a valid response`）。

不引入新的模拟类型名，`GetSimType()` 仍为上游的 `"FAxleSimModule"` / `"FMotorSimModule"`。这一点是硬约束，依据如下：

- `FTorqueSimModule::TransmitTorque` 以 `GetSimType()` 相等判定跳过同类子节点。
- `FSingularisMorphVehicleBuilder::FixupTreeLinks` 以 `IsSimType<FAxleSimModule>()` 识别轮轴并修复扭矩链。
- 网络复制的类型哈希来自 `FModuleFactoryRegister::GetModuleHash(GetSimType())`，改名会使读写两侧的哈希键失配。

本报告同时更正 `2026-09-14-Deferred-Features-Report.md` 第 5 节中「ChaosVehicles 有现成 FMotorSimModule/FAxleSimModule」的表述：两个类在上游存在，但为不可实例化的死代码，需按上述方式补全后方可使用。

## 3. 集群 Actor 适配器装配缺失

`ASingularisMorphVehicleClusterActor` 构造函数缺少集群适配器装配的 4、5 步：未创建 `USingularisMorphVehicleClusterUnionAdapter`，也未赋给 `SimulationComponent->PhysicsAdapter`。其同级类 `ASingularisMorphVehicleClusterPawn` 具备这两步。缺失导致该类（及其蓝图子类）的 `PhysicsAdapter` 恒为空，进入手动模式，模拟树永不构建，`BeginPlay` 中 `AddOwnedComponentsToCluster()` 加入集群的子件不会被任何模块消费。

已按 `ClusterPawn` 补齐：创建适配器、配置 `ClusterUnionComponentReference`、赋给 `PhysicsAdapter`。

## 4. 决策落地

### 4.1 Motor 作为原动机接入动力链

`RebuildFromSnapshot` 的 Pass 6 原先把 `Motor` 挂到 `Chassis` 之下并作为叶节点，而扭矩传递是父推子（`FTorqueSimModule::TransmitTorque` 向子节点的 `SetDriveTorque` 推送），叶节点的驱动扭矩无处传递；且 `FixupTreeLinks::IsTorqueSourceModule` 只识别 `Transmission` 与 `Axle`，挂在 Motor 之下的车轮会被重挂走。因此纯电载具在适配器自动路径下输出零扭矩。

处理：`USingularisMotorSUComponent` 增加与 `USingularisEngineSUComponent` 同形的 `LinkedClutch` 引用；`RebuildFromSnapshot` 的 Pass 2 改为遍历「原动机（引擎/电机）→ 离合器 → 变速箱」动力链，`FirstEngineIndex` 相应更名为 `FirstPrimeMoverIndex`，Pass 6 不再接管 Motor。离合器与变速箱的兜底接入（Pass 2b）同样以首个原动机为父节点。

引擎与电机共用同一接线规则，可共存构成混动。

### 4.2 删除非集群路径

保留集群联合路径，删除非集群路径的四个 Actor/Pawn 与其物理适配器：

- `Public/Actors/SingularisMorphVehicleActor.h` 与 `Private/Actors/SingularisMorphVehicleActor.cpp`
- `Public/Actors/SingularisMorphVehiclePawn.h` 与 `Private/Actors/SingularisMorphVehiclePawn.cpp`
- `Public/Actors/SingularisMorphVehicleSkeletalActor.h` 与 `Private/Actors/SingularisMorphVehicleSkeletalActor.cpp`
- `Public/Actors/SingularisMorphVehicleSkeletalPawn.h` 与 `Private/Actors/SingularisMorphVehicleSkeletalPawn.cpp`
- `Public/Objects/SingularisMorphVehicleHierarchyAdapter.h` 与 `Private/Objects/SingularisMorphVehicleHierarchyAdapter.cpp`

删除后 `Public/Actors` 仅保留两个集群类，`Public/Objects` 保留抽象基类 `USingularisMorphVehiclePhysicsAdapter` 与 `USingularisMorphVehicleClusterUnionAdapter`。抽象基类作为扩展点保留（`UCLASS(Abstract, NotBlueprintable, EditInlineNew)`），新增物理后端仍可通过继承实现。

非集群的 Actor/Pawn 若仍需使用，可在工程蓝图侧以 `ClusterUnionComponent` + `SimulationComponent` + `ClusterUnionAdapter` 自行组合。

### 4.3 保留项

- `USingularisMorphVehicleClusterUnionComponent` 构造函数中的 `SetIsReplicatedByDefault(false)` 保留：集群子件集合仅由权威端组装，其它端由集群复制事件同步。
- `.uplugin` 的 `EnhancedInput` 插件依赖保留：模块 `Build.cs` 的 `PrivateDependencyModuleNames` 确实包含 `EnhancedInput`。

## 5. 明确不改动的项

- **Axle 与 Motor 的网络状态复制**：两者为占位模块，本轮不实现 `FTorqueSimModuleData` 派生数据与配套工厂，`GenerateNetData` 维持返回 `nullptr`。已知影响：这两个模块的角速度属于积分状态，在重演（resimulate）时不会被回滚，联机下可能产生预测偏差。作为占位模块其功能尚不完整，偏差不影响当前可用性。
- **Socket 的 `RTS_ParentBoneSpace` 分支**：`USingularisMorphVehicleClusterUnionComponent::GetSocketTransform` 对该分支执行 `check(false)`，与 `ChaosModularVehicle` 的 `UClusterUnionVehicleComponent` 逐行同构，保持一致不做改动。仅在外部以 `RTS_ParentBoneSpace` 查询用户显式配置的 Socket 名时触发；当前项目内无代码消费这些 Socket。

## 6. 待验证清单

| 优先级 | 验证项 |
| --- | --- |
| 高 | 编译（本轮修复的验证入口） |
| 高 | 集群 Actor 蓝图子类的装配与模拟（第 3 节修复） |
| 高 | 电机驱动载具能否输出扭矩（第 4.1 节变更） |
| 中 | 集群解体 → 重组：`IsReady()` 不阻塞清空、空快照清除全部模块 |
| 中 | 物理状态重建（`OnDestroyPhysicsState` → `OnCreatePhysicsState`）后悬挂约束仍生效 |
| 中 | 骨骼动画与非骨骼动画回归 |
| 中 | 手动模式（`PhysicsAdapter` 留空 + `AddSimulationModule` + `FinalizeModuleUpdates`） |
| 低 | 车轮 `AxisType` 默认值 `X`（ChaosVehicles 默认）与项目资产期望是否一致 |

## 7. 与本轮无关但需工程侧自查

删除第 4.2 节的类后，若工程侧（`VehicleTour`）存在继承这四个类或引用 `USingularisMorphVehicleHierarchyAdapter` 的蓝图/C++，将出现编译失败或资产加载失败，需一并迁移至集群路径。
