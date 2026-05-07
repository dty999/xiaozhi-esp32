# 小智AI机器人 MCP 协议交互时序图

> MCP (Model Context Protocol) 基于 JSON-RPC 2.0，实现云端对设备的灵活控制

## MCP 协议全流程

```mermaid
sequenceDiagram
    participant SYS as ☁️ 云端大模型<br/>Qwen/DeepSeek
    participant SRV as 📡 服务器<br/>xiaozhi.me
    participant PROTO as 🔌 Protocol<br/>WebSocket/MQTT
    participant MCP as 🛠️ McpServer<br/>工具注册中心
    participant HW as 🔩 硬件执行<br/>LED/GPIO/舵机/音量

    Note over SYS,HW: ═══ 阶段一：初始化与工具发现 =========================

    MCP->>MCP: GetInstance() 单例
    MCP->>MCP: AddCommonTools()<br/>AddUserOnlyTools()<br/>注册所有工具到工具表

    Note over MCP: 已注册工具的示例：<br/>• self.volume.get<br/>• self.volume.set<br/>• self.get_battery_level<br/>• self.reboot<br/>• self.led.set_rgb

    PROTO->>SRV: 建立连接（已附带设备能力描述）

    Note over SYS,HW: ═══ 阶段二：用户请求触发 MCP 调用 =====================
    SYS->>SYS: 用户："小智，开灯"
    SYS->>SYS: LLM分析意图<br/>→ 需要调用 self.light.on
    SYS->>SRV: 发出 MCP 工具调用请求

    SRV->>PROTO: JSON: {"type":"mcp","payload":{...}}
    Note right of PROTO: 消息包裹在 type=mcp 中

    PROTO->>MCP: OnIncomingJson回调<br/>ParseMessage(payload)

    Note over MCP: ═══ 阶段三：JSON-RPC 2.0 协议交互 ========================

    MCP->>MCP: 解析 JSON-RPC 2.0 消息

    alt tools/list 请求
        Note right of MCP: 云端查询可用工具列表
        MCP->>MCP: 遍历已注册工具表
        MCP->>PROTO: 返回 JSON: {tools:[{name,description,parameters},...]}
        PROTO->>SRV: 工具列表摘要
        SRV->>SYS: 大模型得知可调用哪些工具
        Note over SYS: LLM 根据工具描述<br/>智能选择调用合适工具

    else tools/call 请求
        Note right of MCP: 云端调用具体工具
        MCP->>MCP: 查找工具名 → 找到回调函数
        MCP->>HW: 执行回调函数体<br/>（操作硬件）
        HW-->>MCP: 返回执行结果
        MCP->>PROTO: 返回 JSON: {result:true/false,...}
        PROTO->>SRV: 工具调用结果

    end

    Note over SYS,HW: ═══ 阶段四：具体工具调用示例 ==========================

    rect rgb(255, 240, 230)
        Note over SYS,HW: 示例1：用户说"把灯光设为蓝色"
        SYS->>SRV: MCP调用: self.led.set_rgb(0,0,255)
        SRV->>PROTO: JSON-RPC 2.0 下发
        PROTO->>MCP: ParseMessage()
        MCP->>HW: SetLedColor(0, 0, 255)
        HW-->>MCP: true (成功)
        MCP->>PROTO: {"jsonrpc":"2.0","result":true,"id":5}
        PROTO->>SRV: 结果返回
    end

    rect rgb(230, 245, 255)
        Note over SYS,HW: 示例2：用户说"音量调到50"
        SYS->>SRV: MCP调用: self.volume.set(50)
        SRV->>PROTO: JSON-RPC 2.0 下发
        PROTO->>MCP: ParseMessage()
        MCP->>HW: 设置Codec芯片音量 → 50%
        HW-->>MCP: true (成功)
        MCP->>PROTO: {"jsonrpc":"2.0","result":true,"id":6}
        PROTO->>SRV: 结果返回
    end
```

## MCP 工具注册示例（设备端代码）

```cpp
// 例1：无参数工具 —— 重启设备
mcp_server.AddTool(
    "self.reboot",               // 工具名
    "重启设备",                   // 描述（给大模型看的）
    PropertyList(),              // 无参数
    [this](const PropertyList&) -> ReturnValue {
        Reboot();
        return true;
    }
);

// 例2：带参数工具 —— 设置RGB灯光
mcp_server.AddTool(
    "self.led.set_rgb",
    "设置LED灯光颜色(rgb)",
    PropertyList({
        Property("r", kPropertyTypeInteger, 0, 255),  // 参数：r, 范围 0-255
        Property("g", kPropertyTypeInteger, 0, 255),  // 参数：g, 范围 0-255
        Property("b", kPropertyTypeInteger, 0, 255)   // 参数：b, 范围 0-255
    }),
    [this](const PropertyList& properties) -> ReturnValue {
        int r = properties["r"].value<int>();
        int g = properties["g"].value<int>();
        int b = properties["b"].value<int>();
        SetLedColor(r, g, b);
        return true;
    }
);
```

## JSON-RPC 2.0 消息格式

| 类型 | JSON 格式 | 说明 |
|------|-----------|------|
| **tools/list** | `{"jsonrpc":"2.0","method":"tools/list","id":1}` | 云端查询设备能力 |
| **tools/call** | `{"jsonrpc":"2.0","method":"tools/call","params":{"name":"self.led.set_rgb","arguments":{"r":0,"g":0,"b":255}},"id":2}` | 云端调用设备工具 |
| **返回结果** | `{"jsonrpc":"2.0","result":true,"id":2}` | 设备返回执行结果 |
| **返回错误** | `{"jsonrpc":"2.0","error":{"code":-32601,"message":"Tool not found"},"id":3}` | 工具不存在 |

## 消息传输路径

```
云端服务器                   设备端
┌──────────┐              ┌─────────────────┐
│ JSON-RPC │              │  type: "mcp"    │
│  2.0     │ ──────────→ │  payload: {...} │
│ 消息     │ ←────────── │                 │
└──────────┘   WebSocket  │  McpServer      │
              或 MQTT     │  ParseMessage() │
                          └─────────────────┘
```
