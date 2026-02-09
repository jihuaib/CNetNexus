# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在本仓库中工作时提供指导。

## 构建命令

```bash
# 安装依赖 (Ubuntu/Debian)
sudo apt install build-essential cmake libxml2-dev libsqlite3-dev pkg-config

# 开发依赖（可选）
sudo apt install gdb inotify-tools  # 用于调试和文件监控

# 构建
mkdir build && cd build && cmake .. && make

# 构建并运行
cmake --build . --target run

# 清理
rm -rf build
```

## 开发工作流

### 快速开始

```bash
# 1. 构建
./scripts/dev/build.sh

# 2. 运行
./scripts/dev/start.sh

# 3. 连接
telnet localhost 3788
```

### 开发脚本

所有开发脚本位于 `scripts/` 目录：

**构建和清理：**
```bash
./scripts/dev/build.sh              # 快速构建（Debug 模式）
./scripts/dev/build.sh --release    # Release 构建
./scripts/dev/build.sh --clean      # 清理 + 重新构建
./scripts/dev/build.sh -j 8         # 8 线程并行构建

./scripts/dev/clean.sh              # 仅清理 build 目录
./scripts/dev/clean.sh --data       # 同时清理 data 目录
./scripts/dev/clean.sh --all        # 清理所有内容
```

**运行和调试：**
```bash
./scripts/dev/start.sh              # 以开发模式启动
./scripts/dev/debug.sh              # 使用 gdb 调试器启动
```

**GDB 调试会话示例：**
```bash
./scripts/dev/debug.sh

# 在 GDB 中：
(gdb) break main                    # 设置断点
(gdb) run                           # 启动程序
(gdb) continue                      # 继续执行
(gdb) backtrace                     # 显示调用栈
(gdb) print variable_name           # 查看变量
(gdb) quit                          # 退出
```

### VSCode 集成

在 VSCode 中打开项目，支持完整的调试功能：

**构建任务 (Ctrl+Shift+B)：**
- Build Debug（默认）
- Build Release
- Clean
- Rebuild（清理 + 构建）
- Watch（修改后自动重建）

**调试配置 (F5)：**
- Debug NetNexus - 构建并调试
- Attach to NetNexus - 附加到运行中的进程
- Run NetNexus (No Debug) - 不调试直接运行

**快捷键：**
- `Ctrl+Shift+B` - 构建
- `F5` - 开始调试
- `Ctrl+F5` - 不调试直接运行
- `Shift+F5` - 停止调试
- `F9` - 切换断点
- `F10` - 单步跳过
- `F11` - 单步进入

### 开发环境

**目录结构：**
```
NetNexus/
├── src/              # 源代码（XML 配置自动发现）
├── include/          # 公共头文件
├── build/            # 构建输出
│   ├── bin/         # 可执行文件
│   └── lib/         # 库文件
├── data/             # 开发数据库存储
├── scripts/          # 开发和部署脚本
└── .vscode/          # VSCode 配置
```

**环境变量：**
- `LD_LIBRARY_PATH` - 自动设置为 `build/lib/`
- `NN_RESOURCES_DIR` - 开发环境无需设置（从 `src/` 自动发现）

**配置文件：**
XML 配置文件从源码目录自动发现：
- `src/cfg/commands.xml`
- `src/dev/commands.xml`
- `src/bgp/commands.xml`
- `src/db/commands.xml`

**数据库文件：**
开发数据库存储在 `data/` 目录：
```
data/
└── bgp/
    └── bgp_db.db
```

保存 `.c`、`.h` 或 `.xml` 文件时，watch 脚本会自动：
1. 检测变更
2. 重新构建项目
3. 显示构建结果

重启 NetNexus（终端 2）即可查看更改效果。

### 调试技巧

**内存泄漏：**
```bash
# 使用 AddressSanitizer 构建
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" ..
make
./bin/netnexus
```

**Valgrind：**
```bash
valgrind --leak-check=full --show-leak-kinds=all \
  ./build/bin/netnexus
```

**Core Dump：**
```bash
# 启用 core dump
ulimit -c unlimited

# 运行并崩溃
./build/bin/netnexus

# 分析 core dump
gdb ./build/bin/netnexus core
```

**详细日志：**
添加调试打印或使用 gdb 跟踪执行：
```c
printf("[DEBUG] %s:%d - Variable: %d\n", __FILE__, __LINE__, var);
```

### 常见开发任务

