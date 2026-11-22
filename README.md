# Linux 响度均衡器

Linux 原生实时响度均衡程序。使用 PulseAudio `module-ladspa-sink` 在 PipeWire/PulseAudio 的音频图内处理，不使用 monitor 录制和独立回放桥接。LADSPA 插件已编进单个主程序，运行时只写入用户可访问的私有临时共享库并在退出时删除。

## 功能

- 默认硬限制：`-32 dBFS`（可配置，范围 `0 到 -100 dBFS`）
- RMS 响度估算与低响度增益
- 单样本硬上限保护，输出不会超过设置值
- 运行时逐 dB 调整：`+` 提高上限，`-` 降低上限
- `0` 重置为保存的默认限制幅度
- `q` 或 `Ctrl+C` 退出并恢复原默认音频设备，同时强制原硬件设备为 0 dB 音量且 mute
- 可启用或禁用用户级开机自启动
- 不联网、不下载资源、不调用第三方命令
- 启动时若硬件被 mute 或设为 0%，程序会恢复硬件为 unmute + 100%，避免启动后完全无声
- 发布包无需单独提供 LADSPA `.so` 文件

## 运行环境

- Linux
- PipeWire-Pulse 或 PulseAudio
- LADSPA 模块支持
- 已安装 `libpulse`
- 典型桌面会话需要图形桌面的用户自启动机制

本程序不提供声学听力保护，数字峰值限制不能补偿扬声器、耳机或环境噪声风险。请先把硬件音量调低，再从低电平测试。

## 依赖与构建

构建只需要编译器、make、pkg-config、PulseAudio 开发包和 LADSPA 头文件。

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential pkg-config libpulse-dev ladspa-sdk
```

### Fedora

```sh
sudo dnf install gcc make pkgconf-pkg-config pulseaudio-libs-devel ladspa-devel
```

### Arch Linux

```sh
sudo pacman -S base-devel pkgconf libpulse ladspa
```

从项目目录编译：

```sh
cd <项目目录>
make clean
make
```

`make` 会生成：

- `loudness-limiter`：控制程序和自启动管理
- `loudness-limiter-ladspa.so`：原生 LADSPA DSP 插件
- `/tmp/loudness-limiter-test`：构建测试并非系统程序

只运行限幅安全测试：

```sh
make test
```

## 安装

安装到 `/usr/local`：

```sh
sudo make install
```

安装内容：

- `/usr/local/bin/loudness-limiter`
- `/usr/local/lib/loudness-limiter/loudness-limiter-ladspa.so`
- `/usr/local/share/applications/loudness-limiter.desktop`

用户目录安装（无需 sudo）：

```sh
make install-user
```

安装到：

- `~/.local/bin/loudness-limiter`
- `~/.local/lib/loudness-limiter/loudness-limiter-ladspa.so`
- `~/.local/share/applications/loudness-limiter.desktop`

卸载：

```sh
sudo make uninstall
```

或用户目录卸载：

```sh
make uninstall-user
```

卸载不会删除用户自启动配置。若需要，一并执行：

```sh
loudness-limiter --disable-autostart
```

## 使用

先降低系统硬件音量，然后运行：

```sh
./loudness-limiter
```

安装后也可运行：

```sh
loudness-limiter
```

程序启动后会加载原生 LADSPA sink，并把现有应用音频迁移到该 sink。`q` 或 `Ctrl+C` 会恢复原默认 sink、移动输入、卸载 LADSPA 模块。

```text
-l, --limit DB        单次运行绝对上限（范围 0 到 -100 dBFS）
-s, --sink NAME       指定硬件 sink 名称
--enable-autostart    为当前用户启用开机自启动
--disable-autostart   禁用当前用户的开机自启动
--autostart-status    查看当前用户的自启动状态
--set-default-limit DB 保存默认限制幅度（0 到 -100 dBFS）
--get-default-limit   查询保存的默认限制幅度
--reset-default-limit 恢复默认限制幅度为 -32 dBFS
-h, --help            显示帮助
```

例：启动时直接使用 `-24 dBFS`：

```sh
loudness-limiter -l -24
```

运行中：

- `+` 或 `=`：硬限制提高 1 dB
- `-` 或 `_`：硬限制降低 1 dB
- `0`：重置为保存的默认限制幅度
- `h` 或 `?`：重新显示按键说明
- `q` 或 `Ctrl+C`：退出并恢复原音频设备

## 调整持久默认限制

保存默认限制：

```sh
loudness-limiter --set-default-limit -24
```

查询当前保存值：

```sh
loudness-limiter --get-default-limit
```

恢复出厂默认值：

```sh
loudness-limiter --reset-default-limit
```

配置文件位于：

默认配置文件放在主程序可执行文件所在目录：

```text
<loudness-limiter 所在目录>/loudness-limiter.conf
```

例如：

```text
<项目目录>/loudness-limiter.conf
~/.local/bin/loudness-limiter.conf
/usr/local/bin/loudness-limiter.conf
```

文件格式：

```text
limit_db=-24.0
```

如果同目录没有配置文件，程序自动使用 `-32 dBFS`。运行 `--set-default-limit DB` 或手工创建同目录的 `loudness-limiter.conf` 即可保存默认值；保存后下一次启动生效。

每次启动时加载该值；命令行 `-l` 只覆盖本次运行。运行中按 `0` 会回到保存的默认值。

## 开机自启动

启用：

```sh
loudness-limiter --enable-autostart
```

禁用：

```sh
loudness-limiter --disable-autostart
```

查看状态：

```sh
loudness-limiter --autostart-status
```

也可直接使用 Makefile：

```sh
make autostart-enable
make autostart-disable
make autostart-status
```

配置文件位于：

```text
~/.config/autostart/loudness-limiter.desktop
```

当当前进程没有可用的 `HOME` 时，程序才回退使用 `XDG_CONFIG_HOME`。

桌面自启动会执行已安装的 `/usr/local/bin/loudness-limiter` 并打开终端。若终端管理器不支持桌面项中的 `Terminal=true`，可修改该文件为静默启动或设置终端前缀。

## 交叉架构或特殊发行版

插件必须与运行进程使用相同架构编译。若 PulseAudio 找不到 LADSPA 模板文件，请确认：

```sh
ls -l /usr/local/lib/loudness-limiter/loudness-limiter-ladspa.so
pactl list modules short
```

若指定硬件 sink 名称，先查询：

```sh
pactl list short sinks
loudness-limiter -s <hardware-sink-name>
```

## 安全说明


此程序限制数字采样峰值和工作流程内响度增益，但不能保证扬声器、耳机或人的安全。数字限制不能增加设备耐受上限，也不能消除失真前后的低频冲击。必须先低音量测试。
