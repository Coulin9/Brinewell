# 更新日志

## 0.3.0

### 新功能

- **DLSS 转 MetalFX 超分辨率**：默认启用 D3D12 和 D3D11 NGX 支持，NGX 核心经过签名。
- **原生 HDR / EDR 输出**：支持 10 位 PQ/HLG 扫描输出、HDR EDID、Gamescope 与 Venus WSI HDR，
  并实时同步 Mac 显示器峰值亮度。
- **自动选择图形后端**：默认按游戏选择，使用随附的离线 Steam 图形索引并在需要时回退到探测；
  为《荒野大镖客：救赎 2》默认启用 DX12 配置，在 Venus 下跳过《毁灭战士：永恒》启动器。
- **两步引导**：自动设置 Apple D3DMetal；设置中可管理来宾 SSH 和 sudo 密码。
- **共享文件夹与 Steam 存储集成**：缩短挂载路径，将 shadercache 绑定到本地 ext4，并加速、并行化
  virtio-fs I/O。
- **进程内虚拟机冷重启**：重启时保留窗口和磁盘锁；来宾虚拟手柄按需创建。

### 图形与兼容性

- 默认启用 32 位 x86 Venus ICD 和 FEX GL thunk；Neptune 载荷包含 `d3d10core.dll`；
  Proton 11 和 Proton Experimental 重定向到 x86-64 版本。
- KosmicKrisp、Venus 和 Zink 统一使用官方 Mesa 26.3.1 源码，Neptune 源码由 Brinewell 管理；
  KosmicKrisp 支持非实体多边形模式、几何着色器和静态 wideLines driconf，Venus 在 macOS
  虚拟机进程中原生渲染。
- 最低系统要求更新为 macOS 26.0。

## 0.2.0

### 新名称与图标

- 项目使用新名称 **Brinewell** 和新应用图标（青色舷窗、海上落日与海浪）。Bundle ID 为
  `org.brinewell.Brinewell`，GitHub 仓库为 `Coulin9/Brinewell`。
- **自动迁移旧版本安装**：首次启动时把旧版本的应用数据目录迁移到
  `~/Library/Application Support/Brinewell`，同步更新配置中位于该目录内的路径并导入一次语言设置；
  迁移失败或配置无法使用时给出提示，且本次不自动启动、不自动缩盘、不进入新装引导。
- **SteamOS 盘保持兼容**：沿用旧盘 ESP 上的启动状态与部署标记（拒绝重新格式化的规则与此前
  一致）；首次启动时迁移每个槽位的 `/etc` 设置与用户目录（键盘、时钟、游戏配置、FEX EAC
  状态等）；受管游戏启动选项改写为新路径，旧绝对路径（自定义启动选项、已安装的 UU 服务）
  继续可用；旧版本推送的开发版 Neptune 载荷不会被启用。
- 升级时请先关闭虚拟机并退出旧版本应用，安装 Brinewell 后删除旧版本应用。
  用 Brinewell 启动过的系统盘不要再用旧版本启动。

### 构建

- 未找到 `brinewell-builder` 时沿用旧版本创建的 podman 连接。
- 检出目录移动后，自动重建 Python venv、Meson 与 SwiftPM 构建目录。

## 0.1.3

### 新功能

- **运行时切换图形后端**：Venus、Neptune（D3DMetal）和 Neptune（Mullion）可在虚拟机运行时切换，
  下次启动游戏即生效，无需重启 Steam 或虚拟机。不再生成 Neptune 兼容性工具，Steam 中选择的
  兼容性工具（官方 Proton、GE-Proton 等）保持不变；Venus 模式下 FEX 启用 Vulkan thunk。
- **调整系统磁盘大小**：设置中的客体磁盘可直接输入容量（GB）。扩容在下次启动时扩展 `/home`；
  缩容在下次启动时由 initramfs 执行 e2fsck/resize2fs，确认文件系统可容纳后才截断磁盘镜像。

### 图形与兼容性

- D3D11 设备上下文命令改为在来宾暂存后批量提交。
- DX11 flip 模型交换链的后缓冲区支持 sRGB 视图。
- 带初始数据的 `IMMUTABLE` 资源在宿主侧按 `DEFAULT` 创建。
- 只狼：与艾尔登法环 黑夜君临相同，游戏线程避开处理来宾中断的 CPU 0。
- Mullion 合并上游 v0.5-alpha.4。

### 修复

- 修复窗口跨越不同缩放比例的屏幕时，虚拟机窗口适配反复递归导致崩溃的问题。

## 0.1.2

### 新功能

- **Easy Anti-Cheat 支持**：客体镜像随附 StonkGuy/FEX `eac-2610.5`（FEX-2610 加 EAC
  兼容补丁），每次启动 Steam 前自动安装到 Steam 的 FEX-Emu 工具中，替换前先备份 Valve 的版本；
  Steam 更新该工具后会在下次启动时重新安装。艾尔登法环 黑夜君临等部分 EAC 游戏可以在启用反作弊的
  情况下运行。客体中可用 `/usr/lib/brinewell/steam-fex-eac status|restore|enable` 查看、恢复或重新启用。

## 0.1.1

### 新功能

- **共享剪贴板**：SteamOS 桌面模式和 Gamescope 游戏模式支持与 macOS 双向同步纯文本，
  单条最多 64 KiB。在 Mac 上复制后切回 VM 窗口即可在来宾内粘贴；在 SteamOS 内复制后
  切回 Mac 即可用 `⌘V` 粘贴。

### 图形性能

- 重构宿主与来宾间的图形 IPC：提交和 fence 改走共享内存环，渲染代理直接拉起各上下文
  worker，并用 Mach 信号量唤醒，去掉 render server 中转。
- 来宾通过映射的 virtio-gpu MMIO doorbell 页通知宿主（Neptune 与 Venus），不再每次
  走 execbuffer；内核新增 doorbell 页映射和计数器等待 ioctl。
- D3D12 fence：完成值由宿主写入共享槽位，`GetCompletedValue`、`SetEventOnCompletion`
  等在来宾本地完成；Win32 完成事件改由上下文状态页的完成队列统一投递，宿主侧 fence
  反馈由完成事件驱动，不再以 100 µs 周期轮询。
- D3D11 `FinishCommandList` 改为异步创建，不再等待宿主回复；资源类型、视图描述等不可变
  查询在来宾本地应答。
- 批量处理 `CopyDescriptorsSimple` 和 `CreateConstantBufferView`（例如 Nightreign
  每秒约百万次调用），并避免重叠的描述符拷贝合并到同一批次。

### 问题修复

- 修复《只狼》中多个延迟上下文以 `NO_OVERWRITE` 映射同一缓冲区时几何体闪烁的问题。
- 修复《只狼》转动视角时远景闪烁：延迟上下文中的查询结果只在命令列表执行后发布，
  不再返回过期的遮挡查询结果。
- 渲染 worker 退出或 VM 断开时立即将上下文标记为丢失，来宾可及时报告设备移除，
  不再等待心跳超时。
- 修复普通冷路径请求与主消费者竞争时 worker 被误判为协议违规并终止的问题。
- 修复内核 doorbell 对象被当作 GEM 对象使用时可能越界读取的问题。

### 构建

- Neptune 构建校验 `d3d12.dll` 的 `D3D12CreateDevice`（序号 101）和
  `D3D12GetDebugInterface`（序号 102）导出序号。
