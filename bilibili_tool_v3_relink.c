#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <curl/curl.h>
#include <jansson.h>
#include <time.h>
#include <direct.h>
#include <process.h>

// 设置控制台编码为UTF-8
void set_console_utf8() {
    system("chcp 65001 >nul");
    SetConsoleOutputCP(65001);
}

// 直播间信息结构体
typedef struct {
    int room_id;
    int live_status;
    char title[512];
    char anchor[256];
    int online;
    char cover_url[512];
    char room_name[512];  // 新增：直播间名称
    int popularity;       // 新增：直播间人气值
} LiveRoomInfo;

// 用户凭证结构体
typedef struct {
    char sessdata[512];
    char bili_jct[512];
    char buvid3[512];
    int uid;
} UserCredentials;

// 配置结构体
typedef struct {
    char greeting_msg[256];    // 开播问候语
    int auto_greeting;         // 自动问候开关
    int recording_enabled;     // 录播开关
    char ffmpeg_path[512];     // FFmpeg路径
    char save_path[512];       // 录像保存路径
    int debug_mode;            // 调试模式
    int last_room_id;          // 上次使用的房间号
    int monitoring_enabled;    // 监控开关
} AppConfig;

// 录播任务结构体
typedef struct {
    int room_id;
    char title[512];
    char anchor[256];
    HANDLE process_handle;
    int is_recording;
    time_t start_time;
} RecordingTask;

// 内存存储结构体
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

UserCredentials g_cred = {0};
AppConfig g_config = {0};
RecordingTask g_recording = {0};
int g_monitoring_active = 0;  // 监控状态标志
int g_should_stop_monitoring = 0; // 停止监控标志

// 函数声明
void stop_monitoring();
void start_monitoring(int room_id);
void monitor_live_status(int room_id);
DWORD WINAPI keyboard_listener(LPVOID lpParam);
void init_default_config();
void save_config();
void save_credentials();
void load_config();
void load_credentials();
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);
int http_get(const char *url, const char *cookies, char **response);
int http_post(const char *url, const char *post_data, const char *cookies, char **response);
char* build_cookie_string(const UserCredentials *cred);
int parse_json_response(const char *json_data, const char *field, char *value, size_t value_size);
int get_live_stream_url(int room_id, char *stream_url, size_t url_size);
int start_recording(int room_id, const char *title, const char *anchor);
int stop_recording();
int check_bilibili_live_status(int room_id, LiveRoomInfo *room_info);
int send_danmaku(int room_id, const char *message);
void setup_credentials();
void setup_greeting();
void setup_recording();
void toggle_debug_mode();
void toggle_monitoring_setting();
void print_live_info(const LiveRoomInfo *room_info);

// 初始化默认配置
void init_default_config() {
    strcpy(g_config.greeting_msg, "主播开播啦，大家快来看呀！");
    g_config.auto_greeting = 1;
    g_config.recording_enabled = 0;
    g_config.debug_mode = 0;
    g_config.last_room_id = 0;
    g_config.monitoring_enabled = 0;
    
    // 检查是否内置FFmpeg
    char exe_path[MAX_PATH];
    GetModuleFileName(NULL, exe_path, MAX_PATH);
    char *last_slash = strrchr(exe_path, '\\');
    if (last_slash) {
        *last_slash = '\0';
        char builtin_ffmpeg[MAX_PATH];
        snprintf(builtin_ffmpeg, sizeof(builtin_ffmpeg), "%s\\ffmpeg.exe", exe_path);
        
        if (GetFileAttributes(builtin_ffmpeg) != INVALID_FILE_ATTRIBUTES) {
            strcpy(g_config.ffmpeg_path, builtin_ffmpeg);
            printf("✅ 检测到内置FFmpeg: %s\n", builtin_ffmpeg);
        } else {
            strcpy(g_config.ffmpeg_path, "ffmpeg");
        }
    } else {
        strcpy(g_config.ffmpeg_path, "ffmpeg");
    }
    
    strcpy(g_config.save_path, "./recordings");
    
    // 创建保存目录
    _mkdir(g_config.save_path);
}

// 保存配置到文件
void save_config() {
    FILE *fp = fopen("bilibili_tool_v3_config.json", "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"greeting_msg\": \"%s\",\n", g_config.greeting_msg);
        fprintf(fp, "  \"auto_greeting\": %d,\n", g_config.auto_greeting);
        fprintf(fp, "  \"recording_enabled\": %d,\n", g_config.recording_enabled);
        fprintf(fp, "  \"debug_mode\": %d,\n", g_config.debug_mode);
        fprintf(fp, "  \"ffmpeg_path\": \"%s\",\n", g_config.ffmpeg_path);
        fprintf(fp, "  \"save_path\": \"%s\",\n", g_config.save_path);
        fprintf(fp, "  \"last_room_id\": %d,\n", g_config.last_room_id);
        fprintf(fp, "  \"monitoring_enabled\": %d\n", g_config.monitoring_enabled);
        fprintf(fp, "}\n");
        fclose(fp);
    }
}

