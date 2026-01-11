# YukiSU Zygisk 集成说明

## 📦 GitHub Workflow 构建流程

libzygisk.so 已完全集成到 GitHub Actions CI/CD 中，与 ksuinit 和 LKM 保持一致的构建方式。

### 1. 构建流程

```
GitHub Actions 触发
    ↓
build-libzygisk workflow
    ├── arm64-v8a → lib64/libzygisk.so
    └── armeabi-v7a → lib/libzygisk.so
    ↓
上传 artifacts
    ↓
build-ksud workflow 下载
    ↓
复制到 ksud/assets/lib64/ 和 lib/
    ↓
embed_assets.py 嵌入到 ksud
    ↓
打包进 Manager APK
    ↓
用户安装时自动提取
```

### 2. Workflow 文件

- **构建**: `.github/workflows/libzygisk.yml`
- **集成**: `.github/workflows/ksud.yml` (自动下载 artifacts)
- **总控**: `.github/workflows/build-manager.yml`

### 3. 本地开发构建

```bash
# 单独构建 libzygisk.so（开发调试用）
cd userspace/libzygisk
ABIS="arm64-v8a" ./build.sh

# 完整本地构建（不推荐，推荐用 CI）
cd userspace/ksud
# 需要先手动复制 libzygisk.so 到 assets/
./build.sh
```

### 4. CI 构建（推荐）

直接 push 到 GitHub，CI 自动构建所有组件：

```bash
git add .
git commit -m "Update libzygisk"
git push

# CI 自动执行：
# 1. build-libzygisk (arm64 + arm32)
# 2. build-ksuinit
# 3. build-lkm (所有 KMI)
# 4. build-ksud (下载上述 artifacts)
# 5. build-manager (打包 APK)
```

### 5. Release 流程

发布时，libzygisk.so 会随其他文件一起上传：

- `libzygisk-arm64-v8a/lib64/libzygisk.so`
- `libzygisk-arm64-v8a/lib64/libzygisk.so.sig`
- `libzygisk-armeabi-v7a/lib/libzygisk.so`
- `libzygisk-armeabi-v7a/lib/libzygisk.so.sig`

### 6. 当前实现状态

- ✅ 独立 GitHub Workflow
- ✅ 自动构建 32/64 位
- ✅ GPG 签名
- ✅ 自动集成到 ksud
- ✅ 自动打包进 APK
- ✅ 自动提取到设备
- ⚠️  PLT hooks 未实际安装（需要集成 lsplt）
- ❌ 模块加载未实现（需要 daemon 协议）

### 7. 与 ksuinit/LKM 对比

| 组件 | Workflow | 输出 | 签名 | 集成方式 |
|------|---------|------|------|---------|
| LKM | `build-lkm.yml` | `*_kernelsu.ko` | ✅ GPG | 复制到 assets |
| ksuinit | `ksuinit.yml` | `ksuinit` | ✅ GPG | 复制到 assets |
| libzygisk | `libzygisk.yml` | `lib*/libzygisk.so` | ✅ GPG | 复制到 assets/lib* |

### 8. 迁移到 Ninja（未来）

如果以后切换到 Ninja 构建系统：

1. 只需修改 `.github/workflows/libzygisk.yml`
2. 将 `cmake --build` 改为 `ninja`
3. 其他流程无需改动

本地 CMake 构建和 CI 构建完全解耦，互不影响。

## 🎯 优势

- ✅ **统一流程**：与 ksuinit/LKM 完全一致
- ✅ **CI 优先**：本地开发可选，CI 自动化
- ✅ **灵活切换**：支持任意构建系统（CMake/Ninja/Make）
- ✅ **签名完整**：所有文件 GPG 签名
- ✅ **自动化**：零手动操作

## 📝 开发注意事项

- 修改 libzygisk 源码后，push 到 GitHub 即可自动构建
- 本地测试用 `./build.sh`，生产环境用 CI
- 不需要手动复制文件到 ksud assets
- CI artifacts 保留 90 天，可随时下载
