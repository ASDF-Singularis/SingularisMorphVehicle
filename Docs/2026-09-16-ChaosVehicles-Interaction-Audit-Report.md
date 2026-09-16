# SingularisMorphVehicle 与 ChaosVehicles 交互面审查报告

日期：2026-09-16
范围：逐符号对比 `ChaosModularVehicle` 对 `ChaosVehicles` 的全部交互，核查 `SingularisMorphVehicle` 的覆盖完整性，并清理继承自参考实现的遗留缺陷。本报告记录已修复项、经审视判定不改动的项，以及需要拍板的设计级事项。

## 1. 审查方法

1. 抽取参考实现使用的全部 `Chaos::` 符号（74 个）与全部头文件引用（44 项），与项目做差集。
2. 对差集中的每一项定位参考实现的使用点，逐条判定是「能力缺失」「等价替代表达」还是「按既定范围删除」。
3. 对两侧成对文件做逐行比对：模拟层（`SimulationCU`）、异步回调与输入管线、悬挂与车轮模块、集群与物理体映射、树更新与拓扑。
4. 所有结论均以 `文件:行` 取证后判定，不采信未验证的推断。

差集方法与已知局限：差集以 `Chaos::X` 全限定写法匹配，因此带 `using namespace Chaos` 的文件中的非限定用法不会出现在差集中。对差集内 9 个符号逐一以非限定形式复扫后确认：`FChaosPhysicsMaterial`、`FDebugDrawQueue`、`FGeometryParticleHandle`、`FPhysicsObjectInterface`、`FSuspensionConstraint` 在项目中均已使用；`FClusterUnionChildData` 通过 `auto` 隐式使用；`eSimModuleTypeFlags` 原用于悬挂射线的行为标志判定，本次改为更强的类型容器判定后不再使用。

**覆盖结论：项目未缺失任何参考实现具备的 ChaosVehicles 交互能力。** 差集中仅两项属真实差异：`FPendingModuleDeletions`（参考消费 `GetDeletedModules()` 处理删除，项目以自维护的 `GuidToCoreModule` 等效处理，见第 2.3 与 2.4 节的残余缺口修复）与 `FChaosArchive`（SimCollection 资产链路，按既定范围删除）。

## 2. 已修复项

### 2.1 并发与内存安全

| 位置 | 问题 | 修复 |
| --- | --- | --- |
| `SimulationCU.cpp` `ActionTreeUpdates` | 树结构变更使用 `UE::TReadScopeLock`，而游戏线程的 `GenerateReplicationStructure` 同样持读锁遍历同一棵树。读锁之间互不排斥，游戏线程会读到半更新的树（悬垂模块指针、越界索引、重分配中的节点数组） | 改为 `UE::TWriteScopeLock` |
| 同上，上报新增模块处 | `FSimTreeUpdates::AppendTreeUpdates` 先处理新增再处理删除；同一批次内「新增后删除」的模块会在删除阶段被 `delete`（引擎 `SimModuleTree.cpp:99-137`、`:216`），其后读取 `GetNewModules()` 中的裸指针即悬垂访问 | 提交前记录「类型 + GUID」，提交后按 GUID 从树中取真实树索引上报；同批次已删除的模块不上报 |
| `SimulationComponent.cpp` `DestroyVehicleSimulation` | 已被 `RemoveSimulationModule` 终止的模块仍留在待新增列表中，此处的 pending 循环再次调用 `OnTermination_External`，导致悬挂约束对同一句柄二次释放 | 以「映射表是否仍持有该模块」为判据，已终止者只释放对象不重复终止 |
| `SimulationComponent.cpp` `RemoveSimulationModule` | 模块未提交却不在映射中时，终止回调无处可发（悬挂约束泄漏）而删除仍会执行 | 新增 `ensureMsgf`，显式化「模块入树必须经 `AddModuleToTree` 登记」的不变量 |
| `SimModuleManagerAsyncCallback.cpp` `BuildData` | `Cast<USingularisMorphVehicleSimulationComponent>(NetworkComponent)->VehicleSimulationPT` 未校验 Cast 结果；该状态数据被挂到非载具组件时解引用空指针。服务器每次写状态历史都会走到 | 先判 Cast 结果再取 `VehicleSimulationPT` |
| 同上 `ApplyData` | `VehicleSimulation->AccessSimComponentTree()->SetSimState(...)`：`Terminate()` 之后、`Initialize()` 之前的窗口期内模拟树为空，解引用空 `TUniquePtr`。触发点是收到状态包或重演落帧 | 取树后先判 `IsValid()` |
| 同上，工厂缺失路径 | `FModuleFactoryRegister::GenerateNetData` 返回空时：令牌 `Init`、SAVING 两个分支、`GetDeltaDataHelper`、`InterpolateData` 共 6 处未判空（元素空洞）；更严重的是 LOADING 分支在该情形下不消费该模块的位流却继续读取，导致归档错位 | LOADING 缺工厂即整包判失败并返回；`Init`/SAVING/插值对空槽做占位与跳过，保持下标与位流对齐 |
| `SingularisMorphVehicleSuspensionSimModule.cpp` | `DestroyConstraint` 不判句柄有效性、释放后不失效句柄、lambda 形参遮蔽实际释放目标；`UpdateConstraint` 未判 `Solver`；`CreateConstraint` 不幂等且失败静默 | 分别补齐：幂等 + 失效句柄 + 使用形参；`Solver` 空判；幂等 + 失败告警 |

