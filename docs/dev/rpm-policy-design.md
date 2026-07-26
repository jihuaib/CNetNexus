# RPM 路由策略设计

## 模块边界

RPM 是策略的唯一配置与持久化入口，业务模块只负责消费：

```text
CLI / DB -> RPM -> 查询或事件 -> BGP 等业务模块 -> 路由求值
```

公共协议位于 `include/rpm.h`。策略用途、匹配条件和动作都使用 bitmask，避免为每个新用途或字段复制一套 IPC：

- `type_mask`：策略用途；当前支持 `BGP_EXPORT`，已预留 `BGP_IMPORT` 和 `REDISTRIBUTE`。
- `match_mask`：节点实际配置的匹配条件；当前支持 `PREFIX`。
- `apply_mask`：节点实际配置的属性动作；当前支持 `LOCAL_PREF`、`MED` 和 `COMMUNITY`。

业务模块用 `rpm_api_subscribe(type_mask, flags)` 订阅自己关心的用途。RPM 只推送与订阅位图有交集的策略；`REPLAY` 标志先回放当前策略快照，再发送 `SMOOTH_END`。业务配置引用策略前必须用 `rpm_api_policy_get(name, required_type_mask)` 做同步存在性和类型校验。

`type_mask` 表示“此策略可被哪些消费场景使用”，不是求值阶段的条件。因此，策略类型用 bitmask 是合适的；业务模块不需要解析不属于自己的策略。

## 求值语义

- 节点按 sequence 升序。
- 节点之间是 OR，首个匹配节点终止。
- 同一节点内不同种类的 `if-match` 是 AND。
- 无 `if-match` 的节点匹配所有路由。
- `permit` 执行动作并允许，`deny` 拒绝且不执行动作。
- 所有节点均未匹配时隐式 deny。

这种语义与主流设备的 route-policy / route-map / policy-statement 模型一致，也便于后续增加 prefix-list、community-list、AS-path、协议来源、下一跳等匹配器。

## 为什么需要 `if-match`

需要。只有 `apply` 而没有 `if-match` 的模型只能对全部出口路由统一改属性，无法实现“只发布某些前缀”“不同前缀设置不同属性”“显式兜底拒绝”等常见出口控制。

第一阶段保留无条件节点，同时实现最小的前缀 `if-match`，足以打通：

```text
route -> match -> permit/deny -> modify attributes -> Adj-RIB-Out
```

后续扩展建议优先复用独立对象，而不是把大型列表复制进 RPM 节点：

1. `if-match ip-prefix <name>`：引用 prefix-list，支持 exact / longer / or-longer。
2. `if-match community <name>`、`if-match as-path <name>`。
3. `apply community ... additive`、AS-path prepend、next-hop。
4. 需要多动作累计时，再引入显式 `continue`；默认仍保持首个命中终止，避免改变已有策略语义。

## 生命周期与一致性

- 新增业务绑定：不存在或类型不兼容时拒绝写入。
- 策略修改：RPM 增加 revision 并推送完整快照，BGP 撤销旧 Adj-RIB-Out、重分组并重新计算。
- 已引用策略被删除：RPM 允许删除，消费者 fail-closed。这样策略模块不反向依赖所有业务数据库，也不会在事件丢失或启动顺序变化时误放行路由。
- 重启恢复：RPM 先从 DB 恢复；BGP 恢复引用时重新查询 RPM。找不到的历史悬空引用仍保留名称并 fail-closed，便于诊断和同名策略重建。
- `startup/cfg` 回放：完整 BGP BDR 使用配置 anchor 延后渲染，保证 RPM 策略定义早于 BGP 引用；否则 module-id 排序会让 BGP(6) 早于 RPM(18)，与强引用校验冲突。

## 厂商模型对照

| 厂商 | 策略结构 | 默认/空条件语义 | 对当前设计的启示 |
| --- | --- | --- | --- |
| Huawei | `route-policy` 由有序 node 组成，node 内 `if-match` + `apply` | node 间 OR、同 node 条件 AND；空条件匹配所有；未命中拒绝 | 当前 CLI 和首命中模型直接对齐 |
| Cisco | route-map 由有序 sequence 组成，`match` + `set` | 无 match 的 sequence 匹配全部；末尾隐式 deny；可选 continue | 第一阶段无需 continue，保留未来扩展点 |
| Juniper | `policy-statement` 由有序 term 组成，`from` + `then`，通过 export 引用 | term 无 `from` 可匹配全部；策略链有显式 accept/reject/next | 策略定义与协议引用解耦、按用途消费是合理边界 |

当前没有照搬某一家厂商的全部语法，重点保持一致的求值直觉和清晰的模块边界。
