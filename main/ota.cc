#include "ota.h"         // OTA 类头文件：声明了本文件中所有函数的原型
#include "system_info.h" // 系统信息：获取 MAC 地址、User-Agent 等
#include "settings.h"    // 设置管理：读写 NVS（非易失性存储）中的配置
#include "assets/lang_config.h" // 语言配置：Accept-Language 请求头

// FreeRTOS 是 ESP32 使用的实时操作系统
#include <freertos/FreeRTOS.h> // FreeRTOS 核心头文件
#include <freertos/task.h>     // FreeRTOS 任务管理（延时、任务切换等）

#include <cJSON.h>           // JSON 解析库，用于解析服务器返回的配置
#include <esp_log.h>         // ESP32 日志系统（ESP_LOGI/LOGE 等）
#include <esp_partition.h>   // ESP32 闪存分区管理（读取分区表）
#include <esp_ota_ops.h>     // ESP32 OTA 操作：分区切换、固件写入等
#include <esp_app_format.h>  // ESP32 固件格式定义（如 esp_app_desc_t）
#include <esp_efuse.h>       // ESP32 eFuse（电子熔丝，一次性可编程存储器）
#include <esp_efuse_table.h> // eFuse 字段定义表
#include <esp_heap_caps.h>   // ESP32 内存分配管理（区分内部 RAM / PSRAM）

// 如果芯片支持 HMAC 硬件加速，则引入对应头文件
// HMAC 用于设备激活时的加密签名
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>     // C 风格字符串操作（strcmp, memcpy 等）
#include <vector>      // C++ 动态数组容器
#include <sstream>     // C++ 字符串流（用于版本号分割）
#include <algorithm>   // C++ 算法库（如 min, max）

// 定义本模块的日志标签，ESP_LOG 系列宏会使用此标签打印日志
#define TAG "Ota"


/**
 * @brief 构造函数：创建 Ota 对象时执行
 * 
 * 此处尝试从 eFuse 读取设备的序列号。
 * eFuse 是 ESP32 芯片上的一块一次性可编程存储区，出厂后可由用户写入数据，
 * 此后不可更改，常用于存储设备唯一标识、安全配置等。
 * 
 * USER_DATA 块是 eFuse 中留给用户自定义数据的区域，此处最多存储 32 字节。
 */
Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA // 仅在芯片有用户数据 eFuse 块时编译此段
    // 定义缓冲区存放序列号，33 字节为 32 字节数据 + 1 个字符串结束符 '\0'
    uint8_t serial_number[33] = {0};
    
    // 从 eFuse USER_DATA 块读取 32*8=256 位（即 32 字节）数据
    // esp_efuse_read_field_blob: ESP32 提供的 eFuse 位读取函数
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        // 检查第一个字节是否为 0：若为 0 表示没有写入序列号
        if (serial_number[0] == 0) {
            has_serial_number_ = false; // 标记为无序列号
        } else {
            // reinterpret_cast: C++ 类型转换，将 uint8_t* 转为 char* 以构造字符串
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;  // 标记为有序列号
        }
    }
#endif
}

/**
 * @brief 析构函数：Ota 对象销毁时执行
 * 
 * 当前无特殊资源需要释放，故为空实现。
 * 若未来需要释放动态内存或关闭句柄，可在此处添加。
 */
Ota::~Ota() {
}

/**
 * @brief 获取 OTA 版本检查的服务器 URL
 * 
 * 优先从 NVS（非易失性存储）的 "wifi" 命名空间中读取 "ota_url" 键值。
 * NVS 是 ESP32 的闪存键值存储系统，掉电不丢失，用于保存用户配置。
 * 
 * 若 NVS 中未设置，则回退使用编译时通过 menuconfig 配置的 CONFIG_OTA_URL。
 * 
 * @return std::string OTA 服务器 URL 地址
 */
std::string Ota::GetCheckVersionUrl() {
    // Settings("wifi", false): 打开名为 "wifi" 的 NVS 命名空间，false=只读
    Settings settings("wifi", false);
    
    // 从 NVS 中读取字符串类型的 "ota_url" 配置
    std::string url = settings.GetString("ota_url");
    
    // 若 NVS 中无此配置（返回空字符串），则使用编译时配置的默认值
    if (url.empty()) {
        url = CONFIG_OTA_URL; // CONFIG_OTA_URL 由 sdkconfig / menuconfig 生成
    }
    return url;
}

