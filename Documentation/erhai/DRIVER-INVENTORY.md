# erhai 驱动成果总清单（OnePlus Pad 2 Pro · 主线 Linux 移植）

> 仓库：https://github.com/BUSHIGEMENNI/linux-sm8750-erhai ｜ 分支：`erhai/v0.3` ｜ 基线：Linux **7.2**（`Makefile` VERSION=7 / PATCHLEVEL=2）
> 设备：OnePlus Pad 2 Pro（OPD2413 / 代号 erhai / Qualcomm SM8750）
> 本文档：2026-11-02 依据分支 git 历史与设备树实证整理。**所有状态结论均附提交哈希；无证据处标 unknown，不臆断。**

## 0. 分支概况与贡献构成

`erhai/v0.3` 可见历史共 **189 个提交**（浅嫁接基线，最老提交为 nt36536 触摸驱动清理系列）：

| 构成 | 提交数 | 说明 |
|---|---|---|
| 本项目新增（本地 `v7.2` 基线 tag 之后） | **129** | 相机双摄三驱动、CPS8601 笔充、39W PPS 充电栈、EEPROM、CAMSS 回移、CCI/pmic_glink/usbss 修复、全套 defconfig 片段与设备树 |
| 基线树携带（tag 之前） | 60 | 来自 yzddmr6/linux-sm8750-erhai 的 bring-up 提交（nt36536 触摸/面板、pogo 键盘、qcom_battmgr PPS、DP、iris 等），以及保留原始作者署名的上游 cherry-pick（Qualcomm clk 系列、ath12k、hci_qca 等） |

关键文件：

- 设备树：`arch/arm64/boot/dts/qcom/sm8750-erhai.dts`（2839 行，49 个提交持续演进；平台级 `sm8750.dtsi` 来自基线树未改动）
- 内核配置片段：`arch/arm64/configs/erhai-firstboot.config`（带完整选型注释）
- 新增驱动文件 13 个（8 个驱动，见 §2），回移 CAMSS 新文件 3 个 + DT binding 2 个

状态词汇：**working** = 实机验证可用；**partial** = 部分可用（缺什么在说明里写明）；**queued** = 方案/情报就绪、未落地或未验证；**unknown** = 证据不足不下结论。

---

## 1. 功能域总表

