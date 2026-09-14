# 算法库与 TIA Agent 并发架构设计、改造说明与测试报告

- **项目**：`Algorithmrepo`
- **文档状态**：并发改造已完成，已完成当前环境下的 C++ 与 Python 回归验证
- **生成日期**：2026-09-13
- **适用范围**：C++ 算法库 HTTP 服务、运行时缓存与 Python HTTP Runner 池，以及 Python Tactical Intelligence Agent（TIA）推理与编排链路

---

## 1. 背景、目标与结论

### 1.1 改造前的主要瓶颈与风险

原有设计中，HTTP 服务使用单个全局互斥量覆盖注册表重载、算法查询、在线 `/run` 推理、`/load` 和 `/unload` 等不同性质的操作。这样虽然能避免部分共享状态竞争，但会带来以下问题：

1. **在线推理被串行化**：一个慢速 Python HTTP 模型调用会阻塞其他请求，即使它们属于不同算法或同一算法池中存在空闲 Runner。
2. **每个在线请求都可能触发注册表读盘**：查询与推理路径对 `registry.json` 的依赖过重，增加了磁盘 I/O、锁竞争和时延。
3. **模型加载存在缓存击穿风险**：同一算法和部署实例的并发首次访问可能重复初始化模型或连接池。
4. **卸载与在途推理存在生命周期风险**：若缓存删除后立即销毁 Runner，在途请求可能访问失效对象，形成 Use-After-Free（UAF）风险。
5. **Python 侧存在跨请求状态串扰风险**：全局图像缓存、模型初始化、相同 `work_item` 的重复计算以及模型内部可变状态都需要更细粒度的并发协议。
6. **同一任务的跟踪状态不能并行更新**：MOTR/Kalman 等跟踪链路依赖上一帧状态；完全无序的并发会破坏时间序列一致性。

### 1.2 本次改造的目标

本次改造不追求“所有代码无限并行”，而是将并发控制下沉到真正共享的资源边界，实现以下目标：

- 在线请求不再被 HTTP 服务的管理锁整体串行化。
- 注册表支持多读一写，读请求获取稳定的值快照。
- 相同模型/部署的首次加载使用 Single-Flight；不同模型可以并行初始化。
- Python HTTP Runner 池支持有界并发、超时背压以及安全的 Draining 卸载。
- `/reload`、注册表写入与磁盘持久化不会发生旧快照覆盖新写入的 lost update。
- 审计 JSONL 在多请求并发写入时保持记录完整。
- Python 侧的图像缓存严格请求隔离；相同工作项去重；同一模型资源安全串行；同一 mission 的状态更新串行。
- 保留并发能力的同时，明确超时、失效、重试和降级语义。

### 1.3 改造后的核心结论

在线请求链路已经由“服务端全局锁”改为“分层资源锁”：

```text
HTTP Worker
  └─ ExecutionCoordinator
      ├─ AlgorithmRegistry：共享读锁取得值快照
      ├─ RuntimeRunnerCache：同 Key Single-Flight + shared_ptr 缓存租约
      ├─ PythonHttpRunnerPool：池容量控制 + 单 Runner 租约
      ├─ Python 模型资源：按资源 / 算力档位 / 设备的推理锁
      └─ ExecutionLogger：按日志路径串行追加 JSONL
```

因此：

- **不同算法、不同部署、不同 mission** 可以在满足资源约束的前提下并行；
- **相同模型首次加载**、**相同 `work_item`** 和 **相同 mission 的状态更新** 会被有意合并或串行；
- **相同重型模型实例的推理** 会被资源锁保护，避免第三方模型内部可变状态发生竞争；
- **`/unload` 不再粗暴销毁在途资源**，而是先摘除缓存、拒绝新租约，再让已借出的租约自然归还。

---

## 2. 总体并发模型

### 2.1 分层职责

| 层级 | 共享资源 | 并发控制机制 | 设计结果 |
|---|---|---|---|
| HTTP 接入层 | Handler 工作线程、请求排队 | `cpp-httplib::ThreadPool`，可设置工作线程和队列上限 | 控制接入并发与排队压力，不再以全局业务锁串行 `/run` |
| 管理面 | 重载、注册、启停、部署信息变更 | `admin_mutex_` | 写型管理操作彼此串行，避免管理动作相互覆盖 |
| 注册表 | 内存条目快照、`registry.json` | `std::shared_mutex` + `persistence_mutex_` | 查询可并发；持久化更新与 reload 不会 lost update |
| Runner 缓存 | Runner 索引、加载状态 | 缓存互斥量、条件变量、generation | 同 Key 只加载一次；不同 Key 可并行加载；失效不会回填旧结果 |
| Python HTTP Runner 池 | 空闲/借出 Runner、池生命周期 | `PoolState` 互斥量、条件变量、RAII `shared_ptr` 租约 | 池容量即并发上限；超时返回错误；卸载安全 draining |
| 审计日志 | 同一路径 JSONL 文件 | 按绝对路径共享的互斥量 | 每条 JSONL 记录完整，读取与写入不交叉破坏文件 |
| TIA 请求上下文 | 图像预取缓存 | `ContextVar` + token 恢复 | 不同请求和嵌套上下文不串扰 |
| TIA 模型缓存 | 单例模型及其加载 | 每个模型 Key 独立锁 + generation | 相同模型仅首次创建一次；不同模型可并行加载 |
| TIA 模型执行 | 第三方模型内部状态、GPU/设备 | `inference_guard` 资源级 `RLock` | 同资源串行，异资源并行 |
| TIA 编排状态 | mission 跟踪状态、work item 结果 | mission 锁 + `SingleFlightCache` | 同 mission 的状态更新串行；同 work item 合并执行 |

### 2.2 并发不变量

