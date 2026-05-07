// ============================================================
// application.cc —— 应用总管之实现体
// ============================================================
// 本文件实现 Application 类的全部方法。
// 从上电初始化到主循环事件处理，从资源更新到固件升级，
// 所有设备生命周期之运转，尽在此间。
// ============================================================

#include "application.h"            // 自身声明（单例入口、事件定义、方法声明）
#include "board.h"                  // 硬件总管 —— 屏幕、音频、LED、按键、网络
#include "display.h"                // 显示屏抽象接口（LcdDisplay / EmoteDisplay）
#include "system_info.h"            // 系统信息 —— 获取版本、用户代理、打印内存统计
#include "audio_codec.h"            // 音频编解码器抽象（音频芯片驱动）
#include "mqtt_protocol.h"          // MQTT+UDP 通信协议实现
#include "websocket_protocol.h"     // WebSocket 通信协议实现
#include "assets/lang_config.h"     // 多语言字符串（中文/英文/日文）
#include "mcp_server.h"             // MCP 协议服务器 —— 注册和响应设备端工具调用
#include "assets.h"                 // 静态资源管理器 —— 字体、表情、皮肤、声纹模型
#include "settings.h"               // NVS 键值对读写封装 —— 持久化配置存取

#include <cstring>                  // C 字符串函数（strcmp 等）
#include <esp_log.h>                // ESP-IDF 日志宏（ESP_LOGI/ESP_LOGW/ESP_LOGE）
#include <cJSON.h>                  // 轻量级 JSON 解析库 —— 解析服务器下发消息
#include <driver/gpio.h>            // GPIO 引脚控制
#include <arpa/inet.h>              // 网络字节序转换（inet_pton 等）
#include <font_awesome.h>           // Font Awesome 图标字体资源

// FreeRTOS 日志标签
#define TAG "Application"


// ============================================================
// 第一部分：构造函数与析构函数
// ============================================================

/**
 * 构造函数 —— 由 GetInstance() 首次调用时自动执行
 * 
 * 此函数创建总管桌上的"基础设施"，共三件事：
 *   1. 创建事件组 —— 即 13 个响铃的总线
 *   2. 决定 AEC 模式 —— 根据编译配置选择回声消除方案
 *   3. 创建时钟定时器 —— 每秒触发 MAIN_EVENT_CLOCK_TICK
 */
