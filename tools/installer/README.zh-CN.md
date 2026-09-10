# FF14 AMD DLSS5 优化版安装补丁 · test14

完整解压本包，双击 **INSTALL.bat**，选择游戏目录中的 **ffxiv_dx11.exe**，核对目录并输入 Y。也可以把游戏目录或该 exe 拖到 INSTALL.bat 上。

安装程序检查游戏是否退出、所需组件是否齐全且版本匹配，备份后替换三个文件：winmm.dll、OptiScaler.ini、amd_presr_perf.ini。**这会重置插件配置**，不修改游戏自身的画面设置。若中途失败，会尝试恢复已替换的文件并报告结果。

## 首次安装需要的其他组件

本包是优化补丁，不包含 AMD runtime、模型或 FSR 运行库。

1. 到原作者的 [1.7.2 发布页](https://github.com/MatheusGViana/dlss-5-amd-project/releases/tag/1.7.2)，下载 **OptiScaler-AMD-PreSR-Multipass-v1.7.2.zip**，不要下载 Source code。
2. 完整解压原版组件包，按原作者说明运行 INSTALAR_AMD.bat，选择 ffxiv_dx11.exe，采用 **winmm.dll** 代理方式安装。已有该版本完整组件的用户可以跳过。
3. 安装 AMD HIP 7 环境；按 [AMD Windows HIP SDK 安装说明](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/install/install.html) 完成安装后重启启动器。游戏需能找到 amdhip64_7.dll。脚本只检查常见位置，不保证驱动或显卡兼容性。
4. 退出游戏，再运行本补丁的 INSTALL.bat。本补丁不会联网下载文件。组件须按各自作者的许可及获取要求取得。

安装程序核对 3 个 dlssnr_amd_pass DLL、模型文件以及 OptiScaler 子目录下的 FSR 加载、超分、插帧 DLL，共 7 个文件的 SHA-256，使用与本版验证一致的组件。不建议随意混入其他版本。

## 安装后的默认设置

- FSR 超分，1.3 倍倍率；覆盖全部挡位和 DRS 最小／最大分辨率。
- DLSS5 已启用，并开启“AMD：在超分之前应用神经渲染”；NR 75%，单轮。
- 插帧默认关闭，需要时按 Insert 在中文完整菜单中开启。
- 在**游戏自身的画面设置**中选择 DLSS。调整插件的超分倍率后，保存设置并重启游戏。

默认保留“仅在实际超分时运行 NR”，帮助跳过菜单阶段。若需要原生分辨率下使用 NR，将 amd_presr_perf.ini 中的 RequireUpscale=1 改为 0 并重启；菜单阶段也可能随之触发 NR。这不是准确的场景或登录状态识别。

## 日志与备份

默认关闭本优化版可控制的日志：OptiScaler 主日志、amd_presr.log 和性能 CSV。**第三方 AMD runtime 等组件仍可能独立生成日志，例如 dlssnr_on_amd.log；这不是“整个游戏目录绝不产生任何日志”的保证。**已有日志不会自动删除。

test13 在 test12 中文修复版上增加 TextLog 开关及安装工具，不改变推理、同步或插帧流程。若需要排查问题，可将 amd_presr_perf.ini 的 Timing、TextLog 改为 1，并将 OptiScaler.ini 中 LogToFile=true、LogLevel=1，重启后重新采样。

旧文件保存在游戏目录中的 **OptiScaler-backup-日期-编号** 文件夹。恢复时退出游戏，将该备份根目录内的原文件复制回游戏目录；不要复制 new-files 子目录。original-files.json 记录安装前各文件是否存在；原先不存在的文件，回退时可删除本次新增的对应文件。请勿删除游戏本体或其他组件。

## 只检查，不安装

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -GameDirectory "D:\游戏目录\game" -CheckOnly
```

bat 只为当前安装进程临时使用 ExecutionPolicy Bypass，不会修改系统执行策略。脚本不自动申请管理员权限；如目录无写权限，请选择实际游戏目录并按需要以管理员身份运行。对使用目录链接的安装位置，请选择实际存放目录。

文件完整性校验、模拟目录安装／备份／失败回退及日志开关检查通过。实际游戏仍需复测；与其他注入或覆盖层工具的兼容性未保证。

## test14 菜单修正

AMD 肤质结构强度滑条恢复为 -1～2，并补充悬停说明。所有负数均表示跟随 AMD 结构强度，默认 -1；0 并不关闭所有脸部或肤色变化。后端参数解释保持不变。已有 test13 用户只需退出游戏并替换 payload/winmm.dll，无需运行安装程序重置配置。
