# ESP32-P4 桌面屏（开源快照）

800×480 横屏 LVGL 界面：拓竹 H2D / A1 mini 打印状态、待机时钟、Codex 配额页、待办列表。旁边还有两套可选的局域网服务：Python 待办 / 飞书同步，以及把打印机摄像头转成 JPEG 给开发板的中转。

本目录是一份**可公开复用的快照**，不是某台机器上的私有工程。适合：

- 同一块板：填自己的 Wi-Fi / 服务地址后编译烧录
- 另一块板：让你自己的 agent 抽走可复用模块，替换显示 / 触摸 / Wi-Fi 胶水

**不是**随便一块 ESP32 都能直接刷的成品固件。给程序看的模块地图见 [AGENTS.md](AGENTS.md)。英文说明见 [README.md](README.md)。

## 这份快照对应的硬件

- **主控：** ESP32-P4，16 MB Flash，PSRAM（200 MHz）。时钟数字缓存在 PSRAM，建议 16 MB PSRAM。
- **Wi-Fi：** 板载 ESP32-C6，经 ESP-Hosted、SDIO slot 1。默认脚在 `sdkconfig.defaults`：CLK 18、CMD 19、D0 14、D1 15、D2 16、D3 17、C6 复位 54。
- **屏：** ST7701 MIPI-DSI，物理 480×800，RGB565，双 lane。LVGL 排版是 800×480，`p4_display.c` 用 PPA 旋转。BSP 选项 `CONFIG_BSP_LCD_TYPE_1024_600` **名字来自旧示例，这里实际是 480×800 ST7701 分支**，不要按名字改成 1024×600 屏。
- **触摸：** GT911，I2C SDA GPIO7、SCL GPIO8。背光 GPIO23，LCD 复位 GPIO5。
- **工具链：** ESP-IDF **5.4.x**（≥5.4 且 &lt;6.0），**LVGL 8.3.11**。不要用 IDF 6 或 LVGL 9。

板子对得上，主要就是填本地凭据再烧录。对不上，留下 `components/portable_ui`、`todo_service`、`video_relay`，换显示 / 触摸 / 无线部分。

## 可以直接拿走的部分

| 路径 | 作用 |
| --- | --- |
| `components/portable_ui/` | 800×480 LVGL 界面（打印机页、时钟、Codex、待办、事件动画） |
| `todo_service/` | 局域网待办 HTTP + SQLite，可选飞书同步 |
| `video_relay/` | 局域网 JPEG 中转，避免 P4 软件解 H2D High Profile |
| `main/bambu_config.c` | NVS 打印机配置 + 本机网页 |
| `assets/` | 字体、图标、界面预览（`assets/preview/`） |

板级胶水：`main/p4_display.c`、`main/main.c` 的 BSP 启动、`vendor/` 里的 BSP/LCD、C6 SDIO 脚、以及大部分 `sdkconfig.defaults`。

## 本地配置（真机烧录前必做）

仓库里**没有**真实密码。先复制示例再改你自己的值：

```text
copy main\wifi_credentials.example.h   main\wifi_credentials.h
copy main\todo_credentials.example.h   main\todo_credentials.h
copy main\codex_credentials.example.h  main\codex_credentials.h
copy todo_service\deploy.env.example   todo_service\deploy.env
```

Linux / macOS 用 `cp`。然后：

1. 在 `main/wifi_credentials.h` 填写 Wi-Fi 名称和密码。
2. 在 `main/todo_credentials.h` 填写待办服务地址 `TODO_URL` 和设备 Bearer（必须与 `deploy.env` 里的 `TODO_DEVICE_TOKEN` 一致）。
3. 在 `main/codex_credentials.h` 填写 Codex 状态 JSON 接口；不用该页可保持占位。
4. 在 `main/video_relay.h` 把 `VIDEO_RELAY_IP` 改成跑 `video_relay/` 的主机（占位是 `192.168.1.10`）。
5. 填写 `todo_service/deploy.env`：网页密码、设备 token、会话密钥，以及可选的飞书应用 id / secret / 回调地址。

这些实文件已在 `.gitignore` 里，不要提交。

打印机的局域网 IP、序列号、访问码**不编进固件**。联网后屏幕会显示开发板 IP 和 6 位 PIN。浏览器打开 `http://<开发板IP>/`，用户名 `admin`，密码为该 PIN，再保存 H2D / A1 mini。PIN 每次重启都会变。打印机 MQTT 为局域网 8883，用户名 `bblp`。本快照不发送暂停 / 取消 / 开始打印指令。

## 编译与烧录

安装 [ESP-IDF 5.4.x](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32p4/get-started/index.html)。在本目录、已设置 `IDF_PATH` 后：

```sh
idf.py build
idf.py -p 串口号 flash
```

Windows 辅助脚本（仍需本机已有 IDF 环境）：

```powershell
.\tools\build.ps1
.\tools\flash.ps1 -Port COMx
```

首次配置会把 LVGL 8.3.11、`esp_lvgl_port` 2.6.0、ESP-Hosted 等下载到 `managed_components/`（已忽略）。下载完成后给 C6 的重复 `STA_START` 打补丁：

```sh
patch -p1 -d managed_components/espressif__esp_hosted < patches/esp-hosted-c6-start.patch
```

Windows 可用 Git 自带的 `patch`，或按补丁手工改。BSP 已放在 `vendor/espressif__esp32_p4_function_ev_board`（旁边是 ST7701 驱动），`main/idf_component.yml` 指向这里，不需要再准备一份 `common_components`。

烧录**不会**整片擦除，NVS 里的打印机配置会保留。不要用从 0x0 写整包镜像的方式覆盖已经配过的板子，除非你就是要清配置。

## 主机服务

**待办**（`todo_service/`）：Python 3 标准库 + SQLite。复制 `deploy.env.example` 为 `deploy.env` 后：

```sh
python todo_service/server.py
```

systemd 用户单元：`todo_service/p4-todo.service`（工作目录 `%h/p4-todo`）。飞书是可选的，自己建开放平台应用，把 `FEISHU_APP_ID` / `FEISHU_APP_SECRET` / `FEISHU_REDIRECT_URI` 写进 `deploy.env`。细节见 `todo_service/README.md`。

**视频中转**（`video_relay/`）：把 H2D 的 RTSPS（以及可选的 A1 JPEG）转成 800×480 JPEG。打印机上同时开局域网直播和 LAN MQTT 会把 MQTT 打掉，所以这块板对 H2D 走中转、对 A1 mini 仍可直连 JPEG。细节见 `video_relay/README.md` 和 `video_relay/P4_DEV.md`。

## 不插板也能跑的测试

```sh
python video_relay/tests.py
python todo_service/test_feishu.py
python tools/check_open_package.py
python tools/secret_scan.py
```

`video_relay/tests.py` 需要 Pillow（部分路径还要 `imageio-ffmpeg` 提供的 ffmpeg）。`tools/clock_dial_check.py` 需要第一次 `idf.py` 拉下来的 LVGL `lv_math.c`。`tools/verify_bambu_config.py` 需要 `IDF_PATH` 和主机 C 编译器。

## 许可证

第一方代码为 MIT（`LICENSE`）。第三方说明见 `NOTICE`。Cisco OpenH264 条款在 `components/openh264/upstream/LICENSE` 和 `firmware/OpenH264-LICENSE.txt`。