Application::Application() {
    // ── 创建 FreeRTOS 事件组 ──────────────────────────────────
    // 这是整个事件驱动架构的核心硬件基础——
    // 一个 32 位的二进制掩码变量，每一位代表一类事件。
    // 其他模块通过 xEventGroupSetBits() 置位通知主循环；
    // 主循环通过 xEventGroupWaitBits() 等待并响应。
    event_group_ = xEventGroupCreate();

    // ── 根据编译配置确定 AEC（回声消除）模式 ─────────────────
    // CONFIG_USE_DEVICE_AEC 和 CONFIG_USE_SERVER_AEC 在
    // sdkconfig（通过 menuconfig 配置）中定义，编译时确定。
#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
    // 两者同时启用在逻辑上矛盾，编译直接报错
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;       // 设备端运算消除回声（延迟低，占用 CPU）
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;       // 服务器端运算消除回声（云端运算，设备省资源）
#else
    aec_mode_ = kAecOff;                // 关闭回声消除（对话模式）
#endif

    // ── 创建 1 秒硬件定时器 ──────────────────────────────────
    // 原码注释：
    //   .callback — 定时器到期后执行的回调（运行在 ESP_TIMER_TASK 上下文中）
    //   .arg — 传递 this 指针，便于回调中访问 Application 对象
    //   .dispatch_method — ESP_TIMER_TASK 表示由专用定时器任务分发（不占用主线程）
    //   .name — 定时器名称（调试用）
    //   .skip_unhandled_events — true = 若上一周期未处理完毕则跳过（防止积压）
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            // 每秒执行：向事件组发送 MAIN_EVENT_CLOCK_TICK 信号
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

/**
 * 析构函数 —— 设备断电/重启时调用
 * 
 * 释放构造时创建的资源：停止定时器、删除事件组。
 * 遵循"后进先出"原则 —— 先创建者后销毁。
 */
Application::~Application() {
    // 若定时器尚在运行，先停止，再删除
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    // 删除 FreeRTOS 事件组，释放内核资源
    vEventGroupDelete(event_group_);
}

// ============================================================
// 第二部分：状态管理与主流程
// ============================================================

/**
 * 请求设备状态转移（线程安全）
 * 
 * 将请求委托给 state_machine_（DeviceStateMachine 实例），
 * 由其判定转移是否合法。
 * 
 * @param state 目标状态
 * @return true = 转移成功，false = 非法转移（被状态机拒绝）
 * 
 * @note 转移成功后，状态机内部会调用 OnStateChanged() 回调，
 *       该回调向事件组发送 MAIN_EVENT_STATE_CHANGED 信号，
 *       最终由主循环中的 HandleStateChangedEvent() 处理实际逻辑。
 */
bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

/**
 * 初始化应用 —— 设备上电后的第一件大事
 * 
 * 此方法按严格顺序完成以下初始化工作：
 *       ① 初始化显示屏 UI（SetupUI）
 *       ② 初始化音频服务（麦克风采集 → OPUS 编码 → 唤醒词检测）
 *       ③ 注册音频回调（音频就绪、唤醒词检测、VAD 变化）
 *       ④ 启动时钟定时器（每秒刷新状态栏）
 *       ⑤ 注册 MCP 工具（云端可调用的设备功能）
 *       ⑥ 注册网络事件回调（WiFi/4G 连接状态变化）
 *       ⑦ 异步启动网络连接
 * 
 * @note Initialize() 立即返回，网络连接在后台异步进行
 */
void Application::Initialize() {
    // ── ① 获取单例，置于"启动中"状态 ──────────────────────────
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // ── ② 初始化显示屏 ─────────────────────────────────────────
    // SetupUI() 根据 device_state.h 中定义的板卡类型，
    // 创建对应的显示驱动（LcdDisplay 或 EmoteDisplay），
    // 并初始化 UI 布局（状态栏、聊天区域、表情区域）。
    auto display = board.GetDisplay();
    display->SetupUI();

    // 在屏幕上显示设备信息（如 "XiaoZhi-ESP32S3/2.0.0"）
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // ── ③ 初始化音频服务 ──────────────────────────────────────
    // codec 是对音频芯片（ES8311/ES8388/MAX98357 等）的抽象封装
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);       // 初始化编解码器与麦克风
    audio_service_.Start();                 // 启动音频采集与处理线程

    // ── ④ 注册音频回调 —— 将音频事件连接到主循环的事件铃 ────
    // 这些回调运行在音频线程上下文中，不能直接操作 UI，
    // 只能通过 xEventGroupSetBits() 发出信号，让主循环处理。
    AudioServiceCallbacks callbacks;
    
    // 铃1: 音频编码包就绪 → 通知主循环发送到服务器
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    // 铃2: 检测到唤醒词 → 通知主循环开始对话
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    // 铃3: 说话/静音状态变化 → 通知主循环更新 LED 指示灯
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // ── ⑤ 注册状态转移监听器 ──────────────────────────────────
    // 每当状态机发生转移（如 Idle → Listening），
    // 此回调向主循环发送 MAIN_EVENT_STATE_CHANGED 信号。
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // ── ⑥ 启动时钟定时器 ──────────────────────────────────────
    // 参数 1000000 = 1,000,000 微秒 = 1 秒。定时器为周期性。
    // 每秒触发一次 MAIN_EVENT_CLOCK_TICK，驱动状态栏刷新。
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // ── ⑦ 注册 MCP 工具（仅注册一次）─────────────────────────
    // AddCommonTools(): 注册所有设备通用的工具
    //   - self.get_battery_level  → 获取电量
    //   - self.get_network_info   → 获取网络状态
    //   - self.volume.set/get     → 音量控制
    //   - self.reboot             → 重启设备
    //   - 等等...
    // AddUserOnlyTools(): 注册仅用户可调用的工具（需鉴权）
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // ── ⑧ 注册网络事件回调 ────────────────────────────────────
    // 此回调在 Board 的网络线程中触发，捕获 Wi-Fi/4G 的各种状态变化，
    // 在屏幕上显示相应提示，并向主循环发送连接/断开信号。
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                // Wi-Fi 扫描中 → 显示通知，通知主循环网络已断开
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;

            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // data 为空 → 蜂窝网络注册中（尚未获取运营商信息）
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // data 非空 → Wi-Fi 或已获取到运营商信息的蜂窝网络
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }

            case NetworkEvent::Connected: {
                // 网络连接成功 → 显示通知 + 通知主循环
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }

            case NetworkEvent::Disconnected:
                // 网络断开 → 通知主循环关闭音频通道
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;

            // Wi-Fi 配网模式进入/退出 —— 由 WifiBoard 内部处理，无需额外操作
            case NetworkEvent::WifiConfigModeEnter:
                break;
            case NetworkEvent::WifiConfigModeExit:
                break;

            // ── 以下为 4G 蜂窝网络特有事件 ─────────────────────

            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;

            case NetworkEvent::ModemErrorNoSim:
                // SIM 卡错误 → 弹严重警告
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;

            case NetworkEvent::ModemErrorRegDenied:
                // 注册被拒 → 弹严重警告
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;

            case NetworkEvent::ModemErrorInitFailed:
                // 模块初始化失败 → 弹严重警告
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;

            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // ── ⑨ 异步启动网络连接 ────────────────────────────────────
    // StartNetwork() 不阻塞，内部创建后台任务处理连接流程。
    // 后续连接事件通过上方注册的回调通知 Application。
    board.StartNetwork();

    // 立即刷新状态栏（显示当前网络状态——初始为未连接）
    display->UpdateStatusBar(true);
}

/**
 * 运行主事件循环 —— 永不返回的永恒循环
 * 
 * 此乃设备上电后除了后台线程之外，唯一持续运行的主任务。
 * 其核心逻辑极为简洁：
 *   while (true) {
 *       // 1. 阻塞等待任意事件铃响
 *       // 2. 按优先级逐一处理被置位的事件
 *       // 3. 回到第 1 步
 *   }
 * 
 * 事件处理无严格优先级要求，因为同一时刻可能有多个事件位被置位，
 * 循环会逐一处理完所有已发生事件后方才继续等待。
 * 
 * @note 此函数在 main.cc 中被调用后，永不返回。
 */
