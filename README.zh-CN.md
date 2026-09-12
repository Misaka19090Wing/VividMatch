# VividMatch

[English](README.md) | **简体中文**

> [!WARNING]
> **本项目使用了 DeepSeek AI 生成。**
>
> 此处的代码、测试与文档均在 DeepSeek AI 的辅助下完成。请把它当作机器辅助的产物：
> 在用于任何重要用途之前请自行审阅。下面记录了算法原理与实测数据，便于核对行为，而
> 不是只能选择相信。

VividMatch 用于判断不同分辨率下的图片或视频，画面内容是否相同。

## 算法

每张输入图片先归一化到 64x64 灰度画布，切成 4x4 共 16 个分块，每块用 2D DCT 哈希
（取低频系数得到 64 位）。比对两张图片时，剔除汉明距离最大的 4 个分块，对剩余 12 块
取平均，得到 `[0, 1]` 的相似度，`1` 表示画面一致。

这只是视觉层。整个设计分为三层，且三层刻意彼此独立，便于分别推理与单独测试：

| 层 | 做什么 | 为什么 |
| --- | --- | --- |
| 视觉层 | 逐帧分块 DCT 哈希，剔除最差分块 | 对分辨率、码率稳健，也能容忍画面局部的台标、水印或马赛克遮挡（抗马赛克） |
| 时序层 | 每秒一个指纹，匹配位置必须单调递增 | 识别混剪：拼接视频能匹配上单帧，但时间轴会来回跳 |
| 音频层 | 每秒的 RMS 能量、频谱质心与 16 个频带能量 | 与分辨率无关，也是画面被重度遮挡时的兜底依据 |

最终的判定把三层合起来看：画面匹配且音轨一致即为同一视频；画面匹配但音轨被替换属于
二次创作；画面匹配度低而音频匹配度高时仍判为同一视频，因为大概率是画面被重度遮挡
（即「音频一票否决」）。

## C++ 实现（OpenCV 5）

C++ 图片模块位于 `cpp/`：

```bash
cpp\build_msvc.bat
cpp\vividmatch_image.exe compare first.png second.png
cpp\vividmatch_image.exe hash image.png
cpp\vividmatch_image_test.exe
```

`build_msvc.bat` 的第一个参数是 OpenCV 根目录。默认阈值为 `0.78`。

### 视频比对

`cpp/video_fingerprint.hpp` 与 `cpp/audio_fingerprint.hpp` 实现了视觉、时序与音频三层。
视频被压缩为每秒一个视觉指纹（与图片同一套分块 DCT 哈希）；两段视频逐帧匹配；
在匹配结果上做时间轴单调性校验；音轨只在画面已对齐的那些秒上比对：

```bash
cpp\vividmatch_video.exe compare first.mp4 second.mp4
cpp\vividmatch_video.exe compare first.mp4 second.mp4 0.78 --no-audio
cpp\vividmatch_video.exe summary clip.mp4
cpp\vividmatch_video.exe batch <文件夹或视频> [更多...]
cpp\vividmatch_video_test.exe
cpp\vividmatch_video_batch_test.exe
```

### 批量视频比对

`cpp/video_batch.hpp` 对一整个文件夹的视频取指纹，找出哪些是同一个视频，并为每组挑出
一个保留项。

```bash
cpp\vividmatch_video.exe batch "D:\clips"
```

设计依据是实测的时间分布，而不是猜测：

- **提取占绝对多数。** 5 个 20 秒视频解码约 445 ms，而全部 10 对比较只要约 1.2 ms。
  因此每个视频只解码一次，跑在线程池上。
- **两两比较仍随规模二次增长**，视频数量与时长都是平方项（每秒采样一次，1 小时视频约
  3600 个采样点）。为此先比每视频 128 字节的签名 —— 各采样帧分块比特的多数表决 ——
  只有超过 `kDefaultSignatureThreshold` 的对才做完整比对。实测分离度：同内容
  0.97-1.00，不同内容 0.63-0.70，阈值落在空隙里，不会漏掉真匹配。
- **音频按需提取**，只为视觉上已匹配的对提取，因为它每个视频都要起一个 ffmpeg 进程。

`partial` 判定故意不并入分组：共享片头、从长视频里剪出的片段都不是同一个视频。而
`identical` 与 `reencoded` 都会并入，因为后者只是画面相同、音轨被换掉。

判定结论如下（仅限本实现确实能区分的几种情况）：

| 判定 | 含义 |
| --- | --- |
| `identical` | 较短视频被一整条单调链匹配，且音轨一致：内容相同，只是分辨率／编码／码率不同。画面匹配度低但音频匹配度高时也返回此项（画面被重度遮挡时的「音频一票否决」） |
| `reencoded` | 画面匹配但音轨不一致：同一个画面，BGM 被替换 |
| `montage` | 存在匹配，但无法全部落在一条单调链上（混剪拼接或倒放） |
| `partial` | 匹配到的部分是有序的，但不足以覆盖较短的视频 |
| `different` | 没有任何采样帧匹配上 |

`compare` 只有在 `identical` 时才返回退出码 `0`，因此可以直接用于脚本。逐帧阈值
（默认 `0.78`）可作第三个参数传入，`--no-audio` 跳过音频层。抽帧采用顺序读取而非
seek，因此同一个视频的不同版本即使帧率或分辨率不同也能对上。

`cpp/parallel_extract.hpp` 让两个指纹计算阶段同时进行：音频阶段与视频解码重叠，两段
视频各自在独立线程解码。在 3 分钟 720p 的一对素材上实测端到端提速 1.4 倍，且指纹逐
字节一致、判定结果不变。硬件解码经过实测后有意未采用 —— 这个 OpenCV 构建无法通过
Media Foundation 或 DirectShow 打开 MP4，而向 FFMPEG 后端申请硬件加速会让读取**比
软件解码更慢**。