| # | 功能域 | 驱动 / 文件 | 状态 | 来源 | 说明（证据） |
|---|---|---|---|---|---|
| 1 | 内屏面板 | `drivers/gpu/drm/panel/panel-novatek-nt36536.c` | working | 基线 | NT36536 DSI 面板；补 60/30 Hz porch 模式（`4689fabd90`） |
| 2 | 面板背光 | dts `led-backlight` + Kinetic KTZ8868 ×2 | working | 基线 | 双灯串并成一路背光（dts 1377/1397 行区） |
| 3 | GPU（Adreno 830） | `drm/msm` + OnePlus 签名 zap shader | working | 基线 | zap 签名固件加载（`c4cb502db1`）、GX GDSC/IFPC 对齐（ac36456442 等 cherry-pick） |
| 4 | DP 外接显示 | msm_dp + pmic_glink altmode + dts | unknown | 基线+自研 | 驱动链就绪：DP over USB-C 使能（`833e1c8679`）、SBU 开关入图（`797fbf8d72`）、AUX 失败重试（`cc6e10632c`）；待实机接坞验证 |
| 5 | 触摸屏 | `drivers/input/touchscreen/nt36536/` | working | 基线 | NT36536 SPI TDDI，panel-follower 时序、vendor 属性清理（8 提交系列，dt-binding `a02ac33d2b`） |
| 6 | 触摸笔数字化 | nt36536 pen 帧 + `pen_scan` sysfs | partial | 基线+自研 | 笔上报进触摸帧（`2d36d712a8`）、用户态开关（`6ff6537773`）、Pencil 2 专用扫描子命令（`f3198011ae`）；2026-11-02 终验：笔经 CPS8601 线圈唤醒+BLE 配对后，数字化仪出笔迹（6125 事件实测）+ BLE 侧自带 OPPO Pencil 2 input（按键通道，uhid 由 BlueZ 自动建立）|
| 7 | Pogo 键盘（吸附矩阵） | `drivers/input/keyboard/oneplus-pogo-keyboard.c` | working | 基线+自研 | Fn 顶行功能层（`1b3a899fba`）；吸附时经 0x2F SYNC 帧解析并 uevent 上报键盘蓝牙地址（`ce79b4c29a`） |
| 8 | 物理按键 | qcom-pon resin | working | 基线 | PON resin 映射音量上（`0ea94c0c77`） |
| 9 | USB 外设支持 | usb-serial CH341、HID 电池等 | working | 基线 | `722ae37dfa`、`03ae7d8585`；可移动存储模块集（`972767e752`） |
| 10 | CAMSS（SM8750 ISP） | `camss-csid-980.c` / `camss-csid-gen4.c` / `camss-vfe-gen4.c` + binding | working | 回移上游 | 高通官方 mailing-list 系列回移：Kaanapali v14（`6b087fd2c5`）+ SM8750 v5（`70a6124582`）+ 3way 丢件补齐（`4d906d99b0`）+ gen4 顺序修正（`a62e7be95e`）；dts 重写为官方 binding（`0ab6397716`） |
| 11 | CCI0（后摄 I2C） | `drivers/i2c/busses/i2c-qcom-cci.c` | working | 自研修改 | bring-up 门控、AHB/MNOC 时钟投票、CPAS-TOP 解锁（`e00635e7d6`）——缺任一时钟则 CCI 复位永无 ACK |
| 12 | 后摄传感器 | `drivers/media/i2c/sc1320cs.c` | working | 移植+深改 | SmartSens 13MP 主摄。自 TheMojoMan 树引入（`a81a2c3028`）后 21 提交深改：曝光半行打包修复（`305efd214`/`f5eebbfec0`）、厂商半帧默认曝光（`1c3ac8ef1b`）、VTS 3200（`4272cc5912`）、selection/init_state（`055ed8d38c`）、SBGGR 相位定案（`74f93e93e1`）、可选 2104×1560 bin 读出（`db3d6bfd51`） |
| 13 | 前摄传感器 | `drivers/media/i2c/sc820cs.c` + `sc820cs-caihong-regs.h` | working（帧速受限） | 自研 | 从零编写的主线驱动（`bfdeea0a16`），寄存器表自 caihong 验证表移植（`56a6e662e6`）；7-bit 地址 0x10（`cf04eff370`）、上电时序照 vendor（`27e33c9787`）、极性/相位三连定案（`f13f94baf3`/`3777a9842e`/`8afe07a3f8`）、双 PLL ÷2 收敛（`0ee444a664`）；缺：~10.9 fps 半速（见 §5） |
| 14 | 对焦 VCM | `drivers/media/i2c/cn3927.c` | working | 移植+改造 | Chipnext 音圈 DAC；无穷远标定 DAC 驻位（`ac423d8b2a`）、remove 轨平衡（`9a27923965`）、park 以 -EBUSY 让 PM core 重试（`7dc0974451`） |
| 15 | 闪光灯 | PM8550 `leds-qcom-flash` + V4L2 flash class | partial | 自研（配置） | 打开 `CONFIG_V4L2_FLASH_LED_CLASS`（`9625198374`）、四通道全声明再逐路认领（`1508d81918`）；实测 sink0/3 为真 LED；缺：后摄闪光与抓帧联测挂起 |
| 16 | 后摄模组 EEPROM | `drivers/misc/eeprom/erhai-cam-eeprom.c` | partial | 自研 | i2c-11 @0x50，SMBus 逐字节驱动（QUP hub 子总线不支持 bulk 读）（`d49614b471`）；缺：校准 map 尚未对齐 camx 布局 |
| 17 | 扬声器（6× AW88261） | dts 六 amp 节点 + `SND_SOC_AW88261` | working | 基线（dts）+自研（路由） | 六功放节点与 ACF 固件为基线携带；出声靠用户态脚本接通 TDM 路由 + WirePlumber 锁 S16/48k（长测零超时） |
| 18 | USB-C 模拟耳机 | dts `wcd-playback` dai-link + UCM2 | working | 自研 | WCD Playback FE→BE 通路补齐（`6661fa4a79`）；插入出 sink、拔出回落扬声器，UCM2 用户态配套 |
| 19 | 麦克风 | swr2 TX capture 修复 | working | 自研 | dout-ports 声明与控制器对齐，修 APM port 120 启动 -110 超时（`6661fa4a79`、复核修正 `288f41f25`） |
| 20 | WSA / SoundWire 链 | dts `sdw20217010e00` ×2 + swr 声明 | working | 基线 | WSA885x 类 slave 枚举正常 |
| 21 | 音频平台细节 | lpass-tx-macro HPF、audioreach PCM 位宽 | working | 基线 | `bc29c081e2`、`4739a12a35` |
| 22 | ADSP 传感器（重力等 7 个） | 用户态 libssc + iio-sensor-proxy（内核侧 ADC5） | working | 自研（用户态） | 118/118 重力自检通过、GNOME 自动旋转恢复；复活战见 §4.4 |
| 23 | 霍尔（合盖检测） | ADSP hall datatype + erhai-lid-poll | partial | 自研（用户态） | 双 json 源冲突已修（vendor 覆盖 odm 的 irq/rail 错值）；缺：KDE 会话下合盖休眠禁用（锁屏崩溃循环规避中） |
| 24 | 温度/热敏 | `qcom-spmi-adc5-gen3` + CPU 被动 trip 点 | working | 基线 | ADC5/热敏声明为基线携带（dts 1716 行区）；CPU 分区被动 trip（`13d2a0c8fa`） |
| 25 | 39W PPS 快充引擎 | `drivers/power/supply/erhai_pps.c`（+ tracepoints） | working | 自研移植 | 自 erhai-charging-lab 14 补丁移植适配 7.2（`cc3c74a262`），实测 `CP 2:1 ENGAGED` 稳态；ADSP 慢启动 15s 重探（`75e7b1d508`）、符号查找重试（`d8d0bbb26e`）、SOC 滞回（`e550239750`）、ARMED 语义（`7d22107c8a`） |
| 26 | glink 充电客户端 | `drivers/power/supply/erhai_otg_glink.c` | working | 自研移植 | 顶替 qcom_battmgr 绑定 power-supply aux 设备，兼管 OPLUS OTG 通知（`cc3c74a262`）；同源修复：client_neuter 加锁（`6737c41305`）、NULL parent 解引用（`4511637d39`）、不透明类型约束（`4c0af8e4ba`） |
| 27 | SC8547A 电荷泵（主/从） | `drivers/power/supply/sc8547a.c` | working（单泵） | 自研移植 | 主泵 i2c_hub_8@6f、从泵 i2c_hub_2@6f（`cc3c74a262`）；读清除寄存器保护（`13f2bdc33`）、CP-ADC 瞬态重试（`081ddcdcc5`）、电压不可读拒绝 tick（`bcdb9df8e5`）；缺：双泵 46-47W engage 未写（queued） |
| 28 | VOOC 快充 | —（无路径） | queued | — | real_type=9 需 DPDM 帧握手唤醒 ADSP voocphy，Linux 侧从未出现；PPS 头是当前唯一快充路线 |
| 29 | USB-C / Type-C | pmic_glink + UCSI + `wcd939x-usbss` SBU mux | working | 基线+自研 | mux 内建（`489e6aa01d`）；DP SVID 才许动 crossbar（`1bf783f8a6`）、altmode 退出释放 dp_active（`3541885692`） |
| 30 | 笔无线充电 | `drivers/power/supply/cps8601.c`（+ 内嵌 bootloader `cps8601_bl.h`） | partial | 自研（逆向） | 从 vendor `oplus_wireless_pen.ko`（134KB）反汇编从零编写，23 提交：小端多字节寄存器（`9fc5721505`）、固件 SRAM 重载（`bfdeea0a16`）、ADSP ACK 门控（`e3816d8d82`）、hboost 借道 otg client（`049c85601c`）、0x8011 兄弟客户端（`238a068f59`）、q-cali 厂商序列 + INT 脚唤醒（`816de373a8`/`eb2d54ea75`/`f94e8b1938`）；协议链全通（线圈辐射、笔 ASK 应答、attach 握手 0x23c）；缺：笔端输入桥（见 §5） |
| 31 | 笔 BLE 连接 | hci_qca + 用户态 connector | partial | 自研（用户态） | 配对/连通已达（2026-11-02，LE Extended Create Connection + 裸 HCI 收养）；缺：HOGP 枚举无 hidraw，需 uhid 桥/PENC 解码 |
| 32 | 蓝牙（WCN7860） | `hci_qca` uart14 serdev | working | 基线+自研 | in-band sleep 复位（FROMLIST `5fbc7cbd11`）；LE supervision timeout 可调（`55c7c8be1`，默认 420ms 低于本控制器加密时延）；桌面 profile 模块集（`eb313ec778`） |
| 33 | WiFi（WCN7860 PCIe） | `ath12k` | working | 基线+自研 | 单 pdev 频段修复（FROMLIST `7a81d184e3`）；用户态设置监管国家（`7b7aae8a9db`）；initramfs 固件竞态 =m 修复（`2ed422f79d`） |
| 34 | 存储（UFS / Android 分区） | UFSHCD 平台链 + f2fs | working | 自研（配置） | f2fs 只读挂载休眠中的 Android userdata（`94fd6e7634`）；exfat/ntfs3 模块化 |
| 35 | 视频编解码（Iris VPU） | `iris` 解码序支持 + 10bit 预配置 | working | 基线 | decode-order 输出（`08265828b1`）、HFI 属性 ID 勘误（`4a21b3e90f`）、10bit CAPTURE 预配置（`1d2371e4c9`）均基线携带（TheMojoMan）；本机实测 HEVC 4K 编码 92 fps / H.264 720p 722 fps |
| 36 | 容器支撑（Waydroid） | binder/PSI + netfilter 模块集 | working | 自研（配置） | binder/zstd 固件加载（`2ed422f79d`）、netfilter family（`05e2e6b333`）、legacy xtables（`4c79c9c440`）、netd 所需表项（`e72be708ce`）、RT_GROUP_SCHED（`73ccaa7686`） |
| 37 | 相机 CMA 池 | dts + `CONFIG_CMA_SIZE_MBYTES` | working | 自研 | 320MB 落在 4GiB DMA 上限下唯一连续大洞（`e912a02ea6b`；512MB 预留必败的实证见提交链 `646f8ef905`→`5b1f236924`→`e912a02ea6b`） |
| 38 | 早期启动取证 | pstore/ramoops 区 | working | 自研 | dts 预留 ramoops 区捕获早期挂死（`3a97173667`） |
| 39 | 振动马达 | — | queued | — | stock 走 ADSP wsa2-macro SoundWire haptics，主线无驱动；待上游出现支持再评估 |

