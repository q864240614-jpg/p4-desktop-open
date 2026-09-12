# ESP32-P4 桌面屏

800×480 横屏 LVGL 界面：拓竹 H2D / A1 mini 打印状态、待机时钟、Codex 配额页、待办列表。项目包含两套可选的局域网服务：Python 待办 / 飞书同步服务，以及向开发板提供 JPEG 画面的视频中转服务。

本项目面向下述 ESP32-P4 开发板，提供固件源码、LVGL 界面和可选的局域网服务。同型号硬件可按配置步骤编译烧录；移植到其他开发板时，需要适配显示、触摸和网络接口。模块划分与开发约定见 [AGENTS.md](AGENTS.md)。[English](README.md)。

## 界面预览

![主要界面](assets/preview/overview.png)

使用项目实际 LVGL 渲染器导出的 800×480 界面示意图，打印机、配额和待办内容均为测试数据，并非实机照片。单张图片与导出步骤见[界面图集](assets/preview/README.md)。

## 支持的硬件

- **主控：** ESP32-P4，16 MB Flash，PSRAM（200 MHz）。时钟数字缓存在 PSRAM，建议 16 MB PSRAM。
- **Wi-Fi：** 板载 ESP32-C6，经 ESP-Hosted、SDIO slot 1。默认脚在 `sdkconfig.defaults`：CLK 18、CMD 19、D0 14、D1 15、D2 16、D3 17、C6 复位 54。
- **屏：** ST7701 MIPI-DSI，物理 480×800，RGB565，双 lane。LVGL 排版是 800×480，`p4_display.c` 用 PPA 旋转。BSP 选项 `CONFIG_BSP_LCD_TYPE_1024_600` **名字来自旧示例，这里实际是 480×800 ST7701 分支**，不要按名字改成 1024×600 屏。
- **触摸：** GT911，I2C SDA GPIO7、SCL GPIO8。背光 GPIO23，LCD 复位 GPIO5。
- **工具链：** ESP-IDF **5.4.x**（参考版本为 5.4.2；依赖清单允许 `>=5.4,<6.0`），**LVGL 8.3.11**。不要用 IDF 6 或 LVGL 9。

使用其他硬件时，可复用 `components/portable_ui`、`todo_service` 和 `video_relay`，并适配板级显示、触摸和无线接口。

## 项目结构

| 路径 | 作用 |
| --- | --- |
| `components/portable_ui/` | 800×480 LVGL 界面（打印机页、时钟、Codex、待办、事件动画） |
| `todo_service/` | 局域网待办 HTTP + SQLite，可选飞书同步 |
| `video_relay/` | 局域网 JPEG 中转，避免 P4 软件解 H2D High Profile |
| `main/bambu_config.c` | NVS 打印机配置 + 本机网页 |
| `assets/` | 字体、图标、界面预览（`assets/preview/`） |

板级适配代码：`main/p4_display.c`、`main/main.c` 的 BSP 启动、`vendor/` 里的 BSP/LCD、C6 SDIO 脚、以及 `sdkconfig.defaults` 中的硬件配置。

## 本地配置

先复制配置示例，再填写本地参数：

```text
copy main\wifi_credentials.example.h   main\wifi_credentials.h
copy main\todo_credentials.example.h   main\todo_credentials.h
copy main\codex_credentials.example.h  main\codex_credentials.h
copy todo_service\deploy.env.example   todo_service\deploy.env
```

Linux / macOS 用 `cp`。然后：

1. 在 `main/wifi_credentials.h` 填写 Wi-Fi 名称和密码。
2. 在 `main/todo_credentials.h` 填写待办服务地址 `TODO_URL` 和设备 Bearer 令牌（必须与 `deploy.env` 里的 `TODO_DEVICE_TOKEN` 一致）。
3. 在 `main/codex_credentials.h` 填写 Codex 状态 JSON 接口；不用该页可保持占位。
4. 在 `main/video_relay.h` 把 `VIDEO_RELAY_IP` 改成运行 `video_relay/` 的主机（占位是 `192.168.1.10`）。
5. 填写 `todo_service/deploy.env`：网页密码、设备 token、会话密钥，以及可选的飞书应用 id / secret / 回调地址。

