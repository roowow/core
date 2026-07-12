# AI 聊天客服方案
监听不同频道的聊天，响应问题，提供陪聊，提供助手服务。

## 硬件环境

- GPU：RTX 4090D（24GB VRAM）
- 内存：64GB
- 系统：Linux

---

## 模型对比评估

### VRAM 与并发能力

| 模型 | 量化 | 权重大小 | 剩余给KV | KV/token | 并发@2K ctx | 并发@4K ctx | 原生上下文 |
|------|------|----------|----------|----------|------------|------------|----------|
| Qwen2.5-7B-Instruct | Q4_K_M | ~4.5GB | ~15.9GB | ~56KB | ~145 会话 | ~72 会话 | 128K |
| GLM-4-9B-Chat | Q4_K_M | ~5.5GB | ~14.9GB | ~40KB | ~190 会话 | ~95 会话 | 128K |
| Qwen3-14B-Instruct | Q4_K_M | ~9GB | ~11.4GB | ~384KB | ~15 会话 | ~7 会话 | 128K |
| Qwen2.5-32B-Instruct | Q4_K_M | ~20GB | ~0.4GB | — | 基本不可用 | — | 128K |

> **KV/token 计算公式**：`2 × num_layers × num_kv_heads × head_dim × 2bytes(FP16)`
>
> - Qwen2.5-7B：2×28×4×128×2 = 57,344 bytes
> - GLM-4-9B：2×40×2×128×2 = 40,960 bytes（KV heads 极少，并发最优）
> - Qwen3-14B：2×48×8×128×2 = 393,216 bytes（层数多，KV开销大）

### 质量与适用场景

| 模型 | 中文质量 | 指令遵循 | 客服适用度 | 推荐场景 |
|------|----------|----------|----------|---------|
| Qwen2.5-7B | ★★★★☆ | ★★★★☆ | ★★★★☆ | 高并发、简单问答 |
| GLM-4-9B | ★★★★☆ | ★★★★☆ | ★★★★☆ | 高并发、中等复杂度 |
| Qwen3-14B | ★★★★★ | ★★★★★ | ★★★★★ | 低并发、复杂业务逻辑 |
| Qwen2.5-32B | ★★★★★ | ★★★★★ | ✗ | 单卡24G不适合 |

### 结论推荐

**Qwen3-14B-Instruct Q4_K_M + Ollama**

- 质量最好，中文和指令遵循均为第一梯队
- Ollama 自动管理显存，无需手动调参

---

## System Prompt 分层结构

System prompt 由多层叠加，启动时加载，SIGHUP 热重载：

```
soul 文件（角色/性格定义）
    +
knowledge dir（服务器知识库 MD 文件，~8,400 tokens）
    +
今天日期 + 当前服务器名 + 当前阶段（config 注入）
    +
频道专属 prompt（按频道类型动态选择）
    +
玩家角色信息（等级/职业/种族/位置，每次请求注入）
    +
world DB 查询结果（仅私聊，按关键词实时查 MySQL）
```

知识库全量注入，无需 RAG（8,400 tokens，占 24576 context 的 34%）。

### 各频道 prompt 上下文逻辑

每个频道在共同基础层（soul + 知识库 + 日期 + 玩家信息）之上，有独立的触发条件、prompt 模板、历史来源和推理策略：

| 维度 | 私聊 | 小队 | 团队 | 公会 | 世界 |
|------|------|------|------|------|------|
| **触发条件** | 无过滤，全响应 | 唤醒门控¹ | 唤醒门控¹ | 唤醒门控¹ | 问句过滤² 或 @提及 |
| **prompt 模板** | `whisper_companion` | `group_wakeup` | `group_wakeup`³ | `guild_wakeup` | `world_question` / `world_summon` |
| **对话历史** | Redis 永久，最近 20 turn | 无 | Redis 滚动缓存，最近20条，按 group_id 隔离，解散清除 | 无 | 无；@提及时有最近50条世界频道滚动缓存 |
| **精华记忆** | MySQL 永久 + Redis 缓存，注入 system（独立一份） | Redis 活动摘要（TTL 1小时），注入 system，**不写 MySQL** | Redis session 摘要（TTL/解散清除），注入 system，**不写 MySQL** | MySQL 永久 + Redis 缓存，注入 system（按公会ID隔离，换公会=新记忆，不跟世界频道共享） | MySQL 永久 + Redis 缓存，注入 system（独立一份，不跟公会共享） |
| **冷却时间** | 无 | 30s/人 | 30s/人 | 无 | 120s/人；@提及单独 60s/人 |
| **think 模式** | 关（快速） | 关（快速） | 开（准确） | 开（准确） | 开（准确，低温 0.2） |
| **verify_grounded** | 否 | 否 | 是 | 是 | 是 |
| **回复预算** | 200 tokens | 200 tokens | 1500 tokens | 1500 tokens | 1500 tokens |
| **Ollama timeout** | 30s | 30s | 180s | 180s | 180s |

