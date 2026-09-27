# 悬挂行程动力学报告

日期：2026-09-27
范围：`SingularisMorphVehicle` 悬挂引入行程动力学（非簧载质量模型），替换上游的「射线距离 → 弹簧长度」瞬时映射。
验证状态：未执行编译与运行验证（按要求不编译、无 LSP），结论基于与 `ChaosVehicles` / `ChaosModularVehicle` / `ChaosVehiclesPlugin` 源码对照的静态核对。

---

## 1. 问题

悬挂的压缩量与被积分的状态量无关，是射线命中距离的代数映射：

- `ChaosVehiclesCore/Private/SimModule/SuspensionModule.cpp` 与 `ChaosVehiclesCore/Private/SuspensionSystem.cpp`（经典插件）都只做
  `SpringDisplacement = MaxLength - max(0, HitDistance - WheelRadius)`；
- 力为 `SpringDisplacement × SpringRate − (LastDisplacement − SpringDisplacement)/Δt × Damping`。

由此产生的三点缺口（本插件此前与上游一致）：

| # | 缺口 | 证据 |
| --- | --- | --- |
| 1 | 车轮没有沿悬挂轴的自自由度：行程瞬时等于射线结果，没有质量、无自然频率、无过冲与衰减 | 两代插件的 `SetSpringLength` 均为纯代数赋值 |
| 2 | 轮胎没有接触刚度：车轮刚性停在地面点上，不能滞后地面、不能离地 | `SetSpringLength` 直接写入压缩量，`ForceIntoSurface` 取弹簧力而非地面反力 |
| 3 | 阻尼默认值在量纲上近似为零：`SpringDamping = 0.9`，该系数与速度（cm/s）相乘得力，量纲为 kg/s；同一车辆的临界阻尼约为 1600 kg/s 量级 | `ChaosModularVehicle` 与经典插件的 `FVehicleSimSuspensionComponent` 默认值同源 |

补充事实：模块化插件（及本插件）默认创建 `Chaos::FSuspensionConstraint`。该约束在 PBD 求解器内以位置弹簧把车体拉向射线命中点（`PBDSuspensionConstraints.cpp` 的 `DLambda = S·(MaxLength − Distance) − D·SpringVelocity`），弹簧与阻尼参数分别为 `SpringRate × 0.25` 与 `SpringDamping × 5`；约束有效时解析弹力不再施加（`if (!ConstraintHandle.IsValid())`），即约束是唯一的簧上载荷路径。这条路径同样不是行程动力学：车轮位置由求解器直接钉向地面点。

## 2. 模型

悬挂行程成为物理线程上的积分状态，车轮按非簧载质量沿悬挂轴运动：

```
C  = 压缩量（cm，0 = 全伸张，MaxLength = 全压缩）        ← 积分状态
V  = 压缩速度（cm/s，压缩方向为正）                      ← 积分状态
Cg = 射线给出的接触压缩量（cm）                          ← 每步由 SetSpringLength 写入

车轮受力（沿压缩方向为正）：
    F_spring  = k·C + 预载                                  （向下）
    F_damper  = c(压缩/回弹)·V                              （向下）
    F_ground  = max(0, k_t·max(0, Cg − C) + c_t·V)          （向上，单侧接触）
    F_grav    = m·g                                         （向下）
    a = (F_ground − F_spring − F_damper − F_grav) / m
```

- 积分：半隐式欧拉，子步长按轮胎-车轮固有频率限制（`ω·Δt_sub ≤ 0.5`，子步数上限 12；超出子步预算的轮胎刚度按预算折算），保证在 60 Hz 及更高刚度下不发散。
- 行程限位：`C` 钳制在 `[0, MaxLength]`，触限时清零继续压向限位的速度（全伸张/全压缩止点）。
- 簧上反力（沿悬挂轴向上）：`max(0, F_spring + F_damper)`，沿解析力路径施加（不再创建悬挂约束）。
- 轮胎载荷：地面法向反力 `F_ground`（含非簧载重量与动态过冲），经 `SuspensionForceEffect` 缩放后交给配对车轮的 `SetForceIntoSurface`。
- 可视化：悬挂模块的动画位移直接取积分行程，不再按射线命中点重算；车轮随地面运动存在滞后、过冲与回弹。

