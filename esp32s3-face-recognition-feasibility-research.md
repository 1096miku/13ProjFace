# ESP32-S3 本地离线人脸检测 + 1:N 人脸识别 — 事实核查报告

- 核查日期：**2026-09-15**（本机系统时间；下文所有"读取日期"均指此日）
- 口径标注：【官方】= Espressif / Seeed 官方仓库、README、文档站、Component Registry API；【社区】= 第三方博客 / 论坛 / 用户 issue 实测
- 本报告只陈述事实与证据（链接 + 日期 + 版本号），**不含项目建议、不含代码**；查不到的项一律写"未找到证据"，不做推测。凡"推算"均显式标注。
- **证据强度分级与未复核项**见第 8 节；第 7 节是"未找到证据"清单。核查过程中 GitHub REST API 出现匿名限流（403），少数 issue 的评论正文与分支详情未能二次复核，均在正文标注"未复核"。
- 三处**必须注意的口径纠正**（详见对应章节）：① 每人特征不是"512 字节"而是 **2048 字节**（512 维 float32，官方另加 2 字节 id = 2050 B/人，见 3.4）；② esp-dl 3.x **不存在** `msr01/mnp01/mnp02` 命名（见 3.1）；③ Seeed 官方那 20 人上限的限定对象是 **Watcher 的 Himax WE2 NPU**，不是 ESP32-S3 CPU（见 5.3）。

---

## 0. 本机约束的自查核对（用于对齐量级）