> ¹ **唤醒门控**：首次消息中提到 bot 名字才激活，此后本次会话持续响应。服务重启清零。  
> ² **问句过滤**：匹配 `？/吗/怎么/咋/哪里/如何/能否/有没有/在哪/什么时候/为什么/是否/几级/多少` 等关键词才进入推理。  
> ³ 团队与小队已拆分为独立处理器（`raid_chat` / `party_chat`），但目前共用 `group_wakeup` 模板；团队额外注入 Redis transcript 和 session 摘要作为上下文。

**设计原则**：
- 私聊：陪伴优先，自由对话，历史完整，不验证（内容误差风险低）
- 小队：快速响应，轻量，不验证（队友场景，容错高）
- 团队/公会：策略性场景，需要准确性，开启 think + 验证，接受更长延迟
- 世界：最严格，低温 + think + 验证，错误答案会被大量玩家看到

### 私聊上下文说明

每个玩家独立会话，陪伴优先，历史跨重启持久化：

| 维度 | 说明 |
|------|------|
| Redis key | `jianjia:hist:<bot_name>:<player>`，按玩家隔离 |
| Redis 生命周期 | 永久保留，无 TTL，不自动清除 |
| 内存 conv state | 30 分钟无操作后释放（`HISTORY_TTL=1800`），下次对话从 Redis 冷启动恢复，玩家无感知 |
| 存储量 | 逐轮积累，满 40 条（20 轮）时压缩触发；压缩后保留最近 20 条（10 轮）；紧急上限 80 条 |
| 注入量 | 全量历史注入（最多 ~4,000~8,000 token） |
| 历史持久化 | Redis 主存（永久），冷启动从 logs DB 恢复并写回 Redis |
| 精华记忆 | MySQL 主存（永久）+ Redis 缓存（1小时 TTL），每 20 轮异步压缩一次 |
| 压缩触发 | 回复成功后检查：历史满 40 条 → 快照 → 保留最近 20 条 → LLM 异步提炼精华记忆写入 MySQL |
| 玩家信息 | 等级/职业/种族/位置注入 system，重启后从 conv state 取 |

context 构成（约占 tokens）：

```
soul + 知识库（~8,400）
+ 日期/服务器信息（~50）
+ whisper_companion 模板（~100）
+ 精华记忆（~0~100）
+ 玩家信息（~50）
+ 对话历史（~4,000~8,000）
+ 当前消息 + 回复预留（~500）
= ~13,100~17,200 tokens（约 53%~70% context）
```

### 小队频道上下文说明

小队无持久历史，轻量快速，容错优先：

| 维度 | 说明 |
|------|------|
| 历史 | 小队 DB 短期历史（`_get_group_history`，按 `[Group:group_id]` 精确匹配），每次请求现查，不做长期存储 |
| 精华记忆 | **无 MySQL 永久存储**，仅 Redis 按 group_id 存一份短活动摘要（`jianjia:party_summary:<realm>:<group_id>`，1小时 TTL 自动过期），每 20 轮异步压缩一次，注入 system。跟私聊/公会/世界频道共用的"精华记忆"（per-player, MySQL 永久）完全独立、不共享——小队是临时组队场景，没必要给单个玩家建立跨会话长期画像，做法上对齐团队（raid）的 Redis-only 方案，但小队没有稳定的解散事件可用（`group_disband` 目前 C++ 侧还没有实际发出，raid 那边同样存在这个缺口），所以用 TTL 兜底自动过期，不依赖解散事件清理 |
| 唤醒 | bot 名字出现在消息中才激活，服务重启清零 |
| 冷却 | 30s/人（`_GROUP_REPLY_CD`），避免高频刷屏 |
| 玩家信息 | 小队不单独维护玩家信息，无对应参数传入（跟公会/世界频道不同，那两个是从消息里带的 `player_info` 参数直接注入） |