// 保存用户凭证
void save_credentials() {
    FILE *fp = fopen("bilibili_v3_credentials.json", "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"sessdata\": \"%s\",\n", g_cred.sessdata);
        fprintf(fp, "  \"bili_jct\": \"%s\",\n", g_cred.bili_jct);
        fprintf(fp, "  \"buvid3\": \"%s\",\n", g_cred.buvid3);
        fprintf(fp, "  \"uid\": %d\n", g_cred.uid);
        fprintf(fp, "}\n");
        fclose(fp);
    }
}

// 加载配置
void load_config() {
    FILE *fp = fopen("bilibili_tool_v3_config.json", "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        char *buffer = malloc(size + 1);
        fread(buffer, 1, size, fp);
        buffer[size] = '\0';
        fclose(fp);
        
        json_error_t error;
        json_t *root = json_loads(buffer, 0, &error);
        if (root) {
            json_t *msg = json_object_get(root, "greeting_msg");
            json_t *greeting = json_object_get(root, "auto_greeting");
            json_t *recording = json_object_get(root, "recording_enabled");
            json_t *debug = json_object_get(root, "debug_mode");
            json_t *ffmpeg = json_object_get(root, "ffmpeg_path");
            json_t *save = json_object_get(root, "save_path");
            json_t *last_room = json_object_get(root, "last_room_id");
            json_t *monitoring = json_object_get(root, "monitoring_enabled");
            
            if (msg && json_is_string(msg)) 
                strcpy(g_config.greeting_msg, json_string_value(msg));
            if (greeting && json_is_integer(greeting)) 
                g_config.auto_greeting = json_integer_value(greeting);
            if (recording && json_is_integer(recording)) 
                g_config.recording_enabled = json_integer_value(recording);
            if (debug && json_is_integer(debug)) 
                g_config.debug_mode = json_integer_value(debug);
            if (ffmpeg && json_is_string(ffmpeg)) 
                strcpy(g_config.ffmpeg_path, json_string_value(ffmpeg));
            if (save && json_is_string(save)) 
                strcpy(g_config.save_path, json_string_value(save));
            if (last_room && json_is_integer(last_room))
                g_config.last_room_id = json_integer_value(last_room);
            if (monitoring && json_is_integer(monitoring))
                g_config.monitoring_enabled = json_integer_value(monitoring);
            
            json_decref(root);
        }
        free(buffer);
    } else {
        init_default_config();
    }
}

// 加载用户凭证
void load_credentials() {
    FILE *fp = fopen("bilibili_v3_credentials.json", "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        char *buffer = malloc(size + 1);
        fread(buffer, 1, size, fp);
        buffer[size] = '\0';
        fclose(fp);
        
        json_error_t error;
        json_t *root = json_loads(buffer, 0, &error);
        if (root) {
            json_t *sessdata = json_object_get(root, "sessdata");
            json_t *bili_jct = json_object_get(root, "bili_jct");
            json_t *buvid3 = json_object_get(root, "buvid3");
            json_t *uid = json_object_get(root, "uid");
            
            if (sessdata && json_is_string(sessdata)) 
                strcpy(g_cred.sessdata, json_string_value(sessdata));
            if (bili_jct && json_is_string(bili_jct)) 
                strcpy(g_cred.bili_jct, json_string_value(bili_jct));
            if (buvid3 && json_is_string(buvid3)) 
                strcpy(g_cred.buvid3, json_string_value(buvid3));
            if (uid && json_is_integer(uid)) 
                g_cred.uid = json_integer_value(uid);
            
            json_decref(root);
        }
        free(buffer);
    }
}

// libcurl写回调函数
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// HTTP GET请求
int http_get(const char *url, const char *cookies, char **response) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookies);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if(res == CURLE_OK) {
            *response = chunk.memory;
            return 0;
        } else {
            free(chunk.memory);
            return -1;
        }
    }
    return -1;
}

// HTTP POST请求
int http_post(const char *url, const char *post_data, const char *cookies, char **response) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if(curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        headers = curl_slist_append(headers, "Referer: https://live.bilibili.com");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_COOKIE, cookies);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if(res == CURLE_OK) {
            *response = chunk.memory;
            return 0;
        } else {
            free(chunk.memory);
            return -1;
        }
    }
    return -1;
}

// 构建Cookie字符串
char* build_cookie_string(const UserCredentials *cred) {
    static char cookies[1024];
    snprintf(cookies, sizeof(cookies),
             "SESSDATA=%s; bili_jct=%s; buvid3=%s; DedeUserID=%d",
             cred->sessdata, cred->bili_jct, cred->buvid3, cred->uid);
    return cookies;
}