| 不变量 | 实现位置 | 含义 |
|---|---|---|
| 在线读使用稳定注册表快照 | `AlgorithmRegistry::Get`、`ExecutionCoordinator::Run` | 在途请求使用值副本，即使并发 reload/生命周期变更也不会读到半更新条目 |
| 磁盘提交不覆盖新写入 | `AlgorithmRegistry::Reload`、各持久化写方法 | 所有“读盘 → 提交快照”和“候选副本 → 保存 → 交换”流程通过 `persistence_mutex_` 串行 |
| 同 Key 只允许一个 Loader | `RuntimeRunnerCache::GetOrLoad` | 其余请求等待同一个 `LoadState` 的条件变量结果 |
| 缓存失效后旧加载不能回填 | `RuntimeRunnerCache::generation_` | 失效操作递增 generation，旧加载完成后返回不可用而不是重新写入缓存 |
| 缓存删除不使在途请求悬空 | `std::shared_ptr<IAlgorithmRunner>` | 请求持有 Runner 所有权，缓存条目删除不等于立即销毁对象 |
| 新请求不会进入 Draining 池 | `PythonHttpRunnerPool::CheckoutRunner` | `Unload()` 将池切换为 `ready=false`、`draining=true`，后续借用被拒绝 |
| 已借出的 Runner 可以安全完成 | `PooledRunnerGuard` | Guard 同时持有 `PoolState` 与 Runner 的 `shared_ptr`，归还后才最终释放 |
| 同一个图像 URI 不跨请求复用 | `agent.inference.image_cache` | 缓存绑定当前 `ContextVar`，不是全局字典 |
| 同一 mission 的状态不会交叉写回 | `TacticalIntelligenceAgent._mission_lock` | 读 `prior_tracks`、执行感知、写回 `_track_state` 处于同一个 mission 临界区 |
| 同一模型资源不会并发访问内部可变状态 | `inference_guard` | 锁 Key 为“资源类型 + 算力档位 + 设备” |

### 2.3 锁顺序与死锁规避

主要锁关系如下：

```text
管理型 HTTP 路由：
  admin_mutex_ → AlgorithmRegistry::persistence_mutex_ → AlgorithmRegistry::mutex_（unique）

注册表读路径：
  AlgorithmRegistry::mutex_（shared）

Runner 缓存：
  RuntimeRunnerCache::mutex_ 仅保护索引/LoadState；绝不在该锁内执行 runner->Load()

Runner 池：
  PoolState::mutex 仅保护池状态；绝不在该锁内执行 runner->Run()

执行日志：
  构造时短暂访问全局路径状态表；读写阶段只持有对应日志路径的互斥量
```

关键原则是：**耗时网络 I/O、模型初始化与模型推理都不在全局索引锁或 HTTP 管理锁内执行**。这既降低锁竞争，也避免因网络阻塞扩大临界区。

---

## 3. C++ 算法库并发设计

### 3.1 HTTP 服务：管理面与数据面分离

涉及文件：

- `include/algolib/server/http_server.h`
- `src/server/http_server.cpp`
- `src/server/main.cpp`

#### 3.1.1 管理锁的职责收缩

HTTP 服务将原本的全局 `mutex_` 调整为 `admin_mutex_`。它只用于相互影响的管理写操作，例如：

- `POST /reload`
- 注册、校验、激活、禁用、删除算法
- 添加、删除、更新部署信息

以下在线或读型路径不再获取管理锁：

- `POST /run`
- `GET /algorithms`
- 单算法详情查询和文件下载
- `GET /traces/{trace_id}/function-executions`
- `POST /load`
- `POST /unload`

这样，慢速推理、模型预热、模型文件传输和审计查询不会阻塞无关的在线请求。它们分别依赖注册表、Runner 缓存、Runner 池和日志锁的内部并发协议。

#### 3.1.2 不再在每次 `/run` / 查询时 reload 注册表

- 服务启动和显式 `POST /reload` 时调用注册表 reload。
- `GET /algorithms` 直接读取内存中的共享快照。
- `POST /run` 直接交给 `ExecutionCoordinator`，由其取得当前算法条目的值副本。

这消除了每个在线请求的注册表磁盘读取，避免了 I/O 放大，也使在线请求不必等待管理锁。

#### 3.1.3 HTTP Worker 与请求队列配置

`HttpServerConfig` 新增：

| 字段 | 含义 | 默认值 |
|---|---|---|
| `worker_threads` | HTTP Handler 工作线程数量 | `0`，使用 `cpp-httplib` 默认行为 |
| `max_queued_requests` | 最大排队请求数 | `0`，不显式限制 |

服务支持命令行参数和环境变量：

```text
--workers <count>
--max-queued-requests <count>
ALGOLIB_SERVER_WORKERS
ALGOLIB_SERVER_MAX_QUEUED_REQUESTS
```

当 `worker_threads > 0` 时，服务配置 `httplib::ThreadPool(worker_threads, max_queued_requests)`；`/health` 会回显当前 `worker_threads`、`max_queued_requests` 与 Runner 缓存条目数，便于运行期核对配置。

#### 3.1.4 文件下载的内存改进

模型文件下载接口不再在业务代码中先把整个文件读入 `std::string`，而改用 `httplib` 的 `set_file_content` 文件接口。该改动减少了应用层显式构造模型全量内容的内存峰值，尤其适用于较大的模型文件。

---

### 3.2 AlgorithmRegistry：读写分离、快照提交与持久化一致性

涉及文件：

- `include/algolib/registry/algorithm_registry.h`
- `src/registry/algorithm_registry.cpp`

#### 3.2.1 读操作使用 `std::shared_mutex`

注册表的查询与校验读取使用 `std::shared_lock<std::shared_mutex>`：

- `Get`
- `List`
- `ListAgentViews`
- `Query`
- `QueryAgentViews`
- 以 `AlgorithmKey` 发起的输入/输出 Schema 校验的第一阶段

这些方法返回 `AlgorithmEntry` 的**值副本**，而不是持有容器内部引用。`ExecutionCoordinator` 取得副本后，输入校验、Runner 选择和输出校验都以该副本为准。因此并发 `/reload` 或算法生命周期更新不会破坏已开始请求的契约一致性。