context 构成（约占 tokens）：

```
soul + 知识库（~8,400）
+ 日期/服务器信息（~50）
+ group_wakeup 模板（~150）
+ 精华记忆（~0~100）
+ 玩家信息（~50）
+ 当前消息（~50）
= ~8,700~8,800 tokens（约 35% context）
```

### 公会频道上下文说明

公会无持久历史，知识类回复为主，需要 think + 验证：

| 维度 | 说明 |
|------|------|
| 历史 | 无（无状态，每条消息独立） |
| 精华记忆 | MySQL 主存（永久）+ Redis 缓存（1小时 TTL），每 20 轮异步压缩一次，注入 system。scope=`"guild:<context_id>"`（按公会ID隔离，换公会=全新一份记忆），跟世界频道的 scope=`"world"` 各自独立存储，互不共享 |
| 唤醒 | bot 名字出现在消息中才激活，服务重启清零 |
| 冷却 | 无（公会频道消息频率低，无需节流） |
| 玩家信息 | 有则注入 system |

context 构成（约占 tokens）：

```
soul + 知识库（~8,400）
+ 日期/服务器信息（~50）
+ guild_wakeup 模板（~200）
+ 精华记忆（~0~100）
+ 玩家信息（~50）
+ 当前消息（~50）
= ~8,750~8,850 tokens（约 36% context）
```

### 团队频道上下文说明

团队可以同时有多个（不同 group_id），每个团队的上下文独立隔离：

| 维度 | 说明 |
|------|------|
| Redis key | `jianjia:raid:<realm>:<group_id>`，不同团队互不干扰 |
| 生命周期 | 无 TTL，设计上由游戏服务器发送 `group_disband` 事件触发 `DEL` 清理；**⚠️ 待确认：C++ 侧目前未实际发出该事件，transcript key 和 summary key 均不会被自动清除** |
| 存储上限 | 最多 100 条/团队（`LTRIM` 硬上限，防止长期副本无限增长） |
| 注入量 | 最近 20 条（`RAID_INJECT_LINES`），压缩去重后约 200~400 token |
| 记录时机 | **所有**团队消息都记录，包括 bot 未响应的（wake-up gate 过滤前） |
| 注入时机 | fetch 在 record 之前，当前消息不出现在 transcript 里（只作为 user 消息出现） |
| 解散清理 | 设计上由 `group_disband` 事件同时清除 transcript key、summary key 和内存唤醒状态；**⚠️ 同上，目前不生效** |
| session 摘要 | Redis 专属 key（`jianjia:raid_summary:<realm>:<group_id>`），**无 TTL**，依赖解散事件清除；若解散事件未到位，旧摘要会永久留存——如果 group_id 是自增复用的，下次同 ID 的团队会读到上次的装备竞拍记录。建议待 `group_disband` 事件接入后验证，或临时给 summary key 加 24 小时 TTL 兜底 |
| 摘要内容 | 只提炼装备竞拍/DKP 分配记录、灭团次数及鼓励、首杀庆贺；战斗流水不记录 |
| 压缩触发 | 每 20 轮 → 异步 LLM → 累积写入 Redis（不写 MySQL） |

context 构成（约占 tokens）：

```
soul + 知识库（~8,400）
+ 日期/服务器信息（~50）
+ raid prompt 模板（~150）
+ session 摘要（~0~100）
+ 玩家信息（~50）
+ 最近20条团队消息 transcript（~200~400）
+ 当前消息（~50）
= ~8,900~9,200 tokens（约 36%~37% context）
```

### 世界频道上下文说明

世界频道无会话历史，每次请求独立，context 占用最小：

| 场景 | context 构成 | 约占 tokens |
|------|-------------|------------|
| 普通问句 | soul+知识库 + 日期 + 玩家信息 + world_question 模板 + 当前消息 | ~9,200 |
| @提及唤醒 | 同上，替换为 world_summon 模板 + 最近50条世界频道消息（压缩后） | ~9,600 |

**滚动缓存**：Redis List 持久化，TTL=2小时，最多存 200 条（`LTRIM` 硬上限）。@提及时注入最近 50 条，注入前压缩去重（连续相同内容合并为 `A/B（×3）：内容`）。Redis 不可用时降级为内存 deque。

- 每条原始消息约 20 token；50 条压缩后约 400~600 token
- 2小时覆盖同一玩家的完整在线时段，重启后仍有上下文
- 普通问句不注入历史（无状态，单次请求）；@提及时才拉取

