// ============================================================
// application.h —— 小智机器人应用主类（总管）
// ============================================================
//
// Application 为全局唯一的应用总管，单例模式。
// 掌理设备生命周期之全过程：初始化各模块 → 运行主事件循环 → 处理网络消息。
// 本文件仅为声明界面；实现体在 application.cc 中。
// ============================================================

// 头文件保护宏 —— 防止重复包含
#ifndef _APPLICATION_H_
#define _APPLICATION_H_

// ── ESP-IDF 框架头文件（尖括号 <> ── 乐鑫官方提供）────────────────

#include <freertos/FreeRTOS.h>       // FreeRTOS 实时操作系统内核入口头
#include <freertos/event_groups.h>   // 事件组 API —— 多事件等待与通知机制
                                       //（本文件之核心：所有模块通过事件组松耦合通信）
#include <freertos/task.h>           // FreeRTOS 任务创建与优先级管理 API
#include <esp_timer.h>               // ESP32 硬件定时器 API

// ── C++ 标准库 ──────────────────────────────────────────────────

#include <string>                    // std::string 字符串
#include <mutex>                     // std::mutex 互斥锁 —— 保护共享资源
#include <deque>                     // std::deque 双端队列 —— 缓存待执行任务
#include <memory>                    // std::unique_ptr 独占指针，自动管理内存

// ── 项目自有头文件（引号 ""）────────────────────────────────────

#include "protocol.h"                // 通信协议抽象接口（WebSocket / MQTT）
#include "ota.h"                     // 空中升级（Over-The-Air）与激活管理
#include "audio_service.h"           // 音频服务总管（采集、编码、解码、唤醒）
#include "device_state.h"            // 设备状态枚举定义（Idle/Listening/Speaking...）
#include "device_state_machine.h"    // 设备状态机 —— 合法状态转移管理

// ============================================================
// 事件位定义 —— 事件总线之"信号线"
// ============================================================
// FreeRTOS 事件组以 32 位二进制掩码表示多种事件。
// 每一位代表一类事件，可同时等待多种事件，=1 表示"此事件已发生"。
// 主循环 Run() 中调用 xEventGroupWaitBits() 等待任一事件位被置位。
//
// 类比：此乃 Application 总管桌上的 13 个响铃——
//   哪个铃响，便处理哪个事件。

#define MAIN_EVENT_SCHEDULE             (1 << 0)   // [铃0] 有延迟任务需执行
                                                    //   Schedule() 方法投递到此
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)   // [铃1] 音频编码完毕，可发送至服务器
                                                    //   音频线程通知主线程取数据发包
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)   // [铃2] 检测到唤醒词（如"小智小智"）
                                                    //   即刻开始语音对话
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)   // [铃3] 说话/静音状态变化
                                                    //   VAD = Voice Activity Detection
#define MAIN_EVENT_ERROR                (1 << 4)   // [铃4] 发生错误，需弹警告
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)   // [铃5] 设备激活完成（联网+鉴权通过）
                                                    //   此后方可正常对话
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)   // [铃6] 秒级时钟脉冲（每秒一次）
                                                    //   用于刷新状态栏、打印内存统计
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)   // [铃7] 网络已连接
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)   // [铃8] 网络已断开
                                                    //   需关闭音频通道，回到待机状态
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)   // [铃9] 用户按下对话按钮
                                                    //   切换 待机↔对话 状态
#define MAIN_EVENT_START_LISTENING      (1 << 10)  // [铃10] 主动开始收音
                                                    //   可由 MCP 远程指令触发
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)  // [铃11] 主动停止收音
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)  // [铃12] 设备状态发生了变化
                                                    //   Idle→Listening→Speaking→Idle


// ============================================================
// AEC 模式枚举 —— 回声消除方案
// ============================================================
// AEC = Acoustic Echo Cancellation（回声消除）
// 小智播放语音时，麦克风会"听到"自己喇叭的声音，若不消除则服务器
// 收到回声会认为用户还在说话。此枚举决定回声由哪一侧负责消除。
enum AecMode {
    kAecOff,            // 不消除回声（单向对话模式，最省资源）
    kAecOnDeviceSide,   // 设备端消除（ESP32 芯片运算，延迟低但占用 CPU）
    kAecOnServerSide,   // 服务器端消除（云端运算，不占设备资源但延迟稍高）
};