工程界（防止射线几何异常演化为物理异常）：

| 界 | 取值 | 原因 |
| --- | --- | --- |
| 接触压缩量上限 | `Cg ∈ [0, MaxLength]` | 射线命中近垂直面时命中距离可任意小，不设上限会让轮胎力无界（撞墙弹飞） |
| 阻尼力上限 | 全压缩行程处的弹簧力 `k·MaxLength + 预载` | 真实减振器的高速泄压段；避免异常相对速度把车体弹飞 |

初值与状态延续：

- 模块首次收到射线结果时以地面位置起步（`bTravelInitialized`），避免创建瞬间从全伸张抽向地面；
- 网络复制数据携带 `SpringDisplacement` 与 `SpringSpeed`，拓扑重建（`FillSimState`）与网络回滚（`SetSimState`）都延续行程状态。

## 3. 参数与默认值

模块侧（`FSingularisMorphSuspensionSettings`）新增，组件侧同名暴露：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| 非簧载质量 | 40 kg | 车轮沿悬挂轴的等效质量，决定跟随快慢 |
| 簧载质量 | 375 kg | 单车轮份额（整车质量 / 悬挂数），仅用于推算阻尼 |
| 轮胎刚度倍率 | 10 | 轮胎径向刚度 = 弹簧劲度 × 该值（实车约 8-12 倍） |
| 轮胎阻尼比 | 0.1 | 相对轮胎-车轮临界阻尼，抑制触地弹跳 |
| 回弹阻尼比 | 0.3 | 相对临界阻尼（经典插件 `SuspensionDampingRatio` 默认 0.5） |
| 压缩阻尼比 | 0.15 | 小于回弹阻尼比，实车特征 |

阻尼系数在模块构造时按 `FSuspensionUtility::ComputeDamping(k, m, ζ) = ζ · 2m√(k/m)` 解算（沿用经典插件的换算，保证量纲自洽）：

- 默认参数下：`k = 20000 kg/s²`（= 20 N/mm）、簧载 375 kg → 回弹 1642 kg/s、压缩 821 kg/s、轮胎 566 kg/s；
- 静态压缩量 = `(簧载质量 × g − 预载) / 弹簧劲度` ≈ 18.1 cm（行程 40 cm）。

`SpringDamping`（原默认 0.9）随瞬时路径与约束路径一同移除：该系数在 kg/s 量纲下近似为零，保留会与新参数重复定义阻尼。

## 4. 取舍（对照两大官方插件）

| 来源 | 内容 | 处理 |
| --- | --- | --- |
| ChaosVehiclesPlugin | 由弹簧劲度、簧载质量与阻尼比推算阻尼系数（`FSuspensionUtility`） | 采用，用于压缩/回弹/轮胎三处阻尼 |
| ChaosVehiclesPlugin | 压缩与回弹阻尼分离 | 采用（上游两代模块化实现只有一个阻尼系数） |
| ChaosModularVehicle | 射线几何、目标点、`SuspensionForceEffect` 载荷缩放约定 | 保留 |
| ChaosVehiclesPlugin / ChaosModularVehicle | 射线距离直接映射弹簧长度 | 剔除，改为行程积分 |
| ChaosVehiclesPlugin | 防倾杆（`RollbarScaling`）与载荷传递控制（`WheelLoadRatio`） | 未采用，见第 6 节 |
| ChaosVehiclesPlugin / ChaosModularVehicle | `FSuspensionConstraint` 位置弹簧承载 | 剔除：约束把车体钉向地面点，与行程动力学互斥；改为解析力承载 |

上游压缩/回弹阻尼的选择条件为 `SpringDisplacement < LastDisplacement`（压缩量减小 = 回弹）却取 `CompressionDamping`，与物理语义相反；本实现按压缩速度的符号选择。上游该两值在默认配置下恒相等，故未产生可观测差异。

