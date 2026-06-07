# LDP CLI 文档

LDP 模块（module-id: 11）提供全局协议开关、LSR ID、定时器、接口启用和状态展示。

## 全局命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ldp` | config | 创建或进入 LDP 视图 |
| `no ldp` | config | 删除 LDP 配置 |

LDP 视图提示符为 `<NetNexus(config-ldp)>`。

## LDP 视图命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `lsr-id <ip>` | ldp | 配置 LSR ID |
| `no lsr-id` | ldp | 删除 LSR ID |
| `hello-interval <ms>` | ldp | 配置全局 hello interval |
| `no hello-interval` | ldp | 恢复默认 hello interval |
| `hold-time <ms>` | ldp | 配置全局 hold time |
| `no hold-time` | ldp | 恢复默认 hold time |
| `keepalive-interval <ms>` | ldp | 配置 keepalive interval |
| `no keepalive-interval` | ldp | 恢复默认 keepalive interval |

## 接口命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ldp enable` | if, if-loop | 在接口启用 LDP |
| `no ldp enable` | if, if-loop | 在接口关闭 LDP |
| `ldp hello-interval <ms>` | if, if-loop | 配置接口 hello interval |
| `no ldp hello-interval` | if, if-loop | 恢复接口 hello interval |
| `ldp hold-time <ms>` | if, if-loop | 配置接口 hold time |
| `no ldp hold-time` | if, if-loop | 恢复接口 hold time |

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show ldp protocol` | global | 显示 LDP 协议状态 |
| `show ldp interface` | global | 显示 LDP 接口状态 |