// ============================================================
// Application 类 —— 全局唯一应用总管（单例）
// ============================================================
//
// 此类掌理整个项目的生命周期：
//   1. 初始化各硬件模块（屏幕、音频、LED、网络）
//   2. 注册 MCP 工具（设备端可被云端调用的功能）
//   3. 启动主事件循环，处理 13 种事件
//   4. 协调音频通道的开/关，管理设备状态转移
//
// 单例访问：Application::GetInstance()
// 主入口：  Initialize() → Run()（永不返回）
// ============================================================
class Application {
public:
    // ── 单例模式核心 ──────────────────────────────────────────────
    //
    // Meyer's Singleton 实现。C++11 保证：
    //   - 首次调用时构造（惰性初始化，节省内存）
    //   - 多线程同时首次调用也只构造一次（线程安全）
    //   - 返回引用，无拷贝开销
    //
    static Application& GetInstance() {
        static Application instance;       // 全局唯一实例，静态局部变量
        return instance;                   // 返回引用（非拷贝）
    }

    // ── 禁止拷贝与赋值 —— 杜绝"分身" ────────────────────────────
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // ============================================================
    // 公共接口 —— 外部可调用的方法
    // ============================================================

    /**
     * 初始化应用
     * 
     * 此方法完成以下工作（详见 application.cc）：
     *   1. 初始化显示屏 UI
     *   2. 初始化音频服务（Microphone + 编码 + 唤醒词检测）
     *   3. 注册音频回调（编码完毕通知、唤醒词检测通知、VAD变化通知）
     *   4. 注册 MCP 通用工具（get_battery_level 等）
     *   5. 设置网络事件回调（连接/断开通知）
     *   6. 启动时钟定时器（每秒刷新状态栏）
     *   7. 异步启动网络连接
     * 
     * @note 该方法立即返回，网络连接在后台异步进行
     */
    void Initialize();

    /**
     * 运行主事件循环（永不返回！）
     * 
     * 此乃设备启动后之永恒循环：
     *   while (true) {
     *       等待任何事件位被置位;
     *       按位处理事件（网络变化、唤醒词、状态转移、时钟脉冲...）;
     *   }
     * 
     * @note 该函数阻塞当前线程，永不返回。设备断电/复位方停止。
     */
    void Run();

    // ── 状态查询 ───────────────────────────────────────────────

    // 返回当前设备状态（Idle / Listening / Speaking / Connecting ...）
    DeviceState GetDeviceState() const { return state_machine_.GetState(); }

    // 当前是否检测到人声？（VAD 的结果）
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * 请求状态转移
     * @param state 目标状态
     * @return true 表示转移成功（状态机判定合法）
     *
     * 内部委托给 DeviceStateMachine，由其判定转移是否合法。
     * 例如：Idle→Listening 合法，Idle→Speaking 非法（须先 Listening）。
     */
    bool SetDeviceState(DeviceState state);

    // ── 任务调度 ───────────────────────────────────────────────

    /**
     * 将回调函数投递到主线程执行
     * @param callback 待执行的回调函数（右值引用，移动语义）
     * 
     * 用途：其他线程（音频线程、网络线程）需要操作 UI 或切换状态时，
     *       不能直接调用（线程不安全），须通过此方法投递到主事件循环。
     * 
     * 内部流程：加锁 → 推入队列 → 置位 MAIN_EVENT_SCHEDULE 铃响
     *           → Run() 收到铃响后取出回调执行
     */
    void Schedule(std::function<void()>&& callback);

    // ── UI 通知 ─────────────────────────────────────────────────

    /**
     * 弹出警告提示
     * @param status  状态文字（显示在状态栏）
     * @param message 消息正文（显示在聊天区域）
     * @param emotion 表情名称（可选，如 "triangle_exclamation"）
     * @param sound   提示音（可选，如 Lang::Sounds::OGG_EXCLAMATION）
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");

    // 关闭警告提示（设备处于 Idle 状态时恢复待机界面）
    void DismissAlert();

    // ── 对话控制 ───────────────────────────────────────────────

    // 中止当前说话（向服务器发送 Abort 指令）
    void AbortSpeaking(AbortReason reason);

    /**
     * 切换对话状态（线程安全）
     * 内部仅发送 MAIN_EVENT_TOGGLE_CHAT 信号，实际处理在 Run() 中。
     * 效果：待机→开始收音；收音/说话中→回到待机
     */
    void ToggleChatState();