#### 3.2.2 写操作采用候选快照提交

注册、校验、激活、禁用、删除、部署变更都遵循统一步骤：

```text
当前 entries_ → 构造 candidate 副本 → 保存 candidate 到 registry.json → entries_.swap(candidate)
```

如果 `store_.Save(candidate)` 失败，当前服务中的 `entries_` 不会被改变。这样可避免“磁盘写失败但内存已切换”的不一致状态。

包校验、远端服务健康检查等耗时工作放在注册表写锁之外。例如 `Validate`：

1. 先用共享读锁复制当前条目；
2. 释放读锁后执行包校验；
3. 再取得持久化锁和写锁，并基于最新条目重新检查状态与部署信息；
4. 保存候选快照后交换内存快照。

因此耗时校验不会阻塞其他在线读请求。

#### 3.2.3 `persistence_mutex_` 解决 reload 覆盖写入问题

仅有内存读写锁不足以保护如下时序：

```text
线程 A：从磁盘读取旧 registry.json ──────────────── 准备替换内存快照
线程 B：注册新算法 → 保存新 registry.json → 更新内存快照
线程 A：把旧磁盘快照写回内存                                      ← lost update
```

为此新增 `persistence_mutex_`，串行化：

- `Reload()` 的“读盘 → 提交内存快照”；
- 所有“候选副本 → 写盘 → 交换内存快照”的写操作。

所有会同时获取两把锁的注册表写路径统一遵循：

```text
persistence_mutex_ → mutex_（unique_lock）
```

这样，reload 不能用旧文件快照覆盖并发刚刚持久化的更新。

> 注意：这一保证覆盖**同一进程内同一个 `AlgorithmRegistry` 实例**。多个独立进程同时写同一个 `registry.json` 时，仍需要文件锁、单独的注册表服务或数据库事务来提供跨进程一致性，详见“已知边界”。

---

### 3.3 RuntimeRunnerCache：Single-Flight、generation 与安全所有权

涉及文件：

- `include/algolib/runtime/runtime_runner_cache.h`
- `src/runtime/runtime_runner_cache.cpp`

#### 3.3.1 缓存 Key 与指纹

缓存索引使用：

```text
AlgorithmKey（algorithm_id + version + backend_type） | deploy_id
```

每个条目还带有由算法键、包路径、卡片路径和 AlgorithmCard JSON 组成的 fingerprint。只有 Key 与 fingerprint 都匹配时才命中缓存。不同 `deploy_id` 会获得不同 Runner/Runner 池实例。

#### 3.3.2 同 Key 首次加载合并

`GetOrLoad` 的流程：

```text
1. 加缓存锁，命中有效 Runner 时直接返回 shared_ptr。
2. 若同一 cache_key 已在 loading_：等待该 LoadState 的条件变量。
3. 若无人加载：写入新的 LoadState，记录当前 generation，释放缓存锁。
4. 在锁外执行 factory.Create() 与 runner->Load(entry)。
5. 重新加锁；若 generation 未变则发布缓存；写入状态并唤醒所有等待者。
```

结果：

- 同一个算法部署的并发首访只会真实执行一次初始化；
- 等待者复用 Loader 的成功结果或异常结果；
- 不同算法/不同部署的 `runner->Load()` 不会被缓存全局锁串行化。

#### 3.3.3 失效 generation 防止旧结果回写

`Invalidate`、`InvalidateDeployment` 和 `Clear` 会：

1. 递增 `generation_`；
2. 删除相应缓存条目；
3. 使正在进行但属于旧 generation 的加载结果无法发布到缓存。

若加载期间发生失效，Loader 会收到“已被缓存失效替代”的不可用状态，而不会将旧 Runner 重新放回缓存。调用方可按错误语义重试 `/load` 或在线请求。

当前 generation 是缓存级全局 generation，属于保守策略：任一失效操作都可能使其他正在加载的旧 generation 结果不发布。这优先保证一致性，代价是高频管理变更期间可能出现额外重试。

#### 3.3.4 `shared_ptr` 形成安全 Runner 租约

缓存返回 `std::shared_ptr<IAlgorithmRunner>`。`ExecutionCoordinator` 在执行期间持有该指针，缓存删除不再意味着对象立刻销毁。该所有权模型是 `/unload` 与在线推理可以安全重叠的基础。

---

### 3.4 PythonHttpRunnerPool：有界并发与 Draining 卸载

涉及文件：

- `include/algolib/runtime/python_http_runner_pool.h`
- `src/runtime/python_http_runner_pool.cpp`

#### 3.4.1 池状态

`PoolState` 由 `shared_ptr` 管理，内部维护：

- `all_runners`：当前池拥有的全部 Runner；
- `idle_runners`：可借出的空闲 Runner；
- `borrowed_count`：已借出数量；
- `ready`、`draining`、`owner_alive`：生命周期状态；
- 互斥量和条件变量。

默认池容量为 4，默认 checkout 超时为 5000 ms；`POST /load` 可通过 `pool_size`、`pool_checkout_timeout_ms` 覆盖。

#### 3.4.2 运行与背压

每次 `Run`：

```text
ready 检查 → CheckoutRunner() → 获得 PooledRunnerGuard → runner->Run() → Guard 析构归还
```

- 空闲 Runner 可立即执行；
- 池已满时等待条件变量；
- 达到 checkout 超时后，返回 `SERVICE_UNAVAILABLE` 类错误，而不是无限等待或死锁；
- Guard 归还 Runner 时唤醒等待者。

因此，同一 Python 服务实例的真实并发度由 `pool_size` 控制。对同一算法而言，端到端有效并发度近似受以下因素共同限制：

```text
min(HTTP 可用工作线程数，PythonHttpRunnerPool.pool_size，上游 Python 服务可承载并发度)
```

#### 3.4.3 Safe Lease 与 UAF 防护