// 解析JSON响应
int parse_json_response(const char *json_data, const char *field, char *value, size_t value_size) {
    json_error_t error;
    json_t *root = json_loads(json_data, 0, &error);
    
    if (!root) {
        printf("JSON解析错误: %s\n", error.text);
        return -1;
    }

    json_t *code = json_object_get(root, "code");
    if (!json_is_integer(code) || json_integer_value(code) != 0) {
        json_t *message = json_object_get(root, "message");
        if (message && json_is_string(message)) {
            printf("API返回错误: %s\n", json_string_value(message));
        }
        json_decref(root);
        return -1;
    }

    if (strcmp(field, "message") == 0) {
        json_t *msg = json_object_get(root, "message");
        if (msg && json_is_string(msg)) {
            strncpy(value, json_string_value(msg), value_size - 1);
            value[value_size - 1] = '\0';
            json_decref(root);
            return 0;
        }
    }

    json_decref(root);
    return -1;
}

// 获取直播流地址
int get_live_stream_url(int room_id, char *stream_url, size_t url_size) {
    char url[256];
    char *response = NULL;
    
    snprintf(url, sizeof(url), 
             "https://api.live.bilibili.com/room/v1/Room/playUrl?cid=%d&quality=0&platform=web", 
             room_id);

    if (http_get(url, "", &response) == 0 && response) {
        json_error_t error;
        json_t *root = json_loads(response, 0, &error);
        
        if (root) {
            json_t *code = json_object_get(root, "code");
            if (json_is_integer(code) && json_integer_value(code) == 0) {
                json_t *data = json_object_get(root, "data");
                if (data) {
                    json_t *durl = json_object_get(data, "durl");
                    if (durl && json_is_array(durl) && json_array_size(durl) > 0) {
                        json_t *first_url = json_array_get(durl, 0);
                        json_t *url_obj = json_object_get(first_url, "url");
                        if (url_obj && json_is_string(url_obj)) {
                            strncpy(stream_url, json_string_value(url_obj), url_size - 1);
                            stream_url[url_size - 1] = '\0';
                            free(response);
                            json_decref(root);
                            return 0;
                        }
                    }
                }
            }
            json_decref(root);
        }
        free(response);
    }
    return -1;
}

// 开始录播
int start_recording(int room_id, const char *title, const char *anchor) {
    if (g_recording.is_recording) {
        printf("❌ 已有录播任务在进行中！\n");
        return -1;
    }
    
    char stream_url[1024];
    if (get_live_stream_url(room_id, stream_url, sizeof(stream_url)) != 0) {
        printf("❌ 获取直播流地址失败！\n");
        return -1;
    }
    
    // 创建文件名（去除特殊字符）
    char safe_title[512];
    strcpy(safe_title, title);
    for (char *p = safe_title; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || 
            *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            *p = '_';
        }
    }
    
    // 处理主播名称中的特殊字符
    char safe_anchor[256];
    strcpy(safe_anchor, anchor);
    for (char *p = safe_anchor; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || 
            *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            *p = '_';
        }
    }
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/%s_%s_%04d%02d%02d_%02d%02d%02d.mp4",
             g_config.save_path, safe_anchor, safe_title,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    // 构建FFmpeg命令
    char command[4096];
    snprintf(command, sizeof(command),
             "%s -i \"%s\" -c copy -f mp4 \"%s\" -y",
             g_config.ffmpeg_path, stream_url, filename);
    
    printf("🎥 开始录播: %s\n", filename);
    printf("📺 直播流: %s\n", stream_url);
    
    // 启动FFmpeg进程
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    
    if (CreateProcess(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        g_recording.room_id = room_id;
        g_recording.process_handle = pi.hProcess;
        g_recording.is_recording = 1;
        g_recording.start_time = time(NULL);
        strncpy(g_recording.title, title, sizeof(g_recording.title)-1);
        strncpy(g_recording.anchor, anchor, sizeof(g_recording.anchor)-1);
        
        printf("✅ 录播任务已启动！\n");
        return 0;
    } else {
        printf("❌ 启动FFmpeg失败！错误代码: %d\n", GetLastError());
        return -1;
    }
}

// 停止录播
int stop_recording() {
    if (!g_recording.is_recording) {
        printf("❌ 没有正在进行的录播任务！\n");
        return -1;
    }
    
    printf("🛑 正在停止录播...\n");
    
    // 终止FFmpeg进程
    if (TerminateProcess(g_recording.process_handle, 0)) {
        WaitForSingleObject(g_recording.process_handle, 5000);
        CloseHandle(g_recording.process_handle);
        
        time_t duration = time(NULL) - g_recording.start_time;
        printf("✅ 录播已停止！持续时间: %lld秒\n", (long long)duration);
        
        g_recording.is_recording = 0;
        return 0;
    } else {
        printf("❌ 停止录播失败！错误代码: %d\n", GetLastError());
        return -1;
    }
}

