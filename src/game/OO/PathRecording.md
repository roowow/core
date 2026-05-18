# Bot 路径录制工具使用说明

## 功能概述

GM 在游戏中走一遍路线，服务端自动采样坐标，停止后生成可直接使用的 C++ 代码文件，
粘贴到 `BattleBotWaypoints2.cpp` 即可为机器人添加新路径。

---

## GM 命令

| 命令 | 说明 |
|---|---|
| `.battlebot path start <名称>` | 开始录制，指定路径变量名（不含空格） |
| `.battlebot path stop` | 停止录制并保存文件 |
| `.battlebot path status` | 查看当前已录制的点数 |

**权限要求**：SEC_ADMINISTRATOR

---

## 使用步骤

### 1. 进入游戏，以 GM 身份前往路径起点

```
.gm on
.go 坐标...
```

### 2. 开始录制

```
.battlebot path start AV_Horde_to_Irondeep_Mine
```

路径名称规则：
- 只用字母、数字、下划线
- 空格会被自动替换为下划线
- 名称将成为 C++ 变量名：`vPath_AV_Horde_to_Irondeep_Mine`

### 3. 沿路线走到终点

- 服务端每 **200ms** 检测一次位置
- 移动超过 **5 码** 才记录新点，自动过滤原地抖动
- 可随时用 `.battlebot path status` 查看已录制点数
- 可骑乘行走，速度不影响结果

### 4. 停止录制

到达终点后：
```
.battlebot path stop
```

系统输出类似：
```
Saved 42 points -> ./logs/botpath_AV_Horde_to_Irondeep_Mine_20240101_120000.cpp
```

---

## 输出文件格式

文件保存在服务端 `./logs/` 目录，内容示例：

```cpp
// Recorded: 20240101_120000  Points: 42
BattleBotPath vPath_AV_Horde_to_Irondeep_Mine =
{
    { -860.9274f, -545.2820f, 57.2399f, nullptr },
    { -845.1230f, -531.4560f, 58.1100f, nullptr },
    { -831.6780f, -518.3310f, 59.4420f, nullptr },
    ...
};
```

---

## 将路径加入代码

### 步骤一：复制到 BattleBotWaypoints2.cpp

在文件中找到同类路径（如其他 AV 路径）的位置，粘贴新路径定义。

### 步骤二：声明外部引用（如需跨文件使用）

在 `BattleBotWaypoints.h` 或对应头文件中添加：

```cpp
extern BattleBotPath vPath_AV_Horde_to_Irondeep_Mine;
```

### 步骤三：加入路径网络

在 `BattleBotWaypoints.cpp` 的 `vPaths_AV` 向量中追加：

```cpp
std::vector<BattleBotPath*> const vPaths_AV =
{
    // ... 现有路径 ...
    &vPath_AV_Horde_to_Irondeep_Mine,   // 新增
};
```

### 步骤四：在目标选择逻辑中使用

在 `StartNewPathToObjective` 的 AV 分支中，调用：

```cpp
return StartNewPathToPosition(minePos, vPaths_AV);
```

---

## 注意事项

- **路径方向**：录制的路径是单向的（从起点到终点）。机器人系统支持反向遍历，但建议两个方向分别录制以提高精度。
- **室内路径**：矿洞等室内区域 navmesh 覆盖可能不完整，需要更密集的路径点（缩短采样间距可手动在代码里补点）。
- **点位检查**：生成文件后建议用 `.battlebot showpath` 命令可视化验证路径是否合理。
- **同时只能录制一条**：每个 GM 账号同时只能有一个活跃录制会话，开始新录制前必须先停止当前录制。
- **日志目录**：确保服务端 `./logs/` 目录存在且有写入权限。

---

## 示例：录制 AV 部落到铁石矿路径

```
# 传送到部落出生点附近
.go -870 -558 57 30

# 开始录制
.battlebot path start AV_Horde_Cave_to_Irondeep_Mine

# 骑马走到铁石矿（北矿）入口，再走到矿区 Boss 附近
# ...

# 查看进度
.battlebot path status
# > Recording in progress: 28 points captured so far.

# 到达目的地后停止
.battlebot path stop
# > Saved 35 points -> ./logs/botpath_AV_Horde_Cave_to_Irondeep_Mine_20240101_120000.cpp
```