`PooledRunnerGuard` 同时持有：

```text
shared_ptr<PoolState>
shared_ptr<IAlgorithmRunner>
```

即使 `PythonHttpRunnerPool` 本体已经析构，已借出的 Guard 依然拥有必要的状态与 Runner 所有权，不会回调到悬空的 `this` 指针。移动赋值会先正确归还原有租约，再接管新租约，避免资源泄漏或重复归还。

#### 3.4.4 Draining 语义

`Unload()` 不等待网络推理完成，也不强制中止已经借出的 Runner。它执行：

```text
ready = false
→ draining = true
→ 清空 idle_runners，拒绝新 Checkout
→ 等待已借租约在各自请求结束时自然归还
→ borrowed_count == 0 后释放 all_runners
```

由此得到的行为是：

| 场景 | 行为 |
|---|---|
| 已在 `Unload()` 前获得租约的请求 | 可以完成推理并安全归还 |
| `Unload()` 后新到达的请求 | 被拒绝，不会借到已进入 draining 的 Runner |
| 缓存失效后的新请求 | 不再从缓存取得旧池；会走新的加载/不可用路径 |
| Pool 析构时仍有在途请求 | Guard 保证在途请求持有对象，不发生 UAF |

---

### 3.5 `/load`、`/unload` 与缓存生命周期

涉及文件：`src/runtime/model_loader.cpp`

#### Load 路径

```text
Registry 快照读取
→ 判断算法为 active
→ 快速检查缓存
→ （可选）更新部署状态为 loading
→ （可选）http_pull 下载模型文件
→ RuntimeRunnerCache::GetOrLoad（Single-Flight）
→ HealthCheck
→ 更新部署状态为 ready
```

`/load` 不占用 HTTP 管理锁：同 Key 由 Runner 缓存合并，不同 Key 可以并行预热或下载。

#### Unload 路径

```text
取得 cached shared_ptr
→ 从 RunnerCache 摘除对应 deploy 或整个算法的缓存
→ 对仍持有的 Runner 调用 Unload()，进入 draining
→ （可选）部署状态更新为 unloaded
```

先摘缓存、后调用 `Unload()` 的顺序很重要：它先阻止新请求继续从缓存借用旧 Runner，同时依靠 `shared_ptr` 让已有请求保持安全引用。

---

### 3.6 ExecutionLogger：多 Coordinator 的 JSONL 完整性

涉及文件：

- `include/algolib/runtime/execution_logger.h`
- `src/runtime/execution_logger.cpp`

每个 `/run` 会构造一个 `ExecutionCoordinator`，因此同一日志路径可能对应多个 `ExecutionLogger` 实例。为避免不同实例分别持有不同锁导致 JSONL 行交错，日志模块维护：

```text
绝对日志路径 → weak_ptr<mutex>
```

同一路径的 Logger 会复用同一把互斥量：

- `Append` 在锁内完成“确保父目录、打开文件、写入一条 JSON、换行”；
- `ReadFunctionExecutions` 在同一把锁内扫描文件。

这样能防止读取到正在写入的半行或两个请求的 JSON 内容交织。`ExecutionCoordinator` 仍保持“审计写失败不影响主推理响应”的既有容错策略；生产环境应配套监控日志写入失败率。

---

## 4. Python TIA Agent 并发设计

### 4.1 请求级图像缓存隔离

涉及文件：

- `agent/inference/image_cache.py`
- `tactical_intelligence_agent/batch_preparer.py`
- `agent/orchestrator.py`

原有全局图像缓存容易造成不同批次之间互相覆盖、清空或误复用。当前实现使用：

```python
ContextVar[Optional[BatchImageCache]]
```

每个批次通过 `begin_batch_cache(scope)` 创建独立缓存，并返回 token；结束时必须使用 `end_batch_cache(token)` 精确恢复父上下文。

执行链路为：

```text
prepare_batch_for_inference()
  → begin_batch_cache(work_item 或 mission_id)
  → 合并传感器元数据、预取视觉帧
  → 返回 batch + token

TacticalIntelligenceAgent._process_mission_batch()
  → try: 感知 → 认知 → 通信
  → finally: finalize_batch_inference(token)
```

另外，`prepare_batch_for_inference` 自身若在预取或准备阶段抛出异常，会立即 reset token，避免调用方尚未接收到 token 时泄漏上下文。

### 4.2 模型单例缓存：每 Key 首次加载合并

涉及文件：`agent/inference/registry.py`

模型缓存由以下结构组成：

```text
_CACHE                    # 已完成的模型实例
_CACHE_LOCK               # 保护缓存与 generation
_KEY_LOCKS[key]           # 每个模型 Key 的首次加载锁
_CACHE_GENERATION         # clear_model_cache 后的代际号
```

`_get_or_create(key, factory)` 采用双重检查：

1. 先在 `_CACHE_LOCK` 内检查已缓存模型；
2. 未命中时只获取该 Key 的锁；
3. 进入 Key 锁后再次检查缓存；
4. 在 Key 锁内执行该模型的 factory；
5. 发布到缓存前检查 generation，避免 `clear_model_cache()` 期间创建的旧实例回填新缓存。

该设计使：

- 相同权重、相同档位、相同设备上的模型只初始化一次；
- 不同模型 Key 可以并行加载；
- 清空模型缓存期间，旧加载结果不会污染新一代缓存。

### 4.3 资源级推理锁：保护重型模型内部状态

部分第三方模型、Predictor 或生成式模型可能在一次调用中修改内部状态，或者对同一设备上的并发访问并不安全。为此新增：

```python
with inference_guard(resource, config):
    ... model inference ...
```

锁 Key 为：

```text
resource : compute_profile : device
```

目前已接入的资源包括：

