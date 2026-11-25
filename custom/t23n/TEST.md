# T23N 设备测试文档（yolo26 @ Ingenic MIPS）

> 更新日期：2026-11-25。涵盖设备信息、连接方式、文件传输、部署位置、编译、测试步骤与参考结果。
> **本目录（`ncnn/custom/t23n/`）存放设备相关工具与文档**（构建/推板/跑板/垫片/二进制存档）；
> 模型与测试图在上一级 `custom/models`、`custom/assets`。不依赖 /tmp。

## 0. 本目录内容

| 文件 | 用途 |
|---|---|
| `TEST.md` | 本文档 |
| `telnet_run.py` | 设备命令执行工具（自动登录 + 等结束 + 返回码） |
| `push_to_device.sh` | PC→设备文件推送（nc 拉取 + md5 校验，依赖同目录 telnet_run.py） |
| `build_trim_mips.sh` | 设备端构建脚本（默认在 `/home/wushiwei/work2/ncnn` 主仓库内构建，可用 `NCNN_SRC=` 覆盖） |
| `math_stub.c` | uclibc 兼容垫片：补 `fesetround` / `fegetround` / `nearbyintf`，只在设备端链接时用，不属于 ncnn 源码 |
| `artifacts/` | 设备二进制存档（`yolo26_c7` 同源），不参与构建 |

模型和测试图不在本目录，而在上一级：`custom/models/`（5 个最终模型）和
`custom/assets/`（测试图母本）。它们与具体板子无关，换设备时只需换 `custom/<board>/`。

## 1. 设备信息

| 项目 | 值 |
|---|---|
| 设备 IP | `192.168.66.56`（开发机侧网卡为 `192.168.66.1`，同网段直连） |
| 登录方式 | **telnet**，用户 `root`，**无密码**（端口 23，无 ssh） |
| SoC / 架构 | Ingenic MIPS（mips-linux-gnu，交叉工具链 mips-gcc540） |
| libc | uclibc（注意：无 `std::strtof` 等新 C++ 符号，工具链 `/opt/mips-gcc540-glibc222-64bit-r3.3.0.smaller`） |
| 内存 | 约 40MB RAM（模型必须小，程序用 ncnn C API + SIMPLEOCV 控制体积） |
| 可写存储 | `/mnt/sdcard/`（测试目录建在这里） |
| 工具链限制 | 自带 cmake 不支持 `-B`，用 `/usr/bin/cmake` |

## 2. 连接设备

手工：

```sh
telnet 192.168.66.56        # login: root，密码直接回车
```

自动化脚本（推荐，自动登录 + 等命令结束 + 返回码）：

```sh
T=/home/wushiwei/work2/ncnn/custom/t23n          # 本目录，下同
D=/home/wushiwei/work2/ncnn/custom               # 模型与测试图在 $D/models、$D/assets
python3 $T/telnet_run.py [-t 秒数] 'shell命令'
# 例：
python3 $T/telnet_run.py -t 60 'cd /mnt/sdcard/trim_test; ls'
```

注意事项：
- **telnet 退出会杀掉它启动的子进程**：长任务要么前台跑完，要么另想会话保活。
- yolo26 程序的**检测框打印在 stdout，计时统计打印在 stderr**。只想要检测框：`2>/dev/null`；只想要计时：`2>&1 >/dev/null`。
- 脚本输出里混有回显的命令文本，用 `grep` 过滤 `class=` / `Average` 等关键字即可。

## 3. 文件传输（PC → 设备）

设备无 scp/tftp 可用，采用 **nc 拉取模式**：PC 端 `nc -l` 监听并发送文件，设备通过 telnet 会话执行 `nc 192.168.66.1 <port> > 目标文件` 拉取。已封装好脚本（含 md5 校验，源文件不存在/为空会直接中止）：

```sh
$T/push_to_device.sh <本地文件> [设备上的文件名]
# 例：
$T/push_to_device.sh /home/wushiwei/work2/ncnn/build-mips-trim-fast/yolo26 yolo26_c7
# 成功输出: PUSHED OK: yolo26_c7
```

默认推到设备 `/mnt/sdcard/trim_test/`。

## 4. 目录布局

### 设备端 `/mnt/sdcard/trim_test/`

只保留程序、测试图和几个历史测速基线；**最终 5 个模型不常驻设备**，测完即删（避免 sdcard 堆积）。

| 文件 | 说明 |
|---|---|
| `yolo26_c7` | 当前推荐二进制：带 `NCNN_WINOGRAD/SGEMM/BF16/PACKING/THREADS` 环境变量开关 |
| `dog_cat_640x480.png` | 测试图（640×480，左猫右狗，由 `$D/assets/` 推送） |
| `f_1x1` / `f_fs` / `f_p3w40` | 上一轮的历史测速基线（nano-1x1 / faststem / p3wide40-faststem） |
| `fd` | 上一轮 fp16/fp32 对照模型 |