| 项 | 本机事实 | 证据 |
|---|---|---|
| 目标固件 app 二进制 | `xiaozhi.bin` = 2,997,376 B ≈ 2.858 MiB | 本地 `D:\vscode\ESP32Project\xiaozhi-esp32-v2.2.4\build\xiaozhi.bin`（2026-09-15 读） |
| assets 分区镜像 | `generated_assets.bin` = 2,851,767 B ≈ 2.719 MiB | 同上目录 |
| app 分区大小 | `ota_0` = `0x3f0000` = 4,128,768 B = 3.9375 MiB = 4.13 MB(十进制) | [partitions/v2/16m.csv](https://github.com/78/xiaozhi-esp32/blob/main/partitions/v2/16m.csv)（本地副本 `xiaozhi-esp32-v2.2.4\partitions\v2\16m.csv`） |
| assets 分区 | `assets, data, spiffs, 0x800000, 8M` | 同上 |
| 已落地组件解析版本 | esp-dl **3.3.11**、esp-sr **2.5.3**、esp32-camera **2.1.7**、dl_fft 0.7.0、esp-dsp 1.8.0、idf **5.5.3**、target esp32s3 | 本地 `D:\vscode\ESP32Project\11_PCA9577\dependencies.lock`（2026-09-15 读，共 139 行） |
| esp-sr 对 esp-dl 的依赖 | esp-sr 2.5.3 声明 `espressif/esp-dl >=3.3.10`（require: private） | 同上 lock 第 60–85 行 |

> 事实要点：**esp-sr 自身就依赖 esp-dl**（`>=3.3.10`），即启用 esp-sr 的项目里 esp-dl 已经在构建图中，两者共用同一份 esp-dl，不存在"两套推理框架"。来源同上 lock 文件（版本 3.3.11 / 2026-09-15 读）【官方构件清单，本机文件】。

---

## 1. esp-who 的维护状态

### 1.1 仓库与提交

| 项 | 事实 | 来源 |
|---|---|---|
| 是否归档 | **未归档**：`archived: false`、`disabled: false` | [api.github.com/repos/espressif/esp-who](https://api.github.com/repos/espressif/esp-who)（2026-09-15 读）【官方】 |
| 最后 push | **2026-08-21T15:10:09Z** | 同上 |
| 最后提交 | `1abda05` "Merge branch 'object_track' into 'master'"（作者 Fan Shen Wei，2026-08-21T15:10:06Z） | [commit 1abda05](https://github.com/espressif/esp-who/commit/1abda05e1c0782237fcb9e8d33a2fa7e105f06b2)【官方】 |
| 其他近期提交 | 2026-07-28 `doc: add idf6.0/idf6.1 in readme.`；2026-08-04 `ocr example`；2026-08-06 `docs: remove EOL note from ESP32-S3-EYE user guides` | [commits?per_page=10](https://api.github.com/repos/espressif/esp-who/commits?per_page=10)【官方】 |
| stars / forks / open issues | 2144 / 546 / 118 | [api.github.com/repos/espressif/esp-who](https://api.github.com/repos/espressif/esp-who)【官方】 |

结论：**未见任何"归档 / 停止维护 / EOL"的官方声明**；仓库在 2026-07 ~ 2026-08 仍有 Espressif 员工提交。【官方】

### 1.2 最后一次 release / 最后一次 tag

- **GitHub Releases 页面只有 2 个 release**（API 返回全部，无分页）：`v0.9.0`（published **2019-01-03**，prerelease，标题 "Face recognition and speech wake up for esp-eye"）与 `v0.5.0`（published **2018-12-03**）。
  → **"最后一次 release" = v0.9.0，2019-01-03**。[releases API](https://api.github.com/repos/espressif/esp-who/releases)【官方】
- **Tags 最新 = `v1.1.0`**，指向 commit `205da6d3193989794a68c49f0b7dcf524bff5a74`，该 commit 日期 **2025-03-25**，提交信息原文：`Update submoudle esp-dl. Now esp-who support up to idfv5.3.` [tags API](https://api.github.com/repos/espressif/esp-who/tags)、[commits?sha=release/v1.1.0](https://api.github.com/repos/espressif/esp-who/commits?sha=release/v1.1.0)【官方】
- tag 列表 = `v1.1.0, v1.0.0, v0.9.4, v0.9.3, v0.9.2, v0.9.1, v0.9.0, v0.6.1, v0.6.0, v0.5.1, v0.5.0, v0.1.1, v0.1.0`。
  → **不存在 v2.x / v3.x tag**。"esp-who v3 / v2.0.0" 作为发布版本号：**未找到证据**。[tags API](https://api.github.com/repos/espressif/esp-who/tags)【官方】

### 1.3 README 声明的支持范围（master 分支）

来源：[esp-who master README](https://raw.githubusercontent.com/espressif/esp-who/master/README.md)（2026-09-15 读）【官方】

- **支持的 ESP-IDF 版本**：`Release/v5.4` ✔、`Release/v5.5` ✔、`Release/v6.0` ✔、`Release/v6.1` ✔
- **支持的开发板**：ESP32-P4 Function EV Board（esp32p4）、**ESP32-S3-EYE**（esp32s3）、**ESP32-S3-Korvo-2**（esp32s3）
- **不支持的芯片**（README 原文）："Some chip such as esp32 and esp32-s2, and examples such as cat face detection, color detection is not available in this branch currently"
- **旧分支**：README 明示 `Old branch can be found here. [old ESP-WHO branch](https://github.com/espressif/esp-who/tree/release/v1.1.0)`
- **esp-dl 版本要求**：master README **未写具体 esp-dl 版本号**；但示例的锁定文件给出了实际解析结果（见第 2 节）。
- **master 分支 examples 目录实际内容**（`api.github.com/repos/espressif/esp-who/contents/examples`）：`human_face_recognition`、`object_detect`、`object_tracking`、`pp_ocr_v6`、`qrcode_recognition`。
  → **master 上已没有 `human_face_detection` 独立示例**（旧分支 release/v1.1.0 有）。【官方】
- **master 的人脸识别示例组成部分**：`main/CMakeLists.txt` requires `who_spiflash_fatfs, who_recognition_app, esp_video_cxx`；顶层组件目录为 `who_app / who_detect / who_frame_cap / who_frame_lcd_disp / who_peripherals / who_pp_ocr_v6 / who_qrcode / who_recognition / who_task`。[raw main/CMakeLists.txt](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/main/CMakeLists.txt)、[contents/components](https://api.github.com/repos/espressif/esp-who/contents/components)【官方】

### 1.4 后继项目 / 相关项目

- **官方未声明 esp-who 有后继项目**（未找到证据）。master README 的"Resources"段把项目分成两类：
  - 部署自己的模型 → [ESP-DL](https://github.com/espressif/esp-dl)、[ESP-DETECTION](https://github.com/espressif/esp-detection)
  - camera 驱动 → [ESP32_CAMERA](https://github.com/espressif/esp32-camera)、[ESP_VIDEO_COMPONENTS](https://github.com/espressif/esp-video-components)
  [README](https://raw.githubusercontent.com/espressif/esp-who/master/README.md)（2026-09-15 读）【官方】
- **官方博客（2025-09-22）明确 esp-video 的继任对象是 esp32-camera，而不是 esp-who**，原文：`esp32-camera is the first-generation camera application development component`；`esp-video is an enhanced version of esp32-camera`；同一篇把 esp-dl **与 esp-who** 并列为端侧 AI 来源：`ESP32-S and ESP32-P series of chips support on-device AI via esp-dl and esp-who, enabling features like face recognition...`。[developer.espressif.com/blog/2025/09/esp-video-introduction/](https://developer.espressif.com/blog/2025/09/esp-video-introduction/)（2025-09-22）【官方】
- `espressif/esp-detection`：定位为目标检测（`Lightweight real-time object detection on ESP series chips, based on Ultralytics YOLOv11`），created 2025-04-29，最后 push 2026-03-05，AGPL-3.0，stars 160。[api.github.com/repos/espressif/esp-detection](https://api.github.com/repos/espressif/esp-detection)（2026-09-15 读）【官方】
- **模型代码在哪**：人脸检测/识别模型并不在 esp-who 仓库里，而是位于 **esp-dl 仓库的 `models/` 目录（ESP-DL Model Zoo）**，以独立 IDF 组件发布（`espressif/human_face_detect`、`espressif/human_face_recognition`）。esp-dl 示例 README 反向指路 esp-who：`See full example in esp-who`。[models/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/README.md)、[examples/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md)（2026-09-15 读）【官方】
  → "human_face_detect / human_face_recognition 是 esp-who 拆分出来的组件"这一说法：**未找到官方声明**（事实是它们由 esp-dl 仓库发布，esp-who 反过来依赖它们）。

---

## 2. 在 ESP-IDF 5.5.x 上能否用

### 2.1 官方声明的版本支持

| 来源 | 原文 / 事实 | 日期 |
|---|---|---|
| esp-who master README | 支持矩阵含 **ESP-IDF Release/v5.5 ✔** | 2026-09-15 读【官方】 |
| esp-dl README | `Please use ESP-IDF release/v5.3 or above.` | 2026-09-15 读【官方】 |
| esp-dl 文档（对应 v3.3.11，PDF 名 `esp-dl-en-v3.3.11-25-gb7e9d88a57.pdf`） | `Please use release/v5.3 or higher version of ESP-IDF.` | [docs.espressif.com/projects/esp-dl/en/latest/getting_started/readme.html](https://docs.espressif.com/projects/esp-dl/en/latest/getting_started/readme.html)【官方】 |
| esp-dl `esp-dl/idf_component.yml`（3.3.11） | 逐 target 约束：`esp32/esp32c2/esp32c3/esp32c6/esp32p4/esp32s2/**esp32s3**` → `idf >=5.3`；`esp32c5` → `>=5.5`；`esp32s31` → `>=6.0` | 2026-09-15 读【官方】 |
| esp-dl 示例 README（human_face_recognition / human_face_detect） | 支持表列 **ESP-IDF v5.3 / v5.4** ✔（S3 与 P4）；**表中未列 v5.5** | 2026-09-15 读【官方】 |
| **Espressif 官方博客《ESP-WHO: Get started》** | 原文：`ESP-WHO currently targets ESP-IDF v5.5.x`（并写明"at the time of writing, 5.5.4"，提示 EIM 快速安装默认 ESP-IDF v6、需选 Custom installation 装 5.5.x）；教程内容即**在 ESP32-S3-EYE 上编译并运行 face recognition 示例**，并 `set-target esp32s3`；同时说明 `The current version targets the ESP32-S3 and the newer ESP32-P4 chips` | [developer.espressif.com/blog/2026/05/esp-who-get-started/](https://developer.espressif.com/blog/2026/05/esp-who-get-started/)（**2026-05-28**，作者 Francesco Bez / Espressif DevRel）【官方】 |

> 口径差异提示（均为官方，需并列引用，不要二选一）：**master README 支持矩阵**写 v5.4/v5.5/v6.0/v6.1 全 ✔；**2026-05-28 官方博客**写 "currently targets ESP-IDF v5.5.x"。该博客还给出官方实测启动日志（`Detected OV2640 camera`、`LVGL: Starting LVGL task`、`button: IoT Button Version: 4.1.5`、`dl::Model: Minimize() will delete variables not used in model inference...`），可佐证 S3 上人脸识别示例确能启动运行。

### 2.2 版本 + 依赖链（Component Registry API）

来源：[components.espressif.com/api/components/espressif/human_face_detect](https://components.espressif.com/api/components/espressif/human_face_detect)、[.../human_face_recognition](https://components.espressif.com/api/components/espressif/human_face_recognition)、[.../esp-dl](https://components.espressif.com/api/components/espressif/esp-dl)（2026-09-15 读）【官方】

| 组件 | 最新版本 | 上传日期 | 声明的依赖 |
|---|---|---|---|
| `espressif/esp-dl` | **3.3.11** | 2026-09-03 | `espressif/dl_fft >=0.6.0`、`espressif/esp_new_jpeg ^1`、`idf *`（**无 IDF 版本上限**）；targets 含 `esp32s3` |
| `espressif/human_face_detect` | **0.5.0** | 2026-06-08 | `espressif/esp-dl ~3.3.0` |
| ─ 上一版 | 0.4.2 | 2026-05-12 | `espressif/esp-dl ~3.3.0` |
| ─ 更早 | 0.4.1 / 0.4.0 | 2026-03-26 / 2026-03-12 | `~3.3.0` / `~3.2.0` |
| ─ 最早 | 0.1.0 | 2024-10-25 | `idf >=5.3` + `esp-dl ^3.0.0`（**唯一显式声明 IDF 门槛的版本**） |
| `espressif/human_face_recognition` | **0.3.2** | 2026-05-12 | `espressif/human_face_detect ~0.4.1`（**不直接依赖 esp-dl**） |
| ─ 上一版 | 0.3.1 / 0.3.0 | 2026-03-26 / 2025-10-21 | `~0.4.1` / `~0.3.0` |

**与 esp-dl 3.x 的兼容性结论（基于上表）**：`human_face_detect 0.5.0` 声明 `esp-dl ~3.3.0`（即 `>=3.3.0,<3.4.0`），**本机 esp-dl 3.3.11 满足**。`human_face_recognition 0.3.2` 经 `human_face_detect ~0.4.1` 间接要求 esp-dl `~3.3.0`，**同样满足**。

**一个版本解析冲突事实**：`human_face_recognition` 最新已发布版 0.3.2 要求 `human_face_detect ~0.4.1`（`>=0.4.1,<0.5.0`），**而 detect 最新是 0.5.0**，两者不互容。因此在"检测+识别"同时使用时，解析结果必然是 **detect 0.4.2 + recognition 0.3.2**（与 esp-who master 锁定文件一致）；只用检测时才可能取到 0.5.0。（master 源码里 recognition 已改为 `~0.5.0`，但**未随 0.3.2 发布**。）来源同 [Registry API](https://components.espressif.com/api/components/espressif/human_face_recognition)、[master idf_component.yml](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/idf_component.yml)【官方】

### 2.3 官方 CI/示例的锁定组合（最强的"IDF 5.5 + esp-dl 3.3 可用"证据）

esp-who master 的 `examples/human_face_recognition/dependencies.lock.esp32_s3_eye`（2026-09-15 读）【官方】：

- `idf: 5.5.5`
- `espressif/esp-dl: 3.3.8`
- `espressif/human_face_detect: 0.4.2`、`espressif/human_face_recognition: 0.3.2`
- `espressif/esp_video: 2.0.1`（`idf >=5.4`）、`espressif/esp_cam_sensor: 2.0.1`、`espressif/esp32_s3_eye (BSP): 6.0.0`（`idf >=5.4`）、`lvgl/lvgl: 9.5.0`
- target: `esp32s3`；`direct_dependencies` 含 `espressif/human_face_recognition`

来源：[dependencies.lock.esp32_s3_eye](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/dependencies.lock.esp32_s3_eye)（对应 IDF 5.5.x 快照）【官方】
→ 该 lock 的 `sdkconfig.bsp.esp32_s3_eye` 同时是 **IDF 5.5.5** 工程，见 [sdkconfig.bsp.esp32_s3_eye](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye)（首行 `# Espressif IoT Development Framework (ESP-IDF) 5.5.5 Project Minimal Configuration`）【官方】

### 2.4 关键 issue（编号 / 标题 / 状态 / 日期）

| Issue | 标题 | 状态 | 日期 | 内容要点 |
|---|---|---|---|---|
| [esp-who#334](https://github.com/espressif/esp-who/issues/334) | `esp32-s3-cam esp who (AIS-2155)` | **open**（更新 2025-10-30） | 创建 2025-10-05 | 用户在 **ESP-IDF 5.5** + **esp-who release v1.1.0** 上编译 `examples/human_face_detection/web`，目标 ESP32-S3-CAM（GOOUUU），烧录后**反复重启**；日志停在 bootloader。作者原文 `Project release version: ESP-IDF 5.5 ，esp-who-release-v1.1.0`。**注意：这是"旧分支 v1.1.0 + 非官方支持板"的组合，不是 master 分支** |
| [esp-dl#226](https://github.com/espressif/esp-dl/issues/226) | `Face recognition documentation doesn't match reality (AIV-776)` | closed（2025-04-08） | 创建 2025-04-03，10 条评论 | 社区实测与文档不符：作者在 **ESP32-S3** 上测 `MFN: 4200 ms`、`MBF: 730 ms`，并怀疑两个模型被调换。**与官方 README 表格（MFN 248.8ms / MBF 1072.4ms）方向相反** |
| [esp-who#240](https://github.com/espressif/esp-who/issues/240) | `IDF 5.0 support. (AIV-574)` | closed（2024-08-02） | 创建 2023-01-24 | 询问 esp-who 是否支持 IDF 5.0，已关闭 |
| [esp-dl#278](https://github.com/espressif/esp-dl/issues/278) 等 | — | — | — | 本轮检索**未找到**"自某版本起 human_face_detect / human_face_recognition 在 IDF 5.x 上不兼容"的官方声明或 issue |

补充事实：`espressif/esp-dl` 的 `3.3.7` 版本状态为 **Yanked**，官方 yank 原因原文 `do not support idf v6.0`（说明维护方会因 IDF 兼容性主动下架版本）。[esp-dl versions](https://components.espressif.com/components/espressif/esp-dl/versions)（2026-09-15 读）【官方】

---

## 3. 模型体积与存放位置

### 3.1 模型文件实际大小（`.espdl`，ESP32-S3 一套）

来源：[api.github.com/repos/espressif/esp-dl/contents/models/human_face_detect/models/s3](https://api.github.com/repos/espressif/esp-dl/contents/models/human_face_detect/models/s3)、[.../human_face_recognition/models/s3](https://api.github.com/repos/espressif/esp-dl/contents/models/human_face_recognition/models/s3)（master 分支，2026-09-15 读；`size` 字段为 Git blob 字节数）【官方】

| 模型（S3 版） | 文件 | 字节 | KiB |
|---|---|---|---|
| 检测 第一阶段 MSR | `human_face_detect_msr_s8_v1.espdl` | 61,168 | 59.7 |
| 检测 第二阶段 MNP | `human_face_detect_mnp_s8_v1.espdl` | 129,968 | 126.9 |
| 检测 单阶段（小） | `espdet_pico_224_224_face.espdl` | 480,384 | 469.1 |
| 检测 单阶段（大） | `espdet_pico_416_416_face.espdl` | 499,312 | 487.6 |
| 识别 MFN（默认） | `human_face_feat_mfn_s8_v1.espdl` | 1,295,168 | 1,264.8 |
| 识别 MBF | `human_face_feat_mbf_s8_v1.espdl` | 3,522,784 | 3,440.2 |

**组合合计（本报告推算，基于上表字节数）**：

- MSR + MNP = **191,136 B ≈ 186.7 KiB ≈ 0.182 MiB**
- MSR + MNP + MFN = **1,486,304 B ≈ 1.417 MiB**
- MSR + MNP + MBF = **3,713,920 B ≈ 3.542 MiB**

> 对照本机 app 分区：ota_0 = 3.9375 MiB，当前固件占 2.858 MiB，**余量 ≈ 1.08 MiB（十进制 4.13−2.86=1.27 MB）**。
> 说明：`.espdl` 文件是仓库中随源码发布的文件（可能含 `export_test_values` 测试向量；官方文档指出部署用可导出**不含测试数据**的版本以减小体积）。来源：[how_to_load_test_profile_model](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_load_test_profile_model.html)（2026-09-15 读）【官方】

**文件大小与"参数量/延迟"的一致性交叉校验**（用于排除误读）：官方表格给出 `mfn_s8_v1` = **1.2 M params / 0.46 GFLOPs / 248.8 ms**，`mbf_s8_v1` = **3.4 M params / 0.90 GFLOPs / 1072.4 ms**。上表中 **MFN 文件 1,295,168 B（小）↔ 对应小模型/快模型**，**MBF 文件 3,522,784 B（大）↔ 对应 3.4M 参数/慢模型**，三处（文件大小、参数量、延迟）方向一致。若有资料把 MFN 记为 3.36 MB、MBF 记为 1.24 MB，则与官方参数量/延迟表矛盾，属**名称与数值错配**，本报告不采用。

**命名澄清（重要）**：**esp-dl 3.x 中不存在 `human_face_detect_msr01` / `mnp01` / `mnp02` 这类文件或组件名**。3.x 的实际命名只有 `MSRMNP_S8_V1`（= `human_face_detect_msr_s8_v1` + `human_face_detect_mnp_s8_v1` 两阶段）、`ESPDET_PICO_224_224_FACE`、`ESPDET_PICO_416_416_FACE`。`msr01/mnp01` 属旧版（esp-dl 2.x / esp-who v1.x 时代）的头文件名，仅见于社区旧工程贴文，**非官方 3.x 声明**：[Arduino Forum 帖](https://forum.arduino.cc/t/human-face-detect-mnp01-hpp-no-such-file-or-directory/1334499)、[SunFounder Forum 帖](https://forum.sunfounder.com/t/cant-compile-iot-2-camera-server-ino-missing-human-face-detect-msr01/4079/3)（社区）。esp-dl 示例目录 `examples/human_face_detection`：**master 上不存在**（只有 `human_face_detect`、`human_face_recognition` 等）→ 若在旧资料里看到 `human_face_detection`，属旧分支命名。【官方目录清单 + 社区】

> 关于 msr01/mnp01/mnp02 的"官方文件大小"：**未找到证据**（3.x 无此文件）。

**构建期会被"打包成单个模型文件"**（官方源码，v3.3.11 口径）：
- 检测组件 `models/human_face_detect/CMakeLists.txt`：按 Kconfig 勾选的模型（`CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1`、`CONFIG_FLASH_ESPDET_PICO_224_224_FACE`、`CONFIG_FLASH_ESPDET_PICO_416_416_FACE`）调用 `pack_espdl_models.py --model_path ... --out_file ${BUILD_DIR}/espdl_models/human_face_detect.espdl`，**合成一个文件**；rodata 模式用 `target_add_aligned_binary_data()` 嵌入，partition 模式用 `esptool_py_flash_to_partition(flash "human_face_det" ${packed_model})`。
- 识别组件同理，合成 `human_face_feat.espdl`，分区名 `human_face_feat`。
- [models/human_face_detect/CMakeLists.txt (v3.3.11)](https://raw.githubusercontent.com/espressif/esp-dl/v3.3.11/models/human_face_detect/CMakeLists.txt)、[models/human_face_recognition/CMakeLists.txt (v3.3.11)](https://raw.githubusercontent.com/espressif/esp-dl/v3.3.11/models/human_face_recognition/CMakeLists.txt)（2026-09-15 读）【官方】
  → 因此**实际落盘/落分区的是"打包后"的模型文件**：只启用 MSR+MNP 时，检测侧打包文件大小 ≈ 191,136 B（两文件之和；打包开销未找到证据）；启用 pico416 时 ≈ 499,312 B；识别侧只启用 MFN 时 ≈ 1,295,168 B。

**版本差异（可作参照）**：v3.0.0 tag 下同目录 `msr = 61,564 B`、`mnp = 130,300 B`（与 master 的 61,168 / 129,968 不同）。[contents?ref=v3.0.0](https://api.github.com/repos/espressif/esp-dl/contents/models/human_face_detect/models/s3?ref=v3.0.0)（2026-09-15 读）【官方】

### 3.2 模型是编进 app 还是可从分区/文件系统加载 —— 官方明确支持三种

esp-dl 官方教程《How to load & test & profile model》定义了三种模型位置，API 为 `fbs::MODEL_LOCATION_*`：
[How to load & test & profile model](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_load_test_profile_model.html)（文档版 v3.3.11，2026-09-15 读）【官方】

| 方式 | 宏 / API | 官方要点原文摘录 |
|---|---|---|
| 编进 app 的 `.rodata` | `MODEL_LOCATION_IN_FLASH_RODATA`；`target_add_aligned_binary_data()` 嵌入；`extern const uint8_t model_espdl[] asm("_binary_model_espdl_start")` | `Large models embedded in .rodata may require increasing the app partition size in partition.csv.` |
| **独立 flash 分区** | `MODEL_LOCATION_IN_FLASH_PARTITION`；`new dl::Model("model", ...)`（参数=分区名）；`esptool_py_flash_to_partition(flash "model" ...)` | 分区要求：Type=`data`，**SubType=`spiffs`**，Size 必须大于模型文件；建议用 `idf.py app-flash` 只烧 app 分区 |
| **SD 卡** | `MODEL_LOCATION_IN_SDCARD`；`new dl::Model("/sdcard/model.espdl", ...)` | FAT32；`Loading from SD card is slower... the model data must be copied from the SD card to RAM` |

两个模型组件 README 也都列出对应 Kconfig 三选一开关，并给出**必需的分区名**：
[human_face_detect README](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_detect/README.md)、[human_face_recognition README](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/README.md)（2026-09-15 读）【官方】

- 检测：`CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA` / `..._IN_FLASH_PARTITION` / `..._IN_SDCARD`
  → 用分区时 `partition.csv` **必须含名为 `human_face_det` 的分区**，且足够大
- 识别：`CONFIG_HUMAN_FACE_FEAT_MODEL_IN_FLASH_RODATA` / `..._IN_FLASH_PARTITION` / `..._IN_SDCARD`；另有 `CONFIG_FLASH_HUMAN_FACE_FEAT_MFN_S8_V1` / `..._MBF_S8_V1` 决定"烧哪些模型"
  → 用分区时 **必须含名为 `human_face_feat` 的分区**
- 识别组件的 `Kconfig` 中 **`model location` 的 default 是 `HUMAN_FACE_FEAT_MODEL_IN_FLASH_RODATA`**（即默认编进 app），默认烧录模型为 MFN。[models/human_face_recognition/Kconfig](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/Kconfig)（2026-09-15 读）【官方】
- 运行时代码路径可佐证：`human_face_recognition.cpp` 中 `#if CONFIG_HUMAN_FACE_FEAT_MODEL_IN_FLASH_RODATA` → 符号 `_binary_human_face_feat_espdl_start`；`#elif ..._IN_FLASH_PARTITION` → `static const char *path = "human_face_feat";`（即分区名）；否则走 SD 卡路径。[human_face_recognition.cpp](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/human_face_recognition.cpp)（2026-09-15 读）【官方】

### 3.3 esp-who 官方示例的分区表（可直接量化的对照）

| 文件 | 内容 | 来源 |
|---|---|---|
| `examples/human_face_recognition/partitions.csv`（默认，模型编进 app） | `factory, app, factory, 0x010000, **7000K**`；`storage, data, fat, , **1M**` | [partitions.csv](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/partitions.csv)【官方】 |
| `examples/human_face_recognition/partitions2.csv`（模型放独立分区） | `factory, app, factory, 0x010000, **1900K**`；`**human_face_det, data, spiffs, , 200K**`；`**human_face_feat, data, spiffs, , 5000K**`；`storage, data, fat, , 1M` | [partitions2.csv](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/partitions2.csv)【官方】 |
| 切换方式（官方说明） | `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 设为 `partitions2.csv` 即可（当模型位置选 FLASH partition 时） | [esp-dl examples/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md)【官方】 |

→ 官方自己的"模型放分区"方案里，**app 只需 1900K**，检测模型占 200K 分区、识别模型占 5000K 分区。

### 3.4 人脸库（1:N 数据库）的体积与存放

- 官方原文：**`Each feature consumes 2050 bytes, including 2 bytes for id and 2048 bytes for feature data.`**
  → **每人 2050 字节**（512 维 float32 = 2048 B + 2 B id）。[esp-dl examples/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md)（2026-09-15 读）【官方】
  - 注：把"每人特征"记为 **512 字节** 与官方数字**不符**；512 是**维度数**，字节数是 **2048**。
- 数据库是一个**文件**，支持三种存放：`CONFIG_DB_FATFS_FLASH`（FATFS on flash）、`CONFIG_DB_SPIFFS`、`CONFIG_DB_FATFS_SDCARD`；flash/spiffs 方案使用分区表里名为 **`storage` 的 1MB 分区**。[同上 README](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md)【官方】
- 代码级证据（esp-dl 源码）：特征以 `float` 存入文件，读取/写入均按 `feat_len * sizeof(float)`；内存中每人的特征缓冲用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 分配（**即人脸特征常驻 PSRAM**）。[dl_recognition_database.cpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/recognition/dl_recognition_database.cpp)（2026-09-15 读）【官方】
- 结构体：`database_meta {uint16_t num_feats_total; uint16_t num_feats_valid; uint16_t feat_len;}`、`database_feat {uint16_t id; float *feat;}`、`result_t {uint16_t id; float similarity;}`。[dl_recognition_define.hpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/recognition/dl_recognition_define.hpp)【官方】
- **1MB storage 分区可容纳人数 ≈ 1,048,576 / 2050 ≈ 511 人**（**本报告推算**，基于官方"2050 B/人"与官方"1MB storage 分区"两个数字；官方未直接给出人数上限）。`num_feats_total` 为 `uint16_t`，理论上限 65535，实际瓶颈是分区大小。

### 3.5 人脸库的存放位置（esp-who 侧）

- esp-who master 示例 `app_main.cpp` 按配置挂载不同文件系统：`CONFIG_DB_FATFS_FLASH` → `spiflash_fatfs_mount()`；`CONFIG_DB_SPIFFS` → `bsp_spiffs_mount()`；`CONFIG_DB_FATFS_SDCARD` → `bsp_sdcard_mount()`。[app_main.cpp](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/main/app_main.cpp)【官方】
- Espressif Developer Portal 工作坊（2026-07-07）原文：数据库是"a file stored in the onboard flash filesystem (FATFS or SPIFFS, configured via menuconfig)"，示例中文件路径为 **`/spiflash/face.db`**，首次启动会打印 `E dl::recognition::DataBase: Failed to open db`（属预期）。[Assignment 2](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-2/)、[Assignment 3](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-3/)（2026-07-07）【官方】

---

## 4. 运行时开销

### 4.1 官方 benchmark（ESP32-S3，esp-dl master，单位 ms）

来源：[models/human_face_detect/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_detect/README.md)、[models/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/README.md)（2026-09-15 读）【官方】

**人脸检测**

| 模型 | 输入 (h*w*c) | preprocess | model | postprocess |
|---|---|---|---|---|
| `msr_s8_v1_s3` | 120*160*3 | 3.8 | **33.1** | 0.3 |
| `mnp_s8_v1_s3` | 48*48*3 | 0.6 | **5.8** | 0.1 |
| `espdet_pico_224_224_face_s8_s3` | 224*224*3 | 7.2 | **131.6** | 0.8 |
| `espdet_pico_416_416_face_s8_s3` | 416*416*3 | 21.8 | **437.0** | 1.3 |

- 精度：msr+mnp mAP50-95 = **0.367**；espdet_pico_224 = 0.504；espdet_pico_416 = 0.598

**人脸识别（特征提取）**

| 模型 | 输入 | preprocess | model | postprocess |
|---|---|---|---|---|
| `mfn_s8_v1_s3` | 112*112*3 | 5.6 | **248.8** | 0.1 |
| `mbf_s8_v1_s3` | 112*112*3 | 5.6 | **1072.4** | 0.1 |

- 模型规模 / 精度：`mfn_s8_v1` = 1.2 M params、0.46 GFLOPs、**TAR@FAR=1E-4 on IJB-C = 90.03%**；`mbf_s8_v1` = 3.4 M params、0.90 GFLOPs、**93.94%**

**Espressif Developer Portal 工作坊（2026-07-07）给的合并口径**【官方】：

- 检测："MSR takes ~33 ms for the whole frame on ESP32-S3, and MNP adds only ~6 ms per surviving candidate. In practice, when one face is in frame, the total detection time is around **37–40 ms per frame**."
- 识别："Recognition takes **~255 ms per frame on ESP32-S3**"；表格列 `MFN_S8_V1 ~255 ms`、`MBF_S8_V1 ~1073 ms`。
- [Assignment 2](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-2/)、[Assignment 3](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-3/)（2026-07-07）

**已有帧率数据（官方）**：**未找到**"端到端 fps"的官方数字。按上表**推算**：单脸检测 37–40 ms → 上限约 25–27 fps（纯检测、不含拍照/显示）；检测+识别 ≈ 33+6+249+5.6 ≈ **294 ms/帧**，即 **≈3.4 fps**（推算值，未计摄像头取帧与预处理以外的开销）。

> 口径警告：官方 benchmark 表**未标注 CPU 频率、是否启用 PSRAM、是否双核调度**。esp-dl README 提到 Dual Core Scheduling（Conv2D / DepthwiseConv2D 支持双核），未说明上表是否启用。[esp-dl README](https://raw.githubusercontent.com/espressif/esp-dl/master/README.md)（2026-09-15 读）【官方】

**Registry 组件 README 上的同表（可交叉验证，同时给出 P4 对照）**：
- 检测（`human_face_detect` v0.5.0 README）：S3 = msr 3.8/33.1/0.3、mnp 0.6/5.8/0.1、pico224 7.2/131.6/0.8、pico416 21.8/437.0/1.3 ms；**P4 = msr 1.3/13.1/0.1、mnp 0.3/2.4/0.0、pico224 2.4/49.6/0.4、pico416 6.6/185.9/0.6 ms**。[human_face_detect v0.5.0 README](https://components.espressif.com/components/espressif/human_face_detect/versions/0.5.0/readme)（v0.5.0 / 2026-06-08）【官方】
- 检测（v0.3.0 README 用 μs 计）：msr_s8_v1_s3 = 4023 + 32403 + 222 μs；mnp_s8_v1_s3 = 1110 + 5551 + 63 μs。[human_face_detect v0.3.0 README](https://components.espressif.com/components/espressif/human_face_detect/versions/0.3.0/readme)（v0.3.0 / 2025-10-21）【官方】
- 识别（`human_face_recognition` v0.3.0 README）：S3 mfn 5.6/248.8/0.1、mbf 5.6/1072.4/0.1 ms；**P4 mfn 2.9/93.0/0.1、mbf 2.9/188.2/0.1 ms**。[human_face_recognition v0.3.0 README](https://components.espressif.com/components/espressif/human_face_recognition/versions/0.3.0/readme)（v0.3.0）【官方】

**官方对三个检测模型的定性描述**："All three models use 8-bit quantization and run entirely on the ESP32-S3's CPU using the ESP-DL inference engine."（即纯 CPU 推理，官方未提 NPU/加速器）。[Assignment 2](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-2/)（2026-07-07）【官方】

**旧版 esp-dl（v1.x，勿与 3.x 混用）的历史数字**：人脸检测 ESP32-S3 `TWO_STAGE=1` → 56,303 μs；`TWO_STAGE=0` → 16,614 μs；人脸识别（旧 MobileFace）S3 8-bit **287 ms** / 16-bit 554 ms。[ESP-DL 用户指南 release-v1.1 性能页](https://docs.espressif.com/projects/esp-dl/zh_CN/release-v1.1/esp32/performance.html)（release-v1.1，页面版权 2018–2023）【官方，旧版本】

### 4.2 社区实测

| 来源 | 平台 | 实测数字 | 日期 / 可信度 |
|---|---|---|---|
| [esp-dl#226](https://github.com/espressif/esp-dl/issues/226) | ESP32-S3 | `MFN: 4200 ms`、`MBF: 730 ms`（作者同时质疑文档把两个模型标反） | 创建 2025-04-03，closed 2025-04-08【社区，直接报障】 |
| [CogletESP 项目调研文档](https://gitea.airlabs.art/Rdzleo/CogletESP-camera-version/raw/commit/f1c2bfce930db856af5f75213aebc018c651522d/docs/phase-01-face-tracking/RESEARCH.md) | ESP32-S3-**N16R8** + xiaozhi-esp32 + esp-dl 3.2.0 + human_face_detect 0.4.1 + OV3660 | 两阶段"总推理耗时 ~38 ms，FPS 上限约 26"；建议限频 10 FPS；"实测上限 15–20 FPS"；任务栈建议 **8 KB**；PSRAM 估算"模型权重 ~200KB + 中间 tensor buffer ~300KB ≈ 500KB PSRAM"；**文档自评置信度 MEDIUM/LOW、需实机验证** | 2026-04-17【社区工程调研文档，非独立基准】 |
| [esp-dl#302](https://github.com/espressif/esp-dl/issues/302) 日志 | ESP32-S3 N16R8，IDF 5.5.2 | 不初始化 esp-sr 时人脸检测正常（框数=1，37 ms 级）；同日志显示 `Free PSRAM: 8355336` | 2026-05-16 ~ 2026-05-25 closed【社区，单点日志】 |
| [esp-dl#186](https://github.com/espressif/esp-dl/issues/186) 日志 | 平台未标注 | `I MemoryManagerGreedy: Maximum mermory size: 840448`（≈820 KiB，**一行日志，缺平台/版本上下文**） | 2024-12-18【社区，弱证据】 |

- **公开社区（Hackster / Medium / ESP32 Forum / Reddit）中带发布日期、可核实的 ESP32-S3 + esp-who 人脸 FPS/内存实测帖：未找到证据。**
- 中文社区相关文章（如 CSDN）多为旧版 esp-who（TFLite Micro 时代）或信息不可核实，**不建议引用**；其中一篇 2026-07-17 的文章称 esp-who 实时性判据为"稳定维持 5–10 FPS"，属二手转述且同文含"ESP32 无法运行 esp-who"等旧版/错误论断。[链接](https://blog.csdn.net/weixin_42153793/article/details/158586465)（2026-07-17）【社区，低可信】
- 一篇含远边缘/边缘分层人脸实现的论文（TriCloudEdge）含实测章节，但未能核出 ESP32-S3 人脸检测的具体 ms/fps 数值。[arXiv:2602.02121](https://arxiv.org/abs/2602.02121)（v1 2026-02-02，v2 2026-02-13）→ 具体数值**未找到证据**

### 4.3 PSRAM 占用 / 任务栈 / 中间张量缓冲

- **官方未给出** human_face_detect / human_face_recognition 的 **PSRAM 占用**或**推荐任务栈大小**的具体数字。esp-dl 文档只说明可用 `model->profile_memory()` 打印 Internal RAM / PSRAM / FLASH 分类占用，并给出分类名（`fbs_model` / `parameter` / `parameter_copy` / `variable` / `others` / `total`）。**未找到证据**（具体 MB 数）。
  [how_to_load_test_profile_model](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_load_test_profile_model.html)【官方】
- 官方给出的**内存策略**事实（同文档）：
  - `param_copy=true`（默认）：模型参数从 FLASH 复制到 PSRAM/内部 RAM 以提速；`param_copy=false` 省 RAM 但更慢，原文建议"Only disable parameter copying if RAM is extremely tight"。
  - 构造参数 `max_internal_size`：限制内部 RAM 使用上限（置 0 表示优先用 PSRAM）。
  - 内存管理器：`dl::MEMORY_MANAGER_GREEDY`（"Static Memory Planner"按用户指定内部 RAM 大小自动为各层选内存位置）。
- 代码级证据：人脸特征缓冲显式用 `MALLOC_CAP_SPIRAM` 分配（见 3.4）。[dl_recognition_database.cpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/recognition/dl_recognition_database.cpp)【官方】
- 一条可引用的**社区** Tensor 缓冲规模数字：esp-dl issue #186 的用户日志中，人脸识别模型加载打印 `I MemoryManagerGreedy: Maximum mermory size: 840448`（约 820 KiB，**一行日志，未标注平台/版本细节**，仅供参考）。[esp-dl#186 comment](https://github.com/espressif/esp-dl/issues/186)（2024-12-18）【社区】
- **API 签名与官方参数说明**（`dl::Model` 构造函数）：`Model(..., int max_internal_size = 0, memory_manager_t mm_type = MEMORY_MANAGER_GREEDY, const uint8_t* key = nullptr, bool param_copy = true, ...)`
  - `max_internal_size` 原文：`In bytes. Limit the max internal size usage. Only take effect when there's a PSRAM, and you want to alloc memory on internal RAM first.`
  - `param_copy=false` 原文：`avoid copy model parameters from FLASH to PSRAM. Only set this param to false when your PSRAM resource is very tight. This saves PSRAM and sacrifices the performance of model inference`
  [ESP-DL Model API Reference](https://docs.espressif.com/projects/esp-dl/en/latest/api_reference/model_api.html)（latest = v3.3.11 文档集，2026-09-15 读）【官方】
- **官方 Static Memory Planner 原文**：`designed for the Internal RAM/PSRAM memory structure … we provide an API that allows users to customize the size of the internal RAM that the model can use. The memory planner will automatically allocate different layers to the optimal memory location based on the size of the internal RAM specified by the user`。官方双核调度实测：conv2d(224×224×3, 3×3, out 112×112×16) 单核 **12.1 ms** / 双核 **6.2 ms**。[esp-dl/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/README.md)（2026-09-15 读）【官方】
- **esp-dl 根 Kconfig 现状**：只有 `CONFIG_PIX_CVT_*` 系列像素转换开关，**没有** `CONFIG_SPIRAM` / `CONFIG_DL_*` 之类内存配置项 —— 内存策略全部通过 API 参数（`max_internal_size` / `param_copy` / `memory_manager_t`）控制。[esp-dl/Kconfig](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/Kconfig)（2026-09-15 读）【官方】
- **任务栈：esp-dl / esp-who 官方"推荐 task stack size"→ 未找到证据**（README、Getting Started、组件 README 均无此数字）。可引用的相邻事实：
  - **IDF 官方规则**：`external RAM will not be used as task stack memory. xTaskCreate() and similar functions will always allocate internal memory for stack and task TCBs.`；要把任务栈放外部 RAM 必须开 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` + `xTaskCreateStatic()`。→ 即**人脸识别任务栈只能占内部 RAM**。[ESP-IDF v5.5.3 Support for External RAM](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/external-ram.html)（v5.5.3）【官方】
  - esp32-camera 的 `CONFIG_CAMERA_TASK_STACK_SIZE` 默认 **4096**。[esp32-camera Kconfig](https://raw.githubusercontent.com/espressif/esp32-camera/master/Kconfig)（2026-09-15 读）【官方】
  - 社区工程文档建议人脸任务栈 **8 KB**（未由官方背书）。[CogletESP RESEARCH.md](https://gitea.airlabs.art/Rdzleo/CogletESP-camera-version/raw/commit/f1c2bfce930db856af5f75213aebc018c651522d/docs/phase-01-face-tracking/RESEARCH.md)（2026-04-17）【社区】

### 4.4 可支持的人脸库规模上限

- 官方**未给出**"可支持 N 人"的声明；只给出 **2050 B/人** 与 **1MB `storage` 分区**。**未找到**官方或可核实实测的人脸库规模上限记录。
- 本报告按官方两个数字**推算**：1 MB / 2050 B ≈ **511 人**（不换分区的前提下）。若把库搬到 SD 卡（`CONFIG_DB_FATFS_SDCARD`）则不受该 1MB 限制（官方支持该模式，但给出"从 SD 卡加载较慢"的性能提示，针对的是**模型**加载，不是人脸库查询）。
- 查询算法事实：`query_feat()` 为**线性全库遍历**（`for (auto it = m_feats.begin() ...)` + 逐元素点积 `cal_similarity`），每次识别对全库每人做一次 512 维点积；`top_k` 默认 1，阈值 `m_thr` 默认 **0.5**。
  来源：[dl_recognition_database.cpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/recognition/dl_recognition_database.cpp)、[human_face_recognition.cpp](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/human_face_recognition.cpp)（`m_thr(0.5), m_top_k(1)`）【官方】
  → 库越大，比对耗时线性增长（512 次 float 乘加/人）；按 2050 B/人 与 1MB 分区推算 511 人时，比对部分约 511×512 ≈ 26 万次乘加（**推算**）。

---

## 5. 替代方案对比（只列事实）

### 5.1 ① 直接用 esp-dl + 模型（不经 esp-who 封装）

- **esp-dl 最新版 3.3.11，2026-09-03 上传**，MIT，targets 含 `esp32s3`；GitHub Release 最新 tag 为 **v3.2.0（2025-10-23）**，3.3.x 无对应 GitHub Release。[Registry API](https://components.espressif.com/api/components/espressif/esp-dl)、[releases](https://api.github.com/repos/espressif/esp-dl/releases)（2026-09-15 读）【官方】
- 官方 News 原文：`[2026/9/1] Starting from ESP-DL v3.3.11, w8a16 mixed quantization is fully supported.` [esp-dl README](https://raw.githubusercontent.com/espressif/esp-dl/master/README.md)【官方】
- **IDF 门槛**：`Please use ESP-IDF release/v5.3 or above.`；`esp-dl/idf_component.yml` 逐 target 给出 `esp32s3 → idf >=5.3`。[README](https://raw.githubusercontent.com/espressif/esp-dl/master/README.md)、[idf_component.yml](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/idf_component.yml)【官方】
- **依赖**：`espressif/dl_fft >=0.6.0`、`espressif/esp_new_jpeg ^1`（3.3.11）[Registry API](https://components.espressif.com/api/components/espressif/esp-dl)【官方】
- **两个独立模型组件**（`human_face_detect` 0.5.0 / `human_face_recognition` 0.3.2）位于 **esp-dl 仓库 `models/`**，以独立 IDF 组件发布；依赖链 `human_face_recognition → human_face_detect → esp-dl`；这两个组件的 `idf_component.yml` **未声明 IDF 版本**，门槛经 esp-dl 继承。[Registry API](https://components.espressif.com/api/components/espressif/human_face_detect)、[master idf_component.yml](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_detect/idf_component.yml)【官方】
- **模型格式与量化工具链**：`.espdl` 为 ESP-DL 标准模型格式，"类似 ONNX 但用 FlatBuffers 取代 Protobuf"，支持零拷贝反序列化；量化工具为 **ESP-PPQ**（基于 PPQ，支持 ONNX/PyTorch，TensorFlow/Paddle 需先转 ONNX），接口 `espdl_quantize_onnx` / `espdl_quantize_torch`；PyPI `esp-ppq` 最新 **1.3.11（2026-09-02）**，`requires_python <3.13,>=3.8`；仓库 `espressif/esp-ppq`（fork 自 OpenPPL/ppq）最后 push **2026-09-02**，`has_issues: false`（issues 归 esp-dl）。[esp-dl README](https://raw.githubusercontent.com/espressif/esp-dl/master/README.md)、[esp-dl getting_started 文档](https://docs.espressif.com/projects/esp-dl/en/latest/getting_started/readme.html)、[PyPI esp-ppq](https://pypi.org/pypi/esp-ppq/json)、[api.github.com/repos/espressif/esp-ppq](https://api.github.com/repos/espressif/esp-ppq)【官方】
- **是否在 ESP32-S3 本地完成**：**是**。官方 API 为设备端推理（`HumanFaceDetect::run(img)`、`HumanFaceFeat`、`HumanFaceRecognizer::enroll/recognize`），示例输出 `id: 3, sim: 0.750027`；人脸库可存 flash 分区或 SD 卡。[models/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/README.md)、[examples/human_face_recognition/README.md](https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md)【官方】

### 5.2 ② esp-video / esp-video-components

- 仓库 `espressif/esp-video-components`：**未归档**，created 2024-03-12，最后 push **2026-09-14**，stars 88 / forks 39 / open issues 41，**tags 为空、releases 为空（无任何 tag/release）**。[api.github.com/repos/espressif/esp-video-components](https://api.github.com/repos/espressif/esp-video-components)（2026-09-15 读）【官方】
- 组件清单只有 4 个：`esp_cam_sensor`、`esp_sccb_intf`、`esp_video`、`esp_ipa`。[README](https://raw.githubusercontent.com/espressif/esp-video-components/master/README.md)【官方】
- **不含人脸检测/识别能力**：组件与示例清单里均无 AI 推理 / 人脸组件（示例为 capture_stream、image_storage、m2m、simple_video_server、uvc、v4l2_cmd、video_custom_format）。**未找到 esp-video 含人脸能力的证据**。[esp_video/examples/README.md](https://raw.githubusercontent.com/espressif/esp-video-components/master/esp_video/examples/README.md)【官方】
- **定位**（官方 2025-09-22 博客原文）：`esp-video is an enhanced version of esp32-camera`；同文把端侧 AI 归于 `esp-dl and esp-who`。**未找到"esp-video 继任 esp-who"的官方表述**。[Espressif Developer Portal](https://developer.espressif.com/blog/2025/09/esp-video-introduction/)（2025-09-22）【官方】
- **芯片能力（官方表）**：ESP32-S3 → DVP ✔、SPI ✔、USB ✔，**MIPI-CSI / ISP / 硬件 JPEG / H.264 均 N/A**；ESP32-P4 → 全支持。[esp_video README](https://raw.githubusercontent.com/espressif/esp-video-components/master/esp_video/README.md)【官方】
- **IDF 要求**：`esp_video` 组件 `idf >=5.4`（master `version: 2.4.1`），targets 含 esp32s3；依赖 `esp_cam_sensor 2.5.*`、`esp_ipa 2.3.*`、`usb_host_uvc 2.5.*`。[esp_video/idf_component.yml](https://raw.githubusercontent.com/espressif/esp-video-components/master/esp_video/idf_component.yml)【官方】
- **文档范围**：只有 ESP32-P4 文档（`/en/latest/esp32p4/`）；ESP32-S3 文档路径 **404**。[P4 文档](https://docs.espressif.com/projects/esp-video-components/en/latest/esp32p4/index.html)、[S3 404](https://docs.espressif.com/projects/esp-video-components/en/latest/esp32s3/index.html)（2026-09-15 读）【官方】
- 文档 Changelog 版本：0.1.0 = 2026-04-16；0.2.0 = 2026-07-10；0.3.0 = **2026-08-31**。[Changelog](https://docs.espressif.com/projects/esp-video-components/en/latest/esp32p4/Changelog.html)【官方】
- **与 GC0308 的关系（事实）**：`esp_cam_sensor` 官方支持表中 **GC0308 在列**：max 640×480、接口 **DVP**、输出格式 `Grayscale / YCbCr422 / **RGB565**`、镜头 1/6.5"；ESP32-S3 的 DVP 接口为 Y。[esp_cam_sensor README](https://raw.githubusercontent.com/espressif/esp-video-components/master/esp_cam_sensor/README.md)（2026-09-15 读）【官方】
- esp-who master 的人脸识别示例已改用 **esp_video + esp_cam_sensor**（而非 esp32-camera）：示例 `direct_dependencies` 含 `espressif/esp_video`，**不含 esp32-camera**；`sdkconfig.bsp.esp32_s3_eye` 里的摄像头项是 `CONFIG_CAMERA_OV2640=y` / `CONFIG_CAMERA_OV2640_DVP_RGB565_BE_240X240_25FPS=y`。[dependencies.lock.esp32_s3_eye](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/dependencies.lock.esp32_s3_eye)、[sdkconfig.bsp.esp32_s3_eye](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye)【官方】

### 5.3 ③ 设备只抓拍、上传服务器比对（Seeed face_rec_api）

- 页面：中文 <https://wiki.seeedstudio.com/cn/face_regonition_with_mcp/>、英文 <https://wiki.seeedstudio.com/face_regonition_with_mcp/>，两页 **Last updated on Apr 7, 2026 by Spencer**。【官方】
- **比对在服务器端，不在设备本地**。英文原文：`By flashing a customized firmware onto the SenseCAP Watcher, the Xiaozhi AI gains a face recognition ability powered by a reComputer with Hailo-8 acceleration... the AI will automatically capture a photo, identify the face against a local database on the reComputer, and respond with the person's name and confidence level.`【官方】
- **设备端固件**：目标 SoC 为 **esp32s3**，但板子是 **SenseCAP Watcher**（不是 XIAO ESP32S3）；wiki 步骤为 `idf.py set-target esp32s3` + `idf.py menuconfig # Select SenseCAP Watcher board`，工具链为 **ESP-IDF 5.5 CMD (5.5.1)**；固件仓库为 `suharvest/xiaozhi-esp32` 的 **`face_rec_api` 分支**（`git clone -b face_rec_api --single-branch`）。【官方】
- 设备端唯一改动点：`main/boards/sensecap-watcher/sscma_camera.cc` 里的 `std::string face_rec_url = "http://192.168.10.131:8001/recognize";` 改成 reComputer 的 IP。【官方】
- **服务器端硬件**：reComputer AI R2130-12（Raspberry Pi + **Hailo-8**），需 `sudo apt install hailo-all`、`hailortcli scan`，服务监听 8001。FAQ 原文：`Yes, as long as the device runs a Linux-based system with Hailo-8 hardware.`【官方】
- **服务器端库不是 face_recognition / dlib**：`suharvest/face_rec_api` README 原文 `Built on InsightFace (SCRFD detection + ArcFace recognition), with per-backend acceleration via HailoRT, TensorRT, and RKNN-Toolkit-Lite2`，性能表标注 `MobileFaceNet embedder and 640×640 input`；**未找到 face_recognition / dlib 的任何引用**。三后端：Hailo-8（RPi5，INT8，`scrfd_10g.hef` + `arcface_mobilefacenet.hef`，**30 ms p50**）/ Jetson Orin TensorRT（JetPack 6.2、TensorRT 10.3、CUDA 12.5）/ RKNN（RK3576/3588，`librknnrt.so >= 2.3.0`）；Software 段列 `Python 3.11+`、`HailoRT 4.21.0`；REST API `/recognize`、`/enroll`、`/reload`、`/infer`；**每用户 ~200 KB（512 维 fp32）**。[face_rec_api README](https://raw.githubusercontent.com/suharvest/face_rec_api/master/README.md)（2026-09-15 读）【官方】
- 仓库状态：`suharvest/face_rec_api` created 2025-11-18，最后 push **2026-09-08**，**stars 1**、forks 2、open issues 0，MIT，Python，未归档。[api.github.com/repos/suharvest/face_rec_api](https://api.github.com/repos/suharvest/face_rec_api)（2026-09-15 读）【官方】
- 上游 `78/xiaozhi-esp32`：stars 29933 / forks 6960 / open issues 664，最后 push 2026-09-14，MIT；README 的"已实现功能"清单**不含人脸识别**（列了离线语音唤醒 ESP-SR、声纹识别 3D-Speaker 等），支持 ESP32-C3/S3/P4。[api.github.com/repos/78/xiaozhi-esp32](https://api.github.com/repos/78/xiaozhi-esp32)【官方】
- fork `suharvest/xiaozhi-esp32` 相关分支：`face_rec_api`、`feature/face-recognition`、`feat/face-identify-on-demand`、`feature/door-access-xiao-grove`、`feat/watcher-face-mode-speaker` 等。[branches API](https://api.github.com/repos/suharvest/xiaozhi-esp32/branches)（2026-09-15 读）【官方】
- **是否有"ESP32-S3 上无法本地做 1:N 识别，所以走服务器"的表述**：**未找到证据**（已核查中/英文 wiki、face_rec_api README、xiaozhi-esp32 README 与 wiki FAQ，均无此表述；FAQ 只限定服务器需 Linux + Hailo-8）。
- **但 Seeed 另有一页官方文档给出了"设备端能力上限"的明确限定（重要，且必须区分对象）**：<https://wiki.seeedstudio.com/solutions/mcp-face-auth-integration/>（页面 Last updated 未取到）原文：
  - `Strictly speaking the compute box is optional: the Watcher can match faces on its own NPU, which is enough for a pilot. In practice that mode caps you at 20 people per device and cannot tell a face from a photograph of one, so production deployments almost always add the box.`
  - Path 1（On-Device）表格：Where matching runs = **`Himax WE2 NPU inside SenseCAP Watcher`**；Enrolled people = **`20 per device, a hard limit of the on-device store`**；Face model = `MobileFaceNet, 128-D, INT8 — fixed`；Anti-spoofing / liveness = **`Not available`**
  - `Combined with the 20-person ceiling, this means most production deployments end up on Path 2. Treat Path 1 as the way to prove the workflow, not as the cheap version of the finished system.`
  - Path 2（LAN 推理服务）契约与 face_rec_api 同构：`POST {endpoint}/infer`（`image_b64` → `faces[].embedding` / `det_score` / `live` / `model_tag`）、`GET {endpoint}/health`；默认余弦阈值 **0.45**；`model_tag` 变更需重算全部 embedding。
  - 同页硬件表：Watcher = **`ESP32-S3 with a Himax WiseEye2 vision co-processor`**；compute box 示例 = reComputer RK3576 / RK3588 / R2135-12(Hailo-8)。
  【Seeed 官方 wiki，日期未取到】
  → **口径必须分开**：该页限制的是 **Seeed Watcher 设备端的 Himax WE2 NPU**（人脸库上限 20 人、无活体检测），**不是 ESP32-S3 CPU 跑 esp-dl 的能力上限**。把"WE2 NPU 上限 20 人"当成"ESP32-S3 不能本地 1:N"是错误外推。ESP32-S3 侧的官方能力与规模上限见第 4.4 节（官方仅给 2050 B/人 + 1MB 分区，未给人头上限）。

### 5.3b 关于"目标固件 xiaozhi-esp32 v2.2.4"的一条事实

- 上游 `78/xiaozhi-esp32` **v2.2.4 tag 的 `main/idf_component.yml` 不含任何 esp-dl / human_face_detect / human_face_recognition 依赖**（只有 esp-sr ~2.3.0、esp32-camera ^2.1.4、lvgl、esp_video 等），`main/` 下也无面部识别源文件。[v2.2.4 main/idf_component.yml](https://raw.githubusercontent.com/78/xiaozhi-esp32/v2.2.4/main/idf_component.yml)（v2.2.4）【官方】
  → 事实陈述：**"xiaozhi-esp32 v2.2.4 + esp-dl 3.3.11"不是 v2.2.4 上游自带组合**；esp-dl 3.3.11 出现在本机 `11_PCA9577` 是通过 **esp-sr 2.5.3 的私有依赖（`esp-dl >=3.3.10`）** 被拉入的（见第 0、6.3 节）。v2.2.4 上游自带的是 esp-sr **2.3.1**。
- 上游相关 issue（**未独立复核**，GitHub API 匿名限流 403）：`78/xiaozhi-esp32` **#246**「增加人脸识别，看到熟人主动打招呼。」创建 2025-02-28，**至今 open**，11 条评论，正文为空 → 属功能请求。【社区/上游 issue，未复核】

### 5.4 三者对比（只列已核实事实）

| 维度 | ① esp-dl + 模型 | ② esp-video | ③ 抓拍 + 服务器比对（Seeed face_rec_api） |
|---|---|---|---|
| 在 ESP32-S3 上**本地**完成 1:N 识别 | **是**（官方设备端 API + 示例） | **否**（不含人脸能力） | **否**（比对在 reComputer/Hailo-8） |
| 提供人脸模型 | 是（detect / recognition 独立组件） | 否 | 设备端无；服务器用 InsightFace |
| IDF 要求 | `>=5.3`（S3） | `>=5.4` | 设备端固件用 IDF 5.5.x |
| 相机接口 | 与相机解耦（可配 esp32-camera 或 esp-video） | DVP/SPI/USB（S3） | 设备端 xiaozhi 固件自身 |
| 额外硬件 | 无 | 无 | **必须有 Linux + Hailo-8 的服务器** |
| 仓库热度/维护 | esp-dl 3.3.11（2026-09-03） | 无 release/tag；push 2026-09-14 | stars 1；push 2026-09-08 |

---

## 6. 已知失败 / 坑（有证据的部分）

### 6.1 摄像头 RGB565 与模型输入格式

- **RGB565 输入是受支持的像素类型**，但有**字节序陷阱**。esp-dl issue #186（标题 `face_recognizer模型输入rgb565 数据不能正常工作 (AIV-735)`，created **2024-12-18**，closed **2024-12-18**，4 条评论）：
  - 用户用 `sw_decode_jpeg` 得到 RGB565 后调用 `face_recognizer->recognize((uint16_t *)bill1, {300,300,3})`，结果"提示没有人脸"。
  - esp-dl 维护者（100312dog，CONTRIBUTOR）回复原文：`提示没有人脸的话, 要不就是检测器效果不好检测不到人脸, 要不就是rgb565数据大小端的问题. **s3这边默认用的是小端rgb565(gggrrrrr bbbbbggg), p4用的是大端(bbbbbggg gggrrrrr)**.`
  - 最终结论：`sw_decode_jpeg默认解rgb565得到的是大端数据，需要加个参数` → 需改为 `sw_decode_jpeg(bill1_jpeg, bill1, true);`，用户回复"感谢，解决了"。
  - [esp-dl#186](https://github.com/espressif/esp-dl/issues/186)（2024-12-18）【官方维护者回复 + 社区报障】
  - → 事实推论（本报告）：**ESP32-S3 上给 esp-dl 的 RGB565 必须是小端排列**；从 JPEG/大端来源转换时需显式指定。这条对"GC0308 按 320×240 RGB565 抓帧"的场景是直接相关的格式风险点。
- **灰度图会导致检测失败**（相关事实）：esp-dl issue #254 `human_face_detect is not working on grayscale images (AIV-2080)`，created **2025-07-10**，closed **2025-07-10**；用户用 OV5640 开 `sensor->set_special_effect(sensor, 2)`（灰度）后，`MSR_S8_V1 + MNP_S8_V1` 在任何角度/距离下检测都失败，并追问是否为模型限制。[esp-dl#254](https://github.com/espressif/esp-dl/issues/254)（2025-07-10）【社区】
- **RGB565 是官方一等支持的输入格式（有 LE/BE 两套枚举）**：`dl::image::img_t.pix_type` 支持 `DL_IMAGE_PIX_TYPE_RGB565LE` / `RGB565BE` / `BGR565LE` / `BGR565BE` / `RGB888` / `BGR888` / `GRAY` / `YUYV` / `UYVY`；esp-dl Kconfig 另提供 `CONFIG_PIX_CVT_RGB565_TO_RGB888_SUPPORT`（default y）等整套像素转换开关，即**可以直接把 RGB565 帧喂进 `HumanFaceDetect::run()`，内部完成色彩转换与缩放**。[dl_image_define.hpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/image/dl_image_define.hpp)、[esp-dl/Kconfig](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/Kconfig)（2026-09-15 读）【官方】
- **转换开销已被官方计入 latency 表的 preprocess 列**：RGB565→（缩放到 120×160）→RGB888→归一化 = **3.8 ms**（msr_s8_v1_s3）；48×48 路径 **0.6 ms**；识别 112×112 路径 **5.6 ms**。同表（见 4.1）【官方】
- **实现细节（性能预期的重要事实）**：像素转换的 SIMD 路径条件是 `#if CONFIG_PIE_V2_BOOST`（`static constexpr bool simd`），即**ESP32-S3 上走标量逐像素循环**，SIMD 加速主要面向 ESP32-P4。[dl_image_process.hpp](https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/image/dl_image_process.hpp)（2026-09-15 读）【官方】
- **RGB565 相关的版本回归与 API 变更 issue**：
  - esp-who **#319**：用 `S3Cam(PIXFORMAT_RGB565, FRAMESIZE_240X240, 4, false)` 时，**RGB565 帧在 human_face_detect 0.2.3 下检不出人脸，0.2.1 正常**（版本回归类）；标题「在human face detect 0.2.3的情况下使用ov2640 直接拍摄rgb565的人脸时无法检测到人脸」，状态 **closed**，创建 2025-05。[链接](https://github.com/espressif/esp-who/issues/319)【社区】
  - esp-who **#335**：`error: 'DL_IMAGE_CAP_RGB565_BIG_ENDIAN' was not declared in this scope`（`who_frame_cap_node.cpp:197`，human_face_detect 示例在 **ESP-IDF 5.5.1** 下）；状态 **open**，创建 2025-10-29，最后更新 2026-02-14。→ RGB565 端序能力位存在作用域/API 变更问题。[链接](https://github.com/espressif/esp-who/issues/335)【社区】
  - esp-dl **#163**：RGB565 + `FRAMESIZE_96X96` 手工转通道顺序错误 → `assert failed: reshape … (size == size_gt)`；closed，创建 2024-05-04，关闭 2024-10-14。[链接](https://github.com/espressif/esp-dl/issues/163)【社区】
  - esp-dl **#236**：`hw_decode_jpeg()` 的 `rgb_order` 默认走 BGR（`swap_color_bytes` 默认 false）→ 说明 esp-dl 默认颜色序与直觉相反；closed，创建 2025-05-04，关闭 2025-05-12。[链接](https://github.com/espressif/esp-dl/issues/236)【社区】
- **GC0308 的驱动级限制（官方源码事实，与"QVGA RGB565 抓帧"直接相关）**：
  - `set_pixformat()` **只处理** `PIXFORMAT_RGB565`、`PIXFORMAT_YUV422`、`PIXFORMAT_GRAYSCALE`，其余打印 `Unsupported format` 并返回 -1；
  - `set_framesize()` 上限 **`FRAMESIZE_VGA`**（超出则强制降为 VGA）；
  - `set_brightness` / `set_denoise` / `set_gainceiling` / `set_quality` / `set_aec2` / `set_awb_gain` / `set_dcw` / `set_bpc` / `set_wpc` / `set_raw_gma` / `set_lenc` **全部指向 `set_dummy()`（返回 -1 + 打印 "Unsupported"）**；
  - GC0308 **无自动对焦**（`af_*` 全 NULL）。
  [esp32-camera sensors/gc0308.c](https://raw.githubusercontent.com/espressif/esp32-camera/master/sensors/gc0308.c)（2026-09-15 读）【官方】
  - GC0308 在 S3 板子上确有可用实测日志：esp-dl #218 日志含 `camera: Detected GC0308 camera` 与 `cam_hal: Allocating 153600 Byte frame buffer in PSRAM`（QVGA RGB565 双缓冲 = 2 × 153,600 B）。[esp-dl#218](https://github.com/espressif/esp-dl/issues/218)（closed，创建 2025-03-17，关闭 2025-03-24）【社区】
- **可降低 CPU 转换开销的硬件开关（官方）**：esp32-camera 提供 `CONFIG_CAMERA_CONVERTER_ENABLED`（"Enable camera RGB/YUV converter"，**depends on `IDF_TARGET_ESP32S3`，default n**），配套 `CONFIG_CAMERA_CONV_PROTOCOL`（BT601/BT709，默认 BT601）与 `CONFIG_LCD_CAM_CONV_FULL_RANGE_ENABLED`（default y）。[esp32-camera Kconfig](https://raw.githubusercontent.com/espressif/esp32-camera/master/Kconfig)（2026-09-15 读）【官方】

### 6.2 Octal PSRAM（ESP32-S3R8 / N16R8）与人脸模型共存

- **官方路径本身就是 Octal PSRAM 配置**。esp-who master 的 ESP32-S3-EYE 配置文件（IDF 5.5.5 工程）原文包含：
  `CONFIG_SPIRAM=y`、**`CONFIG_SPIRAM_MODE_OCT=y`**、**`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`**、`CONFIG_SPIRAM_SPEED_80M=y`、`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`、`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`、`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`、`CONFIG_ESP32S3_DATA_CACHE_64KB=y`、`CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`；摄像头项为 `CONFIG_CAMERA_OV2640_DVP_RGB565_BE_240X240_25FPS=y`（注意：**BE = 大端**）。
  [sdkconfig.bsp.esp32_s3_eye](https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye)（2026-09-15 读）【官方】
  → 即：**官方支持组合里就是 Octal PSRAM @80MHz + XIP from PSRAM + 240MHz + 人脸检测/识别模型共存**，且板子只需 8MB flash。
  → 但**未找到**"Octal PSRAM 与人脸识别模型冲突/不兼容"的 issue 或官方告警（本轮检索范围内）。**未找到证据**。
- **官方 IDF 关于 PSRAM 带宽/缓存争抢的原文（最硬的机理依据，非人脸专用）**：
  - `The bandwidth that DMA accesses external RAM is very limited, especially when the core is trying to access the external RAM at the same time.`
  - `External RAM uses the same cache region as the external flash … when accessing large chunks of data (> 32 KB), the cache can be insufficient, and speeds will fall back to the access speed of the external RAM. Moreover, accessing large chunks of data can 'push out' cached flash, possibly making the execution of code slower afterwards.`
  [ESP-IDF v5.5.3 Support for External RAM](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/external-ram.html)（v5.5.3）【官方】
- **官方频率/模式约束（N16R8 = Quad flash + Octal PSRAM 的组合）**：PSRAM 80 MHz DDR 只能配 flash 80 MHz DDR（同组 B）；`Quad PSRAM only supports STR mode, while Octal PSRAM only supports DTR mode`；**120 MHz DDR 属实验特性**，需开 `CONFIG_IDF_EXPERIMENTAL_FEATURES`，且 `after the temperature increases or decreases over 20 celsius degree, the accesses to/from PSRAM/flash will crash randomly`。
  官方对应缓解手段：`CONFIG_SPIRAM_XIP_FROM_PSRAM` —— PSRAM 为 Octal 模式且配置 80 MHz 时比 Quad flash 更快，且 SPI1 操作期间缓存不会关闭，可避免代码执行变慢（esp-who 官方 S3-EYE 配置正是开了这一项）。[ESP-IDF v5.5.3 SPI Flash and External SPI RAM Configuration](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/flash_psram_config.html)（v5.5.3）【官方】
- 同机理的实证 issue（PSRAM 与 Wi-Fi/DMA 总线争抢，**非人脸模型**）：
  - [esp-idf#12342](https://github.com/espressif/esp-idf/issues/12342)「LCD display corruption caused by Wi-Fi activities (IDFGH-11174)」**open**，创建 2023-10-03，更新 2024-02-14；原文 `LCD display buffer is placed in the PSRAM (Octal mode@80MHz). If we temporarily change it to internal RAM, it seems the problem can be avoided.`
  - [esp-idf#18764](https://github.com/espressif/esp-idf/issues/18764)「DMA TX underflow with psram_dma_direct ... on ESP32-S3 QSPI LCD when WiFi is active」**open**，创建 2026-06-26，更新 2026-07-23；硬件 "8 MB Octal PSRAM, 16 MB flash"，作者结论 `the bottleneck is PSRAM/GDMA bus arbitration, not raw bandwidth margin`。【社区】
- 历史相关记录（**旧版 esp-dl，非 3.x**）：esp-dl issue #116 `Board: ESP32-S3-WROOM Cam ... octal_psram: Found 8MB PSRAM device / Speed: 80MHz` 的日志里，人脸识别示例卡在 `cpu_start.c:142`（`APP CPU up` 不再输出），**但这是 2023-03-14 创建、基于 IDF 5.0.1 / 旧 esp-dl 的报告**，2024-10-21 关闭。[esp-dl#116](https://github.com/espressif/esp-dl/issues/116)（2023-03-14 创建 / 2024-10-21 closed）【社区】
- **反向证据**：esp-dl #302 的日志显示 Octal PSRAM 80 MHz 下 `Free PSRAM: 8355336` 正常、人脸检测 37 ms 级正常 —— 即**能跑**，问题出在与 esp-sr 叠加之后（见 6.3）。[esp-dl#302](https://github.com/espressif/esp-dl/issues/302)（2026-05）【社区】

### 6.3 esp-sr 唤醒词模型 + 人脸模型同存的冲突报告

- **本机已核实的事实（前提）**：`esp-sr 2.5.3` 在 lock 中声明依赖 `espressif/esp-dl >=3.3.10`（private），同时依赖 `espressif/dl_fft >=0.6.0`、`espressif/esp-dsp 1.8.0`、`espressif/cjson ^1.7.19`、`idf >=5.0`；本机 lock 中 esp-dl 解析为 **3.3.11**。
  → **esp-sr 与 esp-dl（进而与 human_face_detect/recognition）共用同一份 esp-dl**，启用 esp-sr 就会把 esp-dl 拉进构建；不存在"两套 esp-dl"的问题。来源：本地 `11_PCA9577\dependencies.lock` 第 60–85 行（2026-09-15 读）【官方构件清单】
- **最直接的冲突报告 —— esp-dl #302**「人脸检测模型和ESP SR一起使用时，检测出的人脸框异常。」（**closed，创建 2026-05-16，关闭 2026-05-25**，label `bug 🐞`）：
  - 环境：**ESP32-S3 N16R8**、ESP-IDF 5.5.2、esp-dl 3.3.3 / **esp-sr 2.4.3** / esp32-camera 2.1.6 / human_face_detect 0.4.2 / human_face_recognition 0.3.1
  - 现象：先建 `HumanFaceDetect` / `HumanFaceRecognizer` 并 run（正常 1 个框），再调 `init_command()`（命令词初始化）后 → `enroll run 正常人脸框应该等于1，实际上是：**7**`；注释掉 `init_command()` 即恢复正常
  - 同日志的内存实况：初始化唤醒词前 `总可用内存: 5984 KB / 内部RAM: **327 KB** / PSRAM: 5656 KB`；加载 wakenet 后 `Free PSRAM: 5453400`；**MultiNet 加载后 `Free PSRAM: 2420508`（一次命令词识别约占 3 MB PSRAM）**
  [esp-dl#302](https://github.com/espressif/esp-dl/issues/302)（2026-05）【社区报障】
- **生态侧修复线索（转述，非 release note 原文 → 部分未核实）**：esp-sr #226 正文写 `Note on v2.4.5's esp-dl/esp-sr coexistence fix: crashes #1-5 below were reproduced on 2.4.7 (already includes the 2.4.5 fix)`，即**存在一个 esp-sr 2.4.5 的 "esp-dl/esp-sr 共存修复"**；本机用的 esp-sr **2.5.3** 理论上已含该修复（#302 的报告环境是 2.4.3）。该修复的官方 release note 原文**未取到**。[esp-sr#226](https://github.com/espressif/esp-sr/issues/226)（**open**，创建 2026-07-30，更新 2026-08-03；ESP32-S3-WROOM1-N16R8、IDF v5.3.4，esp-sr 2.3.1 与 2.4.7 均复现）【社区 + 部分未核实】
- **直击本机所用 esp-sr 2.5.3 的模型加载失败（非内存不足）—— esp-sr #242**：标题 `WakeNet10 models fail to load: package ships wn10_data_p1/_p2, loader looks for wn10_data (esp-sr 2.5.3)`；**open**，创建 **2026-09-08**，更新 2026-09-14；环境 esp-sr **2.5.3** / IDF 6.0.2 / ESP32-S3 16MB flash + PSRAM / xiaozhi-esp32 v2.4.2；报错 `E WAKENET10: can not find wn10_data in model wn10_heynova` → `Failed to create WakeNet model data`；报告者原文：`This is not memory pressure: 132 KB of SRAM free, and the 64000-byte cache allocated in PSRAM without complaint.`（称 2.5.3 的 **全部 8 个 WakeNet10 模型都加载不了**，wn9 正常）。[esp-sr#242](https://github.com/espressif/esp-sr/issues/242)（2026-09-08）【社区，open】
  → 与本机相关度最高的一条：本机 esp-sr 为 **2.5.3**，目标固件 xiaozhi-esp32 用 esp-sr 唤醒词。
- **唤醒词在 N16R8 + Octal PSRAM 80MHz 下的可靠性问题（非内存冲突）—— esp-sr #214**：WakeNet9（wn9_hiesp）**间歇性失效**（无崩溃、无内存泄漏、堆/PSRAM free 字节不变）；环境 ESP32-S3 N16R8、IDF v5.5.4、esp-sr v2.4.6、`CONFIG_SPIRAM_MODE_OCT=y` / `CONFIG_SPIRAM_SPEED_80M=y`，160 与 240 MHz 表现相同；**open**，创建 2026-06-22，更新 2026-07-27。[esp-sr#214](https://github.com/espressif/esp-sr/issues/214)【社区，open】
- **`ESP_ERR_NO_MEM` / flash 分区不足 / 人脸+唤醒词叠加导致模型加载失败的 issue：未找到证据**。最接近的是 esp-who #304（ESP32-S3 WROOM2 32MB，RainMaker + BLE NimBLE + Face Detection 三者同跑 → `BLE_INIT: Malloc failed` → Interrupt WDT 崩溃），但那是 **BLE 侧**而非 esp-sr；**open**，创建 2024-09-06，更新 2024-12-17。[esp-who#304](https://github.com/espressif/esp-who/issues/304)【社区，open；标题字段在 API 返回中被截断，以链接页为准】
- 相关但不同的历史 issue（旧版 esp-who）：[esp-who#334](https://github.com/espressif/esp-who/issues/334)（open，2025-10-05）作者描述"在 esp32-cam 上 RAM 太低导致导出 x/y 时崩溃"，随后换 ESP32-S3-CAM 又遇重启循环。**注意**：release/v1.1.0 + 非官方支持板，不能外推到 master。【社区】

### 6.4 esp-who 在 IDF 5.5.x 上的编译/运行问题（补充）

| Issue | 标题 | 状态 | 日期 |
|---|---|---|---|
| [esp-who#331](https://github.com/espressif/esp-who/issues/331) | `getting error. has no member xCoreID (AIS-2079)`（`components/who_task/who_task_state.cpp:73:37: error: 'TaskStatus_t' … has no member named 'xCoreID'`） | **open** | 创建 2025-07-10，更新 2026-09-09 |
| [esp-who#335](https://github.com/espressif/esp-who/issues/335) | `'DL_IMAGE_CAP_RGB565_BIG_ENDIAN' was not declared in this scope`（**ESP-IDF 5.5.1** 下 human_face_detect 示例） | **open** | 创建 2025-10-29，更新 2026-02-14 |
| [esp-who#334](https://github.com/espressif/esp-who/issues/334) | `esp32-s3-cam esp who (AIS-2155)`（**release/v1.1.0 旧分支** + ESP-IDF 5.5.1 无限重启） | **open** | 创建 2025-10-05，更新 2025-10-30 |
| [esp-who#298](https://github.com/espressif/esp-who/issues/298) | `NT99141 sensor not working properly (AIV-718)`（非 OV 传感器时图像异常 + 刷 `cam_hal: FB-OVF`） | **open** | 创建 2024-10-15 |
| [esp-who#337](https://github.com/espressif/esp-who/issues/337) | `E cpu_start: Failed to init external RAM; continuing without it. (ESP32S3-N16R8) (AIS-2249)`；日志 `octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode`；作者称 OPI/QUAD、40/80 MHz 全部失败 | **open** | 创建 2026-01-06，更新 2026-01-07 |
| [esp-who#235](https://github.com/espressif/esp-who/issues/235) | `Init PSRAM manually trowing failed to allocate frame buffers. (AIV-564)`；作者为 "esp32s3 with 8 mb OCTAL psram"，报 `Allocating 115200 Byte frame buffer in PSRAM` → `frame buffer malloc failed` | closed | 创建 2022-11-30，关闭 2022-12-15 |

- **反证（说明 master 在 5.5.x 上确实能编译）**：#335 的正文日志显示依赖解析与 CMake 配置在 **ESP-IDF 5.5.1** 下成功完成（`-- Processing 14 dependencies: … [14/14] idf (5.5.1)`），失败点是源码里缺命名空间限定，**不是 IDF 不兼容**。同链接。【社区】
- 官方侧的最强正面证据仍是 2026-05-28 官方博客（在 S3-EYE 上跑通 face recognition，`targets ESP-IDF v5.5.x`）与 esp-who master 的 `dependencies.lock.esp32_s3_eye`（IDF 5.5.5 + esp-dl 3.3.8 + human_face_recognition 0.3.2），见第 2 节。【官方】

### 6.5 其他已核实的兼容性坑

| 事实 | 来源 | 日期 |
|---|---|---|
| esp-dl `3.3.7` 被官方 **Yank**，原因 `do not support idf v6.0` | [esp-dl versions](https://components.espressif.com/components/espressif/esp-dl/versions)【官方】 | 2026-09-15 读 |
| esp-dl 自 **3.1.0（2025-01-09）** 起改了模型 schema：`previous models can be load by new schema, but new model is not compatible with previous version` | [esp-dl README](https://raw.githubusercontent.com/espressif/esp-dl/master/README.md)【官方】 | 2025-01-09 |
| 旧 esp-who 分支（release/v1.1.0）只能支持到 **IDF 5.3**（末次提交信息 `Now esp-who support up to idfv5.3.`）；release/v1.1.0 README 声明 `ESP-WHO runs on ESP-IDF release/v5.0 branch` | [commits?sha=release/v1.1.0](https://api.github.com/repos/espressif/esp-who/commits?sha=release/v1.1.0)、[release/v1.1.0 README](https://raw.githubusercontent.com/espressif/esp-who/release/v1.1.0/README.md)【官方】 | 2025-03-25 |
| `human_face_recognition` 0.3.2 ↔ `human_face_detect` 0.5.0 **版本区间不互容**（`~0.4.1`），同时用会解析到 detect 0.4.2 | [Registry API](https://components.espressif.com/api/components/espressif/human_face_recognition)【官方】 | 2026-05-12 |
| 官方 benchmark 文档与社区实测存在方向性矛盾（MFN 248.8ms vs 社区 MFN 4200ms；MBF 1072.4ms vs 社区 MBF 730ms），该 issue 已 closed 但未见文档修订结论 | [esp-dl#226](https://github.com/espressif/esp-dl/issues/226)【社区 + 官方表格】 | 2025-04-03 |
| 首帧/首次启动会打印 `E dl::recognition::DataBase: Failed to open db`（人脸库文件尚不存在），官方说明属预期 | [Assignment 2](https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-2/)【官方】 | 2026-07-07 |
| 官方指出 S3-EYE 工作坊中可能遇到编译错误 `'led_indicator_handle_t' was not declared in this scope`，需手动加 `#include "bsp/esp32_s3_eye.h"` | 同上【官方】 | 2026-07-07 |

---

## 7. 明确"未找到证据"的项（避免误读）

1. esp-who 存在**官方声明的后继项目**——未找到证据。（官方只把 esp-dl / esp-detection / esp32-camera / esp-video-components 列为资源链接；esp-video 的继任对象是 esp32-camera，见 1.4。）
2. esp-who 有 **v2.x / v3.x release 或 tag**——未找到证据（tags 止于 v1.1.0）。
3. `human_face_detect` / `human_face_recognition` 是"由 esp-who 拆分出来"的官方说明——未找到证据（事实：由 esp-dl 仓库 `models/` 发布）。
4. 官方声明"某版本起 human_face_detect / human_face_recognition 在 IDF 5.x 上不兼容"——未找到证据。相反，官方 2026-05-28 博客明确 `ESP-WHO currently targets ESP-IDF v5.5.x`。
5. 在 IDF 5.5.x 上**成功编译并运行** esp-who master 人脸识别示例的**第三方实测博客**（带日期）——未找到可核实的；最强证据是官方博客（2026-05-28，含启动日志）+ 官方 `dependencies.lock.esp32_s3_eye`（IDF 5.5.5 + esp-dl 3.3.8）。
6. 检测/识别模型在 ESP32-S3 上的 **PSRAM 占用 MB 数**的官方或实测数字——未找到证据（只有 `profile_memory()` 用法与 API 参数语义）。
7. **官方"推荐任务栈大小"**——未找到证据（只有 IDF 的"任务栈只能用内部 RAM"规则、esp32-camera 默认 4096、以及社区建议 8 KB）。
8. **可支持的人脸库人数上限**的官方或实测记录——未找到证据（本报告按 2050 B/人 + 1MB 分区**推算** ≈511 人）。
9. **端到端 fps** 的官方数字——未找到证据（仅有分阶段 ms）。
10. 官方 latency 表中**是否启用 PSRAM / CPU 频率**的标注——未找到证据。
11. **`human_face_detect_msr01` / `mnp01` / `mnp02` 的官方文件大小或 3.x 对应物**——未找到证据（3.x 无此命名；属旧版 2.x/v1.x 头文件名，仅见于社区旧帖）。
12. **`espressif/human_face_detect` 组件 zip 内各 `.espdl` 的精确字节数**——未找到证据（`components-file.espressif.com[.cn]` 的包体/CHECKSUMS 在本会话网络下无法下载；已用 GitHub `contents` API 的 `size` 字段替代）。
13. **Octal PSRAM 与人脸识别模型冲突**的 issue / 官方告警——未找到证据（官方 S3-EYE 配置本身即 Octal @80 MHz + XIP from PSRAM + 240 MHz）。
14. **esp-sr 唤醒词模型与人脸模型在 flash/PSRAM 上冲突**导致模型加载失败（`ESP_ERR_NO_MEM` / 分区不足）的 issue——未找到证据；已找到的是**行为异常**类报告（esp-dl#302 人脸框数量异常）与 esp-sr 自身的模型加载 bug（esp-sr#242）。
15. esp-sr **2.4.5 "esp-dl/esp-sr coexistence fix" 的官方 release note 原文**——未取到（GitHub releases API 请求失败；仅有 esp-sr#226 正文转述）。
16. esp-video 含人脸检测/识别能力的证据——未找到证据。
17. Seeed `face_rec_api` 方案中"**ESP32-S3** 无法本地做 1:N 识别所以走服务器"的原文表述——未找到证据。Seeed 官方确实给出过设备端上限，但限定对象是 **SenseCAP Watcher 的 Himax WE2 NPU（20 人 / 无活体）**，不可外推到 ESP32-S3 CPU（见 5.3）。
18. 中/英文公开社区（Hackster / Medium / ESP32 Forum / Reddit）可核实的 ESP32-S3 + esp-who 人脸 FPS/内存实测帖——未找到证据。

---

## 8. 核查方法与证据强度说明（供引用时判断可信度）

- **时间基准**：本机系统时间 2026-09-15（+08:00），所有"读取日期"以此为准。
- **强证据（本报告主体）**：Espressif 官方文档站 / GitHub 官方仓库 raw 文件 / Component Registry API 返回的版本与依赖元数据 / 官方 Developer Portal 文章（2025-09-22、2026-05-28、2026-07-07）/ 本机 `dependencies.lock` 与分区表文件。
- **中等证据**：GitHub issue 的标题、状态、创建/更新日期（经 API 返回），以及官方维护者在 issue 中的回复（如 esp-dl#186 的 RGB565 大小端说明）。
- **弱证据（已在正文标注）**：单条用户日志中的数字（如 `Maximum mermory size: 840448`）、社区工程调研文档（置信度自评 MEDIUM/LOW）、二手转述文章。
- **未复核项（正文已标"未独立复核"）**：`78/xiaozhi-esp32` issue #246 / #2163，以及 `suharvest/xiaozhi-esp32` 各分支的源码级结论 —— 均因 **GitHub REST API 匿名限流 403** 无法二次确认。
- **推算项（正文已标"推算"）**：模型组合合计体积、1 MB `storage` 分区可容纳人数（≈511）、端到端 fps 上限（≈25–27 fps 纯检测 / ≈3.4 fps 检测+识别）、511 人时的点积次数 —— 这些均为**基于官方数字的算术**，非官方结论。

---

## 附录 A：本报告用到的关键一手链接

| 主题 | 链接 |
|---|---|
| esp-who 仓库元数据 | https://api.github.com/repos/espressif/esp-who |
| esp-who master README（IDF 支持矩阵） | https://raw.githubusercontent.com/espressif/esp-who/master/README.md |
| esp-who releases / tags | https://api.github.com/repos/espressif/esp-who/releases · https://api.github.com/repos/espressif/esp-who/tags |
| esp-who S3-EYE 依赖锁定 | https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/dependencies.lock.esp32_s3_eye |
| esp-who S3-EYE sdkconfig（OCT PSRAM） | https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/sdkconfig.bsp.esp32_s3_eye |
| esp-who 分区表（7000K / 1900K+200K+5000K） | https://raw.githubusercontent.com/espressif/esp-who/master/examples/human_face_recognition/partitions.csv · .../partitions2.csv |
| esp-dl README（IDF ≥5.3） | https://raw.githubusercontent.com/espressif/esp-dl/master/README.md |
| esp-dl 模型加载三方式（官方教程） | https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_load_test_profile_model.html |
| 检测模型 README（延迟表 + 三位置 Kconfig + human_face_det 分区名） | https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_detect/README.md |
| 识别模型 README（延迟表 + 精度 + human_face_feat 分区名） | https://raw.githubusercontent.com/espressif/esp-dl/master/models/human_face_recognition/README.md |
| 模型文件列表与字节大小（S3） | https://api.github.com/repos/espressif/esp-dl/contents/models/human_face_detect/models/s3 · .../human_face_recognition/models/s3 |
| 特征 2050 字节/人 + DB 三文件系统 | https://raw.githubusercontent.com/espressif/esp-dl/master/examples/human_face_recognition/README.md |
| 人脸库源码（线性遍历 / SPIRAM 分配） | https://raw.githubusercontent.com/espressif/esp-dl/master/esp-dl/vision/recognition/dl_recognition_database.cpp |
| Registry：esp-dl / human_face_detect / human_face_recognition | https://components.espressif.com/api/components/espressif/esp-dl · .../human_face_detect · .../human_face_recognition |
| Espressif 工作坊 Assignment 2 / 3（S3 延迟、DB、阈值 0.5） | https://developer.espressif.com/workshops/edge-ai-with-esp32-s3/assignment-2/ · .../assignment-3/ |
| esp-video 官方博客（esp32-camera 继任说明） | https://developer.espressif.com/blog/2025/09/esp-video-introduction/ |
| esp_cam_sensor 支持传感器表（含 GC0308 DVP RGB565） | https://raw.githubusercontent.com/espressif/esp-video-components/master/esp_cam_sensor/README.md |
| esp-dl issue #186（RGB565 大小端）/ #254（灰度）/ #226（延迟矛盾）/ #302（esp-sr 共存） | https://github.com/espressif/esp-dl/issues/186 · /254 · /226 · /302 |
| esp-who issue #334（IDF 5.5 + v1.1.0 重启）/ #335（RGB565 cap 编译错） | https://github.com/espressif/esp-who/issues/334 · /335 |
| esp-sr issue #242（2.5.3 WakeNet10 加载失败）/ #214（N16R8 唤醒失效）/ #226 | https://github.com/espressif/esp-sr/issues/242 · /214 · /226 |
| ESP-IDF v5.5.3 外部 RAM / flash+psram 配置 | https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/external-ram.html · .../flash_psram_config.html |
| Espressif 官方博客《ESP-WHO: Get started》 | https://developer.espressif.com/blog/2026/05/esp-who-get-started/（2026-05-28） |
| Seeed face_rec_api wiki / 仓库 / 设备端上限页 | https://wiki.seeedstudio.com/cn/face_regonition_with_mcp/ · https://github.com/suharvest/face_rec_api · https://wiki.seeedstudio.com/solutions/mcp-face-auth-integration/ |
| GC0308 驱动源码（格式/尺寸/功能限制） | https://raw.githubusercontent.com/espressif/esp32-camera/master/sensors/gc0308.c |
| esp-dl v3.3.11 模型打包 CMake | https://raw.githubusercontent.com/espressif/esp-dl/v3.3.11/models/human_face_detect/CMakeLists.txt · .../human_face_recognition/CMakeLists.txt |
| 量化与 .espdl 格式（ESP-PPQ） | https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_quantize_model.html |

---

## 附录 B：本报告引用的 issue 索引（编号 / 状态 / 日期）

| Issue | 标题（摘） | 状态 | 创建 → 关闭/更新 | 与哪个问题相关 |
|---|---|---|---|---|
| [esp-who#334](https://github.com/espressif/esp-who/issues/334) | esp32-s3-cam esp who (AIS-2155) | open | 2025-10-05 → 2025-10-30 | Q2 / Q6（旧分支 + IDF 5.5） |
| [esp-who#240](https://github.com/espressif/esp-who/issues/240) | IDF 5.0 support. (AIV-574) | closed | 2023-01-24 → 2024-08-02 | Q2 |
| [esp-who#331](https://github.com/espressif/esp-who/issues/331) | has no member xCoreID (AIS-2079) | open | 2025-07-10 → 2026-09-09 | Q6.5（编译） |
| [esp-who#335](https://github.com/espressif/esp-who/issues/335) | `DL_IMAGE_CAP_RGB565_BIG_ENDIAN` not declared（IDF 5.5.1） | open | 2025-10-29 → 2026-02-14 | Q6.1（RGB565） |
| [esp-who#337](https://github.com/espressif/esp-who/issues/337) | Failed to init external RAM (ESP32S3-N16R8) | open | 2026-01-06 → 2026-01-07 | Q6.2（Octal PSRAM） |
| [esp-who#319](https://github.com/espressif/esp-who/issues/319) | RGB565 帧在 human_face_detect 0.2.3 下检不出人脸（0.2.1 正常） | closed | 2025-05 | Q6.1（RGB565 回归） |
| [esp-who#235](https://github.com/espressif/esp-who/issues/235) | Init PSRAM manually → frame buffer malloc failed（8MB OCTAL PSRAM） | closed | 2022-11-30 → 2022-12-15 | Q6.2 |
| [esp-who#298](https://github.com/espressif/esp-who/issues/298) | NT99141 sensor not working properly | open | 2024-10-15 | Q6（非 OV 传感器） |
| [esp-who#304](https://github.com/espressif/esp-who/issues/304) | RainMaker + BLE + Face Detection 内存分配失败 | open | 2024-09-06 → 2024-12-17 | Q6.3（资源叠加） |
| [esp-who#254](https://github.com/espressif/esp-who/issues/254) | human_face_recognition 例程录入的人脸无法储存（**旧版**：`fr partition size: 131072 bytes, maximum 62 IDs`） | open | 2023-04-13 → 2023-12-13 | Q4.4（旧版库上限） |
| [esp-dl#302](https://github.com/espressif/esp-dl/issues/302) | 人脸检测 + ESP-SR 一起使用时人脸框异常 | closed | 2026-05-16 → 2026-05-25 | Q6.3（**最关键**） |
| [esp-dl#226](https://github.com/espressif/esp-dl/issues/226) | Face recognition documentation doesn't match reality (AIV-776) | closed | 2025-04-03 → 2025-04-08 | Q4.2（延迟矛盾） |
| [esp-dl#254](https://github.com/espressif/esp-dl/issues/254) | human_face_detect not working on grayscale images | closed | 2025-07-10 | Q6.1（灰度） |
| [esp-dl#186](https://github.com/espressif/esp-dl/issues/186) | face_recognizer 模型输入 rgb565 数据不能正常工作 | closed | 2024-12-18 | Q6.1（**大小端**） |
| [esp-dl#218](https://github.com/espressif/esp-dl/issues/218) | dl::Model: Assign input failed（含 GC0308 实测日志） | closed | 2025-03-17 → 2025-03-24 | Q6.1（GC0308 可用） |
| [esp-dl#163](https://github.com/espressif/esp-dl/issues/163) | reshape assert（RGB565 + 96×96 通道序） | closed | 2024-05-04 → 2024-10-14 | Q6.1 |
| [esp-dl#236](https://github.com/espressif/esp-dl/issues/236) | hw_decode_jpeg defaulting to BGR in rgb_order? | closed | 2025-05-04 → 2025-05-12 | Q6.1（默认色序） |
| [esp-dl#116](https://github.com/espressif/esp-dl/issues/116) | ESP32-S3-WROOM Cam + octal PSRAM，人脸识别卡在 cpu_start | closed | 2023-03-14 → 2024-10-21 | Q6.2（旧版） |
| [esp-sr#242](https://github.com/espressif/esp-sr/issues/242) | WakeNet10 models fail to load (esp-sr **2.5.3**) | open | 2026-09-08 → 2026-09-14 | Q6.3（**直击本机 esp-sr 版本**） |
| [esp-sr#214](https://github.com/espressif/esp-sr/issues/214) | WakeNet9 在 N16R8 + Octal PSRAM 下间歇失效 | open | 2026-06-22 → 2026-07-27 | Q6.2 / Q6.3 |
| [esp-sr#226](https://github.com/espressif/esp-sr/issues/226) | esp-dl / esp-sr 共存崩溃（提及 2.4.5 修复） | open | 2026-07-30 → 2026-08-03 | Q6.3 |
| [esp-idf#12342](https://github.com/espressif/esp-idf/issues/12342) | LCD corruption by Wi-Fi（Octal PSRAM 80MHz 缓冲） | open | 2023-10-03 → 2024-02-14 | Q6.2（总线争抢） |
| [esp-idf#18764](https://github.com/espressif/esp-idf/issues/18764) | DMA TX underflow on ESP32-S3 QSPI LCD + Wi-Fi（8MB Octal PSRAM） | open | 2026-06-26 → 2026-07-23 | Q6.2（总线争抢） |