void Application::Run() {
    // ── 提升主任务优先级 ──────────────────────────────────────
    // nullptr 表示当前任务自身。
    // 优先级 10（FreeRTOS 范围内 0~25），高于一般任务，
    // 确保音频数据的收发不被其他任务阻塞。
    vTaskPrioritySet(nullptr, 10);

    // ── 组合所有事件位（"全铃监听"模式）───────────────────────
    // ALL_EVENTS 是 13 个事件位的按位或（bitwise OR）。
    // 传给 xEventGroupWaitBits 表示：只要其中任何一位为 1，就唤醒。
    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    // ── 主循环（永不退出！除非断电或复位）────────────────────
    while (true) {
        // 等待任意事件发生（阻塞等待，CPU 不空转）
        //   pdTRUE  = 读取后自动清除事件位（"响应即清零"）
        //   pdFALSE = 等待任意一个事件（非全部）
        //   portMAX_DELAY = 无限期阻塞，直至有事件发生
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        // ═══ 以下按顺序处理每一个可能被置位的事件 ═══
        // 每个 if 块检查对应位是否为 1（bits & EVENT_MASK）

        // [铃4] 错误事件 —— 弹警告、回 Idle
        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);  // 先回到待机状态
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        // [铃7] 网络已连接 —— 启动激活流程
        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        // [铃8] 网络已断开 —— 关闭音频通道
        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        // [铃5] 激活完成 —— 释放 OTA 对象，降功耗，进入待机
        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        // [铃12] 状态变化 —— 根据新状态调整 UI、音频、LED
        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        // [铃9] 切换聊天状态 —— 按键触发
        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        // [铃10] 主动开始收音 —— MCP 远程指令或按键触发
        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        // [铃11] 主动停止收音
        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        // [铃1] 音频数据就绪 —— 从发送队列取出包，通过协议发送至服务器
        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // 从队列中循环取出音频包，直至队列空或发送失败
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;  // 发送失败，停止取包（可能网络断开）
                }
            }
        }

        // [铃2] 唤醒词检测 —— 开启音频通道，进入对话
        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        // [铃3] VAD 变化 —— 收音状态下刷新 LED 指示
        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();  // 根据是否检测到人声点亮/熄灭 LED
            }
        }

        // [铃0] 延迟任务调度 —— 执行 Schedule() 投递的回调
        if (bits & MAIN_EVENT_SCHEDULE) {
            // 加锁 → 取出整份队列 → 解锁 → 逐项执行
            //   （使用 swap(move) 将队列搬到栈上，最小化持锁时间）
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);  // 移动到本地变量，原队列为空
            lock.unlock();
            for (auto& task : tasks) {
                task();   // 在无锁状态下执行回调
            }
        }

        // [铃6] 时钟脉冲 —— 每秒刷新状态栏 + 每 10 秒打印内存统计
        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();       // 每秒刷新状态栏（如电量、WiFi信号）
        
            // 每 10 秒打印一次内存统计
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats(); // 输出 SRAM / PSRAM 使用量到串口
            }
        }
    }  // ← while(true) 永远不结束
}

// ============================================================
// 第三部分：网络事件处理
// ============================================================

/**
 * 处理网络连接成功事件（MAIN_EVENT_NETWORK_CONNECTED）
 * 
 * 当设备首次连接网络时，启动激活流程：
 *   1. 切换到"激活中"状态
 *   2. 创建后台 FreeRTOS 任务执行激活逻辑
 *   3. 刷新状态栏
 * 
 * 激活任务在独立线程中执行如下流程（见 ActivationTask）：
 *   检查资源更新 → 检查固件更新 → 建立通信协议 → 通知主循环完成
 */
void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    // 仅在"启动中"或"WiFi配网中"两种状态下才启动激活流程
    // （若已在其他状态，说明设备已激活过，不必重复）
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateActivating);

        // 防重复：若激活任务已在运行，不再创建
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        // 创建 FreeRTOS 激活任务
        //   参数说明：
        //     入口函数   — 匿名 lambda，调用 app->ActivationTask()
        //     任务名     — "activation"
        //     栈大小     — 4096 * 2 = 8192 字节（8KB）
        //     参数       — this 指针
        //     优先级     — 2（较低，不与音频/网络争抢 CPU）
        //     任务句柄   — &activation_task_handle_（保存以便后续管理）
        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();                    // 执行激活逻辑
            app->activation_task_handle_ = nullptr;   // 完成后清除句柄
            vTaskDelete(NULL);                        // 自删除
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // 无论是否启动激活，都立即刷新状态栏显示网络状态
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

/**
 * 处理网络断开事件（MAIN_EVENT_NETWORK_DISCONNECTED）
 * 
 * 当WiFi或4G连接中断时：
 *   1. 若当前在对话中（Connecting / Listening / Speaking），关闭音频通道
 *   2. 刷新状态栏显示"已断开"
 */
void Application::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();

    // 对话活跃状态下断网，须立即关闭音频通道以免崩溃
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