> 注：`yolo26_c4`/`c6` 已删。c4 不编译 ReLU 层，加载 LeakyReLU 融合模型会 segfault，勿用。

### PC 端（`/home/wushiwei/work2/` 下）

| 路径 | 说明 |
|---|---|
| `ncnn/custom/t23n/` | **设备工具目录（本目录）**：文档、脚本、二进制存档（模型/图片在 `custom/` 下） |
| `ncnn/` 本身 | 主 clone（上游 ncnn @ tag `20261020`）。**构建和测试都在这里**；裁剪不改源码，只靠 `custom/t23n/build_trim_mips.sh` 的 `WITH_LAYER_*=OFF` + 编译选项 |
| `ncnn/examples/yolo26.cpp` | 测试程序源码（唯一真源） |
| `ncnn/build-mips-trim-fast/yolo26` | 构建产物（MIPS，约 969KB，`build*/` 已 gitignore） |
| `ncnn/custom/t23n/artifacts/yolo26_c7_O2_20261123` | 设备上 `yolo26_c7` 的二进制存档（与重新构建结果 md5 相同） |
| `ncnn/custom/models/yolo26-lite-nano-mix*/` | 最终部署模型（5 个，`model.ncnn.param/bin` + `metadata.yaml`） |
| `ncnn/custom/assets/dog_cat_640x480.png` | 测试图**母本**（设备用同 md5 副本；ultralytics 内另有一份供容器读，三处 md5 一致） |
| `ncnn/custom/t23n/artifacts/` | 已部署 MIPS 二进制存档 |
| `ultralytics/` | ultralytics 仓库；容器 `train-ultralytics` 只挂载了此目录和 `~/datasets/coco` |

## 5. 编译（设备版）

```sh
cd /home/wushiwei/work2/ncnn
./custom/t23n/build_trim_mips.sh              # 默认 OPT=-O2，产物 build-mips-trim-fast/yolo26 (~969KB)
OPT=-Os ./custom/t23n/build_trim_mips.sh      # 极致压体积 ~842KB（慢约 30%）
NCNN_SRC=/other/ncnn/tree ./custom/t23n/build_trim_mips.sh   # 指定别的源码树
```

> 早期曾用一个独立的 `git worktree`（`ncnn-trim-yolo26`）做构建，但实测它的源码与主仓库
> 完全一致（唯一差异是构建产物），因此已删除；现在主仓库就是唯一源码真源，`git worktree list`
> 只剩 `ncnn` 本身。

构建脚本已内置自检并规避了 5 个坑：
1. **必须显式加 `-ffast-math`**：ncnn 只在 Release 加 `-Ofast`，MinSizeRel 不加会慢 25-30%；但 fast-math 要注入到 `-DCMAKE_<LANG>_FLAGS_MINSIZEREL`，因为工具链 file 里的普通 `set(CMAKE_C_FLAGS)` 会静默吞掉 `-DCMAKE_C_FLAGS`。
2. **源码树根目录不能有残留 `CMakeCache.txt`**（in-source cache 会污染 out-of-source 配置，脚本会自动拦截）。
3. 用 `/usr/bin/cmake`（工具链自带的 cmake 太老不支持 `-B`）。
4. 裁剪层开关是 `WITH_LAYER_xxx=OFF`（小写层名）。
5. **`ReLU` 必须保留在 KEEP 里**（2026-11-23 修复）：ncnn 把 LeakyReLU 融合成
   `activation_type=2`（param `-23310=1,0.01 9=2`），加载时 `create_activation_layer()`
   会 `create_layer_cpu(LayerType::ReLU)`。ReLU 被裁掉时返回 NULL，`load_model` 直接
   **segfault**（旧 yolo26_c4 就是这个问题，模型本身没毛病）。
   注意脚本里有**两处**硬编码的层名单：`KEEP=` 变量和 `grep -vwE '...'` 正则，两处都要加。

> `math_stub.c` 的作用：Ingenic 的 uclibc 里没有 `fesetround/fegetround/nearbyintf`，
> 而 ncnn 的 CPU 层（`src/layer/*`、`src/mat.cpp`）会调用它们；不补这三个符号，链接阶段
> 就会报 undefined reference。它是部署胶水，所以放在本目录而不是 ncnn 源码树里。

改 `examples/yolo26.cpp` 后的完整流程：