**添加新模块：**
1. 创建 `src/mymodule/` 目录
2. 添加带有 constructor 的 `nn_mymodule_main.c`
3. 添加定义 CLI 命令的 `commands.xml`
4. 添加 `CMakeLists.txt`
5. 更新 `src/CMakeLists.txt` 以包含新模块
6. 构建并测试

**添加新 CLI 命令：**
1. 编辑 `src/{module}/commands.xml`
2. 添加元素定义
3. 添加命令表达式
4. 在模块代码中添加命令处理函数
5. 重新构建并测试

**调试崩溃：**
1. 使用调试符号构建：`./scripts/dev/build.sh`
2. 使用 gdb 运行：`./scripts/dev/debug.sh`
3. 设置断点：`break suspicious_function`
4. 运行：`run`
5. 分析：`backtrace`、`print`、`info locals`

**测试数据库变更：**
```bash
# 查看数据库
sqlite3 data/bgp/bgp_db.db ".schema"
sqlite3 data/bgp/bgp_db.db "SELECT * FROM bgp_protocol;"

# 重置数据库
rm -rf data/
./scripts/dev/start.sh  # 会自动重新创建
```

### 性能分析

**使用 perf：**
```bash
# 记录
perf record -g ./build/bin/netnexus

# 分析
perf report
```

**使用 gprof：**
```bash
# 使用性能分析选项构建
cd build
cmake -DCMAKE_C_FLAGS="-pg" ..
make

# 运行
./bin/netnexus

# 分析
gprof ./bin/netnexus gmon.out > analysis.txt
```

## 代码质量

```bash
./scripts/format-code.sh       # 使用 clang-format 格式化所有代码
./scripts/check-format.sh      # 验证格式
./scripts/run-clang-tidy.sh    # 运行静态分析
```

## 运行

```bash
cd build/bin && ./netnexus     # 在端口 3788 启动服务器
telnet localhost 3788          # 连接到 CLI
```

## 构建产物

构建完成后生成以下产物：
- `build/bin/netnexus` - 主可执行文件
- `build/lib/libnn_cfg.so` - CLI 库（CLI 框架）
- `build/lib/libnn_utils.so` - 工具库
- `build/lib/libnn_db.so` - 数据库模块（SQLite 存储）
- `build/lib/libnn_bgp.so` - BGP 模块
- `build/lib/libnn_dev.so` - Dev 模块

## 架构

NetNexus 是一个模块化的 Telnet CLI 服务器，用于网络协议管理。核心架构概念：

### 模块系统
模块在加载时通过 `__attribute__((constructor))` 自注册。每个模块提供：
- 调用 `nn_cli_register_module(name, xml_path)` 进行注册
- 定义命令的 XML 配置文件（位于模块目录中）

参见 [nn_dev_module.c](src/dev/nn_dev_module.c) 了解模式。

### 视图层级
视图代表 CLI 模式（USER、CONFIG、BGP 等）。每个视图包含：
- 独立的命令树
- 带有 `NetNexus` 占位符的提示符模板
- 可选的父视图用于继承

视图和命令在 XML 配置文件中定义，而非 C 代码。

### 命令树
命令是层级树结构，从根到叶的路径构成完整命令。节点类型：
- `KEYWORD`：固定令牌（如 "show"、"config"）
- `ARGUMENT`：可变参数（如 `<hostname>`）

每个树节点有一个 `is_end_node` 标志，表示是否为有效的命令结束点。这允许 "show bgp" 和 "show bgp peer" 同时作为有效命令——"bgp" 节点被标记为结束节点，即使它有子节点。

从 XML `<expression>` 元素构建命令时，每个表达式中的最后一个元素由解析器自动标记为结束节点。

### CLI 输入处理
CLI 支持完整的行编辑和光标定位：
- **ANSI 转义序列**：使用状态机检测方向键（STATE_NORMAL → STATE_ESC → STATE_CSI）
- **光标移动**：上/下方向键浏览命令历史；左/右方向键在当前行内移动光标
- **行内编辑**：可以在任意光标位置插入或删除字符，使用 `memmove()` 移动缓冲区内容
- **Tab/帮助**：基于光标位置工作（仅使用光标前的文本进行匹配）
- **历史记录**：会话级（20 条命令）和全局（200 条命令）历史，包含时间戳和客户端 IP