**总表共 39 行。**

---

## 2. 自研驱动清单（新增文件）

8 个新增驱动、13 个新文件，全部有对应 Kconfig/Makefile 登记与 `erhai-firstboot.config` 开启（`CONFIG_VIDEO_SC820CS` 等见各提交）：

| 驱动 | 文件 | 性质 | 提交数（该文件累计） |
|---|---|---|---|
| SC820CS 前摄 | `drivers/media/i2c/sc820cs.c` + `sc820cs-caihong-regs.h` | 从零编写 | 21 |
| SC1320CS 后摄 | `drivers/media/i2c/sc1320cs.c` | 引入后深度改造（21 提交） | 21 |
| CN3927 VCM | `drivers/media/i2c/cn3927.c` | 引入后改造（协议差 + 生命周期修复） | 5 |
| 后摄 EEPROM | `drivers/misc/eeprom/erhai-cam-eeprom.c` | 从零编写（SMBus 约束） | 1 |
| CPS8601 笔充 | `drivers/power/supply/cps8601.c` + `cps8601_bl.h` | 从零反汇编编写 | 23 |
| erhai_pps 引擎 | `drivers/power/supply/erhai_pps.c` + `erhai_pps_trace.h` | lab 移植 + 本机化 | 7 |
| erhai_otg_glink | `drivers/power/supply/erhai_otg_glink.c` + `.h` | lab 移植 + 本机化 | 5 |
| SC8547A 电荷泵 | `drivers/power/supply/sc8547a.c` + `sc8547a.h` | lab 移植 + 本机化 | 5 |

