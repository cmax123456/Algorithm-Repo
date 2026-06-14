# 三阶段 Agent Demo 结果解析

## 1. Demo 目标

本次 demo 的目标，是把一段较长的业务文本，按照下面三步串成一个真实业务流程：

1. 先做实体归一化
2. 再做风险判断
3. 最后生成摘要

对应脚本：`tools/agent_pipeline_demo.py`

对应算法：

- `llm_entity_normalizer`
- `llm_risk_review_advisor`
- `llm_policy_summarizer`

---

## 2. 输入内容说明

本次 demo 输入的是一段较长的业务文本，核心语义包括：

- `Alice` 提交了一条紧急的策略例外申请
- 内容涉及 `North America payout workflow`
- 文本提到可能存在 `violation`
- 文本要求 `Bob` 升级人工审核
- 文本中还提到了受影响合作方 `Acme Corp`

这类输入比较接近真实业务里的“长文本任务描述 + 若干风险信号 + 结构化实体线索”。

---

## 3. 整体执行过程

`tools/agent_pipeline_demo.py` 内部做了四件事：

### 3.1 初始实体抽取

脚本先用一个非常轻量的规则抽取器，从长文本里找候选实体。当前实现不是正式 NER 模型，而是基于大写单词和特定短语抽取，因此会提取出：

- `Alice`
- `North`
- `America`
- `The`
- `Acme Corp`

这一层的作用是给后续的实体归一化算法提供原始候选结构。

### 3.2 阶段一：实体归一化

脚本把：

- 原始长文本 `task_text`
- 候选实体 `entities`

交给 `llm_entity_normalizer`。

### 3.3 阶段二：风险判断

脚本把：

- 原始长文本 `task_text`
- 归一化后的实体 `normalized_entities`

交给 `llm_risk_review_advisor`。

### 3.4 阶段三：摘要生成

脚本再把：

- 风险判断结果
- 原始长文本
- 归一化实体

拼成新的摘要输入，交给 `llm_policy_summarizer`。

因此整个 demo 是一个典型的串联处理流：

`长文本 -> 候选实体 -> 归一化 -> 风险判断 -> 摘要`

---

## 4. 阶段一结果：实体归一化

### 4.1 选中的算法

- `llm_entity_normalizer`

### 4.2 为什么会选中它

本阶段 Agent 条件里要求：

- `required_capabilities = ["entity_normalization"]`
- `preferred_capabilities = ["structured_summary"]`
- `task_family = generation`

因此：

- `llm_policy_summarizer` 被拒绝，因为不具备 `entity_normalization`
- `llm_risk_review_advisor` 被拒绝，因为不具备 `entity_normalization`
- `llm_entity_normalizer` 成功被选中

### 4.3 输出结果

归一化输出为：

- `alice / person`
- `north / person`
- `america / person`
- `the / person`
- `acme corp / organization`

### 4.4 如何理解这个结果

这说明当前实体归一化算法的核心功能是：

- 把名称统一变成小写 `canonical_name`
- 把类型统一映射到 `entity_type`

它更像一个“结构化清洗器”，而不是复杂的实体理解模型。

### 4.5 注意点

当前候选实体抽取比较粗糙，所以出现了：

- `North`
- `America`
- `The`

这样的误提取。

这说明 demo 里的第一步“原始实体抽取”仍是规则化实现，后续如果想更像真实业务，最值得升级的就是这里。

---

## 5. 阶段二结果：风险判断

### 5.1 选中的算法

- `llm_risk_review_advisor`

### 5.2 为什么会选中它

本阶段 Agent 条件里要求：

- `required_capabilities = ["risk_assessment"]`
- `preferred_capabilities = ["review_recommendation"]`
- `allow_human_review = true`
- `hardware_profile.has_gpu = true`
- `preferred_device = gpu`

因此：

- `llm_entity_normalizer` 被拒绝，因为缺少 `risk_assessment`
- `llm_policy_summarizer` 被拒绝，因为缺少 `risk_assessment`
- `llm_risk_review_advisor` 被选中

同时它还满足：

- GPU 要求满足
- 显存要求满足
- 偏好设备 `gpu` 与当前硬件画像匹配

### 5.3 输出结果

风险判断输出为：

- `risk_level = high`
- `recommendation = Escalate to manual review`
- `confidence = 0.9`

### 5.4 如何理解这个结果

这说明当前这段长文本被系统判定为：

- 风险较高
- 需要人工升级复核

这个结论和原始文本中的信号是一致的：

- `urgent`
- `possible violation`
- `escalate the review`

都属于强风险线索。

### 5.5 业务意义

这一阶段相当于在做一个“预审核决策”：

- 先判断内容有没有高风险特征
- 再决定是否要人工介入

这很适合真实系统里的审核前置环节。

---

## 6. 阶段三结果：摘要生成