| 推理模块 | 资源锁名称 |
|---|---|
| RT-DETR 目标检测 | `detector` |
| ODConv 检测精修 | `odconv` |
| Siamese Mask2Former 损伤评估 | `mask2former` |
| EDL 证据评估/验证 | `edl` |
| MOTR + Kalman 跟踪 | `motr` |
| ImageBind 表征 | `imagebind` |
| 多模态 Mamba 融合 | `mamba` |
| SupCon 元分类 | `supcon` |
| Page Encoder / RAG | `page_encoder` |
| 语义通信压缩 | `semantic_comm` |
| MARL 路由 | `marl_policy` |
| MARL-PPO 调度 | `marl_ppo` |

其效果是：

- 相同资源、相同算力档位和设备上的调用被串行化；
- 不同资源（例如检测与 RAG）仍然可以在不同 mission 中并行；
- 同一类模型在 CPU、MPS、CUDA 或不同档位下使用不同锁，不会不必要地互相阻塞。

这是一种有意识的“**资源级串行、业务级并行**”策略：优先保护模型正确性，再在资源之间释放并发度。

### 4.4 mission 状态保序

涉及文件：`agent/orchestrator.py`

`TacticalIntelligenceAgent` 为每个 `mission_id` 维护独立锁：

```text
mission-1 → Lock A
mission-2 → Lock B
```

同一 mission 的完整批次处理在同一把锁内执行：

```text
读取 prior_tracks
→ 感知（含跟踪）
→ 写回 _track_state[mission_id]
→ 认知与通信
```

因此同一 mission 的状态读写不会交叉。不同 mission 持有不同锁，可以并发进入流水线，再按其使用的具体模型资源锁进行协调。

> `std`/Python 普通互斥锁不提供严格 FIFO 公平性。因此该机制保证“同 mission 互斥和状态一致性”，而不是形式化的网络到达顺序保证。若业务要求严格按上游序号处理，应在请求中加入 sequence，并使用显式有序队列或序列号校验。

### 4.5 同 work item 的 Single-Flight 与幂等结果缓存

`SingleFlightCache[T]` 位于 `agent/orchestrator.py`，被 `TacticalIntelligenceCommanderAgent` 用于 `_get_or_process`：

```text
相同 work_item：
  第一个请求创建 Future 并负责计算
  后续请求等待同一个 Future
  成功后共享同一个结果；失败时共享同一个异常

不同 work_item：
  使用不同 Future，可并行执行
```

缓存最大条目数默认为 1024。超过上限时按字典插入顺序删除最早条目；它是有界缓存，但不是 TTL 缓存，也不是严格 LRU 缓存。

异常场景下，in-flight 项会移除，等待者得到相同异常；下一次请求可以重新计算，避免失败永久粘连。

### 4.6 Python 3.9 兼容性修复

`agent/models/schemas.py` 中将会触发当前 Pydantic/Python 3.9 解析问题的 `float | None` 改为 `Optional[float]`。当前并发回归命令已在 Python 3.9.6 下成功导入并执行，说明该兼容性问题已不再阻塞测试。

---

## 5. 关键时序与竞态处理

### 5.1 在线 `/run` 时序

```text
HTTP Worker
  → 解析 AlgorithmRequest
  → Registry.Get()：shared_lock，复制 AlgorithmEntry 快照
  → 依据快照校验输入
  → RunnerCache.GetOrLoad()：命中 / 等待同 Key 加载 / 自行加载
  → 获得 shared_ptr Runner
  → Runner.Run()
      → Python Pool Checkout
      → 持有 Guard 执行 Python HTTP 调用
      → Guard 析构归还或完成 Drain
  → 依据同一 Entry 快照校验输出
  → 按日志路径锁追加 JSONL 审计记录
  → HTTP 响应
```

### 5.2 reload 与在线推理并发

| 并发事件 | 处理结果 |
|---|---|
| `/run` 已取得 `AlgorithmEntry` 快照，随后 `/reload` | 在途请求继续按旧快照完成；后续请求使用新快照 |
| `/reload` 与注册表持久化写入并发 | `persistence_mutex_` 串行，旧磁盘快照不能覆盖新写入 |
| `/reload` 与 Runner 正在加载并发 | `runner_cache_.Clear()` 递增 generation；旧加载不会重新发布到缓存 |
| `/reload` 与已在运行的 Runner 并发 | 现有请求持有 `shared_ptr`，不会因缓存清除而悬空 |

### 5.3 unload 与在线推理并发

| 并发事件 | 处理结果 |
|---|---|
| 请求已经 Checkout 到 Runner，随后 `/unload` | 当前请求继续完成；Guard 归还时协助最终释放资源 |
| `/unload` 后新请求进入旧 Pool | `ready=false/draining=true`，Checkout 被拒绝 |
| `/unload` 与缓存查找竞争 | 缓存先被摘除；即使已有请求持有旧 `shared_ptr`，对象仍安全存在 |
| 池中无借出 Runner 时 `/unload` | 立即清理 `all_runners` 与空闲队列 |

### 5.4 Python 侧并发时序

| 场景 | 处理结果 |
|---|---|
| 两个批次预取不同图像 | 各自在 ContextVar 中使用独立 `BatchImageCache` |
| 两个请求加载相同模型 | 仅一个线程执行 factory，另一个等待对应模型 Key 锁 |
| 两个 mission 使用同一检测模型 | 都可进入业务流水线，但在 `inference_guard("detector", ...)` 处串行 |
| 两个 mission 使用不同模型资源 | 可并行执行对应模型阶段 |
| 两个请求处理同一 mission | 通过 mission 锁串行，保证 `_track_state` 连续 |
| 两个请求处理同一 work item | 通过 Future 合并，只执行一次引擎处理 |

---

## 6. 新增与调整的文件清单

### 6.1 C++ 侧