// 检查直播间状态
int check_bilibili_live_status(int room_id, LiveRoomInfo *room_info) {
    char url[256];
    char *response = NULL;
    
    snprintf(url, sizeof(url), 
             "https://api.live.bilibili.com/room/v1/Room/get_info?room_id=%d", 
             room_id);

    if (http_get(url, "", &response) == 0 && response) {
        if (g_config.debug_mode) {
            printf("\n=== 调试信息：房间API响应 ===\n");
            printf("%s\n", response);
            printf("=== 调试信息结束 ===\n\n");
        }
        
        json_error_t error;
        json_t *root = json_loads(response, 0, &error);
        
        if (root) {
            json_t *code = json_object_get(root, "code");
            if (json_is_integer(code) && json_integer_value(code) == 0) {
                json_t *data = json_object_get(root, "data");
                if (data) {
                    json_t *live_status = json_object_get(data, "live_status");
                    json_t *title = json_object_get(data, "title");
                    json_t *online = json_object_get(data, "online");
                    json_t *cover = json_object_get(data, "cover");
                    json_t *uid_obj = json_object_get(data, "uid");
                    json_t *room_name = json_object_get(data, "room_name"); // 新增：直播间名称

                    if (live_status && json_is_integer(live_status)) {
                        room_info->live_status = json_integer_value(live_status);
                    }
                    if (title && json_is_string(title)) {
                        const char *title_str = json_string_value(title);
                        strncpy(room_info->title, title_str, sizeof(room_info->title)-1);
                        room_info->title[sizeof(room_info->title)-1] = '\0';
                    }
                    if (room_name && json_is_string(room_name)) {
                        const char *room_name_str = json_string_value(room_name);
                        strncpy(room_info->room_name, room_name_str, sizeof(room_info->room_name)-1);
                        room_info->room_name[sizeof(room_info->room_name)-1] = '\0';
                    } else {
                        // 如果没有room_name字段，使用title作为直播间名称
                        strncpy(room_info->room_name, room_info->title, sizeof(room_info->room_name)-1);
                    }
                    if (online && json_is_integer(online)) {
                        int popularity = json_integer_value(online);
                        room_info->popularity = popularity; // 保存原始人气值
                        
                        if (popularity > 0) {
                            if (popularity < 500) {
                                room_info->online = popularity / 5 + 1;
                            } else if (popularity < 2000) {
                                room_info->online = popularity / 10 + 1;
                            } else if (popularity < 10000) {
                                room_info->online = popularity / 20 + 1;
                            } else if (popularity < 50000) {
                                room_info->online = popularity / 40 + 1;
                            } else {
                                room_info->online = popularity / 80 + 1;
                            }
                            
                            if (g_config.debug_mode) {
                                printf("=== 调试信息：在线人数估算 ===\n");
                                printf("原始人气值: %d\n", popularity);
                                printf("估算真实在线人数: %d\n", room_info->online);
                                printf("=== 调试信息结束 ===\n\n");
                            }
                        } else {
                            room_info->online = 0;
                        }
                    }
                    if (cover && json_is_string(cover)) {
                        strncpy(room_info->cover_url, json_string_value(cover), sizeof(room_info->cover_url)-1);
                        room_info->cover_url[sizeof(room_info->cover_url)-1] = '\0';
                    }
                    
                    strncpy(room_info->anchor, "主播", sizeof(room_info->anchor)-1);
                    
                    long long uid = 0;
                    if (uid_obj && json_is_integer(uid_obj)) {
                        uid = json_integer_value(uid_obj);
                    }
                    
                    if (uid > 0) {
                        free(response);
                        response = NULL;
                        
                        snprintf(url, sizeof(url), 
                                 "https://api.live.bilibili.com/live_user/v1/UserInfo/get_anchor_in_room?roomid=%d", 
                                 room_id);
                        
                        if (http_get(url, "", &response) == 0 && response) {
                            if (g_config.debug_mode) {
                                printf("\n=== 调试信息：主播API响应 ===\n");
                                printf("%s\n", response);
                                printf("=== 调试信息结束 ===\n\n");
                            }
                            
                            json_t *user_root = json_loads(response, 0, &error);
                            if (user_root) {
                                json_t *user_code = json_object_get(user_root, "code");
                                if (json_is_integer(user_code) && json_integer_value(user_code) == 0) {
                                    json_t *user_data = json_object_get(user_root, "data");
                                    if (user_data) {
                                        json_t *info = json_object_get(user_data, "info");
                                        if (info) {
                                            json_t *uname = json_object_get(info, "uname");
                                            if (uname && json_is_string(uname)) {
                                                const char *uname_str = json_string_value(uname);
                                                strncpy(room_info->anchor, uname_str, sizeof(room_info->anchor)-1);
                                                room_info->anchor[sizeof(room_info->anchor)-1] = '\0';
                                            }
                                        }
                                    }
                                }
                                json_decref(user_root);
                            }
                        }
                    }
                    
                    if (strlen(room_info->anchor) == 0 || strcmp(room_info->anchor, "主播") == 0) {
                        free(response);
                        response = NULL;
                        
                        snprintf(url, sizeof(url), 
                                 "https://api.live.bilibili.com/xlive/web-room/v1/index/getInfoByRoom?room_id=%d", 
                                 room_id);
                        
                        if (http_get(url, "", &response) == 0 && response) {
                            json_t *alt_root = json_loads(response, 0, &error);
                            if (alt_root) {
                                json_t *alt_code = json_object_get(alt_root, "code");
                                if (json_is_integer(alt_code) && json_integer_value(alt_code) == 0) {
                                    json_t *alt_data = json_object_get(alt_root, "data");
                                    if (alt_data) {
                                        json_t *anchor_info = json_object_get(alt_data, "anchor_info");
                                        if (anchor_info) {
                                            json_t *base_info = json_object_get(anchor_info, "base_info");
                                            if (base_info) {
                                                json_t *uname = json_object_get(base_info, "uname");
                                                if (uname && json_is_string(uname)) {
                                                    const char *uname_str = json_string_value(uname);
                                                    strncpy(room_info->anchor, uname_str, sizeof(room_info->anchor)-1);
                                                    room_info->anchor[sizeof(room_info->anchor)-1] = '\0';
                                                }
                                            }
                                        }
                                    }
                                }
                                json_decref(alt_root);
                            }
                        }
                    }
                    
                    room_info->room_id = room_id;
                    if (response) free(response);
                    json_decref(root);
                    return 0;
                }
            } else {
                json_t *message = json_object_get(root, "message");
                if (message && json_is_string(message)) {
                    printf("API错误: %s\n", json_string_value(message));
                } else {
                    printf("API返回未知错误，代码: %lld\n", (long long)json_integer_value(code));
                }
            }
            json_decref(root);
        }
        if (response) free(response);
    } else {
        printf("网络请求失败，请检查网络连接\n");
    }
    return -1;
}