音频通过命令行 `ffmpeg`（在 `PATH` 中查找）解码，因为本项目所用的 OpenCV 构建没有
暴露音频解码接口。STFT 用的是 `audio_fingerprint.hpp` 里手写的短基-2 FFT 而非 FFTW，
这样 OpenCV 仍是唯一的库依赖。

### 音频层「未参与」时

命令行（`audio_skipped=`）与界面（`音频：未参与（...）`）都会写明原因，便于判断是否
需要处理：

| 提示 | 原因 | 怎么办 |
| --- | --- | --- |
| `this file has no audio track` | 其中一段视频没有声音（录屏、静音导出、类 GIF 素材） | 无需处理 —— 判定退回画面与时序 |
| `ffmpeg not found on PATH` | 未安装 `ffmpeg.exe`，或不在 `PATH` 中 | 安装 ffmpeg（例如 `winget install Gyan.FFmpeg`），重开程序让它读到新的 `PATH`，或把完整路径传给 `fingerprintAudio` / `compareVideoFiles(..., ffmpegPath)` |
| `the file could not be read` | 文件被截断，或容器不受支持 | 重新导出，或用播放器确认能否播放 |
| `ffmpeg was denied access` | 杀毒软件或目录权限拦住了 ffmpeg 读取该文件或临时目录 | 在杀毒软件中放行 ffmpeg，或修正临时目录权限 |
| `no aligned frames to compare audio on` | 画面从未匹配上，因此没有时间对应关系 | 无需处理 —— 音频只在画面已对齐的秒上比对 |

排查时不需要知道是哪一段视频：音频层是按文件分别报告的，一对里只要有一段没有音轨就
会触发第一行。

## Qt 界面（VividMatchGui）

Qt 6 界面基于 `XMuli/myapp-template` 模板，从功能选择页进入不同的比对模式，共四种：

| 模式 | 作用 |
| --- | --- |
| 图片比对 | 选择两张图片，得到相似度结果 |
| 批量图片比对 | 支持文件夹、多选或拖拽；结果按相似度分到可折叠的「资源管理器」式分组，每个重复组只保留勾选的最佳一张 |
| 视频比对 | 选择两段视频，比对画面、时序与声音 |
| 批量视频比对 | 添加一整个文件夹的视频，自动分组相同视频，并为每组挑出保留项 |

四种模式同时也列在「文件」菜单里。

### 界面语言

界面为双语：**英文**与**简体中文**。英文是原文语言，因此代码里的字符串是英文，中文
放在 `i18n/vividmatch_zh_CN.ts`，编译为 `vividmatch_zh_CN.qm`。这个顺序是刻意的：
一旦 `.qm` 缺失或加载失败，程序会留在英文，而不是退回读者可能看不懂的中文。

首次运行跟随操作系统语言（`QLocale::system()`）：`zh*` 系统显示中文，其它显示英文。
「文件 → 语言 / File → Language」可改为「跟随系统」「English」或「简体中文」，选择会
记录在 `QSettings` 中。切换立即生效，无需重启：Qt 会发送 `LanguageChange` 事件，各页面
据此重建自己的文案，包括结果面板 —— 那里的数字是重新渲染而不是重新翻译。

```bat
gui\build_gui.bat
```

在正常构建中一并生成翻译。它需要 Qt Linguist 工具里的 `lrelease`；没有时会给出警告并
产出仅英文的程序，功能完全可用。

两个脚本用于保证翻译不跑偏（位于 `tests/i18n/`）：

| 命令 | 作用 |
| --- | --- |
| `python tests/i18n/extract_strings.py check` | 每条 `tr()` 都有翻译，且没有失效的映射项 |
| `python tests/i18n/build_translations.py` | 由 `tests/i18n/zh_CN.json` 生成 `i18n/*.ts` 并编译 `.qm` |

这里刻意不使用 `lupdate`：本机可用的 6.11 版本对每个 `.cpp` 都报「has no recognized
extension」，即使加上 `-extensions cpp` 也一样，根本无法生成文件。抽取脚本会像 C++ 与
Qt 那样拼接相邻的字符串字面量 —— 这一点很重要，因为有几处 `tr()` 是跨行的。

```bat
gui\build_gui.bat
gui\run_gui.bat
```

Qt/OpenCV 路径与 VS Code Qt 扩展的配置见 `gui/README.zh-CN.md`
（[English](gui/README.md)）。

如果要把界面给没有安装 Qt/OpenCV 的电脑使用，先在开发机上打一次便携包：

```bat
gui\package_gui.bat
```

便携版输出到 `dist\VividMatchGui\`，无需安装 Qt 或 OpenCV 即可直接运行。

## 样例图片

`samples/` 里是用于手动验证的生成图片：

- `photo_a_800x600.png`、`photo_a_400x300.png`、`photo_tall_300x600.png` 是同一张图
  的不同尺寸／宽高比，必须落在同一个重复组（彼此相似度 100%）。
- `photo_b_640x480.png` 是另一张图，必须落在无重复的一组（与其它图约 57-61%）。

重新生成：

```bash
python tests/fixtures/make_fixtures.py
```

`tests/test_samples.py` 会把同一套素材重新生成到临时目录并断言上述分组，因此样例不会
在无人察觉的情况下变得毫无意义。

## Python 参考实现

仓库里还保留了一份等价的 Python/OpenCV 实现，用于实验：

```bash
python -m vividmatch.cli compare first.png second.png
python -m vividmatch.cli search query.png candidates_dir/
python -m vividmatch.cli hash image.png
```