/**
 * 处理激活完成事件（MAIN_EVENT_ACTIVATION_DONE）
 * 
 * 激活任务完成后，此函数负责"善后"：
 *   1. 回到 Idle 状态（设备就绪，等待用户交互）
 *   2. 显示版本号通知
 *   3. 释放 OTA 对象（以降功耗——升级和激活对象仅在激活期间需要）
 *   4. 降低功耗等级（从"性能模式"切回"低功耗模式"）
 *   5. 播放成功音效（告知用户设备已就绪）
 */
void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();          // 输出激活完成后的内存使用情况
    SetDeviceState(kDeviceStateIdle);      // 回到待机状态

    has_server_time_ = ota_->HasServerTime();  // 记录服务器是否提供了时间

    // 在屏幕上显示当前版本号
    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // 释放 OTA 对象 —— 激活完毕不再需要，释放内存给其他功能使用
    ota_.reset();

    // 降低功耗：I2C/SPI 总线降频，关闭不必要外设
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    // 延迟播放成功音效（等状态机完成 Idle 转移后再播）
    Schedule([this]() {
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

// ============================================================
// 第四部分：激活任务与资源管理
// ============================================================

/**
 * 激活任务入口 —— 运行在独立 FreeRTOS 后台任务中
 * 
 * 激活流程三大步：
 *   ① CheckAssetsVersion()  — 检查是否需要下载新的资源包（emoji/字体/皮肤）
 *   ② CheckNewVersion()     — 检查是否有新固件可升级（OTA）、设备是否需要激活
 *   ③ InitializeProtocol()  — 根据服务器配置选择 MQTT 或 WebSocket 协议，建立连接
 * 
 * 全部完成后，通过事件组通知主循环（置位 MAIN_EVENT_ACTIVATION_DONE 铃）。
 */
void Application::ActivationTask() {
    // 创建 OTA 对象（激活过程必需，包含服务器配置信息）
    ota_ = std::make_unique<Ota>();

    // ① 检查资源更新
    CheckAssetsVersion();

    // ② 检查固件更新与设备激活
    CheckNewVersion();

    // ③ 初始化通信协议（选择 MQTT/WebSocket + 注册所有回调）
    InitializeProtocol();

    // ④ 通知主循环：激活完成
    //    主循环收到此铃后执行 HandleActivationDoneEvent()
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

/**
 * 检查资源包版本 —— 如有更新则下载到 assets 分区
 * 
 * 工作流程：
 *   1. 防重复检查（仅执行一次）
 *   2. 查 NVS，是否有预置的 download_url（上次 OTA 时服务器下发的资源下载链接）
 *   3. 若有，下载新资源包到 Flash 的 assets 分区
 *   4. 加载资源（字体、emoji、皮肤等应用到 UI）
 * 
 * @note "资源包"与"固件"不同——资源是显示用的素材，固件是运行的可执行程序
 */
void Application::CheckAssetsVersion() {
    // ── 防重复：本方法在生命周期中仅执行一次 ────────────────
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    // 若当前板卡不支持 assets 分区（极小内存芯片），直接跳过
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    // ── 查 NVS 中是否有待下载的资源链接 ──────────────────────
    // "assets" 命名空间，"true" 表示只读模式
    Settings settings("assets", true);
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        // 取出后立即删除该键，防止重复下载
        settings.EraseKey("download_url");

        // 弹通知告知用户正在下载资源
        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // 等 3 秒，让用户看清楚提示
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);

        // 提升性能：开启 I2C/SPI 全速，确保下载不卡顿
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        // ── 下载资源文件到 Flash 的 assets 分区 ──────────────
        // 下载过程中实时回调进度（进度百分比 + 速度 KB/s）
        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            // 通过 Schedule() 回到主线程刷新 UI（下载回调在网络线程中）
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        // 下载完成后降功耗
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            // 下载失败 → 弹错误提示，回到激活流程
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // ── 加载资源包 ───────────────────────────────────────────
    // Apply() 解析 assets 分区中的 index.json，
    // 应用字体、表情、皮肤、声纹模型到 UI 和音频服务。
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

/**
 * 检查新固件版本与设备激活 —— 带重试机制
 * 
 * 此函数运行在激活任务的后台线程中，主循环不受其阻塞。
 * 其流程分为两个阶段：
 *   【阶段一】检查新版本：
 *      向服务器查询是否有新固件，若有则自动 OTA 升级。
 *      失败则指数退避重试（10秒、20秒、40秒... 最多 10 次）。
 * 
 *   【阶段二】设备激活：
 *      若无新版本，检查是否需要激活设备。
 *      若服务器返回激活码，则在屏幕上显示并语音朗读，
 *      之后轮询服务器确认激活状态（最多 10 次，每次间隔 3~10 秒）。
 */
void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;       // 最多重试 10 次
    int retry_count = 0;
    int retry_delay = 10;           // 初始重试延迟 10 秒

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        // ── 向服务器查询新版本 ──────────────────────────────
        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            // 弹提示告知失败原因与下次重试时间
            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            // ── 指数退避等待 ──────────────────────────────────
            // 第1次等10秒、第2次等20秒、第3次等40秒...
            // 若等待期间用户切到 Idle（按停止键），则提前退出等待
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // 指数退避
            continue;
        }
        // 查询成功，重置计数
        retry_count = 0;
        retry_delay = 10;

        // ── 有新版本 → 立即 OTA 升级（成功则重启，不返回） ──
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // 升级成功后 Reboot() 会被调用，此行理论上不会执行
            }
            // 升级失败 → 继续正常流程（不阻塞，设备可用旧版本）
        }

        // ── 无新版本 → 标记当前版本有效，检查是否需要激活 ───
        ota_->MarkCurrentVersionValid();

        // 若既无激活码也无激活挑战 → 无需激活，退出循环
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            break;
        }

        // ── 需要激活 → 显示激活码 → 轮询确认激活 ──────────
        display->SetStatus(Lang::Strings::ACTIVATION);

        // 在屏幕上显示激活码，同时语音朗读（ShowActivationCode）
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // 轮询服务器确认激活（最多 10 次）
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;                  // 激活成功
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));   // 超时等 3 秒
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));  // 其他错误等 10 秒
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;                  // 用户取消了，不继续等
            }
        }
    }
}

