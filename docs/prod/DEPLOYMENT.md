# NetNexus Deployment Guide

This guide covers packaging, deployment, and running NetNexus in various environments.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Building](#building)
- [Packaging](#packaging)
- [Deployment Options](#deployment-options)
  - [Production Server](#production-server-deployment)
  - [Docker Container](#docker-deployment)
  - [Development](#development-deployment)
- [Configuration Management](#configuration-management)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### Build Dependencies

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake libxml2-dev libsqlite3-dev pkg-config

# RHEL/CentOS
sudo yum install gcc gcc-c++ cmake libxml2-devel sqlite-devel pkgconfig
```

### Runtime Dependencies

```bash
# Ubuntu/Debian
sudo apt install libxml2 libsqlite3-0

# RHEL/CentOS
sudo yum install libxml2 sqlite
```

## Building

```bash
# Clone repository
git clone <repository-url>
cd NetNexus

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Verify build
./bin/netnexus --help || echo "Build successful"
```

## Packaging

Create a deployment package with all necessary files:

```bash
# From project root
./scripts/package.sh

# Output
package/netnexus-1.0.0.tar.gz
```

The package contains:
```
netnexus-1.0.0/
├── bin/              # Executables
│   └── netnexus
├── lib/              # Shared libraries
│   ├── libnn_cfg.so
│   ├── libnn_db.so
│   ├── libnn_bgp.so
│   ├── libnn_dev.so
│   └── libnn_utils.so
├── config/           # Configuration files
│   ├── cfg/commands.xml
│   ├── dev/commands.xml
│   ├── bgp/commands.xml
│   └── db/commands.xml
├── scripts/          # Deployment scripts
│   ├── deploy.sh
│   └── start.sh
├── VERSION
└── README.txt
```

## Deployment Options

### Production Server Deployment

#### 1. Extract Package

```bash
tar xzf netnexus-1.0.0.tar.gz
cd netnexus-1.0.0
```

#### 2. Deploy

```bash
sudo ./scripts/deploy.sh
```

This installs to `/opt/netnexus`:
- Binaries → `/opt/netnexus/bin/`
- Libraries → `/opt/netnexus/lib/`
- Config → `/opt/netnexus/resources/`
- Data → `/opt/netnexus/data/`
- Systemd service → `/etc/systemd/system/netnexus.service`

#### 3. Start Service

```bash
# Start immediately
sudo systemctl start netnexus

# Enable on boot
sudo systemctl enable netnexus

# Check status
sudo systemctl status netnexus

# View logs
sudo journalctl -u netnexus -f
```

#### 4. Verify

```bash
# Check if listening
sudo netstat -tuln | grep 3788

# Connect via telnet
telnet localhost 3788
```

### Docker Deployment

#### Build Image

```bash
# Build with Docker
docker build -t netnexus:latest .

# Or with version tag
VERSION=1.0.0 docker build \
  --build-arg VERSION=1.0.0 \
  --build-arg GIT_COMMIT=$(git rev-parse HEAD) \
  -t netnexus:1.0.0 .
```

#### Run Container

##### Using docker-compose (Recommended)

```bash
# Start
docker-compose up -d

# View logs
docker-compose logs -f

# Stop
docker-compose down

# Stop and remove volumes
docker-compose down -v
```

##### Using Docker CLI

```bash
# Create volume for persistent data
docker volume create netnexus-data

# Run container
docker run -d \
  --name netnexus \
  -p 3788:3788 \
  -v netnexus-data:/opt/netnexus/data \
  -e NN_RESOURCES_DIR=/opt/netnexus/resources \
  --restart unless-stopped \
  netnexus:latest

# View logs
docker logs -f netnexus

# Stop
docker stop netnexus
docker rm netnexus
```

#### Docker Environment Variables

- `NN_RESOURCES_DIR`: Configuration directory (default: `/opt/netnexus/resources`)
- `LD_LIBRARY_PATH`: Library path (default: `/opt/netnexus/lib`)

#### Custom Configuration

Mount custom config directory:

```bash
docker run -d \
  --name netnexus \
  -p 3788:3788 \
  -v /path/to/custom/config:/opt/netnexus/resources:ro \
  -v netnexus-data:/opt/netnexus/data \
  netnexus:latest
```

### Development Deployment

For development, run directly from build directory:

```bash
cd build/bin
./netnexus
```

Or use the start script:

```bash
cd build/bin
../../scripts/start.sh
```

Configuration files are automatically resolved from `src/*/commands.xml`.

## Configuration Management

### Configuration Files

Each module has its own `commands.xml`:

```
/opt/netnexus/resources/
├── cfg/commands.xml      # Core CLI commands
├── dev/commands.xml      # Development/debug commands
├── bgp/commands.xml      # BGP protocol commands
└── db/commands.xml       # Database module (empty)
```

### Modifying Configuration

1. **Edit config files** in `/opt/netnexus/resources/`
2. **Restart service** to apply changes:
   ```bash
   sudo systemctl restart netnexus
   ```

### Backup Configuration

```bash
# Backup entire config directory
sudo tar czf netnexus-config-backup-$(date +%Y%m%d).tar.gz \
  /opt/netnexus/resources/

# Restore from backup
sudo tar xzf netnexus-config-backup-20260127.tar.gz -C /
```

### Configuration Precedence

The system searches for configuration files in this order:

1. `$NN_RESOURCES_DIR/{module}/commands.xml` (if NN_RESOURCES_DIR set)
2. `/opt/netnexus/resources/{module}/commands.xml` (production)
3. `<exe_dir>/../../src/{module}/commands.xml` (development)
4. `../../src/{module}/commands.xml` (fallback)

## Database Storage

SQLite databases store persistent configuration:

**Production:**
```
/opt/netnexus/data/
└── bgp/
    └── bgp_db.db
```

**Development:**
```
./data/
└── bgp/
    └── bgp_db.db
```

### Backup Databases

```bash
# Backup all databases
sudo tar czf netnexus-data-backup-$(date +%Y%m%d).tar.gz \
  /opt/netnexus/data/

# Backup specific database
sudo cp /opt/netnexus/data/bgp/bgp_db.db \
  /backup/bgp_db-$(date +%Y%m%d).db
```

## Troubleshooting

### Service won't start

```bash
# Check service status
sudo systemctl status netnexus

# View detailed logs
sudo journalctl -u netnexus -xe

# Check if port is already in use
sudo netstat -tuln | grep 3788

# Verify binary exists and is executable
ls -l /opt/netnexus/bin/netnexus
```

### Configuration not found

```bash
# Check environment variable
sudo systemctl show netnexus | grep NN_RESOURCES_DIR

# Verify config files exist
ls -la /opt/netnexus/resources/*/commands.xml

# Test manual start with verbose output
sudo /opt/netnexus/bin/start.sh
```

### Database errors

```bash
# Check database directory permissions
ls -ld /opt/netnexus/data

# Verify SQLite is installed
sqlite3 --version

# Test database access
sqlite3 /opt/netnexus/data/bgp/bgp_db.db ".tables"
```

### Permission denied errors

```bash
# Fix ownership
sudo chown -R root:root /opt/netnexus

# Fix permissions
sudo chmod 755 /opt/netnexus
sudo chmod 755 /opt/netnexus/bin
sudo chmod 644 /opt/netnexus/resources/*/commands.xml
```

### Docker container issues

```bash
# Check container status
docker ps -a | grep netnexus

# View container logs
docker logs netnexus

# Exec into container
docker exec -it netnexus /bin/bash

# Check environment
docker exec netnexus env | grep NN_

# Verify files inside container
docker exec netnexus ls -la /opt/netnexus/resources/
```

### Library not found

```bash
# Check library path
echo $LD_LIBRARY_PATH

# Verify libraries exist
ls -la /opt/netnexus/lib/

# Test library loading
ldd /opt/netnexus/bin/netnexus
```

## Uninstallation

### Remove systemd service

```bash
sudo systemctl stop netnexus
sudo systemctl disable netnexus
sudo rm /etc/systemd/system/netnexus.service
sudo systemctl daemon-reload
```

### Remove installation

```bash
# Backup data first!
sudo tar czf netnexus-backup-$(date +%Y%m%d).tar.gz /opt/netnexus

# Remove installation
sudo rm -rf /opt/netnexus
```

### Remove Docker

```bash
# Stop and remove container
docker-compose down -v

# Remove image
docker rmi netnexus:latest

# Remove volumes
docker volume rm netnexus-data
```

## Upgrading

### Production Server

1. **Stop service:**
   ```bash
   sudo systemctl stop netnexus
   ```

2. **Backup:**
   ```bash
   sudo tar czf netnexus-backup-$(date +%Y%m%d).tar.gz /opt/netnexus
   ```

3. **Deploy new version:**
   ```bash
   tar xzf netnexus-1.1.0.tar.gz
   cd netnexus-1.1.0
   sudo ./scripts/deploy.sh
   ```

4. **Start service:**
   ```bash
   sudo systemctl start netnexus
   ```

### Docker

```bash
# Pull new image
docker pull netnexus:1.1.0

# Update docker-compose.yml with new version
# Then restart
docker-compose up -d
```

## Support

For issues or questions:
- Check logs: `sudo journalctl -u netnexus -f`
- Review configuration: `/opt/netnexus/resources/`
- Verify installation: `/opt/netnexus/bin/netnexus --version`
