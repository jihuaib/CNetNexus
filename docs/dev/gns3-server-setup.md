# GNS3 Server 安装与 NetNexus 镜像部署指南

本文档适用于 Apple Silicon Mac（M 系列芯片），通过 UTM 虚拟机运行 GNS3 Server。

## 环境说明

| 组件 | 版本/平台 |
|------|-----------|
| 宿主机 | macOS (Apple Silicon) |
| 虚拟化 | UTM |
| 虚拟机 | Ubuntu 24.04 ARM64 Server |
| GNS3 GUI (Mac) | 2.2.56.1 |
| GNS3 Server (VM) | 2.2.56.1（必须与 GUI 版本一致） |

## 1. 创建 UTM 虚拟机

### 1.1 安装 UTM

```bash
brew install --cask utm
```

### 1.2 创建虚拟机

1. 下载 Ubuntu 24.04 ARM64 Server ISO
2. UTM → 新建虚拟机 → Virtualize → Linux
3. 配置建议：
   - 内存：4GB
   - CPU：2 核
   - 磁盘：50GB
   - 网络：Shared Network（NAT）

### 1.3 磁盘分区

安装时选择 **"Use entire disk"**（单分区，最简单）。

安装完成后，Ubuntu LVM 默认只分配了部分空间，需要扩展：

```bash
sudo lvextend -l +100%FREE /dev/mapper/ubuntu--vg-ubuntu--lv
sudo resize2fs /dev/mapper/ubuntu--vg-ubuntu--lv
df -h /  # 确认空间已扩展
```

## 2. 安装 Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker

# 验证
docker run --rm hello-world
```

## 3. 安装 GNS3 Server

### 3.1 安装 GNS3 Server

```bash
sudo apt update
sudo apt install -y python3-pip pipx
pipx install gns3-server==2.2.56.1
pipx ensurepath
source ~/.bashrc

# 验证
gns3server --version
```

> **重要**：GNS3 Server 版本必须与 Mac 上的 GNS3 GUI 版本一致。通过 GUI 的 Help → About 查看版本号。

### 3.2 安装 uBridge

GNS3 Docker 节点依赖 uBridge 进行网络桥接：

```bash
sudo apt install -y git build-essential libpcap-dev
git clone https://github.com/GNS3/ubridge.git
cd ubridge
make
sudo make install

# 验证
which ubridge
```

### 3.3 配置 GNS3 Server

关闭认证，允许远程连接：

```bash
mkdir -p ~/.config/GNS3/3.0
cat > ~/.config/GNS3/3.0/gns3_server.conf << 'EOF'
[Server]
host = 0.0.0.0
port = 3080
auth = false
compute_auth = false
EOF
```

gns3server --host 0.0.0.0 --port 3080 --allow

### 3.4 设置开机自启

```bash
# 确认 gns3server 路径
which gns3server
# 通常为 /home/<user>/.local/bin/gns3server

sudo tee /etc/systemd/system/gns3server.service << 'EOF'
[Unit]
Description=GNS3 Server
After=network.target docker.service
Requires=docker.service

[Service]
Type=simple
User=jhb
ExecStart=/home/jhb/.local/bin/gns3server --host 0.0.0.0 --port 3080 --allow
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable gns3server
sudo systemctl start gns3server

# 查看状态
sudo systemctl status gns3server
```

> 注意：将 `User` 和 `ExecStart` 中的路径替换为实际用户名和路径。

### 3.5 验证 Server 运行

```bash
# VM 内部验证
curl http://127.0.0.1:3080/v2/version

# Mac 上验证网络连通
curl http://<VM_IP>:3080/v2/version
```

## 4. 构建并导入 NetNexus 镜像

### 4.1 Mac 上构建镜像

```bash
cd ~/code/CNetNexus
./scripts/dev/build-docker-image.sh
```

### 4.2 导出并传输到 VM

```bash
docker save netnexus:latest | gzip > netnexus-latest.tar.gz
scp netnexus-latest.tar.gz jhb@<VM_IP>:~
```

### 4.3 VM 上导入镜像

```bash
docker load < ~/netnexus-latest.tar.gz

# 验证
docker images | grep netnexus
```

### 4.4 更新镜像

代码修改后重复上述步骤即可覆盖旧镜像：

```bash
# Mac 上
./scripts/dev/build-docker-image.sh
docker save netnexus:latest | gzip > netnexus-latest.tar.gz
scp netnexus-latest.tar.gz jhb@<VM_IP>:~

# VM 上
docker load < ~/netnexus-latest.tar.gz
```

GNS3 中删除旧节点，重新拖入新节点即可使用更新后的镜像。已有项目里的旧节点不会自动替换镜像层。

## 5. GNS3 GUI 连接配置

### 5.1 连接 Server

1. 打开 Mac 上的 GNS3
2. **Preferences → Server → Main server**
3. **取消勾选** "Enable local server"
4. Host: `<VM_IP>`（如 `192.168.48.129`）
5. Port: `3080`
6. Apply

### 5.2 添加 NetNexus Docker 模板

1. **Preferences → Docker → Docker containers → New**
2. 选择镜像 `netnexus:latest`
3. 配置：
   - Template name: `NetNexus`
   - Adapters: `8`
   - Console type: `telnet`
   - Category: `Routers`
4. OK → Apply

建议在模板的 Docker 运行参数中加入：

```text
--cap-add NET_ADMIN --cap-add NET_RAW --sysctl net.ipv6.conf.all.disable_ipv6=0 --sysctl net.ipv6.conf.default.disable_ipv6=0
```

MPLS 转发依赖 GNS3 Server 宿主机加载内核模块：

```bash
sudo modprobe mpls_router
sudo modprobe mpls_iptunnel
sudo modprobe mpls_gso
```

### 5.3 使用节点

1. **File → New blank project**
2. 从左侧设备面板将 `NetNexus` 拖到画布
3. 右键 → **Start** 启动节点
4. 双击节点打开 Console，看到欢迎信息后按回车连接 NetNexus console

```
========================================
        NetNexus Network Device
========================================

  Press ENTER to connect to console...
```

GNS3 的 console 窗口连接的是容器前台 `gns3-entry.sh`。真正的 NetNexus 管理通道是容器内 `/opt/netnexus/run/console.sock`，由 `netnexus-console` 连接；它不依赖 telnet/vty 配置。telnet/vty 默认关闭，需要进入 CLI 后通过 ACCESS line 命令开启。

## 6. 常见问题

### SSH 连接提示 HOST IDENTIFICATION HAS CHANGED

VM 重装后 host key 变化：

```bash
ssh-keygen -R <VM_IP>
ssh jhb@<VM_IP>
```

### 节点启动报 uBridge 错误

```
uBridge is not available, path doesn't exist
```

安装 uBridge 后重启 GNS3 Server：

```bash
sudo systemctl restart gns3server
```

### GNS3 GUI 连不上 Server

1. 确认版本一致：GUI 和 Server 必须为相同版本
2. 确认网络：`curl http://<VM_IP>:3080/v2/version`
3. 确认防火墙：`sudo ufw disable`

### 查看 NetNexus 运行日志

```bash
# supervisor wrapper 日志
docker exec <container_id> tail -f /opt/netnexus/log/supervise.log

# 模块日志
docker exec <container_id> ls -l /opt/netnexus/log
docker exec <container_id> tail -f /opt/netnexus/log/main.log

# 查看容器 ID
docker ps | grep netnexus
```