## 5. 文件变更

| 文件 | 内容 |
| --- | --- |
| `Public/Core/SingularisMorphVehicleSuspensionSimModule.h` | 新增行程动力学参数；压缩/回弹/轮胎刚度与阻尼系数；输出数据结构精简；移除约束与瞬时路径声明 |
| `Private/Core/SingularisMorphVehicleSuspensionSimModule.cpp` | 行程积分、子步布局、限位、簧上反力与轮胎载荷产出；移除约束创建/更新/销毁与射线重算动画 |
| `Public/Components/SingularisSuspensionSUComponent.h` / `Private/...cpp` | 新增六个动力学参数；移除失效的 `SpringDamping`；从世界读取重力写入设置 |
| `Private/Components/SingularisMorphVehicleSimulationComponent.cpp` | 过时注释更新（终止回调 / `OnConstruction_External` 的用途不再特指悬挂约束） |
| `Private/Core/SingularisMorphVehicleSimulationCU.cpp` | 过时注释更新（射线开关关闭时的语义不再依赖已删除的兜底分支） |

行为变化（需要实机确认）：

1. **静态车高由弹簧与质量决定**（`静态压缩 = 簧载质量 × g / 弹簧劲度`）：此前由约束与射线决定。若车体视觉高度与美术设定不符，调 `弹簧劲度`/`簧载质量` 使静态压缩落在设计行程上。
2. **轮胎压缩量不再被补偿**：车轮视觉位置取积分行程，静止时轮胎约下陷 `簧载质量 × g / 轮胎刚度`（默认约 2 cm）。如不可接受，提高 `轮胎刚度倍率`（受子步预算上限约束，默认可提升约 2.9 倍）。
3. **`弹簧阻尼` 属性移除**：其值（0.9）在 kg/s 量纲下近似为零，改用 `回弹阻尼比` / `压缩阻尼比`。

## 6. 未实施

- **防倾杆**：仍缺失（同轴两悬挂压缩量差产生的反向力）。需要配对的同轴悬挂索引，可在 `FixupTreeLinks` 的交叉链接阶段扩展。
- **载荷传递控制**（`WheelLoadRatio`）：上游经典实现按 `WheelLoadRatio` 在悬挂力与静态轮荷间插值以消除弯中内侧轮离地的过度转向；本实现直接使用动态轮胎载荷，未做该插值。
- **轮胎径向刚度上限的自适应子步**：当前子步数封顶 12，超出预算的刚度按上限折算（静默降级，已在头文件注释中说明）。

## 7. 与既有报告的关系

`2026-09-27-Steering-Dynamics-And-Audit-Report.md` 中涉及「悬挂约束」的条目（如 3.1 第 17 条、3.2 第 29 条、4.2 防倾杆、4.3 线程模型）描述的是本次改动前的状态：悬挂约束已随本报告一并移除，相关条目的具体描述以本报告为准；防倾杆与线程模型两节仍然有效。

## 8. 待验证清单

| 优先级 | 验证项 |
| --- | --- |
| 高 | 编译（移除约束头文件依赖、新增 `FSuspensionUtility` 包含） |
| 高 | 静态姿态：车高与设计行程一致，无持续下沉或抬升 |
| 高 | 越障手感：过减速带/路缘时可见压缩-回弹过程，无持续震荡 |
| 高 | 重着陆：全压缩限位不穿透地面，无弹飞 |
| 中 | 撞墙/斜坡：近垂直命中不再产生异常载荷或位移（依赖接触压缩量上限与阻尼力上限） |
| 中 | 网络预测双端：行程状态的复制与重演一致性（`SpringDisplacement`/`SpringSpeed` 已在复制数据中） |
| 中 | 车轮视觉：静止时轮胎下陷量可接受，行驶中不穿过地面 |
| 低 | 变形重建：行程状态延续（`FillSimState`），重建瞬间无抽动 |
| 低 | 高速行驶：子步数（1-12）与性能开销 |