/**
 * @brief 创建并配置 HTTP 客户端
 * 
 * 每个 HTTP 请求都需要一套请求头，本函数统一设置设备身份信息。
 * 返回的 http 对象会自动释放（std::unique_ptr 智能指针管理）。
 * 
 * 设置的请求头说明：
 * - Activation-Version: 激活协议版本（有序列号为 "2"，否则为 "1"）
 * - Device-Id: 设备 MAC 地址，作为设备唯一硬件标识
 * - Client-Id: 设备 UUID，软件层面的设备标识
 * - Serial-Number: 设备序列号（如有）
 * - User-Agent: 标识设备型号和固件版本
 * - Accept-Language: 用户语言偏好
 * - Content-Type: 请求体格式为 JSON
 * 
 * @return std::unique_ptr<Http> 配置好的 HTTP 客户端智能指针
 */
std::unique_ptr<Http> Ota::SetupHttp() {
    // Board::GetInstance(): 获取开发板单例对象（全局唯一的板级管理器）
    auto& board = Board::GetInstance();
    
    // 从板级对象获取网络接口（WiFi / 以太网等）
    auto network = board.GetNetwork();
    
    // CreateHttp(0): 创建 HTTP 客户端实例，参数 0 为默认配置
    auto http = network->CreateHttp(0);
    
    // 获取系统生成的 User-Agent 字符串（包含设备型号、固件版本等）
    auto user_agent = SystemInfo::GetUserAgent();
    
    // 设置激活协议版本号
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    
    // 设置设备 MAC 地址请求头
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    
    // 设置设备 UUID 请求头
    http->SetHeader("Client-Id", board.GetUuid());
    
    // 若有序列号，添加到请求头，并打印日志
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", 
                 user_agent.c_str(), serial_number_.c_str());
    }
    
    // 设置 User-Agent 和语言
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    
    // 声明请求体为 JSON 格式
    http->SetHeader("Content-Type", "application/json");

    // 返回配置好的 HTTP 对象（使用 move 语义转移所有权）
    return http;
}

