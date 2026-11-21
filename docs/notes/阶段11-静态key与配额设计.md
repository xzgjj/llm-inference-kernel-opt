# 阶段 11 设计稿：静态 JSON key 与每 key 配额

- 日期：2026-11-21
- 状态：已实现（2026-11-21）；D1–D4 均按推荐方案确认，代码、单测、契约与集成已落地
- 范围：SPEC-9 多租户 P2；只做静态 key（启动加载，不热更新），不引入数据库、OIDC 或动态签发。

## 1. 现状与缺口

- `auth.hpp` 只有一个全局 `--api-key` / `LLMOPT_API_KEY`，`bearer_allowed` 做定长比较；空值放行。
- `Config` 已有全局 `max_concurrency`、`queue_capacity`、`rate_limit_rps/burst`；没有租户维度。
- `KVBlockManager` 是单池、引用计数 + 前缀哈希；`lookup_prefix` 不看请求归属，跨请求可共享只读块。
- 准入在 `SchedulerCore` 单线程完成；拒绝原因已有 `QueueFull`、`RateLimited` 等错误面。

## 2. 待你确认的四项决策

### D1 key 格式与落盘

推荐：客户端 key 为 `lk_` + 32 位小写十六进制（128 bit，35 字符）；JSON 只存完整 key 的 SHA-256，不存明文。

```json
{
  "version": 1,
  "keys": [
    {
      "id": "tenant-a",
      "sha256": "<64 hex>",
      "quota": {
        "max_concurrency": 4,
        "queue_capacity": 8,
        "rate_limit_rps": 2.0,
        "rate_limit_burst": 4,
        "kv_blocks": 64
      }
    }
  ]
}
```

- `id` 是租户标识，也是指标标签；限 `[a-z0-9-]{1,32}`，从源头控制标签基数。
- 启动时校验：版本、重复 `id`、重复 `sha256`、未知字段、负数/越界；失败退出 3，错误体只报行号与字段名，不打印 key。
- 文件权限要求 0600（Linux）；不满足时**拒绝启动**（已确认，Windows 跳过权限检查）。
- 轮换：v0 **不允许同一 `id` 出现多条**（`id` 与 `sha256` 均去重）；新旧 key 重叠期后续版本再放开。

备选：JSON 存明文 key。运维直观，但配置与备份即泄露面，不推荐。

### D2 每 key 配额维度

推荐 v0 四项：`max_concurrency`（在飞请求）、`queue_capacity`（排队上限）、`rate_limit_rps/burst`（复用现有令牌桶）、`kv_blocks`（该租户已占块上限）。

- 全局 `max_concurrency/queue_capacity` 仍是总上限，租户配额是子上限，取两者较小值。
- 暂不做 tokens/min 或每日 token 配额：需要时间窗口、持久计数与结算口径，容易变成计费系统；若你要，单独定窗口与重置语义。

### D3 KV / 前缀隔离边界

推荐：单池 + 块级租户标记 + 前缀哈希域分离。

- `lookup_prefix` 的哈希输入改为 `hash(tenant_id || token_ids)`，从根上避免跨租户命中；即使同一 prompt 也不共享。
- 每个块记录 `tenant_id`；`attach_prefix` / 共享只读块只允许同租户。
- 淘汰可回收任意租户的缓存块（不涉及在飞块），回收后回到全局池；不做物理硬分区，避免碎片与利用率下降。
- 准入时校验 `tenant.used_blocks + need <= quota.kv_blocks` 且全局可分配；拒绝不产生任何块分配副作用。
- 代价：跨租户相同系统提示词不再命中，前缀命中率下降；这是隔离的必然成本。

备选：每租户独立块池。隔离最硬，但容量按租户切分、难以弹性，v0 不做。

### D4 与现有单 key 的兼容

推荐：`--api-keys-file` 与 `--api-key` 互斥；`--api-key` 等价于单租户 `default` + 全局配额，保持现有部署不变。两者同时给出时启动失败并提示二选一。

## 3. 调度器改动点（确认后才动）

1. HTTP 层解析 Bearer → `(tenant_id, quota)`；未知/缺失 key 回 401，不进入队列。
2. `Request` 携带 `tenant_id`；`SchedulerCore` 维护每租户 `inflight/queued/used_blocks` 计数。
3. 准入顺序：key → 每租户限流 → 每租户并发/队列/KV 配额 → 全局准入。任一步拒绝都不分配块、不扣预算。
4. 收尾/取消/抢占释放走同一 lease，保证租户计数只减一次；用 RAII 防漏减。
5. 组批保持全局 FIFO；v0 只靠每租户 `queue_capacity` 防独占，加权公平留到下一阶段。

错误面：

| 场景 | HTTP | code | retryable |
| --- | --- | --- | --- |
| 缺/错 key | 401 | `UNAUTHORIZED` | false |
| 租户并发/队列/KV 配额满 | 429 | `QUOTA_EXCEEDED` | true |
| 租户限流 | 429 | `RATE_LIMITED` | true |
| 全局队列满 | 429 | `QUEUE_FULL` | true |
| key 文件非法 | 启动退出 3 | `CONFIG_INVALID` | — |

指标（标签只取配置内的 `id`）：

- `llmopt_tenant_requests_total{tenant,status}`
- `llmopt_tenant_inflight{tenant}`
- `llmopt_tenant_queue_depth{tenant}`
- `llmopt_tenant_kv_used_blocks{tenant}`
- `llmopt_tenant_quota_rejected_total{tenant,reason}`

日志只写 `tenant=<id>`，不写 key 或 hash。

## 4. 用例与验收（TDD 顺序）

- unit：JSON 解析（缺字段/重复 id/重复 hash/坏 hex/越界）、key 定长比较、配额算术、哈希域分离、lease 只减一次、标签字符集。
- contract：两租户同前缀零共享；租户 KV 配额满拒绝且块数不变；取消后租户计数与块全归还。
- integration：两 key 并发，A 打满并发时 A=429/B=200；错 key=401；指标标签与拒绝原因；`--api-key` 旧行为不变。
- e2e：沿用 `nightly_real_backend.py`，再加一条双 key 冒烟；不替代五项检查。
- 回归：现有 `auth_test`、`admission_test`、`preempt_test`、`lifecycle_test` 全绿；`run_ci.py --mode pr` 通过。

## 5. 已确认结论与遗留

1. key 用 `lk_` + 32 hex、JSON 只存 SHA-256：已确认并实现。
2. 配额先做并发/队列/RPS/KV 块四项：已确认；token 配额暂缓，`kv_blocks=0` 表示不限。
3. KV 采用单池 + 租户标记 + 哈希域分离：已确认并实现；不做硬分区。
4. `--api-key` 保留为单租户 `default` 的简写，与 `--api-keys-file` 互斥：已确认并实现。

遗留：e2e 双 key 冒烟未加（nightly 仍守单租户五项）。token 日配额、key 热加载、同 id 多摘要轮换经用户确认**暂不实施**，推荐契约与状态记在 `docs/notes/阶段11-实现记录.md` 第 6 节。
