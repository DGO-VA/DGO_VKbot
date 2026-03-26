// DGO_VKbot.h - Библиотека для работы с VK API через Long Poll
// Поддержка ESP8266 и ESP32
// Автор: DGO
// Версия: 1.0.0

#ifndef DGO_VKBOT_H
#define DGO_VKBOT_H

#include <Arduino.h>
#include <functional>
#include <time.h>
#include <ArduinoJson.h>

// Поддержка ESP8266 и ESP32
#ifdef ESP8266
    #include <ESP8266WiFi.h>
    #include <ESP8266HTTPClient.h>
    #include <WiFiClientSecure.h>
#elif defined(ESP32)
    #include <WiFi.h>
    #include <HTTPClient.h>
    #include <WiFiClientSecure.h>
#else
    #error "Платформа не поддерживается. Используйте ESP8266 или ESP32"
#endif

// Типы событий VK Long Poll
enum VkEventType {
    VK_MESSAGE_NEW,
    VK_MESSAGE_REPLY,
    VK_MESSAGE_EDIT,
    VK_UNKNOWN
};

// Структура сообщения
struct VkMessage {
    int id;
    int from_id;
    int peer_id;
    String text;
    unsigned long date;
    
    VkMessage() : id(0), from_id(0), peer_id(0), date(0) {}
    VkMessage(String t, int p) : id(0), from_id(0), peer_id(p), text(t), date(0) {}
};

// Структура обновления
struct VkUpdate {
    VkEventType type;
    VkMessage message;
    
    VkUpdate() : type(VK_UNKNOWN) {}
};

// Класс VK бота
class DGO_VKbot {
private:
    String token;
    String groupId;
    WiFiClientSecure client;
    
    // Long Poll параметры
    String lpServer;
    String lpKey;
    String lpTs;

    enum class PollMode : uint8_t {
        LongPoll,
        ShortPoll
    };

    PollMode pollMode = PollMode::LongPoll;

    // Long Poll настройки
    uint8_t lpWaitSeconds = 25;          // параметр wait=... в запросе (сек)
    uint32_t lpTimeoutMs = 30000;        // таймаут HTTP клиента (мс)
    bool nonBlockingLongPoll = false;    // неблокирующая обработка long poll (кооперативная)

    // === Неблокирующий long poll (кооперативный) ===
    enum class LpState : uint8_t {
        Idle,
        Connecting,
        Sending,
        ReadingHeaders,
        ReadingBody,
        Done,
        Error
    };

    LpState lpState = LpState::Idle;
    String lpHost;
    String lpPath;
    uint16_t lpPort = 443;
    String lpRx;                // буфер принятого HTTP ответа (заголовки + body)
    int32_t lpContentLength = -1;
    int32_t lpBodyStart = -1;
    unsigned long lpStateStartedMs = 0;

    // Простейший парсер URL вида https://host[:port]/path?query
    bool parseUrl(const String& url, String& host, uint16_t& port, String& path) {
        String u = url;
        u.trim();
        if (!u.startsWith("https://")) {
            return false;
        }
        int hostStart = 8; // после "https://"
        int pathStart = u.indexOf('/', hostStart);
        String hostPort = (pathStart >= 0) ? u.substring(hostStart, pathStart) : u.substring(hostStart);
        path = (pathStart >= 0) ? u.substring(pathStart) : "/";

        int colon = hostPort.indexOf(':');
        if (colon >= 0) {
            host = hostPort.substring(0, colon);
            port = (uint16_t)hostPort.substring(colon + 1).toInt();
            if (port == 0) port = 443;
        } else {
            host = hostPort;
            port = 443;
        }
        return host.length() > 0;
    }

    void lpResetState() {
        lpState = LpState::Idle;
        lpHost = "";
        lpPath = "";
        lpPort = 443;
        lpRx = "";
        lpContentLength = -1;
        lpBodyStart = -1;
        lpStateStartedMs = 0;
        if (client.connected()) {
            client.stop();
        }
    }

    String buildLongPollUrl() {
        String url = lpServer + "?act=a_check&key=" + lpKey +
                    "&ts=" + lpTs + "&wait=" + String(lpWaitSeconds) + "&mode=2&version=3";
        return url;
    }

    // Обработка ответа long poll (JSON), общий код для блокирующего и неблокирующего режимов
    bool handleLongPollJson(const String& responseBody) {
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, responseBody);