```sh
cd /home/wushiwei/work2/ncnn
./custom/t23n/build_trim_mips.sh                                  # 交叉编译（~1-2 分钟）
cp build-mips-trim-fast/yolo26 custom/t23n/artifacts/yolo26_c7_O2_$(date +%Y%m%d)   # 可选：存档
./custom/t23n/push_to_device.sh build-mips-trim-fast/yolo26 yolo26_c7
```

## 6. 设备端运行与测试

### 6.1 功能测试（打印检测框）

```sh
# 先把 PC 上的模型推上去（设备不常驻最终模型）
$T/push_to_device.sh $D/models/yolo26-lite-nano-mix/model.ncnn.param mix.param
$T/push_to_device.sh $D/models/yolo26-lite-nano-mix/model.ncnn.bin   mix.bin
python3 $T/telnet_run.py -t 300 'cd /mnt/sdcard/trim_test; \
  ./yolo26_c7 dog_cat_640x480.png mix.param mix.bin 2>/dev/null'
# 用法: ./yolo26_c7 <图> <param> <bin> [conf=0.25]
```

### 6.2 性能测试（基准方法）

程序内置 20 次循环，**第 1 次为预热不计入统计**（输出中已标注）。只提取计时：

```sh
python3 $T/telnet_run.py -t 900 'cd /mnt/sdcard/trim_test; \
  for m in "mix.param mix.bin" "p4w40.param p4w40.bin"; do echo "== $m"; \
  ./yolo26_c7 dog_cat_640x480.png $m 2>&1 >/dev/null | \
  sed -n "/Average/p;/Min time/p;/Max time/p"; done'
```

判定标准：**Average 与 Min 差值应在 1% 内**，否则设备被其他进程干扰，需要重测。

最终 5 个模型的设备耗时（完整程序：读图→预处理→推理→NMS，20 次平均）：

| 模型 | 耗时 |
|---|---|
| `yolo26-lite-nano-mix-slim` | 150.8ms |
| **`yolo26-lite-nano-mix`（推荐）** | **165.0ms** |
| `yolo26-lite-nano-mix-p2deep` | 171.9ms |
| `yolo26-lite-nano-mix-p4wide40-neck2` | 196.0ms |
| `yolo26-lite-nano-mix-p4wide40` | 223.2ms |

历史对照：`f_fs`（上一轮推荐）= 218ms；`lite_xs`（SiLU，配置已删）= 644ms。
同 MACs 下 SiLU→LeakyReLU 差 2 倍以上，这是 MIPS 上最大的单点收益。

实测成本模型：`t ≈ 71 ms + 7.6 ms × MACs(M)`，导出后可以先估耗时再上机。

> 参数对精度/耗时的影响、5 个最终模型与两种子精度见
> `/home/wushiwei/work2/ultralytics/custom/docs/MODEL_COMPARISON.md`（不在本文重复，避免两处过期）。

### 6.3 运行时开关

`yolo26_c7` 支持用环境变量做 A/B（默认全关）：

```sh
NCNN_WINOGRAD=1 NCNN_SGEMM=1 NCNN_BF16=1 NCNN_PACKING=1 NCNN_THREADS=1 \
  ./yolo26_c7 dog_cat_640x480.png f_fs.param f_fs.bin
```

已实测排除，不要重复尝试：

| 开关 | 结果 |
|---|---|
| `NCNN_WINOGRAD=1` | -0.6%，忽略不计 |
| `NCNN_SGEMM=1` | 0% |
| `NCNN_BF16=1` | 慢 2.5× |
| `NCNN_PACKING=1` | MSA=OFF 时无 SIMD 通道，无意义 |

MIPS 标量下只有两个有效手段：**可融合激活** 和 **降 MACs**。
`NCNN_THREADS=n` 单核无效，多核设备可直接开启。

## 7. 正确性参考结果（回归基线）

测试图 `dog_cat_640x480.png`，conf=0.25、IoU=0.7（与 ultralytics 默认一致）。**金标准 = ultralytics 自家 ncnn 后端跑同一 ncnn 模型**，我们的程序与其逐值一致（框差 ≤1px）。

### 最终 5 个模型（PyTorch → ncnn 一致性）

全部 5 个模型（同一份 `model.ncnn.param/bin`）：**类别与分数完全一致，坐标最大偏差 1px**
（letterbox 边界取整，无可感知差异）。验证结果不存档，需要时按 §8 重跑
（设备端 `yolo26_c7` + 容器内 `custom/tools/ref_infer.py`，单模型约 1 分钟）。