### 2.2 物理正确性

| 位置 | 问题 | 修复 |
| --- | --- | --- |
| `SimulationComponent.cpp` `AddModuleToTree` | 模块的 `ClusteredTransform` 被覆盖为「仅含位移」的变换。引擎在 `SimModuleTree.cpp:504` 用它把粒子角速度换算到模块局部坐标系，清零旋转后 `LocalAngularVelocity` 表达在世界旋转系中 | 移除该覆盖，保留初始旋转 |
| 同上 | 以 `PhysicalTransform.Equals(FTransform::Identity)` 判断「是否提供了物理变换」。子件恰好位于集群原点时其 `ChildToParent` 即为单位变换，会被误判为缺实体而退回组件相对变换 | 改为显式布尔参数 `bHasPhysicalTransform`；`AddSimulationModule` 的公开语义不变 |
| `SimulationCU.cpp` 两处悬挂射线 | 以 `IsBehaviourType(eSimModuleTypeFlags::Raycast)` 作为唯一保护后 `static_cast<FSuspensionBaseInterface*>`。行为标志不构成类型保证 | 改为 `Cast<FSuspensionBaseInterface>()` 类型容器判定 |
| `SimulationCU.h` `FrictionOverride` | 默认值 `1.0f` 使 `if (FrictionOverride > 0)` 恒真，物理材质摩擦读取路径（`GetMaterialFromInternalFaceIndex` → `Material->Friction`）在默认配置下永不执行，`ShowRaycastMaterial` 调试开关同时失去意义 | 默认改为 `0.0f`（不覆盖）；大于 0 时按固定值覆盖路面摩擦。**属行为变更**：默认抓地力由「恒为 1.0」改为「取自命中面物理材质」 |
| `SimulationComponent.cpp` `AddModuleToTree` | 以 `RootPhysicsObject != nullptr` 为前提调用 `OnConstruction_External`（悬挂约束创建点），而手动注册路径的根对象缓存发生在其后的 `FinalizeModuleUpdates`；手动路径首批悬挂模块永远拿不到约束 | 入队时按需 `CacheRootPhysicsObject`，使 `OnConstruction_External` 的调用条件恒真 |
| 同上 `RebuildFromSnapshot` Pass 1 | 底盘类型存在但其模块创建失败（`CreateNewCoreModule` 返回空）时，`ChassisIndex` 为 `INDEX_NONE`，后续节点以无效父索引入树，树退化为多根并打乱叶先序执行序 | 新增守卫：清空全部模拟模块并报错 |
| `ClusterUnionAdapter.cpp` 集群事件 | 只处理 `bIsNew`，丢弃 `BonesData` / `RemovedBoneIDs`：骨骼网格体部分物理体的增删（破碎、骨架撕裂）不触发重建，模块继续指向已消失的粒子 | 任何组成变化都置脏，交由全量重建按当前子粒子收敛 |
| 同上 `Initialize` | 事件只在集群组成变化时触发；若子件在 `Initialize` 之前已加入集群，事件已错过且永远不会重建 | `Initialize` 末尾置一次初始脏标记，保证首帧必然拉取快照完成首次装配 |
| `SimulationComponent.cpp` 实体查找表 | 同一物理组件贡献多个子粒子时按组件映射会静默塌缩为同一实体，该组件上的全部 SU 共用同一粒子索引与簇集变换 | 新增告警使该情形可见（完整修复见第 4.1 节） |