// ============================================================
// 第五部分：通信协议初始化
// ============================================================

/**
 * 初始化通信协议 —— 选择协议并注册全部回调
 * 
 * 此函数是设备与服务器"建立沟通管道"的关键。
 * 流程：
 *   ① 根据 OTA 配置选择通信协议（MQTT 或 WebSocket）
 *   ② 注册 5 个核心回调（连接/断连/收音频/收JSON/网络错误）
 *   ③ 启动协议连接
 * 
 * 回调说明（全部运行在协议层线程中，通过 Schedule() 投递到主线程）：
 *   - OnConnected:        协议连接成功 → 关闭警告弹窗
 *   - OnNetworkError:     协议层网络错误 → 通知主循环弹警告
 *   - OnIncomingAudio:    收到服务器下发的音频包 → 送入解码队列
 *   - OnAudioChannelOpened: 音频通道建立 → 提升性能模式
 *   - OnAudioChannelClosed: 音频通道关闭 → 降低功耗、回 Idle
 *   - OnIncomingJson:     收到服务器 JSON 消息 → 解析类型分发
 */
void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // ── ① 根据 OTA 配置决定协议类型 ────────────────────────
    // HasMqttConfig() / HasWebsocketConfig() 取决于服务器返回的配置信息。
    // 若无明确指定，默认使用 MQTT 协议（较轻量，适合嵌入式）。
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    // ── ② 注册协议回调 ──────────────────────────────────────

    // 回调一：协议连接成功
    //   撤销之前的任何警告 UI，恢复干净界面。
    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    // 回调二：协议层网络错误
    //   保存错误消息，通过事件铃通知主循环。
    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    // 回调三：收到服务器下发的音频数据
    //   仅在设备处于 Speaking 状态时接收（否则丢弃）。
    //   音频包送入解码队列，由音频线程解码播放。
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    // 回调四：音频通道建立
    //   提升性能等级（全速运行），因为即将开始高频音频传输。
    //   同时检查采样率是否匹配，若不一致则打印警告（可能导致声音失真）。
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    // 回调五：音频通道关闭
    //   降低功耗等级，清空聊天消息，回到待机状态。
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        // 必须通过 Schedule 投递（此回调可能在协议线程中）
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    // 回调六（核心）：处理服务器下发的 JSON 消息
    //   服务器与设备间除音频流外的所有交互，皆通过 JSON 格式。
    //   根据 "type" 字段分发到不同逻辑处理：
    //
    //   type = "tts"   → 语音合成控制（开始/停止/句子开始）
    //   type = "stt"   → 语音识别结果（用户说的话转文字）
    //   type = "llm"   → 大模型推理时表情设定
    //   type = "mcp"   → MCP 工具调用指令（控制设备外设）
    //   type = "system" → 系统指令（如远程重启）
    //   type = "alert"  → 服务器下发的警告通知
    //   type = "custom" → 自定义消息（需 CONFIG_RECEIVE_CUSTOM_MESSAGE 开启）
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");

        // ── TTS 语音合成消息 ────────────────────────────────
        // 分为三个子状态：
        //   state = "start"            → 服务器开始下发合成音频，设备切 Speaking
        //   state = "stop"             → 合成结束，切回 Listening（自动模式）或 Idle（手动模式）
        //   state = "sentence_start"   → 新一轮句子开始，附带文本内容显示在屏幕上
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;       // 重置中止标志
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        // 自动停止模式 → 说完后继续收音（持续对话）
                        // 手动停止模式 → 说完后回待机（一轮对话结束）
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);  // "<< " = 小智说的话
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        }
        // ── STT 语音识别结果 ──────────────────────────────────
        // 服务器返回"你刚才说了什么"的转写文字
        else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);  // ">> " = 用户说的话
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        }
        // ── LLM 表情设置 ──────────────────────────────────────
        // 大模型在推理过程中可附带 emotion 字段，指示设备显示对应表情
        else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        }
        // ── MCP 工具调用 ──────────────────────────────────────
        // 服务器通过 JSON-RPC 2.0 格式调用设备端注册的工具
        // （如控制 GPIO、LED、音量等），由 McpServer 解析并执行
        else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        }
        // ── 系统指令 ──────────────────────────────────────────
        // 目前仅支持 "reboot" 一条指令 —— 远程重启设备
        else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        }
        // ── 服务器警告 ─────────────────────────────────────────
        // 服务器可主动下发通知（如"服务器维护中"）
        else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        }
        // ── 自定义消息（条件编译，默认关闭）───────────────────
        // 需在 menuconfig 中启用 CONFIG_RECEIVE_CUSTOM_MESSAGE
        // 用于开发者自行扩展消息类型
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
        }