[nn_cli_handler.c](src/cfg/nn_cli_handler.c) 中的关键函数：
- `handle_arrow_up/down/left/right()` - 方向键处理函数
- `redraw_from_cursor()` - 行内编辑后重绘
- `nn_cli_session_history_*()` - 会话历史管理
- `nn_cli_global_history_*()` - 全局历史（带 pthread 互斥锁）

### 目录结构
```
src/
├── main.c                      # TCP 服务器、线程、信号处理
├── cfg/                        # CLI 库 (libnn_cfg.so)
│   ├── nn_cli_handler.c/h      # 客户端会话、命令执行
│   ├── nn_cli_history.c/h      # 命令历史管理
│   ├── nn_cli_tree.c/h         # 命令树匹配
│   ├── nn_cli_view.c/h         # 视图层级管理
│   ├── nn_cli_element.c/h      # CLI 元素处理
│   ├── nn_cli_xml_parser.c/h   # XML 配置解析
│   └── commands.xml            # 核心 CLI 命令
├── utils/                      # 工具库 (libnn_utils.so)
│   └── nn_path_utils.c/h       # 路径工具
├── db/                         # 数据库模块 (libnn_db.so)
│   ├── nn_db_main.c/h          # 模块生命周期
│   ├── nn_db_registry.c/h      # 数据库定义存储
│   ├── nn_db_schema.c          # Schema 管理
│   ├── nn_db_api.c             # CRUD 操作
│   └── commands.xml            # 数据库模块配置
├── interface/                  # 接口定义
├── bgp/                        # BGP 模块 (libnn_bgp.so)
│   ├── nn_bgp_module.c/h
│   └── commands.xml
└── dev/                        # Dev 模块 (libnn_dev.so)
    ├── nn_dev_module.c/h
    └── commands.xml
```

### 库依赖
```
netnexus (可执行文件)
├── libnn_cfg.so (CLI 框架)
│   └── libxml2, pthread, libnn_db
├── libnn_utils.so (工具库)
├── libnn_db.so (数据库模块)
│   └── sqlite3
├── libnn_bgp.so (BGP 模块)
└── libnn_dev.so (Dev 模块)
```

### 全局变量
- `g_nn_cfg_local->view_tree`：视图层级的根节点

## 命名规范

### 文件命名
- 文件名格式：`nn_{module}_xxx.c` / `nn_{module}_xxx.h`
- `{module}` 为模块名，如 `cfg`、`bgp`、`dev`、`db`、`if`
- 示例：`nn_bgp_cli.c`、`nn_db_main.h`、`nn_if_map.c`

### 函数和结构体命名
- 函数名格式：`nn_{module}_xxx()`
- 结构体名格式：`nn_{module}_xxx_t`
- `{module}` 必须与文件所属模块一致
- 示例：`nn_bgp_cli_handle_cmd()`、`nn_db_connection_t`
- 模块内部的静态函数可省略 `nn_{module}_` 前缀

### 全局变量命名
- 全局变量格式：`g_nn_{module}_xxx`
- 示例：`g_nn_cfg_local`、`g_nn_bgp_running`

### 其他命名规则
- `cmd_*`：命令处理函数（CLI 回调）
- `*_t`：类型定义后缀
- `UPPER_CASE`：宏和枚举
- `lower_case`：函数、变量、结构体

## 添加新命令

命令在每个模块目录的 XML 文件（`commands.xml`）中定义。XML 结构使用：

1. **元素** - 定义关键字和参数：
```xml
<element id="1" type="keyword">
    <name>show</name>
    <description>显示信息</description>
</element>
<element id="2" type="parameter">
    <name>&lt;hostname&gt;</name>
    <type>string(1-63)</type>
    <description>系统主机名</description>
</element>
```

2. **命令** - 将元素组合为表达式：
```xml
<command>
    <expression>1 2</expression>  <!-- 引用元素 ID -->
    <views>3</views>               <!-- 命令可用的视图 ID -->
    <view-id>4</view-id>          <!-- 可选：执行后切换到的视图 -->
</command>
```

每个表达式中的最后一个元素自动标记为 `is_end_node`。对于需要在中间节点执行的命令（例如 "show bgp" 和 "show bgp peer" 都有效），需要创建不同表达式长度的独立命令条目。

## 代码风格

- C11，基于 LLVM 的格式化，Allman 风格大括号
- 4 空格缩进，120 字符行宽限制
- 指针右对齐（`char *ptr`）
- 所有控制语句必须使用大括号

## 代码规范限制