三个本地凭据头文件和 `todo_service/deploy.env` 已由 `.gitignore` 排除。`main/video_relay.h` 是受版本控制的文件，公开提交时应保留占位地址。Wi-Fi 和服务凭据会编入本地固件，因此不要发布填入真实配置后构建的二进制文件。

打印机的局域网 IP、序列号、访问码**不编进固件**。联网后屏幕会显示开发板 IP 和 6 位 PIN。浏览器打开 `http://<开发板IP>/`，用户名 `admin`，密码为该 PIN，再保存 H2D / A1 mini。PIN 每次重启都会变。打印机 MQTT 为局域网 8883，用户名 `bblp`。本快照不发送暂停 / 取消 / 开始打印指令。

## 编译与烧录

安装 [ESP-IDF 5.4.x](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32p4/get-started/index.html)。在本目录、已设置 `IDF_PATH` 后：

```sh
idf.py reconfigure
# 依赖下载后应用一次补丁，再编译：
patch -p1 -d managed_components/espressif__esp_hosted < patches/esp-hosted-c6-start.patch
idf.py build
idf.py -p 串口号 flash
```

Windows 辅助脚本（仍需本机已有 IDF 环境）：

```powershell
idf.py -B build-win reconfigure
# 编译前，用 Git patch 应用 patches/esp-hosted-c6-start.patch。
.\tools\build.ps1
.\tools\flash.ps1 -Port COMx
```

首次配置会把 LVGL 8.3.11、`esp_lvgl_port` 2.6.0、ESP-Hosted 等下载到 `managed_components/`（已忽略）。应在编译前应用 C6 重复 `STA_START` 补丁；若重新下载该依赖，需要重新应用。

Windows 可用 Git 自带的 `patch`，或按补丁手工改。BSP 已放在 `vendor/espressif__esp32_p4_function_ev_board`（旁边是 ST7701 驱动），`main/idf_component.yml` 指向这里，不需要再准备一份 `common_components`。

烧录**不会**整片擦除，NVS 里的打印机配置会保留。不要用从 0x0 写整包镜像的方式覆盖已经配过的板子，除非你就是要清配置。

## 主机服务

**待办**（`todo_service/`）：Python 3 标准库 + SQLite。复制 `deploy.env.example` 为 `deploy.env` 后：

```sh
set -a
. ./todo_service/deploy.env
set +a
python todo_service/server.py
```

上述启动命令适用于 Bash。`server.py` 从环境变量读取配置，不会自动加载 `deploy.env`；Windows 用户需在 PowerShell 中设置对应环境变量。

systemd 用户单元：`todo_service/p4-todo.service`（工作目录 `%h/p4-todo`）。飞书是可选的，创建开放平台应用，把 `FEISHU_APP_ID` / `FEISHU_APP_SECRET` / `FEISHU_REDIRECT_URI` 写进 `deploy.env`。详见[待办服务说明](todo_service/README.md)。

**视频中转**（`video_relay/`）：把 H2D 的 RTSPS（以及可选的 A1 JPEG）转成 800×480 JPEG。参考配置中，H2D 视频使用中转，A1 mini 支持直连 JPEG。MQTT 与直播能否同时稳定运行取决于打印机固件和局域网模式，需要在实际设备上验证。详见[视频中转说明](video_relay/README.md)和[协议文档](video_relay/P4_DEV.md)。

## 主机测试

```sh
python video_relay/tests.py
python todo_service/test_feishu.py
python tools/check_open_package.py
python tools/secret_scan.py
```

`video_relay/tests.py` 需要 Pillow（部分路径还要 `imageio-ffmpeg` 提供的 ffmpeg）。`tools/clock_dial_check.py` 需要首次运行 `idf.py` 下载的 LVGL `lv_math.c`。`tools/verify_bambu_config.py` 需要 `IDF_PATH` 和主机 C 编译器。

## 许可证

第一方代码为 MIT（`LICENSE`）。第三方说明见 `NOTICE`。Cisco OpenH264 条款在 `components/openh264/upstream/LICENSE` 和 `firmware/OpenH264-LICENSE.txt`。