### 6.1 选中的算法

- `llm_policy_summarizer`

### 6.2 为什么会选中它

本阶段 Agent 条件里要求：

- `required_capabilities = ["policy_summary"]`
- `preferred_capabilities = ["structured_summary"]`
- `intent_keywords = ["summary", "policy", "workflow"]`
- `allow_human_review = false`
- `max_risk_level = low`

因此：

- `llm_entity_normalizer` 被拒绝，因为缺少 `policy_summary`
- `llm_risk_review_advisor` 被拒绝，因为缺少 `policy_summary`
- `llm_policy_summarizer` 被选中

### 6.3 输出结果

摘要输出为：

- `summary = "Summary: Risk level: high. Recommendation: Escalate to manual review. Original co."`
- `key_points = [
  "Task contains 5 structured entities",
  "Highlight the main policy or workflow change"
]`
- `confidence = 0.86`

### 6.4 如何理解这个结果

这里的摘要已经不只是对原始文本做压缩，而是综合了：

- 原始文本内容
- 风险判断结论
- 归一化后的实体数量

因此它更像一份面向业务消费的“最终说明结果”。

### 6.5 为什么摘要看起来被截断了

当前 `llm_policy_summarizer` 示例服务是规则化实现，会把拼接后的长文本做截断式摘要，所以 `summary` 显示为：

- `Summary: Risk level: high. Recommendation: Escalate to manual review. Original co.`

这不是执行失败，而是当前示例 service 的实现方式决定的。后续如果要更像真实模型，可以把它替换成真正的 LLM 摘要逻辑。

---

## 7. 路由过程说明

这次 demo 不只是三个算法被顺序调用了，更重要的是：每一步都是通过 Agent 路由自动选中的，而不是写死算法名直接调用。

### 7.1 归一化阶段路由

- 候选数：3
- 被拒绝：2
- 选中：`llm_entity_normalizer`
- 原因：唯一满足 `entity_normalization`

### 7.2 风险判断阶段路由

- 候选数：3
- 被拒绝：2
- 选中：`llm_risk_review_advisor`
- 原因：唯一满足 `risk_assessment`，且 GPU 条件满足

### 7.3 摘要阶段路由

- 候选数：3
- 被拒绝：2
- 选中：`llm_policy_summarizer`
- 原因：唯一满足 `policy_summary`

这说明当前 Agent 路由系统已经能根据：

- 任务能力
- 风险约束
- 硬件条件

在 active 的 service 池中自动选择正确算法。

---

## 8. 最终业务结论

如果把这次 demo 当成一次真实业务处理，那么最后得到的整体含义是：

1. 原始长文本中包含多个候选实体
2. 这些实体被归一化成统一结构
3. 文本中出现了明显风险信号，因此被判定为 `high risk`
4. 系统建议：`Escalate to manual review`
5. 最终又把风险结论和原始内容整理成一份摘要，方便下游系统或人工查看

也就是说，这个 demo 已经成功展示了一个真实业务链路：

`输入长文本 -> 结构化清洗 -> 风险审核 -> 结果摘要`

---

## 9. 当前 demo 的优点

### 9.1 已经验证的能力

本次 demo 已经真实验证了：

- 三个 Python service 都能被接入框架
- 三个算法都能被 Agent 正确路由
- 硬件条件会影响算法选择
- 多阶段串联脚本可以跑通完整业务流

### 9.2 作为系统雏形的价值

这个 demo 已经足够用来说明：

- 你的框架支持多算法接入
- Agent 不只会“调一个算法”，而是可以在不同阶段做不同算法决策
- 后续是可以扩展成更正式的 pipeline 的

---

## 10. 当前 demo 的不足与改进建议

### 10.1 原始实体提取太粗糙

问题：

- `North`
- `America`
- `The`

这些都被当成了 `person`

建议：

- 把 demo 的第一步升级成正式的实体抽取算法
- 不要继续依赖正则和首字母大写规则

### 10.2 风险判断与摘要仍偏规则化

当前结果是合理的，但实现更像 mock service 或示例 service。

建议：

- 用真实大模型或真实业务服务替换 `service.py`
- 保留当前 `algorithm_card.yaml + schema + agent routing` 这层框架不变

### 10.3 当前串联发生在脚本层

当前三步编排逻辑在：

- `tools/agent_pipeline_demo.py`

建议后续演进为：

- 统一 pipeline CLI
- 或正式的 pipeline orchestrator

---

## 11. 结论

这次 demo 的执行结果说明：

- 三个算法都已经可以真实工作
- Agent 已经可以按阶段自动挑选正确算法
- 整个流程已经具备“长文本 -> 归一化 -> 风险判断 -> 摘要”的真实业务形态

虽然当前各阶段的 service 还是示例实现，但从架构上看，这套流程已经是一个完整、可运行、可扩展的业务原型。