| 文件 | 新增/调整内容 |
|---|---|
| `include/algolib/registry/algorithm_registry.h` | 新增 `shared_mutex` 与 `persistence_mutex_`；明确快照和持久化并发语义 |
| `src/registry/algorithm_registry.cpp` | 读写分离、候选快照提交、reload/持久化串行、校验耗时工作移出注册表写锁 |
| `include/algolib/runtime/runtime_runner_cache.h` | 引入 `LoadState`、条件变量、generation、`shared_ptr` Runner 缓存 |
| `src/runtime/runtime_runner_cache.cpp` | 实现同 Key Single-Flight、失效 generation、防旧加载回写和线程安全缓存访问 |
| `include/algolib/runtime/python_http_runner_pool.h` | Runner 租约改为 `shared_ptr`；补强 Guard 移动语义 |
| `src/runtime/python_http_runner_pool.cpp` | 实现 PoolState、Checkout 超时、RAII 归还、Draining 卸载和析构安全 |
| `src/runtime/model_loader.cpp` | 调整卸载顺序：先从缓存摘除，再对仍持有的 Runner 执行 drain |
| `include/algolib/runtime/execution_logger.h` | 日志实例持有按路径共享的互斥状态 |
| `src/runtime/execution_logger.cpp` | 同日志路径的多实例互斥追加和查询，保护 JSONL 完整性 |
| `include/algolib/server/http_server.h` | 新增 HTTP 工作线程和最大排队请求配置 |
| `src/server/http_server.cpp` | 去除在线路径全局管理锁；管理路由使用 `admin_mutex_`；增加线程池配置；优化文件响应 |
| `src/server/main.cpp` | 增加 `--workers`、`--max-queued-requests` 和对应环境变量 |
| `tests/python_runner_pool_tests.cpp` | 新增/加强 Python Runner 池并发、超时、HTTP 并发和 unload-during-run 回归测试 |

### 6.2 Python 侧

| 文件/模块 | 新增/调整内容 |
|---|---|
| `agent/inference/image_cache.py` | 全局缓存改为 `ContextVar` 批级缓存，并提供 token 生命周期管理 |
| `tactical_intelligence_agent/batch_preparer.py` | 建立请求级图像缓存，异常时立即恢复上下文，返回 token 供 finally 清理 |
| `agent/orchestrator.py` | 新增泛型 `SingleFlightCache`；新增按 mission 的状态锁；保留 ContextVar 上下文的处理入口 |
| `agent/inference/registry.py` | 模型 Key 级首次加载锁、缓存 generation、资源级 `inference_guard` |
| `agent/inference/{vision,tracking,damage,edl,embed,fusion,classify,rag,routing,scheduling,semantic_comm}.py` | 在共享重型模型调用点接入资源级推理锁 |
| `tactical_intelligence_agent/service.py` | Commander Agent 复用 `SingleFlightCache`，实现同 work item 去重与异常传播 |
| `agent/models/schemas.py` | 使用 `Optional[...]` 修复 Python 3.9/Pydantic 类型兼容问题 |
| `tactical_intelligence_agent/test_concurrency.py` | 新增 Python 并发回归测试：缓存隔离、Single-Flight、推理锁、mission 状态保序、Commander 幂等 |

---

## 7. 测试设计与执行结果

### 7.1 测试环境

| 项目 | 实际值 |
|---|---|
| 操作系统 | macOS |
| Python | `Python 3.9.6` |
| CMake | `cmake version 4.3.2` |
| C++ 编译器 | `Apple clang version 21.0.0` |
| `ALGOLIB_WITH_ONNXRUNTIME` | `OFF` |

### 7.2 C++ 构建与 CTest 结果

执行命令：

```bash
cmake --build /Users/cmax/Downloads/Algorithmrepo/build --target algolib_tests -j 4 \
  && ctest --test-dir /Users/cmax/Downloads/Algorithmrepo/build --output-on-failure
```

结果：

```text
[100%] Built target algolib_tests
1/1 Test #1: algolib_tests ....................   Passed    2.94 sec

100% tests passed, 0 tests failed out of 1
Total Test time (real) = 2.95 sec
```

说明：当前 CMake 将多组 C++ 单元/集成测试聚合为一个名为 `algolib_tests` 的 CTest 目标，因此 CTest 显示为 `1/1`；这不表示只有一个内部断言或一个业务场景。

### 7.3 C++ 并发相关子测试结果

随后直接执行：

```bash
cd /Users/cmax/Downloads/Algorithmrepo
./build/algolib_tests
```

进程退出码为 `0`。其中下列并发相关测试均输出 `[PASS]`：

| 测试 | 覆盖行为 | 结果 |
|---|---|---|
| `TestPythonRunnerPoolLoadRunUnload` | 池加载、健康检查、运行、卸载基本生命周期 | 通过 |
| `TestPythonRunnerPoolConcurrentRuns` | `pool_size=3` 下 6 个并发请求均完成并归还 Runner | 通过 |
| `TestPythonRunnerPoolCheckoutTimeout` | 池耗尽时超时返回错误，不死锁 | 通过 |
| `TestPythonRunnerPoolViaRuntimeCache` | 缓存命中、按 `deploy_id` 隔离及失效 | 通过 |
| `TestPythonRunnerPoolViaHttpServer` | 通过真实 HTTP Server 发起 6 个并发 `/run`，验证不再退化为全局锁串行 | 通过 |
| `TestPythonRunnerPoolUnloadDuringRun` | 在途请求跨 unload 安全完成；draining 后新请求被拒绝 | 通过 |

#### HTTP 并发测试的判定条件

`TestPythonRunnerPoolViaHttpServer` 使用：

- Mock Python `/predict` 延迟：100 ms；
- Python Runner 池容量：3；
- 并发 `/run` 请求数：6；
- 断言：全部 6 个请求成功；总耗时小于 450 ms；Mock 服务至少接收到 6 个在线请求加上初始化/健康检查产生的调用。

这一区间可区分“两批池化执行”的预期行为与历史“6 个请求被服务端全局锁完全串行”的行为。测试通过说明 HTTP `/run` 路径已经能够利用 Runner 池并发，而不是被旧的服务端锁限制为 1。

#### Draining 测试的判定条件