    /**
     * 主动开始收音（线程安全）
     * 可由按键或 MCP 远程指令触发。
     * 内部仅发送 MAIN_EVENT_START_LISTENING 信号到主循环。
     */
    void StartListening();

    /**
     * 主动停止收音（线程安全）
     * 内部仅发送 MAIN_EVENT_STOP_LISTENING 信号到主循环。
     */
    void StopListening();

    // ── 系统操作 ───────────────────────────────────────────────

    // 软重启设备（调用 esp_restart()）
    void Reboot();

    // 手动触发唤醒词（由 MCP 或按键模拟唤醒词检测）
    void WakeWordInvoke(const std::string& wake_word);

    // 下载并升级固件（OTA），成功则重启设备
    // @param url     固件下载地址
    // @param version 版本号（可选，用于屏幕显示）
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");

    // 设备当前是否可进入睡眠模式？
    // 条件：Idle 状态 + 音频通道关闭 + 音频服务空闲
    bool CanEnterSleepMode();

    // ── MCP 通信 ────────────────────────────────────────────────

    // 向服务器发送 MCP 消息（JSON-RPC 2.0 格式）
    // 内部通过 Schedule() 投递到主线程，确保线程安全
    void SendMcpMessage(const std::string& payload);

    // ── AEC 控制 ────────────────────────────────────────────────

    // 设置回声消除模式
    // 修改后若音频通道已打开，则自动关闭重新建立
    void SetAecMode(AecMode mode);

    // 获取当前回声消除模式
    AecMode GetAecMode() const { return aec_mode_; }

    // ── 音频与协议 ─────────────────────────────────────────────

    // 播放本地音效（如弹出提示音、成功音）
    void PlaySound(const std::string_view& sound);

    // 获取音频服务对象的引用（供 MCP 工具等直接操作音频）
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * 重置协议资源（线程安全）
     * 
     * 关闭音频通道，释放 protocol_ 和 ota_ 对象。
     * 通常用于 WiFi 切换或服务器地址更改后重新连接。
     * 可从任意线程调用，内部通过 Schedule() 在主线程执行。
     */
    void ResetProtocol();

private:
    // ── 构造与析构（私有 —— 仅 GetInstance() 可调用）─────────────
    Application();
    ~Application();

    // ── 成员变量（"Application 总管桌上之物"）──────────────────

    std::mutex mutex_;                         // 互斥锁
                                               //   保护 main_tasks_ 队列的并发安全

    std::deque<std::function<void()>> main_tasks_;
                                               // 待执行任务队列
                                               //   Schedule() 投递 → Run() 取出执行

    std::unique_ptr<Protocol> protocol_;       // 通信协议对象（WebSocket 或 MQTT）
                                               //   管理音频通道的开/关、消息收发

    EventGroupHandle_t event_group_ = nullptr; // FreeRTOS 事件组句柄
                                               //   此乃 13 个事件铃之所在，
                                               //   全部模块通过此句柄发信号给主循环

    esp_timer_handle_t clock_timer_handle_ = nullptr;
                                               // 时钟定时器句柄（每秒响一次）

    DeviceStateMachine state_machine_;         // 状态机 —— 管理设备状态转移
                                               //   (Idle ↔ Connecting ↔ Listening ↔ Speaking)

    ListeningMode listening_mode_ = kListeningModeAutoStop;
                                               // 当前收音模式：
                                               //   AutoStop = 用户说完自动停止
                                               //   ManualStop = 用户手动停止
                                               //   Realtime = 实时对话模式

    AecMode aec_mode_ = kAecOff;               // 当前回声消除模式

    std::string last_error_message_;           // 最近一次错误消息（弹警告时用）

    AudioService audio_service_;               // 音频服务总管
                                               //   含 Microphone 采集、OPUS 编解码、
                                               //   唤醒词检测（ESP-SR）、VAD

    std::unique_ptr<Ota> ota_;                 // OTA 升级与激活管理对象
                                               //   负责检查新版本、下载固件、设备激活

    // ── 标志位 ──────────────────────────────────────────────────