另有回移上游 CAMSS 新文件（`camss-csid-980.c`、`camss-csid-gen4.c`、`camss-vfe-gen4.c`、两个 DT binding yaml），以及对 5 个存量驱动的实质修改：`i2c-qcom-cci`（bring-up 门控与时钟投票）、`pmic_glink`（neuter 加锁、不透明类型，含 `include/linux/soc/qcom/pmic_glink.h` 收紧）、`wcd939x-usbss`（DP crossbar 语义，含新增 `include/linux/usb/wcd939x-usbss.h`）、`nt36536` 触摸（Pencil 2 扫描子命令）、`oneplus-pogo-keyboard`（蓝牙地址 uevent）。注：基线树的 `qcom_battmgr` PPS 系列现被 erhai 充电栈替代并经配置下线（`CONFIG_BATTERY_QCOM_BATTMGR=n`）。

---

## 3. 亮点：技术含量最高的五件事

### 3.1 相机三驱动——从反汇编寄存器表到相位/曝光全链定案

前摄 SC820CS 无任何主线驱动可用：先以 caihong 机型验证过的 3264×2448 寄存器表起步（`56a6e662e6`），再用「裸 RAW 直抓 → RAW10 解包 → 四位置统计 + 已知颜色实物终审」把 Bayer 相位从三次反转中钉死（`74f93e93e1`、`8afe07a3f8`——R/B 互换统计上完全对称，只有拿黄物让主人肉眼裁决才能破循环论证）。后摄曝光的双重静默 bug（半行计数器移位量写错 + 默认值截断一位十六进制）靠「两个互不相关的厂商数据源互解方程」定案（`305efd214`、`f5eebbfec0`、`1c3ac8ef1b`）：旧代码 AGC 满行程上限还不如新代码的起点。VCM 的无穷远驻位修复（`ac423d8b2a`）解决上电后焦平面卡死。整条链以 v8 终审全绿收口（24.3 fps、AGC 探针、无内核告警）。