`TestPythonRunnerPoolUnloadDuringRun` 使用单 Runner 池和 180 ms Mock 推理延迟：

1. 等待在途请求已经到达 Python 服务；
2. 调用 `Unload()`；
3. 验证新请求被拒绝；
4. 验证已经借出 Runner 的请求仍成功完成；
5. 验证 drained 池不保留已归还的空闲裸指针。

该测试通过，验证了 Safe Lease 与 Draining 卸载的核心安全语义。

### 7.4 Python 并发回归测试结果

执行命令：

```bash
cd /Users/cmax/Downloads/Algorithmrepo
PYTHONPATH="$PWD" TIA_SKIP_WARMUP=1 \
  python3 -m unittest tactical_intelligence_agent.test_concurrency -v
```

结果：

```text
Ran 8 tests in 0.470s
OK (skipped=2)
```

详细结果：

| 测试类 | 场景 | 结果 |
|---|---|---|
| `ImageCacheConcurrencyTest` | 两个线程的 ContextVar 图像缓存互不干扰 | 通过 |
| `SingleFlightCacheTest` | 相同 Key 仅计算一次，6 个调用共享结果 | 通过 |
| `SingleFlightCacheTest` | 不同 Key 可以并行计算 | 通过 |
| `InferenceGuardTest` | 同一资源的峰值并发数为 1 | 通过 |
| `InferenceGuardTest` | 不同资源可以同时进入，峰值并发数为 2 | 通过 |
| `MissionOrderingTest` | 同 mission 的先验轨迹和写回状态保持串行连续 | 通过 |
| `CommanderSingleFlightTest` | 同一 work item 仅处理一次 | 跳过 |
| `CommanderSingleFlightTest` | 不同 work item 可以并行 | 跳过 |

跳过原因是当前独立仓库环境未安装外部依赖 `a2a_protocol`。测试文件会仅在异常模块确实为 `a2a_protocol` 时跳过 Commander 场景，其他导入或业务错误不会被吞掉。

### 7.5 当前验证结论

| 验证项 | 结论 |
|---|---|
| C++ 改造后的重新编译 | 通过 |
| C++ 聚合 CTest | 通过，`1/1` 目标通过 |
| Python HTTP Runner 池并发、超时、Draining | 通过 |
| HTTP `/run` 利用 Runner 池并发 | 通过 |
| Python ContextVar 缓存隔离 | 通过 |
| Python Single-Flight 与资源锁 | 通过 |
| Python mission 状态串行 | 通过 |
| Commander 与真实 A2A SDK 的并发回归 | 当前环境跳过，待安装 `a2a_protocol` 后补跑 |
| 真实 ONNX Runtime 并发推理 | 当前未验证，构建选项为 `ALGOLIB_WITH_ONNXRUNTIME=OFF` |

---

## 8. 构建、启动与验证指引

### 8.1 C++ 构建与测试

```bash
cd /Users/cmax/Downloads/Algorithmrepo

cmake -S . -B build -DALGOLIB_BUILD_TESTS=ON
cmake --build build --target algolib_tests -j 4
ctest --test-dir build --output-on-failure

# 可查看聚合测试内部的逐项输出
./build/algolib_tests
```

### 8.2 Python 并发回归测试

```bash
cd /Users/cmax/Downloads/Algorithmrepo

PYTHONPATH="$PWD" TIA_SKIP_WARMUP=1 \
  python3 -m unittest tactical_intelligence_agent.test_concurrency -v
```

`TIA_SKIP_WARMUP=1` 的目的是在回归测试时避免真实模型预热影响测试时延和外部依赖。生产启动时是否预热应按部署环境、模型体积和设备容量决定。

### 8.3 启动带并发配置的算法库服务

以下示例以本机注册表和 8 个 HTTP Worker、64 个最大排队请求为例：

```bash
cd /Users/cmax/Downloads/Algorithmrepo

export ALGOLIB_REGISTRY_PATH="$PWD/.algolib/registry.json"
export ALGOLIB_EXECUTION_LOG_PATH="$PWD/.algolib/execution_audit.jsonl"
export ALGOLIB_SERVER_WORKERS=8
export ALGOLIB_SERVER_MAX_QUEUED_REQUESTS=64

./build/algolib_server \
  --host 127.0.0.1 \
  --port 8088
```

也可以通过命令行显式覆盖：

```bash
./build/algolib_server \
  --host 127.0.0.1 \
  --port 8088 \
  --registry "$PWD/.algolib/registry.json" \
  --execution-log "$PWD/.algolib/execution_audit.jsonl" \
  --workers 8 \
  --max-queued-requests 64
```

检查服务与并发配置：

```bash
curl -sS http://127.0.0.1:8088/health
```

响应中应包含：

```json
{
  "ok": true,
  "status": "ready",
  "runner_cache_size": 0,
  "worker_threads": 8,
  "max_queued_requests": 64
}
```

### 8.4 预热 Python HTTP 服务算法

在算法已经注册并激活后，可为目标部署显式加载 Runner 池：

```bash
curl -sS -X POST http://127.0.0.1:8088/load \
  -H 'Content-Type: application/json' \
  -d '{
    "algorithm_id": "<algorithm_id>",
    "version": "<version>",
    "backend_type": "python_http_service",
    "deploy_id": "<optional_deploy_id>",
    "pool_size": 4,
    "pool_checkout_timeout_ms": 5000
  }'
```

卸载采用 Draining 语义：

```bash
curl -sS -X POST http://127.0.0.1:8088/unload \
  -H 'Content-Type: application/json' \
  -d '{
    "algorithm_id": "<algorithm_id>",
    "version": "<version>",
    "backend_type": "python_http_service",
    "deploy_id": "<optional_deploy_id>"
  }'
```

返回 `unloaded` 表示缓存已摘除并已开始/完成 drain；它不表示强行中断在途请求。

---

## 9. 容量规划与运行建议

### 9.1 参数关系

