# fit-coach 姿态服务器部署

设备把 JPEG 帧推给 `POST /frame`，服务器用 MediaPipe（需求文档同款算法）
回传肩-髋-膝角度与状态；计数在设备端完成，服务器无状态、可随意重启。

## 依赖（Python 3.8~3.11；mediapipe 0.9.x 不支持更新的 Python）

```sh
python -m venv venv
venv\Scripts\activate          # Windows
pip install -r requirements.txt
```

⚠️ mediapipe 必须锁 `0.9.1.0`（新版移除 `mp.solutions`，需求文档坑位）。

## 本机 / 局域网盒子

```sh
uvicorn pose_server:app --host 0.0.0.0 --port 8000
```

设备的配网门户里填 `http://<本机局域网IP>:8000`（如 `http://192.168.1.10:8000`）。

联调（不装 mediapipe 也能起，角度走 3 秒周期正弦）：

```sh
# Windows PowerShell
$env:FC_MOCK="1"; uvicorn pose_server:app --host 0.0.0.0 --port 8000
# bash
FC_MOCK=1 uvicorn pose_server:app --host 0.0.0.0 --port 8000
```

验证：

```sh
curl http://127.0.0.1:8000/health
curl -X POST -H "X-FC-TS: 123" --data-binary @some.jpg http://127.0.0.1:8000/frame
```

## 宝塔面板部署（Python 项目）

1. 宝塔 → Python 项目 → 添加项目，路径选本目录；
2. 框架选 `python通用`，启动方式 `gunicorn`（或 command 方式：
   `venv/bin/uvicorn pose_server:app --host 0.0.0.0 --port 8000`）；
3. 端口放行 8000；如需外网访问，配 Nginx 反代 + `client_max_body_size 4m`
   （单帧 JPEG ~100KB，默认 1m 即可，留余量）；
4. 设备在配网门户填 `http://<服务器IP或域名>:8000`。

## 性能参考

单帧 720p JPEG 在普通 x86 盒子上 MediaPipe 推理约 30~80ms；
设备默认 300ms/帧（约 3fps），单人训练足够，CPU 占用很低。