### 3.2 CPS8601 笔充——134KB vendor .ko 反汇编出的完整协议

芯海 CPS8601 无公开文档。从 super 分区抠出的 `oplus_wireless_pen.ko`（not-stripped、206 符号）反汇编出寄存器表、上电时序、内嵌 8051 bootloader（固化进 `cps8601_bl.h`）。一路上钉死的硬事实：多字节寄存器是小端（`01 86` 原始字节流对 `0x8601`，`9fc5721505`）、第五根控制线是 boost-enable 不是 off-state（`7052c6662c`）、本机拉 sleep 线会杀整条 I2C 总线（vendor 的 GP_0 唤醒法在此板不可用，改走 INT 脚握手，`eb2d54ea75`）、hboost 轨必须借 ADSP 就绪后的 glink client 申请（`049c85601c`、`238a068f59`）。最终 q-cali 校准成功、线圈首次辐射、笔端 ASK/EPT 应答实锤、attach 握手 0x23c 全通——一个原本连 I2C 都不响应的芯片，被逆向到协议级点亮。

### 3.3 Pogo 键盘协议——串口 0x2F SYNC 帧与「不广播的配件」

OPPO 配件走 non-discoverable 白名单，Linux 侧永远扫描不到。键盘的蓝牙地址藏在 Pogo 串口协议的 0x2F SYNC 帧里（data[3..8]=MAC）：主线驱动在吸附瞬间解析并经 uevent 上报（`ce79b4c29a`），与安卓日志双源互证。再往下是 bond 跨系统转移战：安卓 PENC 私有配对格式的字段序/端序/LTK 方向三处全错，靠写 12 个组合变体、重启、连接、btmon 看 Encryption Change 的自动化穷举 15 秒一个全部跑完定案——逆向格式没有文档时，穷举比推理快且可靠。