// 发送弹幕
int send_danmaku(int room_id, const char *message) {
    if (g_cred.uid == 0) {
        printf("错误: 请先设置用户凭证！\n");
        return -1;
    }

    char url[] = "https://api.live.bilibili.com/msg/send";
    char post_data[1024];
    char *response = NULL;
    
    time_t t = time(NULL);
    long timestamp = (long)t;
    
    snprintf(post_data, sizeof(post_data),
             "color=16777215&fontsize=25&mode=1&msg=%s&rnd=%ld&roomid=%d&bubble=0&csrf=%s&csrf_token=%s",
             message, timestamp, room_id, g_cred.bili_jct, g_cred.bili_jct);

    char *cookies = build_cookie_string(&g_cred);
    
    printf("正在发送弹幕: %s\n", message);
    
    if (http_post(url, post_data, cookies, &response) == 0 && response) {
        char result_msg[256];
        if (parse_json_response(response, "message", result_msg, sizeof(result_msg)) == 0) {
            printf("发送结果: %s\n", result_msg);
            free(response);
            return 0;
        } else {
            printf("发送失败，无法解析响应\n");
        }
        free(response);
    } else {
        printf("网络请求失败\n");
    }
    
    return -1;
}

// 设置用户凭证
void setup_credentials() {
    printf("\n=== B站账号凭证设置 ===\n");
    printf("请按照以下步骤获取凭证：\n");
    printf("1. 登录B站网页版 (https://www.bilibili.com)\n");
    printf("2. 按F12打开开发者工具\n");
    printf("3. 进入 Application/存储 -> Cookies -> https://www.bilibili.com\n");
    printf("4. 找到并复制以下值：\n\n");
    
    printf("请输入 SESSDATA (登录凭证): ");
    fgets(g_cred.sessdata, sizeof(g_cred.sessdata), stdin);
    g_cred.sessdata[strcspn(g_cred.sessdata, "\n")] = 0;
    
    printf("请输入 bili_jct (CSRF令牌): ");
    fgets(g_cred.bili_jct, sizeof(g_cred.bili_jct), stdin);
    g_cred.bili_jct[strcspn(g_cred.bili_jct, "\n")] = 0;
    
    printf("请输入 buvid3 (设备标识): ");
    fgets(g_cred.buvid3, sizeof(g_cred.buvid3), stdin);
    g_cred.buvid3[strcspn(g_cred.buvid3, "\n")] = 0;
    
    printf("请输入 DedeUserID (用户ID): ");
    char uid_str[100];
    fgets(uid_str, sizeof(uid_str), stdin);
    g_cred.uid = atoi(uid_str);
    
    save_credentials();
    printf("\n✅ 凭证设置完成并已保存！\n");
    printf("用户ID: %d\n", g_cred.uid);
}

