# 算法库模型集成项目周报（2026-05-24）

## 一、项目背景与目标

本项目的目标，是在现有 C++ 主系统中建设一套“动态算法库（Algorithm Library）”能力，让不同算法资产能够以统一规范接入、注册、校验、启用、执行和管理。

项目当前以 [`algorithm_library_model_integration_SPEC.md`](./algorithm_library_model_integration_SPEC.md) 作为唯一事实来源（source of truth），并结合 [`algorithm_library_flows.md`](./algorithm_library_flows.md) 约束接入与运行流程。

第一版聚焦的核心方向有三个：

1. 建立统一的算法卡片（Algorithm Card）规范，明确算法元数据、输入输出 schema、运行时配置、风险信息等内容。
2. 建立注册表（Algorithm Registry），支持算法的登记、校验、启用、禁用、删除和查询。
3. 为两类后端建立统一接入机制：
   - `onnx`
   - `python_http_service`

项目第一版明确不做以下能力：

- Pipeline orchestration
- Agent planning
- 自动后端选择
- ONNX 与 Python Service 的 fallback
- 训练能力
- gRPC
- Embedded Python
- 原生 C++ plugin backend
- TensorRT / OpenVINO / Triton / LibTorch

换句话说，这个项目当前解决的是“算法资产如何被规范接入并安全执行”的问题，而不是“模型训练和复杂编排”的问题。

---

## 二、本周工作主题

本周工作的主线，是把项目从“规范和治理层”逐步推进到“可运行的统一执行层”。

从交付节奏上看，项目采用分阶段推进方式，SPEC 中总共定义了 7 个阶段：

- Phase 0：工程骨架
- Phase 1：算法卡片与 Registry
- Phase 2：Schema 校验
- Phase 3：Python HTTP Service Backend
- Phase 4：ONNX Backend
- Phase 5：统一执行接口
- Phase 6：示例算法资产

截至本周，项目整体已经完成了从 Phase 0 到 Phase 4 的核心建设，并正在推进 Phase 5。

---

## 三、项目在做什么

从业务角度理解，这个项目本质上是在做一个“算法接入平台的基础设施层”，它把原本分散的模型文件、服务地址、输入输出约定和运行流程，统一纳入一个标准化框架中。

具体来说，项目现在承担以下职责：

### 1. 统一描述算法资产

每个算法都通过 `algorithm_card.yaml` 来描述自身，包括：

- 算法标识：`algorithm_id`、`version`
- 展示信息：`display_name`、`task_family`
- 能力信息：`capabilities`
- 输入输出模态：`modalities`
- 风险与约束：`safety`、`constraints`
- 机器可执行配置：`machine_spec`

其中 `machine_spec` 会进一步指向：

- `input.schema.json`
- `output.schema.json`
- ONNX 模型路径、tokenizer、预处理/后处理配置
- 或 Python HTTP Service 的 `endpoint`、`health_endpoint`、`metadata_endpoint`

这一步的意义是：把“算法说明文档”变成“可被系统读取和执行的标准契约”。

### 2. 管理算法生命周期

项目通过 Registry 对算法进行统一管理，支持以下 CLI 能力：

- `register`
- `validate`
- `activate`
- `disable`
- `delete`
- `list`
- `show-card`

系统按唯一键存储算法：

`algorithm_id + version + backend_type`

这意味着：

- 同一个算法 ID 和版本，可以同时存在不同后端实现
- 删除采用逻辑删除，不会直接破坏历史记录
- 只有符合状态约束的算法，才能进入后续执行流程

### 3. 在执行前后做强校验

项目不只是把算法“登记进去”，而是会在多个关键节点做约束验证：

- 注册/校验阶段：
  - 检查 `algorithm_card.yaml` 结构是否合法
  - 检查必填字段是否存在
  - 检查被引用文件是否存在
  - 检查 `input.schema.json` 与 `output.schema.json` 是否可加载
- 执行阶段：
  - 执行前做输入 schema 校验
  - 执行后做输出 schema 校验

这样做的价值在于：

- 避免“配置已注册，但运行必崩”的资产进入系统
- 避免上游乱传输入导致后端异常
- 避免后端返回不合规结构污染上游调用链

### 4. 打通两类后端

当前项目只支持两类后端：

#### `onnx`

面向本地推理型算法，核心流程是：

- 加载 `model.onnx`
- 按 `preprocess.yaml` 做预处理
- 生成中间 tensor
- 交给 ONNX runner 执行
- 按 `postprocess.yaml` 输出标准 JSON
- 用 golden case 做校验

#### `python_http_service`

面向已经独立部署好的 Python 模型服务，核心流程是：