#endif
        // ── 未知消息类型 ──────────────────────────────────────
        else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    // ── ③ 启动协议连接 ────────────────────────────────────────
    // Start() 启动连接流程（包括 DNS 解析、TLS 握手等），
    // 后续回调将异步触发。
    protocol_->Start();
}

// ============================================================
// 第六部分：UI 辅助功能
// ============================================================

/**
 * 显示激活码 —— 语音朗读 + 屏幕显示
 * 
 * 将激活码的每一位数字用预录音频朗读出来。
 * digit_sounds 数组建立了 '0'~'9' 到对应 OGG 音频文件的映射。
 * 
 * 为何要朗读？因为有些设备无屏幕或屏小看不清，
 * 通过语音播报让用户听到激活码。
 */
void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    // 数字到音频的映射表（静态，编译时生成）
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // 先在屏幕上弹警告显示激活码，同时播放激活提示音
    // （Alert 内部调用 PlaySound 会占用 ~9KB SRAM，需等待其完成）
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    // 逐位朗读激活码数字
    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

/**
 * 弹出警告提示 —— 状态栏 + 表情 + 聊天消息 + 音效 四合一
 * 
 * 用一套组合拳告知用户发生了什么：
 *   - 状态栏更新为 status（如 "ERROR" 或 "LOADING"）
 *   - 表情切换为 emotion（如 "triangle_exclamation"）
 *   - 聊天区显示 message 正文
 *   - 可选播放提示音
 */
void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

/**
 * 关闭警告提示
 * 
 * 仅在设备处于 Idle 状态时才恢复为"待机"界面，
 * 避免在对话中误覆盖正在显示的消息。
 */
void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

// ============================================================
// 第七部分：对话控制事件处理
// ============================================================

// ── 发铃方法（线程安全，仅 xEventGroupSetBits）─────────────────

/**
 * 切换聊天状态
 * 内部仅仅设置事件位，不执行实际逻辑。
 * 可从任意线程安全调用（按键中断、MCP 回调等）。
 */
void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

/**
 * 开始收音（线程安全版）
 */
void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

/**
 * 停止收音（线程安全版）
 */
void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

// ── 实际事件处理逻辑（仅主循环调用）────────────────────────

/**
 * 处理聊天切换事件（MAIN_EVENT_TOGGLE_CHAT）
 * 
 * 根据当前状态决定行为：
 *   - Activating  → 取消激活，回 Idle
 *   - WifiConfiguring → 进入音频测试模式
 *   - AudioTesting → 退出音频测试，回配网模式
 *   - Idle        → 打开音频通道，开始收音（对话）
 *   - Speaking    → 中止说话
 *   - Listening   → 关闭音频通道，回到 Idle
 */
void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    // ── 特殊状态处理 ─────────────────────────────────────────
    if (state == kDeviceStateActivating) {
        // 激活中按对话键 = 取消激活
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        // 配网中按对话键 = 进入音频测试（可听自己说话的声音）
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        // 测试中再按 = 退出测试
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    // ── 协议未初始化则无法对话 ──────────────────────────────
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    // ── 正常对话状态切换 ─────────────────────────────────────
    if (state == kDeviceStateIdle) {
        // 待机 → 开始对话
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            // 先切 Connecting，UI 更新后再开通道（Schedule 延迟执行）
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        // 通道已开着，直接进入 Listening
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        // 说话中按对话键 = 中止说话
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        // 收音中按对话键 = 关闭通道，回 Idle
        protocol_->CloseAudioChannel();
    }
}

/**
 * 继续打开音频通道（由 Schedule 延迟调用）
 * 
 * 此时状态机已确认为 Connecting，再次校验后执行实际 OpenAudioChannel 操作。
 * 为何要延迟？因为 SetDeviceState 在事件处理前半部分调用，
 * 必须先让状态转移完成（HandleStateChangedEvent 先执行），
 * 再执行可能阻塞的操作（OpenAudioChannel 可能耗时 ~1 秒）。
 */
void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // 二次检查状态（防止中间被取消）
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 若通道尚未打开，尝试打开。成功则进入 Listening。
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;  // 打开失败，不继续
        }
    }

    SetListeningMode(mode);
}

/**
 * 处理主动开始收音事件（MAIN_EVENT_START_LISTENING）
 * 
 * 与 ToggleChat 类似，但区别在于：
 *   - ToggleChat: Idle→收音 且 收音→Idle（双向切换）
 *   - StartListening: 仅 Idle→收音（单向），使用 ManualStop 模式
 * 
 * 可用于 MCP 远程指令或物理"对话"按键。
 */
void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    // 特殊状态处理（同 ToggleChat）
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 使用 ManualStop 模式（不自动停止，须用户主动关闭）
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        // 说话中要求收音 → 先中止说话再开始
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

/**
 * 处理主动停止收音事件（MAIN_EVENT_STOP_LISTENING）
 */
