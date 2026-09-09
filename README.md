# FFXIV AMD NR 优化版 · test10

基于 [MatheusGViana/dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project) 的实验性 OptiScaler 分支，重点改善 FF14 中 AMD Neural Rendering 的随机超时和处理效率。

推理核心沿用已验证的 **test8**：捕获完成后通知 HIP 推理，等待 worker 全部收尾，再提交回写；超分消费完结果后才复用资源。本地及两轮游戏采样未记录到超时，包括一次 FG/多挡位测试。**这不是对所有游戏、设置和长期稳定性的保证。**

## 来源与范围

- 源码基线：上游 `1.7.2`，提交 `e58ff27a3d5ac86184875e01b317ff1463a247c1`。
- 选择性移植：上游 `1.7.3`，提交 `7b9dcb9c3864fc82fd4e0f7473a6f27c459a0707` 的 NR 缩放及桥接改进。不是完整复刻 1.7.3 的全部实验功能。
- 保留 OptiScaler 及相关依赖的许可证和署名；原始实现来源还包括 [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) 与 [OptiScaler](https://github.com/optiscaler/OptiScaler)。
- 私有 AMD runtime 和模型来自 [DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)，本分支不修改或新增分发它们。

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | OptiScaler 源码、解决方案及构建依赖 |
| `tools/build.py` | 可指定 VS/SDK 的 Windows 构建入口 |
| `tests/` | GPU 冒烟、完成时序和分段边界测试 |
| `config/amd_presr_perf.ini` | 启用 test8 分段提交的配置 |
| `docs/implementation.md` | 实现、验证摘要与限制 |

目录已移除上游旧安装包、二进制发布产物、分析工具环境、游戏日志及 SDK 示例图片/手册。第三方构建所需的头文件、库、源码和许可证保留。历史提交继续可追溯；清理当前目录不等于从 Git 历史中擦除旧文件。

## 构建

需要 Windows、Python 3、VS 2022 的 C++ x64 工具链及 Windows SDK。验证环境为 MSVC 14.44.35207、SDK 10.0.22621.0。项目是完整 OptiScaler，仍需其上游已有构建依赖。

静态库从 OptiScaler 固定提交按 SHA-256 校验获取（清单见 `tools/dependencies.json`）。补充已验证的 Microsoft DirectX-Headers，然后构建：

```powershell
git clone https://github.com/microsoft/DirectX-Headers.git .deps/DirectX-Headers
git -C .deps/DirectX-Headers checkout 48a762973271c5a75869946bf1fdbc489a628a5c
python tools/dependencies.py
python tools/build.py --rebuild
```

可用 `--vs`、`--sdk`、`--directx-headers` 指定其他安装位置。更换头文件或工具链后使用 `--rebuild` 重新生成预编译头。输出为 `src/x64/Release/OptiScaler.dll`；构建日志位于 `.build/`。本脚本不改动游戏安装目录。

## 简易设置（test10）

启动时不会弹出设置窗口。按原有菜单快捷键（默认 Insert）打开中文简易设置，只保留 FSR 超分倍率、FSR FG、DLSS5 开关、处理分辨率、色调与结构强度、帧率限制及帧率显示。菜单仍沿用原有快捷键设置。

“打开原生完整设置”进入上游完整界面；其中的 “Back to simple settings” 可以返回。关闭菜单后再次用快捷键打开，默认回到简易界面。中文使用 Windows 系统字体；缺少可用中文字体时回退英文。窗口支持拖动、调整大小与滚动。

AMD 路径的色调/结构强度连接到实际生效的参数；原生 NVIDIA `Intensity` 不用于这条 AMD 路径。肤质等细项保留在完整界面。

NR、风格强度和限帧滑块松开后应用，保存按钮写入现有 OptiScaler.ini。

FFXIV 的超分倍率修改必须保存并重启游戏后生效。简易面板始终显示提醒，并在本次修改后持续提示重启；“已配置”只表示配置项齐全，不代表当前游戏已使用新倍率。

“一键应用 FFXIV 必需设置”开启 Override all、DRS Override Minimum 和 Override Maximum，关闭分挡倍率覆盖，保留当前倍率。选择简易面板中的倍率也会自动补齐这些设置。上下限固定到所选倍率对应的渲染尺寸，避免游戏的动态分辨率范围影响倍率选择。简易面板不提供撤销必需项的开关；高级用户仍可在完整菜单调整。

`config/ffxiv/OptiScaler.ini` 是首次安装的超分配置模板，默认 1.3x 并开启上述三项，其余选项采用插件默认值。已有配置请使用面板按钮，不要用模板覆盖自己的完整配置。模板不是完整依赖安装包。

首次配置 FSR FG 需要保存并重启，之后可直接开关。1.0x 超分表示原生分辨率抗锯齿，关闭游戏超分需在游戏设置中操作。

界面已通过完整 DLL 构建及离屏 DX11/WARP 交互检查，**test10 尚待实际游戏验证**。Dalamud 虚表模式是待验证的兼容性缓解方案，尚未标记为修复；旧 API 插件仍需更新。

## 使用

退出游戏，备份现有代理 DLL 和配置。对已经使用 winmm 代理方式安装 OptiScaler 的 FF14，将新 DLL 命名为 `winmm.dll`，与 `config/amd_presr_perf.ini` 一起放到游戏目录，保留现有其他依赖。私有 runtime/weights 需自行取得；其版本必须匹配 `src/OptiScaler/dlssnr/amd/RuntimeHash.h` 中校验值。

当前实用参考组合：**2560×1440 输出、FSR 约 1.3 倍超分、NR 75%、单轮、FSR FG 开启**。这是一次 RX 9070 XT 实测的取舍，不保证其他硬件达到同样效果。NR 和 FG 的视觉效果、输入延迟需要自行评估。上游对在线游戏及第三方插件的适用性说明同样需要注意。

随包配置保留 `WaitForDx11Input=1`、`RequireUpscale=1`，菜单不进行实际超分时跳过 NR。`PhasedSubmission=1`、`AsyncSinglePass=0`、`DiagnosticStages=0` 启用已验证的同步分段路径。改为 `PhasedSubmission=0` 可回到保留的旧提交路径；修改后重启游戏。

## 测试

```powershell
python tools/build.py split
.build/tests/split-test.exe
python tools/build.py worker
.build/tests/worker-completion-test.exe
python tools/build.py continuity
.build/tests/continuity-test.exe
python tools/build.py smoke
python tools/build.py menu
.build/tests/simple-settings-test.exe
```

GPU 测试需 AMD HIP 7 和匹配 runtime/weights。将四个依赖文件放到被忽略的 `.runtime/`，或设置 `AMD_TEST_RUNTIME_DIR`。测试目录与依赖需在同一卷（使用硬链接）。

```powershell
python tests/run_tests.py --case sync-default --run first --phased
python tests/run_tests.py --case scales-multipass-queues --run multi --phased
python tests/run_tests.py --case game-shape --run game-size --phased --game-shape --frames 600
python tests/run_tests.py --case sync-default --run capture --phased --capture-dependency
python tests/run_tests.py --case post-return-signal --run consumer --phased
python tests/run_tests.py --case sync-default --run tail --phased --tail-sync --frames 2
```

菜单测试只在独立 WARP 设备上渲染，检查交互并将 RGBA 预览写入 `.build/menu-preview/`，不连接游戏。

每次使用新的 `--run` 名称，避免覆盖证据。测试专用 GPU 标志读回和故障注入不编入产品 DLL。

## 已知限制

- 首次 NR 初始化和首次增加推理轮数仍会停顿；多轮推理成本明显增加。
- Dalamud 偶发在包装交换链上分配 Hook 跳转缓冲区失败，与 DLL 加载地址布局有关，**尚未修复**。部分插件的 API 版本不兼容是独立问题。
- 未完成 GPU debug layer 验证；实测计时为 CPU 观察值，不能直接换算成 FG 后 FPS。
- 菜单版本检查失败可能反复写日志；本次发布未把这类后续优化混入已验证的产品源码。

详见 [实现与验证](docs/implementation.md)。