- 调用 `GET /health`
- 调用 `GET /metadata`
- 调用 `POST /predict`
- 校验服务健康状态和元数据一致性
- 校验返回 `outputs` 是否符合本地 `output.schema.json`

这一步的意义是：把“本地模型”和“远端服务”统一纳入同一套框架下管理和执行。

---

## 四、本周已完成内容

### 1. Phase 0：工程骨架已完成

已完成 C++17 工程基础设施搭建，包括：

- `CMake` 构建工程
- 基础目录结构拆分
- `Status / ErrorCode` 统一错误模型
- `BackendType / AlgorithmStatus`
- `AlgorithmKey`
- `JSON / YAML` 工具层

当前仓库已经具备稳定的编译、测试和扩展基础。

### 2. Phase 1：算法卡片与 Registry 已完成

已完成：

- `AlgorithmCard` 数据模型
- `algorithm_card.yaml` 解析
- `AlgorithmEntry`
- `AlgorithmRegistry`
- 生命周期管理：
  - `register`
  - `validate`
  - `activate`
  - `disable`
  - `delete`
  - `list`
  - `show-card`
- 逻辑删除机制
- 脱敏后的 Agent View 输出

当前系统已经能把算法资产稳定地“登记起来、查出来、控状态”。

### 3. Phase 2：Schema 校验已完成

已完成：

- `input.schema.json` 加载
- `output.schema.json` 加载
- 基础 JSON Schema 校验能力
- 输入输出校验接口

当前已支持的基础 schema 规则包括：

- `type`
- `required`
- `properties`
- `additionalProperties`
- `items`
- `enum`
- `minLength / maxLength`
- `minItems / maxItems`
- `minimum / maximum`

这使系统具备了“强契约执行”的基础。

### 4. Phase 3：Python HTTP Service Backend 已完成

已完成：

- `/health` 检查
- `/metadata` 检查
- `/predict` 联调
- timeout 和 HTTP 状态码处理
- 响应结构校验
- `PythonServiceValidator`

当前系统已经可以在注册/校验阶段真正联通远端 Python 服务，而不是只记录一个 endpoint。

### 5. Phase 4：ONNX Backend 已完成

已完成：

- ONNX package validator
- ONNX session wrapper
- `OnnxRunner`
- `no_op` preprocess / postprocess
- `tensor_from_json`
- `classification_postprocess`
- golden case 校验

当前实现对 ONNX Runtime 做了正式接口保留，并按 SPEC 允许的方式提供了 stub 路径，便于在未完全接入真实 ONNX Runtime 的开发环境中继续推进项目。

这意味着：

- ONNX 资产可以按规范注册
- golden case 可以真正执行
- 预处理 / 后处理链条已经具备模块化接口

### 6. Phase 5：统一执行接口正在推进

本周已经开始把前面几个阶段串成统一 run 流程，主要方向包括：

- `RuntimeFactory`
- 统一执行协调层
- CLI `run`
- 执行审计日志
- 按 `backend_type` 选择不同 runner
- 执行失败直接返回标准错误结构

这部分已经进入代码实现与联调阶段，但截至本周仍处于“收尾验证”状态，尚未作为完全收口的阶段标记。

---

## 五、当前系统架构概览

从仓库结构看，当前项目已经形成比较清晰的模块分层：

### 1. `include/algolib/core`

核心数据模型和基础类型：

- `algorithm_card.h`
- `algorithm_entry.h`
- `algorithm_key.h`
- `backend_type.h`
- `algorithm_status.h`
- `error_code.h`
- `status.h`
- `schema_validator.h`

### 2. `include/algolib/registry` 与 `src/registry`

算法注册表与持久化层：

- `AlgorithmRegistry`
- `RegistryStore`

### 3. `include/algolib/validation` 与 `src/validation`

校验层：

- 算法卡片校验
- Python Service 校验
- ONNX package 校验
- golden case 执行校验

### 4. `include/algolib/runtime` 与 `src/runtime`

运行时层：

- `IAlgorithmRunner`
- `OnnxRunner`
- ONNX pipeline
- ONNX session wrapper
- 统一请求/结果结构
- 统一执行入口的推进实现

### 5. `include/algolib/io` 与 `src/io`

通用 IO 工具：

- 文件处理
- JSON 工具
- YAML 工具
- HTTP client

### 6. `src/cli`

CLI 接口层，当前已承担：

- Registry 操作入口
- 算法卡片查询入口
- 正在接入统一 `run` 入口

### 7. `tests`

测试层已覆盖：

- Registry 生命周期
- Schema 校验
- Python Service 校验
- ONNX Phase 4 相关验证

整体来看，项目已经从单点代码实现，发展成“规范层 + 校验层 + 运行时层 + CLI 层 + 测试层”的完整结构。

---