void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        // 向服务器发送停止收音通知
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * 处理唤醒词检测事件（MAIN_EVENT_WAKE_WORD_DETECTED）
 * 
 * 这是设备从"待机"进入"对话"的最常见触发方式。
 * 当检测到唤醒词（如"小智小智"）时，根据当前状态有三种处理：
 * 
 *   ① Idle → 编码唤醒词音频 → 打开音频通道 → 进入 Listening 对话
 *   ② Speaking/Listening → 中断当前对话 → 重新收音（用户急着说新话）
 *   ③ Activating → 取消激活，回 Idle
 */
void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        // ── 待机中，正常唤醒 ──────────────────────────────────
        // EncodeWakeWord(): 将唤醒词音频编码为 OPUS 格式，
        //   放入发送队列（后续由 MAIN_EVENT_SEND_AUDIO 发送至服务器）
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            // 通道未开 → 先切 Connecting，再 Schedule 延迟打开
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // 通道已开 → 直接进入对话
        ContinueWakeWordInvoke(wake_word);

    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        // ── 对话进行中，被唤醒词打断 ──────────────────────────
        AbortSpeaking(kAbortReasonWakeWordDetected);

        // 清空发送队列中的残留音频数据（避免服务器收到"半句话"）
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            // 已在收音→通知服务器重新开始 + 重置解码器 + 播放弹出音
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // 重新启用唤醒词检测（因为检测到唤醒词后会自动暂停）
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // 在说话→设置标志，等进入 Listening 状态时播放弹出音
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }

    } else if (state == kDeviceStateActivating) {
        // ── 激活中被唤醒词打断 → 取消激活，回 Idle ──────────
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * 继续唤醒词调用（由 Schedule 延迟执行）
 * 
 * 此函数与 HandleWakeWordDetectedEvent 的分工：
 *   - HandleWakeWordDetectedEvent: 设置状态 Connecting
 *   - ContinueWakeWordInvoke: 在状态机确认 OK 后执行实际的通道打开
 * 
 * 为何分开？因为 OpenAudioChannel 可能阻塞 ~1 秒（TLS 握手），
 * 必须先让状态机转移完成（HandleStateChangedEvent 更新 UI），
 * 再执行此耗时操作，避免 UI 卡顿。
 */
void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // 二次检查状态（防止被中途取消）
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 打开音频通道（若尚未打开）
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // 打开失败 → 重新启用唤醒词检测，等待下次唤醒
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());

#if CONFIG_SEND_WAKE_WORD_DATA
    // ── 向服务器发送唤醒词音频数据 ──────────────────────────
    // 将缓冲的唤醒词音频包发送到服务器（供声纹识别/唤醒词纠错）
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // ── 不发送唤醒词数据，直接进入收音 ──────────────────────
    // 设置 play_popup_on_listening_ 标志，而非直接播放音效，
    // 是因为 PlaySound 可能被 EnableVoiceProcessing 中的 ResetDecoder 覆盖。
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

/**
 * 处理状态变化事件（MAIN_EVENT_STATE_CHANGED）
 * 
 * 这是状态机的"执行者"——根据新状态做相应的硬件动作。
 * 每次状态转移都会被调用，是设备行为变化的核心枢纽。
 * 
 * 各状态下的动作：
 *   Idle/Unknown:     恢复待机界面，关闭语音处理，打开唤醒词检测
 *   Connecting:       显示"连接中"，清屏
 *   Listening:        启动语音处理（麦克风→编码→发送），关闭/打开唤醒词
 *   Speaking:         关闭语音处理/唤醒词（非实时模式），重置解码器
 *   WifiConfiguring:  关闭所有音频功能
 */
void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;   // 重置时钟计数（用于状态栏超时等逻辑）

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();   // 根据状态更新 LED 指示灯
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // 待机 → 显示"就绪"、清空聊天、表情中性
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();
            display->SetEmotion("neutral");
            // 停止语音处理（停止麦克风采集与编码）
            audio_service_.EnableVoiceProcessing(false);
            // 重新启用唤醒词检测（准备下一次对话）
            audio_service_.EnableWakeWordDetection(true);
            break;

        case kDeviceStateConnecting:
            // 连接中 → 显示"连接中"、表情中性、清屏
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;

        case kDeviceStateListening:
            // 收音中 → 显示"正在听"、表情中性
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // 启动语音处理器（采集 → 编码 → 发送到服务器）
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // 自动停止模式下，先等音频播放完毕，
                // 防止网络抖动导致 STOP 消息迟到，截断音频
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // 通知服务器：开始接收音频（带上收听模式参数）
                protocol_->SendStartListening(listening_mode_);
                // 启用语音处理（启动麦克风采集等）
                audio_service_.EnableVoiceProcessing(true);
            }

            // 根据编译配置决定收音模式下是否启用唤醒词
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // 允许在收音中也可检测唤醒词（"打断"功能，仅 AFE 模式支持）
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // 收音中关闭唤醒词（防止自己说话误触发）
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // 播放弹出提示音（ResetDecoder 已调用过，此时可以安全播放）
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;

        case kDeviceStateSpeaking:
            // 说话中 → 显示"正在说"
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                // 非实时模式 → 停止语音处理（设备端只播放，不采集）
                audio_service_.EnableVoiceProcessing(false);
                // 说话中仅 AFE 模式的唤醒词可以检测（打断功能）
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            // 重置音频解码器（准备接收新的 TTS 音频流）
            audio_service_.ResetDecoder();
            break;

        case kDeviceStateWifiConfiguring:
            // Wi-Fi 配网模式 → 关闭所有音频功能
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;

        default:
            // 其他状态（Upgrading、Activating 等）→ 不操作
            break;
    }
}

