# Installing Clang Tools

## Check if Already Installed
```bash
which clang-tidy clang-format
```

## Installation

### Ubuntu/Debian
```bash
sudo apt update
sudo apt install clang-tidy clang-format
```

### Fedora/RHEL/CentOS
```bash
sudo dnf install clang-tools-extra
```

### Arch Linux
```bash
sudo pacman -S clang
```

### From Source (if package not available)
```bash
# Install LLVM/Clang from official releases
wget https://github.com/llvm/llvm-project/releases/download/llvmorg-17.0.0/clang+llvm-17.0.0-x86_64-linux-gnu-ubuntu-22.04.tar.xz
tar xf clang+llvm-17.0.0-x86_64-linux-gnu-ubuntu-22.04.tar.xz
sudo cp clang+llvm-17.0.0-x86_64-linux-gnu-ubuntu-22.04/bin/clang-* /usr/local/bin/
```

## Verify Installation
```bash
clang-tidy --version
clang-format --version
```

## Quick Start

After installation, run:
```bash
# Format all code
./scripts/format-code.sh

# Run static analysis
./scripts/run-clang-tidy.sh
```