        if (error) {
            Serial.print("[VK] JSON ошибка: ");
            Serial.println(error.c_str());
            return false;
        }

        // Обновляем ts для следующего запроса
        if (doc["ts"].is<String>()) {
            lpTs = doc["ts"].as<String>();
        }

        // Проверяем на ошибки Long Poll (failed, pts)
        if (doc["failed"].is<int>()) {
            int failed = doc["failed"].as<int>();
            if (failed == 1) {
                // Нужно обновить ts
                if (doc["ts"].is<String>()) {
                    lpTs = doc["ts"].as<String>();
                }
                return true;
            } else if (failed == 2 || failed == 3) {
                // Нужно переподключиться
                Serial.println("[VK] Long Poll требует переподключения");
                return getLongPollServer();
            }
        }

        // Обрабатываем события
        if (doc["updates"].is<JsonArray>()) {
            JsonArray updates = doc["updates"].as<JsonArray>();

            for (JsonObject update : updates) {
                VkUpdate vkUpdate;
                String type = update["type"].as<String>();

                if (type == "message_new") {
                    vkUpdate.type = VK_MESSAGE_NEW;
                    JsonObject msg = update["object"]["message"];
                    vkUpdate.message.id = msg["id"].as<int>();
                    vkUpdate.message.from_id = msg["from_id"].as<int>();
                    vkUpdate.message.peer_id = msg["peer_id"].as<int>();
                    vkUpdate.message.text = msg["text"].as<String>();
                    vkUpdate.message.date = msg["date"].as<unsigned long>();

                    if (newMessageCallback) {
                        newMessageCallback(vkUpdate);
                    }
                }
            }
            return true;
        }