// 设置问候语
void setup_greeting() {
    printf("\n=== 设置开播问候语 ===\n");
    printf("当前问候语: %s\n", g_config.greeting_msg);
    printf("请输入新的问候语: ");
    fgets(g_config.greeting_msg, sizeof(g_config.greeting_msg), stdin);
    g_config.greeting_msg[strcspn(g_config.greeting_msg, "\n")] = 0;
    
    save_config();
    printf("✅ 问候语设置完成！\n");
}

// 设置录播参数
void setup_recording() {
    printf("\n=== 设置录播参数 ===\n");
    printf("当前FFmpeg路径: %s\n", g_config.ffmpeg_path);
    printf("当前保存路径: %s\n", g_config.save_path);
    
    printf("是否启用录播功能？(1=启用, 0=禁用): ");
    char input[100];
    fgets(input, sizeof(input), stdin);
    g_config.recording_enabled = atoi(input);
    
    if (g_config.recording_enabled) {
        printf("请输入FFmpeg路径 (直接回车使用默认): ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) > 0) {
            strcpy(g_config.ffmpeg_path, input);
        }
        
        printf("请输入录像保存路径 (直接回车使用默认): ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) > 0) {
            strcpy(g_config.save_path, input);
            _mkdir(g_config.save_path);
        }
    }
    
    save_config();
    printf("✅ 录播设置完成！\n");
}

// 切换调试模式
void toggle_debug_mode() {
    printf("\n=== 调试模式设置 ===\n");
    printf("当前调试模式: %s\n", g_config.debug_mode ? "✅ 开启" : "❌ 关闭");
    printf("是否切换调试模式？(1=开启, 0=关闭): ");
    char input[100];
    fgets(input, sizeof(input), stdin);
    g_config.debug_mode = atoi(input);
    
    save_config();
    printf("✅ 调试模式设置完成！\n");
}

// 停止监控
void stop_monitoring() {
    if (g_monitoring_active) {
        g_should_stop_monitoring = 1;
        printf("🛑 正在停止监控...\n");
    } else {
        printf("❌ 监控模式未运行！\n");
    }
}

// 切换监控模式设置
void toggle_monitoring_setting() {
    printf("\n=== 监控模式设置 ===\n");
    printf("当前监控模式: %s\n", g_config.monitoring_enabled ? "✅ 开启" : "❌ 关闭");
    printf("是否切换监控模式？(1=开启, 0=关闭): ");
    char input[100];
    fgets(input, sizeof(input), stdin);
    int new_setting = atoi(input);
    
    if (new_setting != g_config.monitoring_enabled) {
        g_config.monitoring_enabled = new_setting;
        save_config();
        
        if (g_config.monitoring_enabled) {
            printf("✅ 监控模式已启用！\n");
            printf("💡 提示：现在可以在主菜单中选择 [5] 来启动监控\n");
        } else {
            printf("✅ 监控模式已禁用！\n");
            if (g_monitoring_active) {
                printf("🛑 检测到监控正在运行，正在停止...\n");
                stop_monitoring();
            }
        }
    } else {
        printf("ℹ️  监控模式设置未改变\n");
    }
}

// 打印直播间信息
void print_live_info(const LiveRoomInfo *room_info) {
    printf("\n==============================================\n");
    printf("📺 直播间信息\n");
    printf("==============================================\n");
    printf("  房间号: %d\n", room_info->room_id);
    printf("  直播间名称: %s\n", room_info->room_name); // 新增：直播间名称
    printf("  主播: %s\n", room_info->anchor);
    printf("  状态: %s\n", 
           room_info->live_status == 1 ? "🔴 直播中" : 
           room_info->live_status == 2 ? "⏸️ 轮播中" : "⚫ 未开播");
    printf("  标题: %s\n", room_info->title);
    printf("  人气值: %d\n", room_info->popularity); // 新增：人气值
    printf("  在线人数: %d\n", room_info->online);
    printf("==============================================\n\n");
}