| 模型 | 框数（app / ref） | 最大坐标偏差 |
|---|---|---|
| nano-mix-slim | 2 / 2 | 1.00px |
| nano-mix | 1 / 1 | 0.43px |
| nano-mix-p2deep | 2 / 2 | 0.46px |
| nano-mix-p4wide40-neck2 | 3 / 3 | 1.00px |
| nano-mix-p4wide40 | 2 / 2 | 1.00px |

例（nano-mix，推荐档）：

```
# app（设备端 yolo26_c7）
class=1 score=0.5566 box=208.1,131.0,637.1,479.0
# ref（ultralytics ncnn 后端）
class=1 score=0.5566 box=208.14,130.98,637.13,479.44
```

## 8. ultralytics 参考推理（容器）

参考推理脚本只有一份，在 ultralytics 仓库：
`/home/wushiwei/work2/ultralytics/custom/tools/ref_infer.py`（容器已挂载，无需复制）。

```sh
# conf=0.25，输出 boxes.txt + 画框 png 到 ultralytics/custom/verify/（目录自动创建）
docker exec train-ultralytics python /home/wushiwei/work2/ultralytics/custom/tools/ref_infer.py \
    runs/detect/yolo26-lite-nano-mix-catdog/weights/best_ncnn_model yolo26-lite-nano-mix
```

要点：
- 模型在容器内路径 `runs/detect/yolo26-lite-nano-mix*-catdog/weights/best_ncnn_model`（ultralytics 目录已挂载）。
- 传目录时 ultralytics 猜不出 task，`YOLO()` 会报 `not a supported model format`；先 `cp -r` 成 `*_ncnn_model` 目录再传，或显式传目录名。
- **容器看不到 ncnn 目录**，测试图用挂载内的 `/home/wushiwei/work2/ultralytics/custom/assets/dog_cat_640x480.png`（母本在 `custom/assets/`，两份 md5 必须一致）。
- ncnn 导出被 ultralytics **强制 end2end=False**（exporter.py:667-669），输出是逐网格点原始 `[6 × 540]`，host 侧 NMS 是正常做法；输出行 0-3 为 letterbox 像素下的 **cx,cy,w,h**（不是 xyxy，历史上踩过这个坑）。
- `best.pt`（PyTorch）与 ncnn 导出分数有固有漂移（如 dog 头名 0.4835 vs 0.6146），是 pnnx 导出精度问题，不是 C++ 侧问题——对比时以 **ncnn 后端参考**为准。

## 9. 踩坑备忘

| 坑 | 症状 | 对策 |
|---|---|---|
| MinSizeRel 无 fast-math | 设备慢 25-30% | 构建脚本已注入 `-ffast-math` 到 `*_FLAGS_MINSIZEREL` |
| in-source CMakeCache.txt | 编译参数诡异不生效 | 删掉源码树根的 CMakeCache.txt（脚本已自动拦截） |
| uclibc 缺 `std::strtof` | 设备编译报错 | 用全局 `strtof(argv[4], NULL)` |
| telnet 断开杀子进程 | 长任务中途消失 | 前台跑完或分段执行 |
| 检测框在 stdout、计时在 stderr | 结果/计时互相"丢失" | `2>/dev/null` 或 `2>&1 >/dev/null` 分开取 |
| 推送不存在的文件 | 假 "PUSHED OK" | push 脚本已加 `[ -s ]` 拦截；仍异常看 md5 输出 |
| 画框图对比 | 误差看不出来 | **对比 stdout 的 bbox 数值**，不看图 |
| out_sum 类工具在设备上极慢 | 单次前向 >2300s 未结束 | 未查明；数值校验改在 PC native 做 |
| 脚本放 /tmp 丢失 | 工具突然没了 | 全部收进 `ncnn/custom/t23n/`，别再依赖 /tmp |

## 10. 相关文件速查

```
PC:
  /home/wushiwei/work2/ncnn/custom/t23n/                            测试资产（本文档 + 全部脚本 + 二进制存档）
  /home/wushiwei/work2/ncnn/                                        主 clone = 源码真源（构建/测试都在这里）
  /home/wushiwei/work2/ncnn/examples/yolo26.cpp                     测试程序源码
  /home/wushiwei/work2/ncnn/custom/models/                          5 个最终 ncnn 模型
  /home/wushiwei/work2/ncnn/custom/assets/                          测试图母本
  /home/wushiwei/work2/ultralytics/custom/                          本项目文件（docs/tools/experiments）
  /home/wushiwei/work2/ultralytics/custom/docs/MODEL_COMPARISON.md  选型总表
设备(192.168.66.56):
  /mnt/sdcard/trim_test/                          测试目录（程序 + 模型 + 测试图）
容器 train-ultralytics:
  /home/wushiwei/work2/ultralytics/               挂载（模型、参考脚本、测试图）
```