        return true;
    }

    // Неблокирующая обработка long poll: один шаг state machine за tick()
    bool processLongPollNonBlockingStep() {
        if (!client.connected() && lpState != LpState::Idle && lpState != LpState::Connecting) {
            // если соединение упало посреди чтения — сбрасываемся
            lpResetState();
        }

        const unsigned long now = millis();
        if (lpState != LpState::Idle && lpTimeoutMs > 0 && (now - lpStateStartedMs) > lpTimeoutMs) {
            // кооперативный таймаут на весь запрос
            lpResetState();
            return true; // таймаут для long poll — нормальная ситуация, просто начнем заново
        }

        if (lpState == LpState::Idle) {
            String url = buildLongPollUrl();
            if (!parseUrl(url, lpHost, lpPort, lpPath)) {
                Serial.println("[VK] Ошибка парсинга Long Poll URL");
                return false;
            }
            lpRx.reserve(4608);
            lpState = LpState::Connecting;
            lpStateStartedMs = now;
        }

        if (lpState == LpState::Connecting) {
            client.setTimeout((uint32_t)max((uint32_t)1000, lpTimeoutMs)); // на всякий случай
            if (!client.connect(lpHost.c_str(), lpPort)) {
                // не валим все — попробуем на следующем тике
                lpResetState();
                delay(1);
                return true;
            }
            lpState = LpState::Sending;
        }

        if (lpState == LpState::Sending) {
            String req = "GET " + lpPath + " HTTP/1.1\r\n";
            req += "Host: " + lpHost + "\r\n";
            req += "Connection: close\r\n";
            req += "User-Agent: DGO_VKbot\r\n\r\n";
            client.print(req);
            lpState = LpState::ReadingHeaders;
        }

        // Читаем данные, которые уже пришли, не блокируя
        while (client.available() > 0) {
            char c = (char)client.read();
            lpRx += c;

            // нашли конец заголовков?
            if (lpBodyStart < 0) {
                int idx = lpRx.indexOf("\r\n\r\n");
                if (idx >= 0) {
                    lpBodyStart = idx + 4;
                    // Content-Length (если есть)
                    int cl = lpRx.indexOf("Content-Length:");
                    if (cl >= 0) {
                        int lineEnd = lpRx.indexOf("\r\n", cl);
                        if (lineEnd > cl) {
                            String v = lpRx.substring(cl + 15, lineEnd);
                            v.trim();
                            lpContentLength = v.toInt();
                        }
                    }
                    lpState = LpState::ReadingBody;
                }
            }
            // ограничение на рост буфера (защита от OOM)
            if (lpRx.length() > 16384) {
                Serial.println("[VK] Long Poll ответ слишком большой");
                lpResetState();
                return false;
            }
        }

        // Если body уже началось, проверяем, полностью ли оно получено
        if (lpState == LpState::ReadingBody && lpBodyStart >= 0) {
            int bodyLen = lpRx.length() - lpBodyStart;
            if (lpContentLength >= 0) {
                if (bodyLen >= lpContentLength) {
                    String body = lpRx.substring(lpBodyStart, lpBodyStart + lpContentLength);
                    lpResetState();
                    return handleLongPollJson(body);
                }
            } else {
                // Без Content-Length: считаем, что конец по закрытию соединения
                if (!client.connected()) {
                    String body = lpRx.substring(lpBodyStart);
                    lpResetState();
                    return handleLongPollJson(body);
                }
            }
        }

        // Если соединение закрыто до заголовков/боди — сбросимся и попробуем снова
        if (!client.connected() && lpState != LpState::Idle) {
            lpResetState();
        }

        return true;
    }
    
    // Флаг запуска
    bool started;
    
    // Callback для новых сообщений
    std::function<void(VkUpdate&)> newMessageCallback;
    
    // Управление временем и таймзоной
    time_t systemTime;              // Системное время в UTC
    unsigned long lastTimeUpdate;   // Когда последний раз обновляли время (millis)
    int timezoneOffset;             // Смещение таймзоны в секундах (по умолчанию 0 - UTC)
    
    // Кодирование URL
    String urlEncode(String str) {
        String encoded = "";
        char c;
        for (unsigned int i = 0; i < str.length(); i++) {
            c = str.charAt(i);
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else if (c == ' ') {
                encoded += "%20";
            } else {
                encoded += "%";
                if (c < 16) encoded += "0";
                encoded += String(c, HEX);
            }
        }
        return encoded;
    }

    // Подсчет длины строки в UTF-16 code units (как требует VK для format_data)
    // Входная строка предполагается в UTF-8 (Arduino String).
    int utf16Len(const String& s) {
        const uint8_t* p = (const uint8_t*)s.c_str();
        int count = 0;
        while (*p) {
            uint32_t cp = 0;
            if ((*p & 0x80) == 0x00) {          // 1 byte
                cp = *p;
                p += 1;
            } else if ((*p & 0xE0) == 0xC0) {   // 2 bytes
                if (!p[1]) break;
                cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                p += 2;
            } else if ((*p & 0xF0) == 0xE0) {   // 3 bytes
                if (!p[1] || !p[2]) break;
                cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                p += 3;
            } else if ((*p & 0xF8) == 0xF0) {   // 4 bytes
                if (!p[1] || !p[2] || !p[3]) break;
                cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                p += 4;
            } else {
                // Некорректный байт — пропускаем
                p += 1;
                continue;
            }

            count += (cp > 0xFFFF) ? 2 : 1;
        }
        return count;
    }

    // UTF-16 offset начала part в fullText.
    // Возвращает -1, если part не найдена.
    int utf16OffsetOf(const String& fullText, const String& part) {
        int bytePos = fullText.indexOf(part);
        if (bytePos < 0) return -1;
        return utf16Len(fullText.substring(0, bytePos));
    }

    String buildFormatDataItem(const String& type, int offset, int length, const String& urlValue = "") {
        DynamicJsonDocument doc(256);
        doc["version"] = 1;
        JsonArray items = doc.createNestedArray("items");
        JsonObject it = items.createNestedObject();
        it["type"] = type;
        it["offset"] = offset;
        it["length"] = length;
        if (type == "url" && urlValue.length() > 0) {
            it["url"] = urlValue;
        }
        String out;
        serializeJson(doc, out);
        return out;
    }

    // Объединить несколько format_data items (у всех будет version=1)
    String mergeFormatData(const String& a, const String& b) {
        if (a.length() == 0) return b;
        if (b.length() == 0) return a;
        DynamicJsonDocument da(512);
        DynamicJsonDocument db(512);
        if (deserializeJson(da, a)) return a;
        if (deserializeJson(db, b)) return a;
        if (!da["items"].is<JsonArray>() || !db["items"].is<JsonArray>()) return a;
        for (JsonObject it : db["items"].as<JsonArray>()) {
            da["items"].add(it);
        }
        String out;
        serializeJson(da, out);
        return out;
    }
    
    // Получение Long Poll сервера
    bool getLongPollServer() {
        HTTPClient http;
        String url = "https://api.vk.com/method/groups.getLongPollServer?";
        url += "access_token=" + token;
        url += "&group_id=" + groupId.substring(1); // Без минуса
        url += "&v=5.199";
        
        http.begin(client, url);
        http.setTimeout(5000);
        
        if (http.GET() == 200) {
            String response = http.getString();
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, response);
            
            if (!error && doc["response"].is<JsonObject>()) {
                lpServer = doc["response"]["server"].as<String>();
                lpKey = doc["response"]["key"].as<String>();
                lpTs = doc["response"]["ts"].as<String>();
                
                Serial.print("[VK] Long Poll сервер: ");
                Serial.println(lpServer);
                return true;
            } else if (doc["error"].is<JsonObject>()) {
                Serial.print("[VK] API ошибка: ");
                Serial.println(doc["error"]["error_msg"].as<String>());
            }
        }
        
        http.end();
        Serial.println("[VK] Ошибка получения Long Poll сервера");
        return false;
    }
    
    // Обработка Long Poll событий
    bool processLongPoll() {
        HTTPClient http;
        String url = buildLongPollUrl();
        
        http.begin(client, url);
        http.setTimeout(lpTimeoutMs);
        
        int httpCode = http.GET();
        
        if (httpCode == 200) {
            String responseBody = http.getString();
            http.end();
            return handleLongPollJson(responseBody);
        } else if (httpCode == -1) {
            // Таймаут - нормально для Long Poll
            http.end();
            return true;
        } else {
            // Серверная ошибка - переподключаемся
            Serial.print("[VK] Long Poll HTTP ошибка: ");
            Serial.println(httpCode);
            delay(1000);
            http.end();
            return getLongPollServer();
        }
        
        http.end();
        return false;
    }

    // === Short Poll (короткие запросы) ===
    // Важно: этот режим опрашивает последнее сообщение у конкретного пользователя/peer.
    // Нужен соответствующий токен, у которого есть доступ к этому методу.
    String shortPollUserId;
    uint32_t shortPollIntervalMs = 2000;
    uint32_t shortPollTimeoutMs = 5000;
    unsigned long lastShortPollMs = 0;
    int lastShortMsgId = 0;

    bool processShortPoll() {
        const unsigned long now = millis();
        if ((uint32_t)(now - lastShortPollMs) < shortPollIntervalMs) {
            return true;
        }
        lastShortPollMs = now;

        if (token.length() == 0 || shortPollUserId.length() == 0) {
            return false;
        }

        HTTPClient http;
        String url = "https://api.vk.com/method/messages.getHistory?";
        url += "access_token=" + token;
        url += "&user_id=" + shortPollUserId;
        url += "&count=1&v=5.199";

        http.begin(client, url);
        http.setTimeout(shortPollTimeoutMs);
        int httpCode = http.GET();

        if (httpCode == 200) {
            String responseBody = http.getString();
            http.end();

            DynamicJsonDocument doc(4096);
            DeserializationError error = deserializeJson(doc, responseBody);
            if (error) {
                Serial.print("[VK] JSON ошибка: ");
                Serial.println(error.c_str());
                return false;
            }

            // items[0]
            if (!doc["response"]["items"].is<JsonArray>() || doc["response"]["items"].size() == 0) {
                return true;
            }

            JsonObject msg = doc["response"]["items"][0];
            int msgId = msg["id"].as<int>();
            int out = msg["out"].as<int>(); // 0 = входящее, 1 = исходящее

            // Новое входящее сообщение
            if (msgId > lastShortMsgId && out == 0) {
                lastShortMsgId = msgId;

                VkUpdate vkUpdate;
                vkUpdate.type = VK_MESSAGE_NEW;
                vkUpdate.message.id = msgId;
                vkUpdate.message.from_id = msg["from_id"].as<int>();
                vkUpdate.message.peer_id = msg["peer_id"].as<int>();
                vkUpdate.message.text = msg["text"].as<String>();
                vkUpdate.message.date = msg["date"].as<unsigned long>();

                if (newMessageCallback) {
                    newMessageCallback(vkUpdate);
                }
            }

            return true;
        }

        http.end();
        return true;
    }