### 2.3 输入与网络

| 位置 | 问题 | 修复 |
| --- | --- | --- |
| `SimulationComponent.cpp` `AssimilateComponentInputs` | 换挡输入为布尔脉冲，但 `FModuleInputSetup` 的 `bClearAfterConsumed` 默认 false。一次游戏帧内物理线程会执行多个子步（异步物理按固定频率推进），`FTransmissionSimModule::Simulate` 的 `ChangeUp()/ChangeDown()` 会在每个子步各触发一次，而 `ChangeUp()` 每次都是目标挡位 +1 且无去抖 → 按一次换挡实际跳多挡 | 在输入配置聚合的单点入口为 ChangeUp/ChangeDown 置 `bClearAfterConsumed`（消费即清零） |
| `SimModuleManagerAsyncCallback.cpp` 减少带宽输入路径 | 数量不一致的「失败分支」先置 `bOutSuccess = false`，随后与自身做一次无增量序列化；该调用按引用写回 `bOutSuccess = true`，失败标记被覆盖，接收端把默认值当作真实输入且不上报失败 | 在该调用之后恢复失败标记 |

### 2.4 清理

- 移除 `AddModuleToTree` 中的无意义自赋值 `InitialTransform.SetLocation(InitialTransform.GetLocation())`。

## 3. 经审视判定不改动的项

- **`SuspensionRaycastsEnabled` 门控悬挂落点解算**：该 CVar 的文档为 "Enable/Disable Suspension Raycasts"，语义自洽——关闭时落点解算、地面摩擦、地面交互一致地退回（弹簧长度取最大行程，与悬挂 `Simulate` 的兜底分支一致）。因此它不是「仅显示」开关，行为正确，仅在代码处补充注释说明它会改变物理结果。唯一残留代价是关闭时仍执行射线查询（性能），默认开启，无实际影响。
- **`BuildData` 使用带物理线程断言的 `GetSimComponentTree()`**：与参考实现同构；该函数在项目的其他调用点均在物理线程上下文，调用线程不在本工作区可核验范围内，故不改动。
- **`IsDefaultState` / `ApplyDefaultState` 未覆写（每包发送全部模块状态）**：与参考实现一致，属引擎默认语义（默认 `false` 即始终发送）。若要优化带宽需成对覆写这两个虚函数，不能只覆写其一。
- **死数据与死字段**：`FWheelBaseInterface::SuspensionSimTreeIndex`（引擎侧无读取点）、`VehicleInputs.Reverse`（无写入点）。两者在参考实现中同构存在，保留以对齐 ChaosVehicles 接口。
- **`USingularisMorphVehicleSuspensionFactory`**：与参考的 `FSuspensionFactory` 同构，实际生效的是带 `TSimFactoryAutoRegister` 的 `*SimFactory` 版本；保留以维持与参考的结构对应。

## 4. 决策记录（经项目方确认）

### 4.1 骨骼网格体载具的粒子绑定 —— 由专用适配器承担，不在本路径实现

结论：骨骼网格体路径不使用 SU 组件，其「骨骼 ↔ 模块」配置由骨骼网格体适配器自身携带；该适配器尚未实现，属待建功能而非当前缺陷。