### 3.4 传感器栈复活——两行 LD_LIBRARY_PATH 与一场 registry 考古

erhai 的加速度计等传感器全在 ADSP 固件里，Linux 侧靠厂商私有前缀二进制（libssc 系）桥接。全栈哑火一整晚的根因只是 gate 脚本和 iio-sensor-proxy 单元都没带 `LD_LIBRARY_PATH=/usr/lib/erhai/sensors/lib`——修两行后 118/118 重力自检通过、GNOME 自动旋转复活。随后霍尔传感器永不发布的悬案，用「孤儿变量法」在 ADSP sensor registry 考古出双 json 源冲突：vendor 树的错值（irq 65/无 rail）每次开机覆盖 odm 树的对值，孤儿变量法一击定位——7 个活传感器全部 odm-only，唯一双源冲突的唯一死。

### 3.5 笔 BLE 连通战——WCN7860 控制器的四道暗门

笔（OPPO Pencil 2）与键盘同在白名单，不广播、只接受定向连接。连通战连破四坑：常驻扫描服务每秒掀翻连接请求；WCN7860 的 legacy Create Connection 恒返 0x0c，必须用 LE Extended Create Connection；BlueZ 白名单路径对未配对设备根本不发连接命令，只能裸 HCI 建链再 8 秒后「收养」；HCI 地址是 LSB-first。叠加内核侧 LE supervision timeout 可调化（`55c7c8be1`，默认 420ms 低于该控制器 ~600ms 的加密时延，加密完成瞬间必断），2026-11-02 配对/连接/信任全通，GATT 全 dump 抢到 114 个对象（标准 HOGP + PENC 私有服务 + Nordic DFU）。制胜序列全文存 `~/erhai-pen/CONNECT-SOLUTION-20261102.md`。

---

## 4. 构建与部署

三步出包链（集成仓库 `~/opp2p`；**第二步不可省**——vendor_boot 里的 DTB 是 `build-erhai-abl-symbols.py` 从内核 dtb + 原厂 vendor_boot/dtbo 合成的独立产物，漏跑则启动链吃陈旧 DT、相机整链静默消失）：

```bash
# 1. 编译内核（Image + sm8750-erhai.dtb + modules）
cd ~/opp2p && export ALLOW_UNPINNED_KERNEL=1 ERHAI_WORKSPACE=$HOME \
    KERNEL_SOURCE=$HOME/kernel/linux OUT_DIR=$HOME/out/kernel-batch1 JOBS=$(nproc)
tools/build-kernel.sh

# 2. 合成 ABL dtb（内核 dtb ⊕ 原厂 vendor_boot/dtbo）
ERHAI_WORKSPACE=$HOME python3 tools/build-erhai-abl-symbols.py \
    --mainline-dtb $OUT_DIR/arch/arm64/boot/dts/qcom/sm8750-erhai.dtb \
    --vendor-boot ~/cache/stock-c34/images/vendor_boot.img \
    --dtbo-img ~/cache/stock-c34/images/dtbo.img \
    --dtc /usr/bin/dtc --output ~/out/abl-dtb-current

# 3. 打 deb（boot 三件套 + 模块树，AVB 签名）
ERHAI_WORKSPACE=$HOME python3 tools/build-debs.py \
    --avb-key ~/erhai-plan/keys/boot.pem --avbtool ~/cache/tools/avbtool.py \
    --kernel-out ~/out/kernel-batch1 --dtb ~/out/abl-dtb-current/erhai-abl.dtb
```