// 监控直播状态
void monitor_live_status(int room_id) {
    if (g_monitoring_active) {
        printf("❌ 监控模式已在运行中！\n");
        return;
    }

    g_monitoring_active = 1;
    g_should_stop_monitoring = 0;
    
    printf("\n🎯 启动直播监控模式\n");
    printf("监控房间: %d\n", room_id);
    printf("按 Ctrl+C 或返回主菜单停止监控\n\n");
    
    int last_live_status = -1;
    time_t last_check_time = 0;
    
    while (!g_should_stop_monitoring) {
        time_t current_time = time(NULL);
        
        // 每5秒检查一次
        if (current_time - last_check_time >= 5) {
            LiveRoomInfo room_info;
            if (check_bilibili_live_status(room_id, &room_info) == 0) {
                // 状态变化检测
                if (last_live_status != room_info.live_status) {
                    printf("[%s] 状态变化: ", 
                           room_info.live_status == 1 ? "🔴 直播中" : 
                           room_info.live_status == 2 ? "⏸️ 轮播中" : "⚫ 未开播");
                    printf("%s - %s\n", room_info.anchor, room_info.title);
                    
                    // 开播检测和自动问候
                    if (last_live_status != 1 && room_info.live_status == 1) {
                        printf("🎉 检测到主播开播！\n");
                        
                        if (g_config.auto_greeting && g_cred.uid != 0) {
                            printf("💬 正在发送问候弹幕...\n");
                            send_danmaku(room_id, g_config.greeting_msg);
                        }
                        
                        // 自动开始录播
                        if (g_config.recording_enabled && !g_recording.is_recording) {
                            printf("🎥 自动开始录播...\n");
                            start_recording(room_id, room_info.title, room_info.anchor);
                        }
                    }
                    
                    // 下播检测
                    if (last_live_status == 1 && room_info.live_status != 1) {
                        printf("💤 主播下播了\n");
                        
                        // 自动停止录播
                        if (g_recording.is_recording && g_recording.room_id == room_id) {
                            printf("🛑 自动停止录播...\n");
                            stop_recording();
                        }
                    }
                    
                    last_live_status = room_info.live_status;
                }
                
                // 显示在线人数（每分钟更新一次）
                static time_t last_online_display = 0;
                if (current_time - last_online_display >= 60) {
                    printf("[%s] 在线人数: %d\n", 
                           room_info.live_status == 1 ? "🔴 直播中" : "⚫ 未开播", 
                           room_info.online);
                    last_online_display = current_time;
                }
            }
            last_check_time = current_time;
        }
        
        Sleep(1000); // 1秒延迟，减少CPU占用
    }
    
    g_monitoring_active = 0;
    printf("✅ 监控模式已停止\n");
}

// 键盘监听线程（用于快捷键）
DWORD WINAPI keyboard_listener(LPVOID lpParam) {
    printf("⌨️  快捷键监听已启动 (R=开始/停止录播, M=停止监控, ESC=退出)\n");
    
    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            printf("\n🛑 退出程序...\n");
            if (g_recording.is_recording) {
                stop_recording();
            }
            if (g_monitoring_active) {
                stop_monitoring();
            }
            exit(0);
        }
        
        if (GetAsyncKeyState('R') & 0x8000) {
            if (g_recording.is_recording) {
                printf("\n⌨️ 快捷键: 停止录播\n");
                stop_recording();
            }
            Sleep(500); // 防抖
        }
        
        if (GetAsyncKeyState('M') & 0x8000) {
            if (g_monitoring_active) {
                printf("\n⌨️ 快捷键: 停止监控\n");
                stop_monitoring();
            }
            Sleep(500); // 防抖
        }
        
        Sleep(100);
    }
    return 0;
}

// 显示主菜单
void show_main_menu() {
    printf("\n");
    printf("==============================================\n");
    printf("           B站直播监控工具 v3.0\n");
    printf("==============================================\n");
    printf("  1. 查询直播间状态\n");
    printf("  2. 发送弹幕\n");
    printf("  3. 设置账号凭证\n");
    printf("  4. 设置问候语\n");
    printf("  5. 启动监控模式\n");
    printf("  6. 设置录播参数\n");
    printf("  7. 开始录播\n");
    printf("  8. 停止录播\n");
    printf("  9. 调试模式设置\n");
    printf("  10. 监控模式设置\n");
    printf("  0. 退出程序\n");
    printf("==============================================\n");
    
    // 显示当前状态
    printf("当前状态: ");
    if (g_monitoring_active) {
        printf("🔴 监控中 | ");
    }
    if (g_recording.is_recording) {
        printf("🎥 录播中 | ");
    }
    if (g_cred.uid != 0) {
        printf("✅ 已登录 | ");
    }
    printf("调试: %s\n", g_config.debug_mode ? "✅ 开" : "❌ 关");
    printf("==============================================\n");
}

