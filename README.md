# FFXIV AMD NR 优化版 · test11

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

## 中文原生完整菜单（test11）

按原菜单快捷键（默认 Insert）直接打开中文原生完整菜单，启动时不会自动弹窗。保留全部原生选项、后端切换和应用按钮，撤下简易菜单与一键设置按钮。恢复完整菜单不会替用户修改游戏或已有配置。

FFXIV 中请分别检查以下设置：

1. 游戏自身的超分选项选择 DLSS，菜单不会替你修改游戏选项。
2. 在“超分辨率算法”中选择实际需要的 FSR 后端，并点击“切换超分算法”；游戏输入和插件输出是不同概念。FFXIV 默认 FSR2 和本分支默认 FSR 2.2 后端都不等于自动选好了目标后端。
3. AMD DLSS5 需同时勾选“启用神经渲染（DLSS5）”及“AMD：在超分之前应用神经渲染”。后者关闭时会明确提示效果未生效。游戏还必须实际运行超分。
4. 设定倍率时开启“覆盖全部挡位”和 DRS 的“覆盖最小分辨率／覆盖最大分辨率”，保存并重启游戏后生效。

原生菜单保留 FSR／FG 全部设置、AMD NR 分辨率、处理轮数、色调、结构、肤质结构强度，以及锐化、低延迟、纹理、输入、快捷键和调试设置。AMD 参数按已有运行路径处理，NVIDIA 专属选项仍遵守原来的显示条件，不因翻译而启用。

已整理 878 条中文显示词条，包含选项、下拉项、状态和悬停说明。参数键、文件名、算法名称及原始运行日志保留原文。显示语言与设置值分离；使用 Windows 中文字体，缺少字体时回退英文。菜单尺寸限制在当前视口内，内容较多时可滚动。字体和缩放沿用原生设置。

`config/ffxiv/OptiScaler.ini` 仅为首次安装的倍率／DRS 模板，其余参数采用原有默认值；已有配置不要用此模板覆盖。它不是完整安装配置，也不会自动开启 AMD 超分前处理。

完整 Release DLL 构建和离屏中文控件交互检查通过；**test11 尚待实际游戏验证**。兼容性问题仍处于验证阶段，未因本次本地化被标记为解决。

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
python tools/localization.py --check
python tools/build.py menu
.build/tests/localization-test.exe
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

菜单测试在独立 WARP 设备上检查本地化控件、隐藏标识符、数值选择、独立 AMD 开关和英文回退，将控件预览写入 `.build/menu-preview/`。预览是控件验证场景，不是完整菜单的实际游戏截图。

每次使用新的 `--run` 名称，避免覆盖证据。测试专用 GPU 标志读回和故障注入不编入产品 DLL。

## 已知限制

- 首次 NR 初始化和首次增加推理轮数仍会停顿；多轮推理成本明显增加。
- Dalamud 偶发在包装交换链上分配 Hook 跳转缓冲区失败，与 DLL 加载地址布局有关，**尚未修复**。部分插件的 API 版本不兼容是独立问题。
- 未完成 GPU debug layer 验证；实测计时为 CPU 观察值，不能直接换算成 FG 后 FPS。
- 菜单版本检查失败可能反复写日志；本次发布未把这类后续优化混入已验证的产品源码。

详见 [实现与验证](docs/implementation.md)。