**槽位机制一句话**：boot 三件套（boot / vendor_boot / dtbo）走 A/B 槽轮换 + Android 侧完整备份，经 KernelSU 切换器进出 Linux，最坏情况可从 fastboot 入口恢复——每次实验性刷写都有完整退路。

---

## 5. 已知限制与路线

| 限制 | 现状与证据 | 路线 |
|---|---|---|
| 前摄帧速半速 | ~10.9 fps（官方标称 ≈23.4）；caihong 借用表与官方 FTM 表零冲突但都不含 PLL 配置，真表在 camx 闭源 blob（`com.qti.sensormodule.erhaifront.bin`，已能重提取） | blob 模式寄存器表破解交付后换正主表（多模式批次一并落地） |
| 后摄帧速 | 实测 24.3 fps = 官方标称 ×0.8（blob 的 fps/pixel_clk 是声明值非实测值；SmartSens 参考设计按 24 MHz 晶振、本机喂 19.2 MHz） | dts MCLK 已提 24 MHz（`863b859228`，非官方超频、模拟余量待长测）；CSIPHY 调谐随之匹配 |
| 多模式 | 三官方 camx 读出模式（2104×1560 / 1920×1080 / 4160×2336）曾被引入又回退（`0a0e27b29`→`65f6d5851`）；blob m0 表定音实验证明公共初始化段无罪、毒药在各模式专属块 | 逐模式隔离差异块后再引入；2104 bin 读出已以 module gate 形式就位（`db3d6bfd51`） |
| VOOC 快充 | real_type=9（VOOC 族）从未在 Linux 侧出现；需 DPDM 帧握手唤醒 ADSP voocphy | 无路径；PPS 头是当前唯一已通快充（39W） |
| 双泵 46-47W | SKU 天花板（厂商 DT 55W 上限、SC8547 VBUS_OVP=10V）；lab 未竟项 | 自写 slave 泵 engage（主/从节点已在 dts：`cc3c74a262`） |
| 笔端输入 | BLE 配对/连接已通；HOGP 枚举无 hidraw（报告描述符非标准） | uhid 桥 + PENC 私有特征（notify 0x42）解码；另测 digitizer 直读 |
| 键盘蓝牙直连 | bond 转移（LTK 穷举）已通、加密成功；键盘 HID 可能只走经典蓝牙（跨传输疑点） | 加密窗口手工读 0x2A4B 裁决；必要时安卓行取经典 LinkKey |
| 后摄 EEPROM 校准 | 驱动可读（SMBus 逐字节），map 未对齐 camx 布局（AWB/LSC/PDAF 校准缺） | 对照 OnePlusOSS camx 的 EEPROM map 解析头（magic/版本/模块 ID） |
| 后摄闪光联测 | sink0/3 实物灯确认在背面；与抓帧严格同步的联测挂起 | 复测需 strobe 与曝光窗口同步；dts 收敛为 2 通道 |
| DP 外显 | 驱动链全部就绪（altmode/SBU/AUX 重试/音频） | 等实机接坞验证（零编译） |
| 合盖灭屏（KDE） | GNOME 会话全功能交付；KDE 下锁屏崩溃循环拖死挂起 | kscreenlocker/ Mesa kopper 路径专项 |
| 振动马达 | stock 走 ADSP wsa2-macro SoundWire haptics，主线无驱动 | 待上游 swr-haptics 支持再评估 |

---

## 6. 上游反馈评估表（占位）

> 本章待补：逐项评估哪些成果适合回馈上游（linux-mainline / yzddmr6 树 / 原始 lab 仓库），
> 含许可证、署名链（cherry-pick 保留原作者）、拆分粒度与实机证据清单。