因此不向 SU 组件增加粒子/骨骼选择字段。集群联合适配器以物理组件为装配单位，遇到一个组件贡献多个子粒子时无法拆分绑定（该组件上的全部 SU 会共用同一粒子索引与簇集变换）。该用法属骨骼网格体适配器的职责范围，集群联合适配器命中时输出告警（`RebuildFromSnapshot`）并指明需要专用适配器，不静默降级。

骨骼网格体适配器的待实现要点：与集群联合适配器同构的 SPI（`IsReady` / `IsDirty` / `ConsumeSnapshot` / `GetPhysicsProxy` / `GetReferenceTransform`）；快照实体以「物理组件 + 粒子索引」为键；骨骼 ↔ 模块配置保存在适配器自身。

### 4.2 静止位姿的重基准 —— 维持现状

引擎在动画阶段把模块的动画位移写回集群：`Particle->ChildToParent().SetTranslation(RestPos + Movement)`，其中 `RestPos` 取自模块的 `InitialParticleTransform`。项目的全量重建会在每次拓扑变更时重新读取实时 `ChildToParent` 并写入 `InitialParticleTransform`，即把当帧的悬架压缩量当作新的静止基准。

参考实现通过「只在部件加入集群时读一次」规避该问题，但它因此不支持变形。对变型载具而言，重基准本身是符合直觉的（部件的相对位置确实改变了），需要区分的是「变形导致的位移」与「动画导致的位移」。若实测出现静止长度或可视轮位逐次漂移，需要在快照中区分「静止基准变换」与「当前簇集变换」两个字段；当前不修改。

### 4.3 减少带宽输入路径 —— 维持现状

该路径需 `p.ModularVehicle.EnableInputReducedBandwidth=1` 才激活（默认关闭）。两处残留：输入令牌只携带 Types 与 DecayValues，`bInverseInputDecay` / `bClearAfterConsumed` 不随包传递；输入按位置与上一包配对，不校验名称与类型，两端配置顺序不同时会静默沿用错误输入名的旧值。修复需要扩展令牌结构（改变线格式），维持现状；开启该 CVar 前需先补此项。

### 4.4 测试输入缓冲的双时基 —— 已实施

已移除游戏线程侧按物理步号的回放取值与 `StartStep == 0` 哨兵（`USingularisMorphVehiclePlaybackInputProducer::ProduceInput` 不再参与取值，`StartStep` 成员随之删除）：回放索引统一由物理线程按求解器帧号推进，循环回放由物理线程的越界回绕承担。游戏线程的按步索引原本就会被物理线程以同一份缓冲覆盖，属无消费方的取值，移除后行为不变。

### 4.5 `SetClustered(false)` 从未调用 —— 维持现状

模块的 `bClustered` 恒为真，「使用部件自身变换」的分支不可达；悬挂约束也始终建立在集群根物理对象上。项目在部件脱离集群时由适配器事件置脏并在 `PreTickGT` 全量重建收口（窗口最多一帧），参考的增量路径无此收口。若需要支持「部件脱落但保留模块」，需在脱离时对该模块置 `SetClustered(false)` 并重建约束归属粒子。

## 5. 待验证清单

| 优先级 | 验证项 |
| --- | --- |
| 高 | 编译 |
| 高 | 抓地力默认值变更后的手感与地形适应性（第 2.2 节 `FrictionOverride`） |
| 高 | 按一次换挡只跳一挡（第 2.3 节换挡脉冲） |
| 中 | 手动模式首批悬挂是否创建约束（第 2.2 节根物理对象缓存） |
| 中 | 集群拓扑变更帧不崩溃（第 2.1 节树锁与悬垂指针） |
| 中 | 联机状态下状态包收发与重演不崩（第 2.1 节工厂缺失与空树守卫） |
| 中 | 部件脱落/回挂后不出现幽灵模块与约束泄漏 |
| 低 | 骨骼网格体载具是否出现第 4.1 节的告警（预期：使用专用适配器前必然出现） |
| 低 | 回放输入生产者的循环回放仍正常（第 4.4 节，索引现在完全由物理线程推进） |

## 6. 回归诊断与修复（追加）

### 6.1 现象与日志证据