### 禁止跨模块直接包含头文件
- 模块之间**不允许**直接 `#include` 其他模块的头文件
- 例如：`src/if/` 下的代码**不能** `#include "nn_bgp_cli.h"`，`src/bgp/` 下的代码**不能** `#include "nn_if_cli.h"`
- 模块间通信必须通过 `include/` 下的公共接口（如 `nn_cfg.h`、`nn_dev.h`、`nn_errcode.h`）或消息机制（pub/sub）
- `include/` 目录下的头文件是跨模块共享的公共接口，所有模块均可包含
- 模块内部的头文件（如 `nn_if_cli.h`、`nn_bgp_cli.h`）仅限本模块内部使用

### 注释和文档语言统一使用中文
- 所有代码注释（包括行注释 `//` 和块注释 `/* */`）必须使用中文
- Doxygen 风格注释（`@brief`、`@param`、`@return` 等）的描述文本使用中文
- 文档文件（`.md` 等）使用中文
- 代码中的 `printf`/日志输出、变量名、函数名等仍使用英文

### 新增文件必须包含文件注释
所有新建的 `.c` 和 `.h` 文件必须在文件头部添加注释，格式如下：
```c
/**
 * @file   nn_xxx_yyy.c
 * @brief  简要描述文件功能
 * @author 作者
 * @date   创建日期
 */
```

### include/ 目录下的公共 API 必须添加注释
`include/` 目录下的头文件中，所有公共函数声明、结构体定义、宏定义和枚举必须添加注释说明：
```c
/**
 * @brief 函数功能简要描述
 * @param param1 参数1说明
 * @param param2 参数2说明
 * @return 返回值说明
 */
int nn_xxx_function(int param1, const char *param2);

/**
 * @brief 结构体用途说明
 */
typedef struct nn_xxx
{
    int field1;    /**< 字段说明 */
    char name[64]; /**< 字段说明 */
} nn_xxx_t;

/** 宏定义说明 */
#define NN_XXX_MAX_SIZE 1024
```

## 部署

### 创建部署包

创建包含所有二进制文件和配置文件的部署包：

```bash
# 先构建项目
mkdir build && cd build
cmake .. && make
cd ..

# 创建部署包
./scripts/package.sh

# 输出：package/netnexus-1.0.0.tar.gz
```

部署包包含：
- 二进制文件（`bin/netnexus`）
- 库文件（`lib/libnn_*.so`）
- 配置文件（`config/*/commands.xml`）
- 部署脚本

### 生产环境部署

部署到 `/opt/netnexus`：

```bash
# 解压部署包
tar xzf netnexus-1.0.0.tar.gz
cd netnexus-1.0.0

# 部署（需要 sudo）
sudo ./scripts/deploy.sh
```

部署脚本功能：
- 安装到 `/opt/netnexus`
- 复制配置文件到 `/opt/netnexus/resources/`
- 保留现有配置（创建 `.bak` 备份）
- 安装 systemd 服务
- 设置环境变量

### 服务管理

```bash
# 启动服务
sudo systemctl start netnexus

# 开机自启
sudo systemctl enable netnexus

# 查看状态
sudo systemctl status netnexus

# 查看日志
sudo journalctl -u netnexus -f

# 手动启动
sudo /opt/netnexus/bin/start.sh
```

### Docker 部署

使用 Docker 构建和运行：

```bash
# 构建 Docker 镜像
docker build -t netnexus:latest .

# 使用 docker-compose 运行
docker-compose up -d

# 查看日志
docker-compose logs -f

# 停止
docker-compose down
```

Docker 配置：
- 暴露端口：`3788`（telnet）
- 持久化数据：`/opt/netnexus/data`（卷）
- 配置目录：`/opt/netnexus/resources`
- 环境变量：`NN_RESOURCES_DIR=/opt/netnexus/resources`

### 配置路径解析

系统按以下优先级解析 XML 配置文件：

1. **环境变量** `NN_RESOURCES_DIR`：`/opt/netnexus/resources/{module}/commands.xml`
2. **生产路径**：`/opt/netnexus/resources/{module}/commands.xml`
3. **开发路径**：`build/bin/../../src/{module}/commands.xml`
4. **回退路径**：`../../src/{module}/commands.xml`

### 数据库存储

SQLite 数据库存储位置：
- **开发环境**：`./data/{module}/{db_name}.db`
- **生产环境**：`/opt/netnexus/data/{module}/{db_name}.db`（通过环境变量）

示例：BGP 模块数据库位于 `/opt/netnexus/data/bgp/bgp_db.db`