## 六、本周主要成果总结

本周最大的成果，不是新增了某一个函数，而是把项目的能力边界向“可运行系统”推进了一大步。

主要体现在以下几个方面：

### 1. 项目已经不再只是一个静态注册工具

早期阶段的重点是：

- 读卡片
- 验字段
- 存注册表

而到目前为止，系统已经具备：

- 注册算法
- 校验算法
- 管理状态
- 联调远端服务
- 执行 ONNX golden case
- 校验输入输出

也就是说，项目已经从“资产登记系统”逐渐演进为“算法接入与运行基础设施”。

### 2. 双后端路线已经建立起来

当前两类后端都已经有了相对完整的接入逻辑：

- `onnx`：偏本地执行
- `python_http_service`：偏远端服务调用

这为后续统一执行接口落地提供了现实基础。

### 3. 测试体系已经能为后续迭代提供保护

当前项目不仅有功能实现，也配套了较完整的测试：

- 单元测试
- mock Python service
- ONNX 示例资产
- service 示例资产
- golden case

这意味着后续继续推进 Phase 5 和 Phase 6 时，已经具备相对稳定的回归保护能力。

---

## 七、当前问题与风险

虽然项目整体推进顺利，但当前仍有几个明确风险点需要持续关注。

### 1. Phase 5 尚未完全收口

统一执行接口已经开始落地，但还处在联调和测试收尾阶段。

当前风险主要在于：

- 新增执行链路是否完全覆盖 `onnx` 与 `python_http_service`
- 审计日志与错误回传是否和 SPEC 完全对齐
- CLI `run` 的行为是否足够稳定

### 2. ONNX Runtime 目前仍以 stub 方式承接

当前 ONNX backend 已建立正式接口，但真实 ONNX Runtime 还没有完全接入。

这意味着：

- 当前可以验证流程和接口设计
- 但真实模型加载、真实 tensor 推理能力仍需后续增强

### 3. 统一执行层与长期运行缓存机制尚未充分展开

目前项目主要面向 CLI/验证式使用路径推进，后续如果要做常驻服务，还需要进一步考虑：

- runner 生命周期管理
- session 复用
- 资源回收
- 并发安全
- 更细粒度日志策略

### 4. 示例资产虽然可用，但还需要继续补强

当前示例已经足够支撑 Phase 1-4 的开发和测试，但如果要进一步提升团队协作效率，后续还可以继续补：

- 更丰富的 golden case
- 更复杂的 schema 示例
- 更贴近真实业务的算法卡片内容

---

## 八、下周建议工作计划

结合当前进度，建议下周重点推进以下内容：

### 1. 完成 Phase 5 收口

优先级最高的工作是把统一执行接口彻底收尾，包括：

- 完成 `CLI run`
- 跑通两类 backend 的统一执行路径
- 补齐执行审计日志验证
- 收敛失败直返逻辑

### 2. 完成 Phase 6 示例资产整理

在已有示例基础上，把示例资产整理成更完整的交付包，便于：

- 新成员理解项目
- 测试快速复现
- 后续演示和验收

### 3. 评估真实 ONNX Runtime 接入计划

建议开始讨论：

- 是否在下一阶段接入真实 ONNX Runtime
- 依赖管理方式如何处理
- 平台兼容性如何控制

### 4. 逐步为后续服务化做准备

如果后续要把 CLI 路径扩展为服务接口，可以提前规划：

- `/algorithms/run`
- `/algorithms/cards`
- `/algorithms/list`
- 日志与审计存储策略

---

## 九、项目当前总体判断

截至本周，这个项目已经完成了从“规范设计”到“可执行基础设施”的关键跨越。

如果用一句话总结当前项目状态，可以表述为：

> 该项目正在把 ONNX 模型和 Python HTTP 模型服务统一接入到 C++ 算法库框架中，当前已完成算法资产治理、schema 校验、双后端接入和 ONNX/Service 校验能力，正进入统一执行接口的收尾阶段。

从成熟度来看，可以这样理解：

- Phase 0-2：基础设施和规范体系已经稳定
- Phase 3-4：双后端接入已经建立起来
- Phase 5：正在把“能接入”推进为“能统一执行”

整体方向清晰，模块边界已经逐步成形，后续工作的重点更多是联调、收口和增强，而不是推翻重来。

---

## 十、附：适合对外说明的简版一句话

如果需要在周会或同步会上快速解释这个项目，可以直接使用下面这段：

> 我们正在做一个 C++ 侧的动态算法库框架，用统一的算法卡片、注册表、schema 校验和运行时接口，把 ONNX 模型和 Python HTTP 服务两类算法资产规范接入系统，目前已经完成接入治理和双后端校验，正在推进统一执行接口。
