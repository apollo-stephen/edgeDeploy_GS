# Edge Impulse 模型部署到 ESP32-S3：问题、根因与解决方案

本文记录 OV5640 128×128 图像分类模型从 Edge Impulse 导出后，部署到
ESP32-S3 + ESP-IDF 5.5.4 工程时遇到的问题。重点不是罗列改了哪些宏，而是说明
错误从哪里产生、为什么选定的改法能解决根因，以及更换下一版模型时哪些检查
必须重新执行。

## 最终结果

硬件连续运行日志显示：

- DSP：`26 ms`
- classification：`289–290 ms`
- anomaly：`0 ms`
- 推理周期：约 2 秒一次
- 三个标签：`harmful`、`recycleable`、`wet`
- 未再出现 persistent buffer 分配失败、heap corruption、
  `StoreProhibited`、Guru Meditation 或 task watchdog 报警

优化前参考卷积路径一次 classification 约 `13.6 s`，优化后约 `0.289 s`，
classification 延迟降低约 47 倍。这个提升主要来自 ESP-NN；把任务放到 CPU1
主要用于隔离 Wi-Fi/HTTP 与推理负载，并不能单独带来同等级的加速。

日志中的概率是 INT8 量化输出，例如：

```text
harmful: 0.26172
recycleable: 0.54688
wet: 0.18750
Prediction: uncertain (0.54688)
```

量化值以约 `1/256` 为步长，三项之和可能因为取整而接近、但不严格等于 1。
模型阈值为 `0.60`，所以最高分 `0.54688` 仍输出 `uncertain`，这是阈值策略正常
工作，不是分类器故障。

当前固件代码新增的网页设计为：

- 左侧：实时 MJPEG 画面；
- 右侧：真正送入最近一次成功推理的 JPEG 快照；
- 下方：与右侧快照具有相同 sequence 的预测、各标签概率和耗时。

这部分仍需要重新烧录后的浏览器与串口同序号验收，不能从上面的纯串口日志直接
推断已经通过。

## 部署架构与资源边界

运行链路如下：

1. CAMERA 组件以 `PIXFORMAT_JPEG`、`FRAMESIZE_128X128` 初始化 OV5640。
2. CAMERA 组件使用 mutex 串行化帧所有权，HTTP 和推理不能同时持有同一帧。
3. `ei_inference` FreeRTOS 任务固定在 CPU1，每 2 秒申请一帧。
4. JPEG 被解码到 PSRAM 中的 49,152 字节 RGB888 缓冲区。
5. 原始 camera frame 在分类前释放，减少摄像头被占用的时间。
6. Edge Impulse EON/TFLite 运行 INT8 模型，卷积由随导出包提供的 ESP-NN
   ESP32-S3 内核执行。
7. 成功推理后，组件原子发布带 sequence 的 JPEG 快照和元数据；HTTP 只提供
   拷贝接口，避免跨任务暴露可变指针。

主要固定资源包括：

- 约 346 KB 的模型 tensor arena，主要依赖 PSRAM；
- 49,152 字节 RGB888 缓冲区；
- 两个 8,192 字节推理 JPEG 双缓冲区；
- 一个 8,192 字节 HTTP JPEG 发送缓冲区；
- 8,192 字节推理任务栈；
- 256 个 EON overflow buffer 指针槽，在 32 位 ESP32-S3 上约 1,024 字节。

## 问题一：模型组件与固件资源配置

### 现象

Edge Impulse 导出目录已经放进工程，但只复制目录并不能保证固件可用。早期构建
需要同时处理以下问题：

- ESP-IDF 不知道 `modev1` 是一个组件；
- 生成模型、Edge Impulse SDK 与端口层的源文件没有完整进入构建；
- 模型 arena 和 RGB 缓冲超出普通内部 RAM 的合理容量；
- Edge Impulse SDK 与模型使固件超过默认 1 MB application partition；
- 摄像头、HTTP 与推理可能争用 frame buffer。

### 根因