// ============================================================
// 第八部分：工具方法与辅助功能
// ============================================================

/**
 * 调度延迟任务（线程安全）
 * 
 * 外部线程通过此方法将回调投递到主事件循环。
 * 
 * 生命周期：加锁 → 推入队列 → 解锁 → 置位铃 → 
 *           主循环收到铃后 → 加锁、取出、解锁、执行
 * 
 * @note std::move(callback) 使用右值引用避免不必要的拷贝
 */
void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);   // RAII 自动加锁
        main_tasks_.push_back(std::move(callback));   // 移动语义，零拷贝
    }                                                  // lock_guard 析构自动解锁
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

/**
 * 中止当前说话
 * 
 * 向服务器发送中止指令，服务器收到后会停止 TTS 合成。
 * aborted_ 标志告知状态机：本次说话是被用户中断的（非正常结束）。
 */
void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

/**
 * 设置收音模式并立即进入 Listening 状态
 * 
 * 两步合一：保存模式 → 请求状态转移。
 * 状态转移成功后会触发 HandleStateChangedEvent → 实际启动语音处理。
 */
void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

/**
 * 获取默认收音模式
 * 
 * 规则：若 AEC 关闭（AutoStop 模式），则说完自动停；
 *       若 AEC 开启（Realtime 模式），则持续双向对话。
 */
ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

/**
 * 软重启设备
 * 
 * 操作顺序：
 *   ① 关闭音频通道（通知服务器）
 *   ② 释放通信协议对象
 *   ③ 停止音频服务（释放硬件）
 *   ④ 等待 1 秒（让数据写完）
 *   ⑤ 调用 esp_restart() —— ESP-IDF 提供的系统重启函数
 */
void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();   // ← 永远不会返回
}

/**
 * OTA 固件升级
 * 
 * 流程：
 *   ① 关闭音频通道
 *   ② 弹升级通知（等 3 秒让用户看到）
 *   ③ 切换到 Upgrading 状态
 *   ④ 停止音频服务 → 提升性能 → 下载固件
 *   ⑤ 成败判断：失败则恢复音频服务继续运行，成功则立即重启
 * 
 * @param url     固件下载 URL（HTTP/HTTPS）
 * @param version 版本号字符串（仅用于屏幕显示）
 * @return true  表示升级成功并即将重启（实际上此函数不会返回 true 给调用者）
 *         false 表示升级失败，设备继续运行
 */
bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // ① 关闭音频通道
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    // ② 弹通知 + 等 3 秒
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // ③ 切升级状态
    SetDeviceState(kDeviceStateUpgrading);
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    // ④ 提升性能 → 停止音频 → 下载固件
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 静态方法 Upgrade() 下载固件到 OTA 分区，进度通过回调传递
    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    // ⑤ 成败处理
    if (!upgrade_success) {
        // 失败 → 恢复音频服务，设备继续运行
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // 成功 → 短暂显示"升级成功" → 重启
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        Reboot();
        return true;  // Reboot() 会先于此处执行，此行理论上不会返回给调用者
    }
}

/**
 * 手动触发唤醒词（由 MCP 远程指令调用）
 * 
 * 行为如同真的检测到唤醒词：
 *   - Idle  → 编码唤醒词 → 打开通道 → 对话
 *   - Speaking → 中止说话
 *   - Listening → 关闭通道，回 Idle
 */
void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

/**
 * 检查是否可进入睡眠模式
 * 
 * 满足三个条件方可睡眠：
 *   ① 设备状态为 Idle（不在对话中）
 *   ② 音频通道已关闭
 *   ③ 音频服务空闲（不在播放音效）
 */
bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }
    if (!audio_service_.IsIdle()) {
        return false;
    }
    return true;
}

/**
 * 向服务器发送 MCP 消息（JSON-RPC 2.0）
 * 
 * 内部通过 Schedule() 投递到主线程以确保线程安全。
 * 因为 protocol_->SendMcpMessage() 内部可能涉网络操作，
 * 必须在主线程上下文中执行。
 */
void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

/**
 * 设置 AEC（回声消除）模式
 * 
 * 先保存模式值，再通过 Schedule 投递到主线程执行实际切换逻辑。
 * 切换后若音频通道已打开，则自动关闭重新建立。
 * （因为 AEC 模式更改会影响音频流的格式，需要重新协商）
 */
void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // AEC 模式变更后，关闭音频通道以触发重新协商
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

/**
 * 播放本地音效
 * 
 * 委托给 audio_service_，支持 OGG 音频文件。
 * 用于播放提示音、成功音、错误音等。
 */
void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

/**
 * 重置协议资源（线程安全）
 * 
 * 关闭音频通道，释放 protocol_ 对象。
 * 用于 WIFI 切换或服务器地址变更后重新连接。
 * 
 * @note 此方法不会立即重置——通过 Schedule() 投递到主线程安全执行
 */
void Application::ResetProtocol() {
    Schedule([this]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        protocol_.reset();
    });
}

