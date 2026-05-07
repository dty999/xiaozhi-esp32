// ============================================================
// 头文件引入 —— 各司其职
// ============================================================

// 日志宏（ESP_LOGI/ESP_LOGE 等），输出调试信息至串口
#include <esp_log.h>
// 错误码定义（ESP_OK/ESP_FAIL 等），用于判断操作成败
#include <esp_err.h>
// NVS 键值对存储接口，用于读写配置项（如 WiFi 密码、音量等）
#include <nvs.h>
// NVS Flash 初始化接口，上电后必须调用以启用持久化存储
#include <nvs_flash.h>
// GPIO 引脚控制接口，读写电平、配置输入输出模式
#include <driver/gpio.h>
// 事件循环框架，系统内各模块通过事件总线松耦合通信
#include <esp_event.h>
// FreeRTOS 实时操作系统内核，提供任务调度与同步原语
#include <freertos/FreeRTOS.h>
// FreeRTOS 任务创建与管理 API
#include <freertos/task.h>

// 应用主类头文件，封装了设备初始化与主循环的全部逻辑
#include "application.h"

// 日志标签，标识此模块的日志来源（串口输出时显示为 "[main]"）
#define TAG "main"

/**
 * @brief 程序入口函数 —— ESP32 固件之起点
 * 
 * 此函数乃固件上电后执行的第一个用户函数。
 * ESP-IDF 框架规定：入口函数必须声明为 `extern "C"`，
 * 以防止 C++ 的名称修饰（name mangling）导致链接失败。
 * 
 * 其职责有二：
 *  1. 初始化 NVS Flash（非易失性存储），使系统可持久化保存配置
 *  2. 启动应用主类 Application，进入主事件循环
 * 
 * @note 此函数仅在设备上电 / 复位时执行一次
 */
extern "C" void app_main(void)
{
    // --------------------------------------------------
    // 步骤一：初始化 NVS Flash 存储
    // --------------------------------------------------
    // NVS (Non-Volatile Storage) 是 ESP32 提供的键值对存储机制，
    // 底层基于 Flash 实现，在断电后数据不会丢失。
    // 系统依赖 NVS 保存：WiFi 名称与密码、服务器地址、音量等用户配置。
    esp_err_t ret = nvs_flash_init();

    // 若初始化失败，可能因 Flash 数据损坏或版本不兼容。
    // 此时需擦除整个 NVS 分区，重新初始化（牺牲已存配置以换取可用性）
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 发出警告日志，告知用户 NVS 将被擦除
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        // 擦除整个 NVS 分区（调用失败会自动断言，停止执行）
        ESP_ERROR_CHECK(nvs_flash_erase());
        // 擦除后重新尝试初始化
        ret = nvs_flash_init();
    }
    // 若仍失败，则触发断言，固件停止运行（因无 NVS 系统无法正常工作）
    ESP_ERROR_CHECK(ret);

    // --------------------------------------------------
    // 步骤二：启动应用主类
    // --------------------------------------------------
    // Application 为单例类（Singleton），全局唯一实例。
    // GetInstance() 返回其引用，确保整个系统中只有一个应用对象。
    auto& app = Application::GetInstance();

    // Initialize() 执行各模块的初始化：
    //   - 读取 NVS 中的配置（WiFi、服务器地址等）
    //   - 初始化硬件（屏幕、音频、LED 等）
    //   - 注册 MCP 工具（设备功能注册）
    //   - 建立网络连接（WiFi 或 4G）
    //   - 启动音频采集与处理线程
    app.Initialize();

    // Run() 进入主事件循环（无限循环，永不返回）：
    //   - 监听网络消息（来自云端服务器的指令）
    //   - 调度 MCP 工具调用（控制外设）
    //   - 处理 OTA 升级请求
    //   - 响应按键事件
    // 此函数阻塞执行，直至设备断电或复位。
    app.Run();  // 一去不复返 —— 主循环永无止境
}
