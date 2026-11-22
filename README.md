# NekoWaShock

**YOU WA SHOCK 的 64 位版本** —— 一个原生 x64 VST2 插件。

![界面截图](docs/screenshot.png)

## 功能

| 参数 | 范围 | 默认 | 说明 |
|---|---|---|---|
| Volume | -2 ~ +2 | 1.0 | 总音量，1.0 = 0 dB |
| Panning | -1 ~ +1 | 0 | 平衡式声像，居中不衰减 |
| Sep | -1 ~ +1 | 0 | 立体声分离：+1 加宽，-1 收成单声道 |
| LVolume | -2 ~ +2 | 1.0 | 左声道音量 |
| RVolume | -2 ~ +2 | 1.0 | 右声道音量 |
| CircPan | On / Off | Off | 圆形（等功率）声像律 |
| Satur | A / B / C / D | A | 饱和模式 |

### 饱和模式

| 档位 | 行为 |
|---|---|
| **A** Default | 直通 |
| **B** No saturation | 直通 |
| **C** Sat A | 软饱和（drive 1.5×） |
| **D** Sat B | 强饱和（drive 3.0×） |

C / D 会给信号加入谐波，让声音更厚。

## 安装

1. 从 [Releases](../../releases) 下载 `NekoWaShock.dll`
2. 放进你的 64 位 VST2 插件目录
3. 让宿主（Vegas Pro / REAPER / FL Studio 等）扫描

插件窗口内的旋钮可上下拖动改值，`Circular panning law` 与 `A B C D` 为点击切换。

## 构建

需要 CMake 3.20+ 与 Visual Studio（MSVC，x64）。

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物：`build/Release/NekoWaShock.dll`

## 实现说明

- 参数实时生效，随宿主工程保存与自动化
- 界面为深色背景 + 自绘圆形旋钮
- 饱和采用 tanh 的 Padé 近似软削波，并做归一化处理

## 许可

[MIT](LICENSE) © MCNeko-Forum