/* 
 * @brief 检查服务器是否有新版本固件
 * 
 * 向 OTA 服务器发送请求，获取最新固件信息、MQTT/Websocket 配置、
 * 设备激活信息以及服务器时间。服务器 API 规范详见飞书文档。
 * 
 * 通信流程：
 * 1. 获取当前运行的固件版本号
 * 2. 从 NVS 或编译配置获取 OTA 服务器 URL
 * 3. 创建 HTTP 连接，发送设备信息（POST）或简单查询（GET）
 * 4. 解析服务器返回的 JSON 响应
 * 5. 提取 firmware / mqtt / websocket / activation / server_time 等字段
 * 6. 判断是否有新版本需要升级
 * 
 * 规范文档：https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    // 获取板级单例对象
    auto& board = Board::GetInstance();
    
    // esp_app_get_description(): 获取当前运行固件的描述信息结构体
    // 返回的指针指向固件头部嵌入的元数据，包含版本号、项目名称等
    auto app_desc = esp_app_get_description();

    // 记录当前固件版本号（字符串形式，如 "1.0.0"）
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    // 获取 OTA 检查 URL
    std::string url = GetCheckVersionUrl();
    
    // 简单校验 URL 是否有效（长度小于 10 视为无效）
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG; // 返回无效参数错误码
    }

    // 创建并配置 HTTP 客户端（自动设置设备身份请求头）
    auto http = SetupHttp();

    // 获取设备系统信息（JSON 字符串），包含设备状态、能力等
    std::string data = board.GetSystemInfoJson();
    
    // 若有系统信息，使用 POST 请求发送；否则使用 GET 请求
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data)); // std::move: 转移字符串所有权，避免复制

    // 打开 HTTP 连接，发送请求
    // Open 方法内部会执行 DNS 解析、TCP 连接、TLS 握手（HTTPS 时）、发送 HTTP 请求
    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error; // 返回底层网络错误码
    }

    // 获取 HTTP 响应状态码
    auto status_code = http->GetStatusCode();
    if (status_code != 200) { // 200 OK 为正常响应
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code; // 返回 HTTP 错误状态码
    }

    // 读取服务器返回的全部响应体（JSON 字符串）
    data = http->ReadAll();
    http->Close(); // 关闭 HTTP 连接，释放网络资源

    // Response 示例: { "firmware": { "version": "1.0.0", "url": "http://..." } }
    // 解析 JSON 响应，判断是否有新版本
    
    // cJSON_Parse: 将 JSON 字符串解析为内存中的树形结构
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE; // 返回无效响应错误码
    }

    // ========== 解析 activation（设备激活）字段 ==========
    // 服务器可能要求设备激活，返回激活码和挑战值
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    
    // cJSON_GetObjectItem: 从 JSON 对象中获取指定名称的字段
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) { // 确认该字段存在且为 JSON 对象
        // 读取激活提示消息（如 "请扫描二维码激活"）
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        
        // 读取激活码（用于用户界面展示，如 "ABCD-1234"）
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        
        // 读取挑战值（用于 HMAC 签名，防止重放攻击）
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        
        // 读取激活超时时间（毫秒）
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    // ========== 解析 mqtt 字段并保存到 NVS ==========
    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        // Settings("mqtt", true): 打开 "mqtt" 命名空间，true=需要写入（自动创建）
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        
        // cJSON_ArrayForEach: 遍历 JSON 对象的所有键值对
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                // 若 NVS 中的值与服务器返回的不同，则更新 NVS
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true; // 标记已获取 MQTT 配置
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    // ========== 解析 websocket 字段并保存到 NVS（逻辑与 mqtt 相同）==========
    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    // ========== 解析 server_time 字段并设置系统时间 ==========
    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间：struct timeval 是 POSIX 标准的时间结构体
            struct timeval tv;
            double ts = timestamp->valuedouble; // 毫秒时间戳
            
            // 若服务器提供了时区偏移（分钟），加到时间戳上
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 分钟转毫秒
            }
            
            // tv_sec: 秒数部分；tv_usec: 微秒部分（1秒 = 1,000,000微秒）
            tv.tv_sec = (time_t)(ts / 1000);  // 毫秒转秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000; // 余数转微秒
            settimeofday(&tv, NULL); // 设置系统实时时钟
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    // ========== 解析 firmware 字段，判断是否有新版本 ==========
    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        // 读取服务器上的最新固件版本号
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        
        // 读取固件下载地址
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // 比较版本号：如 0.1.0 > 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            
            // 若服务器设置 force 标志为 1，则强制升级（忽略版本号比较）
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    // 释放 cJSON 解析分配的内存，防止内存泄漏
    cJSON_Delete(root);
    return ESP_OK; // 返回成功
}

/**
 * @brief 标记当前固件为有效状态
 * 
 * ESP32 OTA 机制支持"回滚"功能：新固件首次启动后，若未显式标记为有效，
 * 下次重启会自动回退到旧版本。此函数在确认新固件运行正常后调用，
 * 取消回滚标记，使新固件永久生效。
 * 
 * 说明：
 * - factory 分区为出厂固件，不需要标记（永远有效）
 * - ESP_OTA_IMG_PENDING_VERIFY: 固件处于"待验证"状态
 * - esp_ota_mark_app_valid_cancel_rollback(): 标记有效并取消回滚
 */