    bool has_server_time_ = false;             // 服务器是否提供了时间戳
    bool aborted_ = false;                     // 当前对话是否已被中止
    bool assets_version_checked_ = false;      // 资源包版本是否已检查过（防重复）
    bool play_popup_on_listening_ = false;     // 进入收音状态后是否需播放弹出提示音
                                               //   唤醒词触发时设为 true，入 Listening 时播放

    int clock_ticks_ = 0;                      // 时钟脉冲计数（秒）
                                               //   每 10 秒打印一次内存统计

    TaskHandle_t activation_task_handle_ = nullptr;
                                               // 激活任务句柄（跑在后台线程中）
                                               //   用于管理激活任务的启动与防重复


    // ── 事件处理函数（私有——仅主循环 Run() 调用）───────────────
    // 每个函数对应一个事件位（见上方宏定义）

    void HandleStateChangedEvent();            // 响应 MAIN_EVENT_STATE_CHANGED
                                               //   根据新状态调整 UI、音频、LED 行为
    void HandleToggleChatEvent();              // 响应 MAIN_EVENT_TOGGLE_CHAT
                                               //   切换聊天状态
    void HandleStartListeningEvent();          // 响应 MAIN_EVENT_START_LISTENING
    void HandleStopListeningEvent();           // 响应 MAIN_EVENT_STOP_LISTENING
    void HandleNetworkConnectedEvent();        // 响应 MAIN_EVENT_NETWORK_CONNECTED
                                               //   启动激活任务
    void HandleNetworkDisconnectedEvent();     // 响应 MAIN_EVENT_NETWORK_DISCONNECTED
                                               //   关闭音频通道
    void HandleActivationDoneEvent();          // 响应 MAIN_EVENT_ACTIVATION_DONE
                                               //   激活完成后释放 OTA 对象，降低功耗
    void HandleWakeWordDetectedEvent();        // 响应 MAIN_EVENT_WAKE_WORD_DETECTED
                                               //   开启音频通道，进入对话模式

    // ── 辅助方法 ────────────────────────────────────────────────

    // 继续打开音频通道（由 Schedule 延迟调用，在状态机确认 OK 后执行）
    void ContinueOpenAudioChannel(ListeningMode mode);

    // 继续处理唤醒词（由 Schedule 延迟调用）
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // 激活任务体 —— 跑在独立 FreeRTOS 后台任务中
    // 流程：检查资源更新 → 检查固件更新 → 初始化通信协议 → 通知主循环
    void ActivationTask();

    // 检查 assets 分区是否有新资源需下载
    void CheckAssetsVersion();

    // 检查是否有新固件版本可升级
    void CheckNewVersion();

    // 初始化通信协议（根据服务器配置选择 WebSocket 或 MQTT）
    // 并注册各类回调（收音频、收JSON、网络错误等）
    void InitializeProtocol();

    // 在屏幕上显示激活码（数字+音频朗读）
    void ShowActivationCode(const std::string& code, const std::string& message);

    // 设置收音模式并进入 Listening 状态
    void SetListeningMode(ListeningMode mode);

    // 获取默认收音模式（AEC 开启时为 Realtime，否则为 AutoStop）
    ListeningMode GetDefaultListeningMode() const;
    
    // 状态转移回调 —— 由状态机调用，然后发送 MAIN_EVENT_STATE_CHANGED 到主循环
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


// ============================================================
// 辅助工具类：任务优先级临时提升（RAII 模式）
// ============================================================
//
// 用法示例（在 audio_service.cc 等文件中）：
//   void some_critical_operation() {
//       TaskPriorityReset raii(12);    // 构造时提升当前任务优先级到 12
//       // ... 执行关键操作 ...
//   }                                   // 析构时自动恢复原优先级
//
// 用途：某些对实时性要求高的操作（如音频编码、唤醒词检测）需要
//       临时提升当前任务的 FreeRTOS 优先级，操作完成后自动恢复。
//       RAII 保证即使发生异常或提前 return，优先级也能安全恢复。
//
class TaskPriorityReset {
public:
    // 构造时记录当前优先级，并提升至指定优先级
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);   // 记下原有优先级
        vTaskPrioritySet(NULL, priority);               // 提升到目标优先级
    }

    // 析构时自动恢复原优先级
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;   // 原有优先级（析构时恢复用）
};

#endif // _APPLICATION_H_