Edge Impulse 导出包是“源码和模型资源”，不是可直接运行的 ESP-IDF 应用。
ESP-IDF 仍需要组件注册、include 路径、源文件收集、依赖关系、分区表和 PSRAM
策略。缺少其中任一层，结果可能是编译失败、链接缺符号、启动时 arena 分配失败，
或者固件镜像无法放入分区。

### 解决方案

- 用独立 `modev1` 组件注册模型与 Edge Impulse SDK；
- 只收集当前推理需要的源文件，避免把无关平台端口全部编进固件；
- 在 `sdkconfig.defaults` 启用 16 MB flash、Octal PSRAM 和 large-allocation
  PSRAM 路由；
- 使用自定义 4 MB factory application partition；
- 由 CAMERA 组件统一管理帧申请和释放；
- 推理 RGB 缓冲明确使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`。

### 为什么这样解决

这些改动分别修复 ESP-IDF 的构建边界、链接边界、运行时内存边界和 flash
布局边界。单独“增加任务栈”或“打开 PSRAM”无法替代完整组件注册，也不能解决
application partition 太小的问题。

## 问题二：参考卷积导致约 13.6 秒推理与看门狗报警

### 现象

模型能够给出结果，但一次 classification 约 `13.6 s`。推理任务长时间占用 CPU0，
超过 5 秒 task watchdog 窗口，日志报告 idle task 无法运行。

### 根因

导出包中虽然包含 ESP-NN，但当时的模型组件只收集 C++ 源文件，并显式关闭
`EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN`。ESP-NN 的 `.c` 和 ESP32-S3 `.S`
实现没有进入固件，TFLite Micro 只能执行通用参考 INT8 卷积。

CPU 亲和性是另一个问题：推理最初与 Wi-Fi/HTTP 共享 CPU0，但把未加速的
13.6 秒任务简单移动到 CPU1，只会把 watchdog 压力从一个核转移到另一个核。

### 解决方案

- 收集 Edge Impulse 导出包内部 `porting/espressif/ESP-NN` 下的 `.c` 和 `.S`
  文件；
- 启用 ESP32-S3 ESP-NN 宏；
- 将推理任务固定到 CPU1，保留 CPU0 给 Wi-Fi 与 HTTP；
- 保持任务优先级、栈、两秒周期和 5 秒 watchdog 配置不变。

### 为什么不从 Component Registry 再下载 ESP-NN

当前 Edge Impulse 导出包已经带有与该 SDK 版本匹配的 ESP-NN 源码。额外下载
Registry 版本会同时存在两套实现，可能造成 API、宏、优化内核或版本不匹配。
使用导出包自带版本既减少依赖，也使模型、TFLite Micro 和优化内核保持同一
发布组合。

### 为什么不能只放宽看门狗

增加或关闭 watchdog 只会隐藏 CPU 长时间不让出的问题，13.6 秒延迟仍然存在，
网页、Wi-Fi 和其他实时任务也仍会受到影响。ESP-NN 从计算路径上消除了瓶颈，
CPU1 则隔离剩余负载，两者作用不同但需要同时使用。

## 问题三：EON overflow buffer 数量不足

### 现象

启用 ESP-NN 后，模型初始化报告：

```text
Failed to allocate persistent buffer of size 576,
does not fit in tensor arena and reached EI_MAX_OVERFLOW_BUFFER_COUNT
```

随后 Prepare 阶段使用未成功建立的数据结构，引发 `StoreProhibited`。

### 根因

生成的 EON 模型包含大量卷积/深度卷积节点。部分 persistent 或 scratch buffer
不能放入主 tensor arena，需要 runtime 用 overflow 列表记录额外分配。SDK 默认
只有 30 个指针槽，本模型在 setup 阶段超过这个数量。

这个错误不是“缺少 576 字节总内存”，而是“还有内存，但记录额外分配的指针表
已经满了”。只增加 tensor arena 既不一定覆盖生成布局，也没有直接修复这个计数
限制。

### 解决方案

在模型组件中设置：

```text
EI_MAX_OVERFLOW_BUFFER_COUNT=256
```

同时修改 Edge Impulse 端口头，使项目显式提供的值不会被 SDK 默认值 30 覆盖。

### 为什么 256 可以接受

这个宏扩大的是指针 bookkeeping 表，不会预先分配 256 个大 buffer。ESP32-S3
指针为 4 字节，因此 256 个槽约占 1,024 字节；实际 overflow buffer 仍按模型
需要动态分配。相比初始化失败，这个固定成本很小，并为下一版相近结构的模型
保留余量。

## 问题四：ESP-NN scratch buffer 未按 16 字节对齐

### 现象

overflow 数量修正后，模型能进入推理，但在 reset/free 阶段发生：

```text
CORRUPT HEAP
Guru Meditation Error: StoreProhibited
```

调用链最终位于 `tflite_learn_*_reset -> ei_free -> free`。当时捕获的 scratch
地址以 `0x14` 结尾，只满足 4 字节而不是 16 字节对齐。

### 根因

ESP32-S3 优化卷积内核对 scratch/persistent buffer 有 16 字节对齐假设。
`ei_malloc()` 的 ESP-IDF 5.x 路径已经使用 16 字节 aligned allocation，但
`ei_calloc()` 仍调用普通 `heap_caps_calloc()`。EON overflow buffer 正好通过
`ei_calloc()` 分配，ESP-NN 写入时破坏相邻 heap metadata，直到 reset 调用
`free()` 才暴露出来。

因此崩溃位置在 free，不代表 free 是根因；真正的越界/未对齐写发生在更早的
优化卷积 scratch 使用阶段。

### 解决方案

ESP32-S3、ESP-IDF 5.x 分支改为：

```cpp
heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_DEFAULT)
```

### 为什么这样解决

- `16` 满足 ESP-NN S3 内核的对齐要求；
- aligned calloc 保留 calloc 的全零语义，不需要另写 `malloc + memset`；
- `MALLOC_CAP_DEFAULT` 保持原有 capability 分配策略；
- ESP-IDF 允许这类 aligned capability allocation 通过正常 `free()` 路径释放，
  不需要修改 Edge Impulse 的 `ei_free()`；
- 只改 S3 + IDF 5.x 分支，不影响旧 IDF 和其他目标。

关闭 ESP-NN 虽然也会避开这条 scratch 路径，但会恢复约 13.6 秒的参考卷积；
跳过 model reset 则只是隐藏 heap corruption，并造成持续内存泄漏。

## 为什么 CPU1 与 ESP-NN 要同时使用

两项修改解决不同层次的问题：

| 修改 | 主要作用 | 不能单独解决什么 |
|---|---|---|
| ESP-NN | 把参考卷积替换为 ESP32-S3 优化内核，classification 降到约 289 ms | 不决定 Wi-Fi/HTTP 与推理运行在哪个核 |
| CPU1 affinity | 让 CPU0 更稳定地服务 Wi-Fi、HTTP 和系统任务 | 不会自动加速未优化的 13.6 秒卷积 |

推荐组合是 CPU1 上运行已加速推理，CPU0 处理网络和控制服务。watchdog 保持 5 秒，
继续作为回归保护，而不是被放宽来迁就慢路径。

## 硬件验收方法

每次更换模型或 Edge Impulse SDK 后，不要以“编译成功”代替硬件验收。至少检查：

1. 启动时摄像头、PSRAM、HTTP 和 inference task 均初始化成功；
2. 连续至少 5 次推理输出三个标签、最终预测和 timing；
3. classification 始终低于 5 秒，本模型目标约 289–290 ms；
4. 低于 0.60 的最高分输出 `uncertain`；
5. 网页右侧 sequence 对应的 JPEG、概率与串口同一次推理一致；
6. 左侧 MJPEG 在摄像头短暂争用后仍可继续工作；
7. 不出现 allocation failure、overflow count、heap corruption、
   `StoreProhibited`、Guru Meditation、IDLE0/IDLE1 watchdog；
8. 长时间运行时 free heap、free PSRAM 和 frame buffer 不持续下降。

## 后续重新训练与模型升级清单

下一版模型重新训练后，建议按以下顺序升级：

1. 在 Edge Impulse 固定输入尺寸、颜色格式、量化方式和目标硬件后重新导出 C++
   library；
2. 用新导出目录整体替换 `modev1` 中对应的 SDK、model parameters 和 TFLite
   model，避免混用新旧生成文件；
3. 确认标签数量不超过网页快照接口当前容量 8，标签名不超过 31 字符；
4. 检查输入宽高仍与摄像头 128×128 配置一致；若变化，摄像头、RGB 缓冲和网页
   尺寸需要共同评审；
5. 重新测量 tensor arena、overflow buffer 数量、最终固件大小和 4 MB app
   partition 余量；
6. 运行全部 host tests 和干净 ESP-IDF build；
7. 在真实垃圾样本上重新评估 accuracy、混淆矩阵和 `0.60` threshold，阈值不能
   因为旧模型合适就直接沿用；
8. 重复上一节的 timing、内存、网页同帧和长时间运行验收；
9. 记录模型版本、Edge Impulse project/version、导出日期、训练数据版本和固件
   commit，便于回滚。

## 后续路线图与优先级

### P1：推理可视化与耐久验证

完成当前双栏 dashboard，在多种类别、光照和网络连接状态下确认图片与结果严格
同帧，并进行长时间 heap/PSRAM/watchdog 观察。

### P2：重新训练并替换模型

扩充数据集、重做训练/量化，使用上一节清单替换导出包。先稳定模型接口和资源
上限，再引入远程升级，避免 OTA 与模型问题同时调试。

### P3：双分区应用 OTA

模型当前链接在 application image 中，所以应用 OTA 已经能同时升级业务代码和
模型，不需要先更新 bootloader。推荐基础方案：

- 16 MB flash 重新规划 `ota_0`、`ota_1` 和 0x2000 字节 `otadata`；
- 两个 OTA app slot 都要容纳当前固件，并为新模型预留余量；
- 使用签名镜像和 HTTPS 下载；
- 启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；
- 新固件首次启动快速检查 NVS、PSRAM、摄像头、推理和 HTTP，成功后调用
  `esp_ota_mark_app_valid_cancel_rollback()`；失败则调用
  `esp_ota_mark_app_invalid_rollback_and_reboot()`；
- 设计版本检查与 anti-rollback 时，先评估 eFuse `secure_version` 的不可逆消耗
  和回滚镜像兼容性；
- 在正式部署设备前就烧录支持 OTA 的分区表。现有 factory-only 布局迁移到双槽
  布局本身是高风险操作，不能把普通 app OTA 当成自动安全迁移分区表的手段。

### P4（最后）：Bootloader OTA

普通 application OTA 不会改写 second-stage bootloader。ESP-IDF 5.5.4 提供
bootloader OTA 相关的临时 `bootloader,ota` 分区和 `bootloader,recovery` 恢复
分区机制，但这不应成为第一版 OTA 的范围。

只有出现“新应用确实无法兼容已部署 bootloader”的明确需求时才评估 bootloader
OTA，并在实现前满足：

- ROM 能通过 eFuse 指定地址启动 recovery bootloader；
- recovery 分区、签名验证、secure boot/flash encryption 策略经过联合评审；
- 主 bootloader 写入中断、电源掉电、镜像损坏和回滚路径均有实板测试；
- 保留串口或物理恢复手段；
- 先在非生产设备分批验证，禁止无恢复能力的全量推送。

Bootloader 位于应用启动之前，写坏后新旧两个 app slot 都可能无法启动；相比应用
OTA 自动回滚，其失败半径更大。因此它保持最低优先级，不与当前网页或下一次
模型训练绑定实施。

## 参考资料

- [ESP-IDF 5.5.4 OTA API](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/ota.html)
- [ESP-IDF 5.5.4 Partition Tables](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/partition-tables.html)
- [ESP-IDF Bootloader Compatibility](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/bootloader.html)
- [ESP-NN issue #7: aligned scratch allocation](https://github.com/espressif/esp-nn/issues/7)
