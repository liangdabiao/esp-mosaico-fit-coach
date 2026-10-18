# fit-coach 上机测试手册

> 服务器侧联调已全部完成（本机 mediapipe 0.10.14 实测：无人帧/真人帧/坏帧
> 10/10 通过；30 帧压测 p95=121ms；90 秒 300 帧设备节流闭环计数正确）。
> 本手册只差"插上设备"这最后一步。

## 0. 你需要

- ESP-Mosaico 主机 + 摄像头模块（LEFT 槽）
- 本 PC（已备好服务器环境 `server/venv`）与设备在同一 WiFi
- 数据线连接设备（设备操作只走 `mosaico.py`）

## 1. 启动本地服务器（本 PC 当服务器）

```powershell
cd projects\fit_coach
powershell -ExecutionPolicy Bypass -File server\start_server.ps1        # 真 MediaPipe
powershell -ExecutionPolicy Bypass -File server\start_server.ps1 -Mock  # 正弦模拟（调试用）
```

脚本会打印设备配网时要填的地址，形如 `http://192.168.1.2:8000`。

**若设备连不上服务器**：Windows 防火墙默认拦入站。用**管理员** PowerShell
执行一次（脚本启动时也会提示）：

```powershell
netsh advfirewall firewall add rule name="fit-coach 8000" dir=in action=allow protocol=TCP localport=8000
```

（首次绑定时系统也可能弹"允许访问"窗口，勾选专用网络点允许即可。）

自检（本机验证服务器活着）：

```powershell
curl http://127.0.0.1:8000/health
# -> {"ok": true, "mode": "mediapipe", ...}
```

## 2. 刷机（工作区根目录 D:\esp-mo\esp-mosaico-vibe-main）

```sh
# 先看设备是否在线（记下 Device ID，全程核对同一台）
python mosaico.py iris status --all --json
python mosaico.py iris list --details --json --project projects/fit_coach

# 新应用首次安装：system-update（不要 app-update）
python mosaico.py iris system-update --project projects/fit_coach

# 空白板/不确定基础固件时先：
python mosaico.py recover --project projects/fit_coach
```

刷完等设备重启进应用（主页：金色标题「姿态健身教练」）。
用 `python mosaico.py iris device-status --device-id <ID>` 取证设备健康状态。

## 3. 测试矩阵（按序勾选）

### A. 演示模式（不依赖网络，先验固件/UI）
- [ ] 主页显示今日/目标/模式「演示模式 · 正常」
- [ ] 点「开始训练」：大计数每 ~3 秒 +1、状态在「躺平/坐起」间切换、
      角度指针左右摆、进度条增长
- [ ] 计数音「哔」、达标（默认 30 个）奏琶音并显示「达标」
- [ ] 「暂停」后计数停、指针停；「继续」恢复
- [ ] 「结束」→ 结果页次数/时长正确 → 「完成」回主页
- [ ] 「历史」页出现本组记录；「设置」页各项可调（目标 ±、阈值 ±、音效开关）

### B. 配网 + 服务器模式（主路径）
- [ ] 若设备已存旧 WiFi：设置页「重置网络配置」（按两次）→ 重启后开热点
- [ ] 手机连热点 `fitcoach-XXXX` → 访问 `http://192.168.4.1`
- [ ] 填 WiFi 密码 + 服务器地址 `http://<PC的IP>:8000` → 保存 → 设备重启联网
- [ ] 主页模式显示「服务器 · 正常」（或训练页右上「正常」）
- [ ] 人离开镜头：训练页显示「请入镜」；人入镜侧躺：状态切换、角度合理
- [ ] **实际做 10 个仰卧起坐**：计数与实际一致（±1 以内）、每次「哔」
- [ ] 结束后历史记录正确

### C. 断网恢复
- [ ] 训练中把服务器 Ctrl+C 停掉 → 几秒内右上变「无信号/失败」横幅，计数保留
- [ ] 重启服务器 → 恢复「正常」，计数继续累加

### D. 云端 AI 模式（可选，需 OpenAI 兼容视觉 API）
- [ ] 重置网络 → 门户里填 VLM 端点/模型名/key（服务器地址留空）→ 保存
- [ ] 设置页确认模式「云端直连」；做几个慢速仰卧起坐
- [ ] 预期：每 4 秒一批（4 帧组批）出结果；响应慢属正常（1~3s/批），
      计数仍应正确（时间戳在采集侧）

### E. 重新安装/断电
- [ ] 断电重启：设置与历史保留（NVS）；自动重连 WiFi
- [ ] 代码更新后重刷：`iris system-update --project projects/fit_coach`

## 4. 遇到问题

| 现象 | 排查 |
| --- | --- |
| 刷机后黑屏/反复重启 | `python mosaico.py iris device-status` 取 core dump/log；确认不是摄像头模块接触不良 |
| 训练页一直「请入镜」 | 侧向全身入镜、光线充足；PC 上直接 curl 一帧看 visible 是否 true（可临时用 mock 排除设备侧） |
| 一直「无信号」 | 服务器没起/IP 填错/防火墙（见 §1）；手机浏览器访问 `http://<PC IP>:8000/health` 应返回 JSON |
| SC101IOT 花屏/偏色 | 上游 esp_cam_sensor 2.6.0 与 BSP PCLK 补丁不兼容（已知），换 OV3640 验证 |
| 计数偏多/偏少 | 设置页微调「躺下阈值/坐起阈值」（默认 110/55）；两阈值间是防抖区 |
| GPIO33 提示 | 摄像头占用 USB-Serial/JTAG pad，console 走 UART 属预期；mosaico.py 走 Iris HS USB 不受影响，若设备操作异常先拔摄像头模块重试 |

## 5. 取证

```sh
python mosaico.py iris device-status --all --json     # 实时身份/状态
python mosaico.py iris operation-status --json        # 操作记录
python mosaico.py iris screenshot                     # 屏幕实拍（验收存档）
```