// 主函数
int main() {
    set_console_utf8();
    
    // 初始化
    printf("🚀 初始化B站直播监控工具...\n");
    curl_global_init(CURL_GLOBAL_ALL);
    load_config();
    load_credentials();
    
    // 启动键盘监听线程
    CreateThread(NULL, 0, keyboard_listener, NULL, 0, NULL);
    
    printf("✅ 初始化完成！\n");
    printf("💡 提示: 使用快捷键 R(录播) M(监控) ESC(退出)\n");
    
    // 启动时要求输入直播间号码
    printf("\n🎯 请输入直播间号码: ");
    char input[100];
    fgets(input, sizeof(input), stdin);
    int room_id = atoi(input);
    
    if (room_id > 0) {
        LiveRoomInfo room_info;
        printf("\n正在获取直播间信息...\n");
        if (check_bilibili_live_status(room_id, &room_info) == 0) {
            print_live_info(&room_info);
            g_config.last_room_id = room_id;
            save_config();
            
            // 询问用户是否配置账户
            printf("是否现在配置B站账户凭证？(Y/N): ");
            fgets(input, sizeof(input), stdin);
            if (input[0] == 'Y' || input[0] == 'y') {
                setup_credentials();
            } else {
                printf("✅ 已跳过账户配置，您可以在主菜单中选择 [3] 进行配置\n");
            }
        } else {
            printf("❌ 获取直播间信息失败！\n");
        }
    } else {
        printf("❌ 无效的房间号！\n");
    }
    
    int choice;
    
    while (1) {
        show_main_menu();
        printf("请选择操作 (0-10): ");
        
        fgets(input, sizeof(input), stdin);
        choice = atoi(input);
        
        switch (choice) {
            case 1: {
                printf("请输入房间号: ");
                fgets(input, sizeof(input), stdin);
                int room_id = atoi(input);
                
                LiveRoomInfo room_info;
                if (check_bilibili_live_status(room_id, &room_info) == 0) {
                    print_live_info(&room_info);
                    g_config.last_room_id = room_id;
                    save_config();
                } else {
                    printf("❌ 获取直播间信息失败！\n");
                }
                break;
            }
            
            case 2: {
                if (g_cred.uid == 0) {
                    printf("❌ 请先设置账号凭证！\n");
                    break;
                }
                
                printf("请输入房间号: ");
                fgets(input, sizeof(input), stdin);
                int room_id = atoi(input);
                
                printf("请输入弹幕内容: ");
                char message[256];
                fgets(message, sizeof(message), stdin);
                message[strcspn(message, "\n")] = 0;
                
                send_danmaku(room_id, message);
                break;
            }
            
            case 3:
                setup_credentials();
                break;
                
            case 4:
                setup_greeting();
                break;
                
            case 5: {
                int room_id = g_config.last_room_id;
                if (room_id == 0) {
                    printf("请输入要监控的房间号: ");
                    fgets(input, sizeof(input), stdin);
                    room_id = atoi(input);
                } else {
                    printf("使用上次查询的房间号 %d？(y/n): ", room_id);
                    fgets(input, sizeof(input), stdin);
                    if (input[0] != 'y' && input[0] != 'Y') {
                        printf("请输入房间号: ");
                        fgets(input, sizeof(input), stdin);
                        room_id = atoi(input);
                    }
                }
                
                if (room_id > 0) {
                    start_monitoring(room_id);
                } else {
                    printf("❌ 无效的房间号！\n");
                }
                break;
            }
            
            case 6:
                setup_recording();
                break;
                
            case 7: {
                if (!g_config.recording_enabled) {
                    printf("❌ 录播功能未启用，请先设置录播参数！\n");
                    break;
                }
                
                printf("请输入房间号: ");
                fgets(input, sizeof(input), stdin);
                int room_id = atoi(input);
                
                LiveRoomInfo room_info;
                if (check_bilibili_live_status(room_id, &room_info) == 0) {
                    if (room_info.live_status == 1) {
                        start_recording(room_id, room_info.title, room_info.anchor);
                    } else {
                        printf("❌ 该直播间未在直播，无法开始录播！\n");
                    }
                } else {
                    printf("❌ 获取直播间信息失败！\n");
                }
                break;
            }
            
            case 8:
                stop_recording();
                break;
                
            case 9:
                toggle_debug_mode();
                break;
                
            case 10:
                toggle_monitoring_setting();
                break;
                
            case 0:
                printf("👋 感谢使用，再见！\n");
                if (g_recording.is_recording) {
                    stop_recording();
                }
                if (g_monitoring_active) {
                    stop_monitoring();
                }
                curl_global_cleanup();
                return 0;
                
            default:
                printf("❌ 无效的选择，请重新输入！\n");
        }
        
        printf("\n按回车键继续...");
        fgets(input, sizeof(input), stdin);
    }
    
    curl_global_cleanup();
    return 0;
}

// 启动监控（包装函数）
void start_monitoring(int room_id) {
    monitor_live_status(room_id);
}