void Ota::MarkCurrentVersionValid() {
    // 获取当前正在运行的闪存分区信息
    auto partition = esp_ota_get_running_partition();
    
    // strcmp: C 字符串比较函数，返回 0 表示相等
    // 若当前运行在出厂分区（factory），则跳过标记
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    
    // 获取当前分区的 OTA 状态
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    // 若状态为"待验证"，则标记为有效
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

/**
 * @brief 执行固件下载与刷写（OTA 升级核心函数）
 * 
 * 此函数为静态函数，不依赖 Ota 对象实例，可被直接调用。
 * 整个升级过程分为以下步骤：
 * 
 * 1. 获取 OTA 更新分区（ESP32 闪存中专门用于存放新固件的分区）
 * 2. 建立 HTTP GET 连接，下载固件二进制数据
 * 3. 分块读取数据到内存缓冲区（每次 4KB）
 * 4. 校验固件头部结构（确保是合法的 ESP32 固件）
 * 5. 将缓冲区数据写入 OTA 分区（通过 esp_ota_write）
 * 6. 下载完成后验证固件完整性
 * 7. 设置启动分区为新固件分区
 * 
 * @param firmware_url 固件下载地址（HTTP/HTTPS URL）
 * @param callback 进度回调函数，参数为 (progress百分比, 当前下载速度字节/秒)
 * @return bool true=升级成功, false=升级失败
 */
bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    
    // esp_ota_handle_t: OTA 操作句柄，后续写入和结束操作都需要它
    esp_ota_handle_t update_handle = 0;
    
    // 获取下一个可用于更新的闪存分区
    // ESP32 通常配置两个 OTA 分区（ota_0 和 ota_1），交替升级
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", 
             update_partition->label, update_partition->address);
    
    // 标记固件头部是否已校验
    bool image_header_checked = false;
    // 临时存储固件头部数据（用于校验）
    std::string image_header;

    // 创建 HTTP 客户端用于下载固件（与 SetupHttp 不同，此处不设置额外请求头）
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 以 GET 方法打开固件下载 URL
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    // 检查 HTTP 响应码，必须为 200（OK）
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    // 获取响应体的总长度（Content-Length），用于计算下载进度
    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    // ESP32 的闪存写入必须以页为单位（4KB），故分配 4096 字节缓冲区
    // heap_caps_malloc: 从指定内存区域分配内存
    // MALLOC_CAP_INTERNAL: 要求从 ESP32 内部 SRAM 分配（DMA 操作需要）
    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    // buffer_offset: 当前缓冲区中已填充的字节数
    size_t buffer_offset = 0;
    // total_read: 总共下载的字节数；recent_read: 最近 1 秒内下载的字节数
    size_t total_read = 0, recent_read = 0;
    // last_calc_time: 上次计算速度的时间戳（微秒）
    auto last_calc_time = esp_timer_get_time();
    
    // 循环读取 HTTP 响应数据，直到全部下载完成
    while (true) {
        // 从 HTTP 连接读取数据到缓冲区剩余空间
        // ret: 实际读取到的字节数；0 表示数据已读完；负数表示出错
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            heap_caps_free(buffer); // 出错时释放缓冲区内存
            return false;
        }

        // 计算下载速度和进度（每秒更新一次或数据读完时）
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        
        // esp_timer_get_time(): 返回开机以来的微秒数
        // 1000000 微秒 = 1 秒
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length; // 计算百分比
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", 
                     progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read); // 调用回调通知上层更新 UI
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        // 首次收到足够数据时，校验固件头部结构
        if (!image_header_checked) {
            // 将缓冲区数据追加到 image_header 字符串
            image_header.append(buffer, buffer_offset);
            
            // 检查头部是否足够大：
            // esp_image_header_t: 固件魔数和段数
            // esp_image_segment_header_t: 段加载地址和长度
            // esp_app_desc_t: 固件描述信息（版本号、项目名等）
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                // 计算 esp_app_desc_t 结构体在固件中的偏移位置并拷贝出来
                memcpy(&new_app_info, 
                       image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), 
                       sizeof(esp_app_desc_t));

                // 开始 OTA 写入：擦除目标分区并准备接收数据
                // OTA_WITH_SEQUENTIAL_WRITES: 优化连续写入模式，减少擦写次数
                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle); // 出错时中止 OTA
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true; // 头部校验通过
                std::string().swap(image_header); // 清空临时字符串，释放内存
            }
        }

        // 当缓冲区满（4KB）或数据全部读完时，写入闪存
        bool is_last_chunk = (ret == 0); // ret==0 表示数据流结束
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            // esp_ota_write: 将缓冲区数据写入 OTA 分区
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle); // 写入失败时中止 OTA
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0; // 重置缓冲区偏移，准备接收下一批数据
        }

        // 若数据全部读完，退出循环
        if (is_last_chunk) {
            break;
        }
    }
    
    // 关闭 HTTP 连接，释放缓冲区
    http->Close();
    heap_caps_free(buffer);

    // 结束 OTA 写入，验证固件完整性（如 CRC 校验）
    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    // 设置下次启动时从刚写入的分区启动
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

/**
 * @brief 使用已保存的固件 URL 启动升级
 * 
 * CheckVersion() 成功后，firmware_url_ 中已存储服务器返回的固件下载地址。
 * 本函数直接使用该地址调用 Upgrade()。
 * 
 * @param callback 进度回调函数
 * @return bool true=成功, false=失败
 */
bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


/**
 * @brief 将版本号字符串解析为数字数组
 * 
 * 例如 "1.2.3" 解析为 [1, 2, 3]。
 * 使用 std::stringstream 和 std::getline 按 '.' 分割字符串。
 * 
 * @param version 版本号字符串（如 "1.0.0"）
 * @return std::vector<int> 版本号各段的数字数组
 */