**精华记忆**：每次成功回复后静默积累到 conv state（scope=`"world"`，跟公会的 scope=`"guild:<context_id>"` 各自独立、互不共享），满 20 轮异步压缩写入 MySQL/Redis，并注入 system prompt（被动问答、@提及召唤两条路径都会读取）。

## 底座架构方案

```
游戏服务器
    ↓ publish
Redis pub/sub（jianjia_in:<realmId>）
    ↓ subscribe
jianjia_chat.py（ThreadPoolExecutor，max 50 线程）
    ├── whisper      → 私聊，有持久化对话历史，陪伴为主
    ├── party_chat   → 小队，唤醒门控，快速响应，不验证
    │     └── quest_complete → 任务完成鼓励（小队频道发言）
    ├── raid_chat    → 团队，唤醒门控，think+验证，策略性回复
    ├── guild_chat   → 公会，唤醒门控，think+验证，知识类回复
    ├── world_chat   → 世界，问句过滤/@提及，think+验证，低温严格
    └── bg_afk       → 战场 AFK 警告
    ↓ publish
Redis pub/sub（jianjia_out:<realmId>）
    ↓
游戏服务器
```

### 关键组件

| 组件 | 说明 |
|------|------|
| **Ollama** + qwen3:14b | 推理服务，num_ctx=24576，串行处理（信号量=1） |
| **Redis** pub/sub | 游戏服务器与 AI 服务的消息通道 |
| **Redis** history | 私聊对话历史持久化，永久（无 TTL） |
| **Redis** memory cache | 精华记忆缓存，TTL 1小时，冷启动从 MySQL 回填 |
| **MySQL** world DB | 物品/任务/NPC 实时查询，辅助私聊回复 |
| **MySQL** logs DB | 冷启动时从聊天记录恢复私聊历史 |
| **MySQL** jianjia_player_memory | 精华记忆永久存储，每玩家一行，LLM 异步压缩写入 |
| soul 文件 | 角色性格定义，热重载 |
| knowledge dir | 服务器知识库 MD 文件，热重载 |
| jianjia_prompts/ | 各频道专属 prompt 模板，热重载 |

---

## 上下文分配策略

`num_ctx=24576`，各频道 context 构成不同：

| 部分 | 私聊 | 小队 | 团队 | 公会 | 世界 |
|------|------|------|------|------|------|
| soul + 知识库 | ~8,400 | ~8,400 | ~8,400 | ~8,400 | ~8,400 |
| 日期/服务器信息 | ~50 | ~50 | ~50 | ~50 | ~50 |
| 频道 prompt 模板 | ~100 | ~150 | ~150 | ~200 | ~200 |
| 玩家信息 | ~50 | ~50 | ~50 | ~50 | ~50 |
| world DB 查询 | ~200 | — | — | — | — |
| 精华记忆 | ~0~100 | ~0~100 | — | ~0~100 | ~0~100 |
| 对话历史 | ~4,000~8,000 | DB 最近10条 ~200 | — | — | — |
| 团队 transcript | — | — | ~200~400 | — | — |
| 团队 session 摘要 | — | — | ~0~100 | — | — |
| 世界滚动缓存 | — | — | — | — | @提及时 ~400~600 |
| 当前消息 + 回复预留 | ~500 | ~500 | ~500 | ~500 | ~500 |
| **合计** | **~13,300~17,400** | **~9,450** | **~9,300~9,650** | **~9,300** | **~9,200~9,800** |

总占用约 37%~71%，私聊历史较长时最高。**原则：知识固定全量，历史滚动截断，超出由压缩机制消化。**

---

## 快速起步

```bash
# 安装 Ollama，拉取模型
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen3:14b

# 启动服务（jianjia.toml 配置好后）
python3 jianjia_chat.py

# 热重载 soul/knowledge/prompts（无需重启）
python3 jianjia_chat.py --reload
```

## 建议步骤

1. 安装 Ollama，拉取 `qwen3:14b`
2. 编辑 `jianjia.toml`（参考 `jianjia.toml.example`），配置 Redis、MySQL、soul 文件路径
3. 编写 soul 文件（角色性格定义）
4. 将知识 MD 文件放入 `jianjia_knowledge/` 目录
5. 按需调整 `jianjia_prompts/` 下的频道 prompt 模板
6. 启动 `jianjia_chat.py`，观察启动日志中 context 占用百分比