| 参数 | 控制对象 | 建议 |
|---|---|---|
| `worker_threads` | HTTP Handler 并发度 | 至少覆盖期望的在线并发；不能低于关键模型池容量，否则池容量无法充分利用 |
| `max_queued_requests` | 入口排队压力 | 生产环境建议设置有界值，避免突发流量无限占用内存 |
| `pool_size` | 单个 Python HTTP 服务算法/部署的 Runner 并发度 | 根据上游 Python 服务 worker 数、GPU 显存和模型线程安全能力设置 |
| `pool_checkout_timeout_ms` | Runner 池满时的最大等待时间 | 应小于或等于上游网关/调用方可接受的超时预算 |
| `SingleFlightCache.max_entries` | work item 结果缓存上限 | 当前默认 1024；长期运行时需结合业务工作项唯一性评估 |

### 9.2 起始配置建议

在尚未获得压测数据前，可从以下保守配置开始：

```text
HTTP workers：8
HTTP max queued requests：64
单个 Python 模型 pool_size：2～4
pool checkout timeout：5000 ms 或小于调用链总超时
```

随后根据以下指标调整：

- HTTP 排队时间、请求超时率；
- Runner checkout 超时率；
- Python 服务的 P95/P99 时延；
- GPU/CPU 使用率与显存峰值；
- 资源级推理锁的等待时间；
- 同 work item 的命中率和结果缓存大小；
- 日志写入失败率。

### 9.3 预期行为说明

- 增加 HTTP Worker 数不一定线性提高模型吞吐；同一 Python 服务最终仍受 `pool_size` 与上游服务容量限制。
- 增加 `pool_size` 前应确认 Python 服务和模型实例可承受对应并发；否则可能出现显存抖动、上游排队或模型内部不安全并发。
- 同一 TIA 模型资源被 `inference_guard` 串行是设计使然。若确认某个模型线程安全、显存充足且有吞吐需求，应为该模型设计可验证的实例池，而不是直接删除资源锁。
- `/unload` 是优雅卸载，不是抢占式取消。若要求任务取消，需要在请求协议、Runner 和上游 Python 服务之间增加取消令牌和截止时间传播。

---

## 10. 已知边界、未覆盖项与后续建议

### 10.1 当前已知边界

1. **注册表和审计日志锁是进程内锁**：`persistence_mutex_` 和按路径日志锁不能协调多个独立进程同时写同一个文件。
2. **真实 ONNX Runtime 未在本次环境验证**：当前 `ALGOLIB_WITH_ONNXRUNTIME=OFF`，因此尚未覆盖真实 ORT/GPU 会话的高并发压力。
3. **Commander A2A 测试被依赖缺失跳过**：安装并配置 `a2a_protocol` 后，应补跑两个 Commander Single-Flight 场景。
4. **mission 锁不保证 FIFO 公平性**：它保证状态互斥一致性，不保证严格按网络到达顺序执行。
5. **结果缓存没有 TTL**：同一 `work_item` 会在缓存容量淘汰前复用结果。若工作项 ID 可被重复使用，应增加版本、输入哈希、TTL 或显式失效策略。
6. **锁表可能随高基数 Key 增长**：`_mission_locks`、模型 Key 锁和资源锁当前没有淘汰策略；长期多租户运行时应增加清理机制或使用有界锁管理器。
7. **缓存 generation 是全局代际**：高频、无关算法的失效也可能使其他正在初始化的旧 generation 加载返回需重试状态；可在未来演进为按 Key generation。
8. **审计日志失败不会使在线推理失败**：这是可用性优先设计，但应通过指标、告警或异步可靠投递补足可观测性。

### 10.2 建议的后续验证

| 优先级 | 建议 | 目的 |
|---|---|---|
| 高 | 在 CI 中增加 ThreadSanitizer / AddressSanitizer 构建 | 捕获数据竞争、UAF 和内存生命周期问题 |
| 高 | 添加 `Reload/Register/Activate/Load/Unload/Run` 混合并发压力测试 | 验证管理面与数据面混合时序 |
| 高 | 在安装 `a2a_protocol` 的集成环境补跑 Commander 测试 | 验证真实 A2A 基类接入后的 work item 幂等 |
| 高 | 使用真实 ONNX Runtime、目标 GPU/MPS 进行 Soak Test | 验证真实模型、设备与显存行为 |
| 中 | 增加 Runner 池指标 | 观察 `borrowed_count`、等待时长、checkout timeout、draining 耗时 |
| 中 | 增加每 Key 加载次数与 Single-Flight 等待指标 | 验证缓存击穿被有效抑制 |
| 中 | 对跨进程注册表改用文件锁、数据库或独立注册表服务 | 提供多实例部署下的一致性保证 |
| 中 | 为 mission 加入 sequence/版本校验 | 若业务要求严格有序，避免互斥锁公平性依赖 |
| 低 | 为缓存与锁表增加 TTL/容量治理 | 防止长期高基数请求导致元数据增长 |

---

## 11. 验收结论

本次改造已经完成从“单一全局服务锁”到“按资源分层并发控制”的架构升级：

1. C++ 服务端将管理操作与在线请求分离；
2. 注册表实现多读一写、候选快照提交和持久化防 lost update；
3. Runner 缓存实现 Single-Flight、generation 失效和 `shared_ptr` 安全所有权；
4. Python HTTP Runner 池实现容量控制、超时背压和 Draining 卸载；
5. 审计日志实现同路径多实例安全追加；
6. TIA 实现请求级缓存隔离、模型加载去重、资源级模型锁、mission 状态串行和 work item 去重；
7. C++ 聚合测试、重点并发池测试以及 Python 并发回归测试均已在当前 macOS 环境通过。

在当前验证范围内，改造满足在线并发、生命周期安全、状态隔离与基本回归的目标。投入更高并发或多进程生产环境前，应按第 10 节完成真实模型压测、TSAN/ASAN 验证、A2A 集成验证和跨进程一致性治理。