std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers; // 存储解析后的版本数字
    std::stringstream ss(version);    // 将字符串包装为输入流
    std::string segment;
    
    // std::getline: 从流中读取字符串，第三个参数为分隔符
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment)); // stoi: 字符串转整数
    }
    
    return versionNumbers;
}

/**
 * @brief 比较两个版本号，判断 newVersion 是否比 currentVersion 更新
 * 
 * 逐段比较版本号数字，如 [1, 0, 0] vs [1, 0, 1]，后者更新。
 * 若前面所有段都相同，段数更多的版本视为更新。
 * 
 * @param currentVersion 当前版本号
 * @param newVersion 服务器上的新版本号
 * @return bool true=有新版本, false=无新版本或版本相同
 */
bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    // std::min: 取两个数组长度中的较小值，逐段比较
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;   // 新版本某段更大，确实有更新
        } else if (newer[i] < current[i]) {
            return false;  // 新版本某段更小，无需更新（可能是降级）
        }
        // 相等则继续比较下一段
    }
    
    // 前面段都相同，若新版本段数更多，则视为更新（如 1.0 < 1.0.1）
    return newer.size() > current.size();
}

/**
 * @brief 构建设备激活的请求载荷（JSON 格式）
 * 
 * 设备激活流程：
 * 1. 服务器返回 challenge（随机挑战值）
 * 2. 设备使用芯片内置 HMAC 密钥对 challenge 计算签名
 * 3. 设备将序列号、challenge、签名发送回服务器验证
 * 
 * HMAC-SHA256 是一种消息认证码算法，确保消息未被篡改。
 * SOC_HMAC_SUPPORTED: 部分 ESP32 芯片（如 ESP32-S2/S3/C3）支持硬件 HMAC 加速。
 * 
 * @return std::string 激活请求的 JSON 载荷
 */
std::string Ota::GetActivationPayload() {
    // 若无序列号，无法完成激活，返回空 JSON
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex; // 存储 HMAC 结果的十六进制字符串
    
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出固定为 32 字节（256 位）
    
    // 使用 eFuse 中预烧录的 Key0 计算 HMAC
    // HMAC_KEY0: ESP32 的 HMAC 密钥槽位 0
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, 
                                       (uint8_t*)activation_challenge_.data(), 
                                       activation_challenge_.size(), 
                                       hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    // 将 32 字节二进制数据转换为 64 字符十六进制字符串（每字节转 2 位十六进制）
    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3]; // 2 位十六进制 + 字符串结束符
        sprintf(buffer, "%02x", hmac_result[i]); // %02x: 输出 2 位小写十六进制，不足补 0
        hmac_hex += buffer;
    }
#endif

    // 使用 cJSON 构建激活请求 JSON 对象
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256"); // 签名算法
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str()); // 设备序列号
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str()); // 挑战值
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str()); // HMAC 签名结果
    
    // cJSON_PrintUnformatted: 将 JSON 对象序列化为紧凑字符串（无换行和缩进）
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);   // cJSON_PrintUnformatted 分配的内存需要 cJSON_free 释放
    cJSON_Delete(payload);  // 释放 JSON 对象本身

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

/**
 * @brief 向服务器发送设备激活请求
 * 
 * 调用流程：
 * 1. 检查是否有激活挑战值（CheckVersion 时服务器下发的 challenge）
 * 2. 构造激活 URL：{ota_url}/activate
 * 3. 构建包含 HMAC 签名的激活载荷
 * 4. 发送 POST 请求到服务器
 * 5. 根据状态码判断激活结果：
 *    - 200: 激活成功
 *    - 202: 服务器正在处理，需稍后重试
 *    - 其他: 激活失败
 * 
 * @return esp_err_t ESP_OK=成功, ESP_ERR_TIMEOUT=需重试, ESP_FAIL=失败
 */
esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    // 构造激活请求 URL，确保末尾有 "/activate" 路径
    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    // 创建 HTTP 客户端并配置请求头
    auto http = SetupHttp();

    // 构建激活请求体（JSON）
    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    // 发送 POST 请求
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        // HTTP 202 Accepted: 请求已接受但尚未处理完成
        return ESP_ERR_TIMEOUT; // 返回超时错误，调用方应稍后重试
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", 
                 status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