现象：蓝图未作任何改动的情况下，载具逐帧剧烈抖动并向底盘对面方向持续移动。

日志证据：

- `[ClusterUnionAdapter] OnClusterComponentAdded` 每帧对同一批 5 个子件重复触发；
- 随之每帧 `[RebuildFromSnapshot] Removing 9 old modules`：全部模块（含 4 个悬挂）被销毁并重建，物理节点在两段索引区间之间来回轮转（节点数 9↔18，FreeList 复用）。

### 6.2 根因

上一轮为支持「骨骼级组成变化」而移除了适配器事件处理中的 `bIsNew` 过滤（`USingularisMorphVehicleClusterUnionAdapter::OnClusterComponentAdded`）。该过滤在参考实现中存在（`ModularVehicleBaseComponent.cpp:1451`），作用是忽略引擎在物理重同步时对同一批子件的重复上报。移除后每一次重复上报都会置脏，导致逐帧全量重建。

### 6.3 症状机制

每次全量重建都会：

1. 经 `OnTermination_External` 释放全部悬挂约束，新建模块时再 `CreateSuspension` —— 约束的销毁与重建本身产生冲击（抖动）；
2. 以当时的实时 `ChildToParent` 重新写入 `InitialParticleTransform`（静止基准）。引擎在动画阶段会把 `ChildToParent` 写为 `RestPos + Movement`（`SimModuleTree.cpp:318-332`），其中 `Movement` 含当前悬挂压缩量。因此重建会把压缩后的位置当作新的静止位姿，悬挂约束的 `LocalOffset` 逐帧漂移，产生持续单向力（定向移动）。

这与日志中「逐帧重建 + 持续定向力」的现象完全吻合。

### 6.4 修复

| 位置 | 措施 | 理由 |
| --- | --- | --- |
| `SingularisMorphVehicleClusterUnionAdapter.cpp` | 恢复 `bIsNew` 过滤，并在该日志行追加 `IsNew` / `Bones` / `RemovedBones` 字段 | 与参考实现一致；追加字段使下一次运行可直接确认事件的真实性质，无需再依赖推断 |
| `SingularisMorphVehicleSimulationComponent.cpp`（`RebuildFromSnapshot` 5a） | 新增幂等守卫：SU 数量、每个 SU 的驱动组件、其物理粒子索引与模块存在性均一致时直接返回 | 不依赖事件的 `bIsNew` 取值，对「重复上报」这类冗余信号具有结构性免疫；判据任一项异常即回退到全量重建，不会漏掉真实拓扑变更 |
| `SingularisMorphVehicleSimulationComponent.cpp` / `.h` | 回退 `ClusteredTransform` 的旋转覆盖（恢复参考语义）与 `PhysicalTransform` 的显式单位变换判定（恢复原表达式与函数签名） | 两处均属未经运行时验证的行为改动；回归排查期间保持与上次可用版本语义一致，作为待定项记录于下文 |

保留：`FrictionOverride` 默认 `0`（材质摩擦生效）。该字段目前未暴露 CVar，需修改默认值后重新编译才能恢复旧的恒定抓地力；如需运行时对比，可为其补一条 CVar（本轮未做）。

### 6.5 待定项（已回退，需定向验证后再改）

- `ClusteredTransform` 的旋转：引擎在 `SimModuleTree.cpp:504` 以它的逆变换把粒子角速度换算到模块局部坐标系，清零旋转会使 `LocalAngularVelocity` 表达在集群坐标系。该信号的下游消费者未逐一定位，且回退后行为与参考一致，故暂不改。
- `PhysicalTransform` 为单位变换时的语义：以变换值作「未提供」哨兵无法区分「子件恰在集群原点」；修复需引入显式标志（曾一度实现并回退）。仅在实测到该情形导致模块位姿错误时再引入。

### 6.6 下次运行需确认

- `OnClusterComponentAdded` 的 `IsNew/Bones/RemovedBones` 取值，用于判定重复上报是否带 `bIsNew=false`；
- 稳定工况下不再出现逐帧 `[RebuildFromSnapshot] Removing N old modules`；
- 载具不再抖动与定向漂移。