public:
    // Конструктор
    DGO_VKbot() : started(false), systemTime(0), lastTimeUpdate(0), timezoneOffset(0) {
        client.setInsecure();
    }
    
    // Установить токен
    void setToken(String t) {
        token = t;
    }
    
    // Установить ID группы (с минусом!)
    void setGroupId(String id) {
        groupId = id;
    }
    
    // Запуск бота
    bool begin() {
        if (token.length() == 0 || groupId.length() == 0) {
            Serial.println("[VK] Установите токен и ID группы!");
            return false;
        }
        
        if (pollMode == PollMode::LongPoll) {
            if (!getLongPollServer()) {
                return false;
            }
        }
        
        started = true;
        if (pollMode == PollMode::LongPoll) {
            Serial.println("[VK] Бот запущен с Long Poll");
        } else {
            Serial.println("[VK] Бот запущен с Short Poll");
        }
        return true;
    }
    
    // Прикрепить обработчик сообщений
    void attach(std::function<void(VkUpdate&)> callback) {
        newMessageCallback = callback;
    }
    
    // Отправить сообщение
    bool sendMessage(VkMessage msg) {
        return sendMessage(msg.text, msg.peer_id, "");
    }

    // Отправить сообщение (с format_data JSON)
    bool sendMessage(String text, int peer_id, String format_data_json) {
        if (!started) {
            Serial.println("[VK] Бот не запущен!");
            return false;
        }
        
        HTTPClient http;
        String url = "https://api.vk.com/method/messages.send?";
        url += "access_token=" + token;
        url += "&peer_id=" + String(peer_id);
        url += "&message=" + urlEncode(text);
        if (format_data_json.length() > 0) {
            url += "&format_data=" + urlEncode(format_data_json);
        }
        url += "&random_id=" + String(random(1000000));
        url += "&v=5.199";
        
        http.begin(client, url);
        http.setTimeout(5000);
        int httpCode = http.GET();
        
        bool success = false;
        if (httpCode == 200) {
            String response = http.getString();
            DynamicJsonDocument doc(512);
            DeserializationError error = deserializeJson(doc, response);
            
            if (!error) {
                if (doc["response"].is<int>()) {
                    success = true;
                } else if (doc["error"].is<JsonObject>()) {
                    Serial.print("[VK] Ошибка отправки: ");
                    Serial.print(doc["error"]["error_code"].as<int>());
                    Serial.print(" - ");
                    Serial.println(doc["error"]["error_msg"].as<String>());
                }
            }
        } else {
            Serial.print("[VK] HTTP ошибка отправки: ");
            Serial.println(httpCode);
        }
        
        http.end();
        return success;
    }
    
    // Быстрая отправка (перегрузка)
    bool sendMessage(String text, int peer_id) {
        return sendMessage(text, peer_id, "");
    }

    // === Форматирование сообщений (format_data) ===
    // Хелперы возвращают JSON строку для параметра format_data.
    // Смещения и длины считаются в UTF-16 code units (VK требование).
    String fmtBold(const String& fullText, const String& part) {
        int offset = utf16OffsetOf(fullText, part);
        if (offset < 0) return "";
        int length = utf16Len(part);
        return buildFormatDataItem("bold", offset, length);
    }

    // Форматировать весь текст
    String fmtBoldAll(const String& fullText) {
        return buildFormatDataItem("bold", 0, utf16Len(fullText));
    }

    String fmtItalic(const String& fullText, const String& part) {
        int offset = utf16OffsetOf(fullText, part);
        if (offset < 0) return "";
        int length = utf16Len(part);
        return buildFormatDataItem("italic", offset, length);
    }

    // Форматировать весь текст
    String fmtItalicAll(const String& fullText) {
        return buildFormatDataItem("italic", 0, utf16Len(fullText));
    }

    String fmtUnderline(const String& fullText, const String& part) {
        int offset = utf16OffsetOf(fullText, part);
        if (offset < 0) return "";
        int length = utf16Len(part);
        return buildFormatDataItem("underline", offset, length);
    }

    // Форматировать весь текст
    String fmtUnderlineAll(const String& fullText) {
        return buildFormatDataItem("underline", 0, utf16Len(fullText));
    }

    String fmtUrl(const String& fullText, const String& part, const String& href) {
        int offset = utf16OffsetOf(fullText, part);
        if (offset < 0) return "";
        int length = utf16Len(part);
        return buildFormatDataItem("url", offset, length, href);
    }

    String fmtItalicAt(int offsetUtf16, int lengthUtf16) {
        return buildFormatDataItem("italic", offsetUtf16, lengthUtf16);
    }

    String fmtUnderlineAt(int offsetUtf16, int lengthUtf16) {
        return buildFormatDataItem("underline", offsetUtf16, lengthUtf16);
    }

    String fmtUrlAt(int offsetUtf16, int lengthUtf16, const String& href) {
        return buildFormatDataItem("url", offsetUtf16, lengthUtf16, href);
    }

    // Объединить несколько format_data JSON в один
    String fmtMerge(const String& a, const String& b) {
        return mergeFormatData(a, b);
    }
    
    // Получить время сервера VK
    unsigned long getServerTime() {
        if (!started) {
            Serial.println("[VK] Бот не запущен, невозможно получить время");
            return 0;
        }
        
        HTTPClient http;
        String url = "https://api.vk.com/method/utils.getServerTime?";
        url += "access_token=" + token;
        url += "&v=5.199";
        
        http.begin(client, url);
        http.setTimeout(5000);
        int httpCode = http.GET();
        
        unsigned long serverTime = 0;
        if (httpCode == 200) {
            String response = http.getString();
            DynamicJsonDocument doc(512);
            DeserializationError error = deserializeJson(doc, response);
            
            if (!error && doc["response"].is<unsigned long>()) {
                serverTime = doc["response"].as<unsigned long>();
            } else if (doc["error"].is<JsonObject>()) {
                Serial.print("[VK] Ошибка получения времени: ");
                Serial.println(doc["error"]["error_msg"].as<String>());
            }
        } else {
            Serial.print("[VK] HTTP ошибка при получении времени: ");
            Serial.println(httpCode);
        }
        
        http.end();
        return serverTime;
    }
    
    // Тикер - обработать события
    void tick() {
        if (started) {
            if (pollMode == PollMode::LongPoll) {
                if (nonBlockingLongPoll) {
                    processLongPollNonBlockingStep();
                } else {
                    processLongPoll();
                }
            } else {
                processShortPoll();
            }
        }
    }

    // === НАСТРОЙКИ LONG POLL ===
    // Важно: для неблокирующего режима обычно ставят wait=0..1.
    void setLongPollWaitSeconds(uint8_t seconds) {
        lpWaitSeconds = seconds;
    }

    // Таймаут (мс) для long poll. В блокирующем режиме — это HTTPClient timeout.
    // В неблокирующем — общий таймаут state machine на один запрос.
    void setLongPollTimeoutMs(uint32_t timeoutMs) {
        lpTimeoutMs = timeoutMs;
    }

    // Включить кооперативный неблокирующий long poll.
    // Рекомендация: setLongPollWaitSeconds(0 или 1) + частый вызов tick() без delay(1000).
    void setNonBlockingLongPoll(bool enabled) {
        if (nonBlockingLongPoll != enabled) {
            nonBlockingLongPoll = enabled;
            lpResetState();
        }
    }

    // === ВЫБОР РЕЖИМА ===
    // Long Poll (по умолчанию). begin() запросит server/key/ts.
    void setModeLongPoll() {
        pollMode = PollMode::LongPoll;
    }

    // Short Poll как в примере: опрашиваем последнее сообщение у user_id каждые intervalMs.
    // Рекомендация: intervalMs >= 1000, чтобы не упираться в лимиты API.
    void setModeShortPoll(String userId, uint32_t intervalMs = 2000, uint32_t timeoutMs = 5000) {
        pollMode = PollMode::ShortPoll;
        shortPollUserId = userId;
        shortPollIntervalMs = intervalMs;
        shortPollTimeoutMs = timeoutMs;
        lastShortPollMs = 0;
        lastShortMsgId = 0;
    }
    
    // Проверить подключение
    bool isStarted() {
        return started;
    }
    
    // === УПРАВЛЕНИЕ ВРЕМЕНЕМ И ТАЙМЗОНОЙ ===
    
    // Установить смещение таймзоны в секундах (например, 10800 для UTC+3)
    bool setTimezoneOffset(int offsetSeconds) {
        if (offsetSeconds < -43200 || offsetSeconds > 50400) {
            Serial.print("[VK] Неверное значение таймзоны: ");
            Serial.print(offsetSeconds);
            Serial.println(" секунд (должно быть от -43200 до +50400)");
            return false;
        }
        
        timezoneOffset = offsetSeconds;
        int hours = offsetSeconds / 3600;
        int minutes = (abs(offsetSeconds) % 3600) / 60;
        Serial.print("[VK] Таймзона установлена: UTC");
        if (offsetSeconds >= 0) Serial.print("+");
        Serial.print(hours);
        if (minutes > 0) {
            Serial.print(":");
            if (minutes < 10) Serial.print("0");
            Serial.print(minutes);
        }
        Serial.print(" (");
        Serial.print(offsetSeconds);
        Serial.println(" секунд)");
        return true;
    }
    
    // Установить таймзону в часах (например, 3 для UTC+3)
    bool setTimezone(int offsetHours) {
        if (offsetHours < -12 || offsetHours > 14) {
            Serial.print("[VK] Неверное значение таймзоны: ");
            Serial.print(offsetHours);
            Serial.println(" часов (должно быть от -12 до +14)");
            return false;
        }
        return setTimezoneOffset(offsetHours * 3600);
    }
    
    // Получить текущее смещение таймзоны в секундах
    int getTimezoneOffset() {
        return timezoneOffset;
    }
    
    // Синхронизировать время с сервером VK (UTC)
    bool syncTime() {
        if (!started) {
            Serial.println("[VK] Бот не запущен, невозможно синхронизировать время");
            return false;
        }
        
        Serial.println("[VK] Синхронизация времени с сервером VK...");
        
        unsigned long vkTime = getServerTime();
        if (vkTime == 0) {
            Serial.println("[VK] Ошибка получения времени от VK API");
            return false;
        }
        
        systemTime = vkTime;
        lastTimeUpdate = millis();
        
        struct tm *timeinfo_utc = gmtime(&systemTime);
        if (timeinfo_utc != nullptr) {
            char timeStr[30];
            strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S UTC", timeinfo_utc);
            Serial.print("[VK] Время синхронизировано: ");
            Serial.print(systemTime);
            Serial.print(" (");
            Serial.print(timeStr);
            Serial.print(")");
            
            if (timezoneOffset != 0) {
                time_t localTime = systemTime + timezoneOffset;
                struct tm *timeinfo_local = gmtime(&localTime);
                if (timeinfo_local != nullptr) {
                    strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S", timeinfo_local);
                    Serial.print(", локальное: ");
                    Serial.print(timeStr);
                }
            }
            Serial.println();
        }
        
        return true;
    }
    
    // Получить текущее время с учетом таймзоны
    time_t getCurrentTime() {
        if (systemTime == 0) {
            return 0;
        }
        
        unsigned long elapsedSeconds = (millis() - lastTimeUpdate) / 1000;
        time_t currentTime = systemTime + elapsedSeconds + timezoneOffset;
        
        return currentTime;
    }
    
    // Получить текущее время как строку
    String getCurrentTimeString() {
        time_t currentTime = getCurrentTime();
        
        if (currentTime < 946684800) {
            return "Время не синхронизировано";
        }
        
        struct tm *timeinfo = gmtime(&currentTime);
        if (timeinfo == nullptr) {
            return "Ошибка получения времени";
        }
        
        if (timeinfo->tm_hour > 23 || timeinfo->tm_min > 59 || timeinfo->tm_sec > 59) {
            return "Некорректное время";
        }
        
        char timeStr[30];
        strftime(timeStr, sizeof(timeStr), "%d.%m.%Y %H:%M:%S", timeinfo);
        return String(timeStr);
    }
    
    // Получить секунды с начала дня (0-86399)
    unsigned long getSecondsFromMidnight() {
        time_t currentTime = getCurrentTime();
        
        if (currentTime < 946684800) {
            return 0;
        }
        
        struct tm *timeinfo = gmtime(&currentTime);
        if (timeinfo == nullptr) {
            return 0;
        }
        
        if (timeinfo->tm_hour > 23 || timeinfo->tm_min > 59 || timeinfo->tm_sec > 59) {
            return 0;
        }
        
        return timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
    }
    
    // Проверить, синхронизировано ли время
    bool isTimeSynced() {
        return (systemTime != 0);
    }
};

#endif // DGO_VKBOT_H
