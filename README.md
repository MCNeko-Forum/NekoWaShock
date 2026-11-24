# NekoWaShock

**YOU WA SHOCK 的 64 位版本** —— 一个原生 x64 VST2 插件。

![界面截图](docs/screenshot.png)

## 功能

| 参数 | 范围 | 默认 | 说明 |
|---|---|---|---|
| Drive | 0 ~ 100% | 0% | 界面大旋钮：饱和驱动量，0% = 直通 |
| SatMode | A / B / C / D | A | 界面右侧四档饱和模式 |
| Volume | -2 ~ +2 | 1.0 | 总音量，1.0 = 0 dB |
| Pan | -1 ~ +1 | 0 | 平衡式声像，居中不衰减 |
| Separation | -1 ~ +1 | 0 | 立体声分离：+1 加宽，-1 收成单声道 |
| LeftVol / RightVol | -2 ~ +2 | 1.0 | 左 / 右声道音量 |
| CircPan | On / Off | Off | 圆形（等功率）声像律 |

### 饱和模式

界面上只有一个大旋钮和 A B C D 四档：旋钮决定驱动量，字母决定过载特性。

| 档位 | 驱动上限 | 行为 |
|---|---|---|
| **A** 温和 | 3× | 轻度过载，适合给总线加一点厚度 |
| **B** 直通 | — | 不改变声音，方便对比 |
| **C** 中等 | 6× | 明显的谐波与压缩 |
| **D** 强烈 | 12× | 重度过载，接近失真 |

小信号增益始终为 1（输出补偿 = 1/驱动），只有大信号被软削波压缩，
所以听起来是「变厚、变脏」，而不是单纯「音量变大」。

## 安装

1. 从 [Releases](../../releases) 下载 `NekoWaShock.dll`
2. 放进你的 64 位 VST2 插件目录
3. 让宿主（Vegas Pro / REAPER / FL Studio 等）扫描

插件窗口中央的大旋钮上下拖动调整驱动量，右侧 `A B C D` 点击切换饱和档位。
其余参数不出现在界面上，可通过宿主的参数列表或自动化控制。

## 构建

需要 CMake 3.20+ 与 Visual Studio（MSVC，x64）。

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物：`build/Release/NekoWaShock.dll`

## 实现说明

- 参数实时生效，随宿主工程保存与自动化
- 界面为深色背景 + 自绘大旋钮（红色环形指示）、右侧 A/B/C/D 档位与 `[?]` 标记
- 饱和采用 tanh 的 Padé 近似软削波，输出补偿取 1/驱动，保证小信号增益为 1

## 许可

[MIT](LICENSE) © MCNeko-Forum
