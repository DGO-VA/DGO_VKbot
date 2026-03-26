# DGO_VKbot

Библиотека для работы с VK API для ESP8266 и ESP32.

Поддерживает два режима получения сообщений:
- **Long Poll**: классический VK Long Poll (может быть блокирующим или кооперативным неблокирующим).
- **Short Poll**: короткие запросы (опрос последнего сообщения через `messages.getHistory` с заданным интервалом).

## Возможности

- Long Poll API VK (включая неблокирующий режим для отзывчивого `loop()`)
- Short Poll режим (короткие запросы через `messages.getHistory`)
- Отправка и получение сообщений
- Синхронизация времени через VK API
- Управление таймзоной
- Поддержка ESP8266 и ESP32

## Установка

1. Скопируйте папку `DGO_VKbot` в папку `lib` вашего проекта PlatformIO
2. Библиотека автоматически подключится при компиляции

## Зависимости

- ArduinoJson (версия 7.x)
- WiFi библиотеки (встроенные для ESP8266/ESP32)

Для примера с DHT11 также потребуется библиотека DHT sensor library.

## Быстрый старт

```cpp
#include <DGO_VKbot.h>

#define WIFI_SSID "your_wifi"
#define WIFI_PASS "your_password"
#define VK_TOKEN "your_token"
#define GROUP_ID "-your_group_id"

DGO_VKbot bot;

void onNewMessage(VkUpdate& update) {
  if (update.type == VK_MESSAGE_NEW) {
    bot.sendMessage("Привет!", update.message.peer_id);
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  bot.setToken(VK_TOKEN);
  bot.setGroupId(GROUP_ID);
  bot.attach(onNewMessage);
  
  // Long Poll: для отзывчивого loop() включаем неблокирующий режим
  bot.setModeLongPoll();
  bot.setNonBlockingLongPoll(true);
  bot.setLongPollWaitSeconds(1);
  bot.setLongPollTimeoutMs(3000);
  bot.begin();
}

void loop() {
  bot.tick();
  delay(1);
}
```

## Режимы получения сообщений

### Long Poll

По умолчанию библиотека работает через VK Long Poll. В классическом (блокирующем) режиме `tick()` может ждать события до \(wait\) секунд.

Чтобы `loop()` не “залипал” на 25–30 секунд, включайте **кооперативный неблокирующий** режим и ставьте небольшой `wait`:

```cpp
bot.setModeLongPoll();
bot.setNonBlockingLongPoll(true);
bot.setLongPollWaitSeconds(0); // или 1
bot.setLongPollTimeoutMs(3000);
```

Рекомендация: вызывайте `bot.tick()` часто и не используйте большие `delay()` (периодику делайте через `millis()`).

### Short Poll (короткие запросы)

Если long poll не нужен, можно опрашивать последнее сообщение у пользователя через `messages.getHistory` с заданным интервалом:

```cpp
bot.setModeShortPoll("123456789", 2000, 5000); // user_id, intervalMs, timeoutMs
```

В этом режиме `begin()` не запрашивает long poll сервер/ключ/ts.

## Фильтрация по ID отправителя

Если нужно, чтобы бот отвечал только определенному пользователю, можно добавить проверку ID отправителя в обработчике сообщений:

```cpp
#define MY_USER_ID 123456789  // ID пользователя VK, которому будет отвечать бот

void onNewMessage(VkUpdate& update) {
  if (update.type == VK_MESSAGE_NEW) {
    // Фильтруем: отвечаем только на сообщения от указанного пользователя
    if (update.message.from_id != MY_USER_ID) {
      Serial.print("Сообщение от другого пользователя (ID: ");
      Serial.print(update.message.from_id);
      Serial.println("), пропускаем");
      return;
    }
    
    // Обрабатываем сообщение
    String text = update.message.text;
    int peer_id = update.message.peer_id;
    
    // Ваша логика обработки
    bot.sendMessage("Сообщение получено!", peer_id);
  }
}
```

**Поля структуры VkUpdate для фильтрации:**
- `update.message.from_id` - ID отправителя сообщения
- `update.message.peer_id` - ID чата (для отправки ответа)
- `update.message.text` - текст сообщения
- `update.message.id` - ID сообщения
- `update.message.date` - время отправки (Unix timestamp)

Если не использовать фильтрацию, бот будет отвечать всем пользователям, которые пишут в группу.

## API

### Основные методы

- `setToken(String token)` - установить токен VK
- `setGroupId(String id)` - установить ID группы (с минусом!)
- `begin()` - запустить бота
- `attach(callback)` - прикрепить обработчик сообщений
- `sendMessage(String text, int peer_id)` - отправить сообщение
- `tick()` - обработать события (вызывать в loop)

### Настройка режима получения сообщений

- `setModeLongPoll()` - включить режим Long Poll
- `setNonBlockingLongPoll(bool enabled)` - кооперативный неблокирующий long poll
- `setLongPollWaitSeconds(uint8_t seconds)` - параметр `wait` (сек)
- `setLongPollTimeoutMs(uint32_t timeoutMs)` - таймаут (мс)
- `setModeShortPoll(String userId, uint32_t intervalMs=2000, uint32_t timeoutMs=5000)` - режим Short Poll

### Форматирование сообщений (format_data)

Библиотека поддерживает отправку `format_data` в `messages.send`.

- `sendMessage(String text, int peer_id, String format_data_json)` - отправка с разметкой
- `fmtBold(const String& fullText, const String& part)` - выделить подстроку жирным
- `fmtBoldAll(const String& fullText)` - выделить весь текст жирным
- `fmtItalic(const String& fullText, const String& part)` - выделить подстроку курсивом
- `fmtItalicAll(const String& fullText)` - выделить весь текст курсивом
- `fmtUnderline(const String& fullText, const String& part)` - выделить подстроку подчеркиванием
- `fmtUnderlineAll(const String& fullText)` - выделить весь текст подчеркиванием
- `fmtUrl(const String& fullText, const String& part, const String& href)` - сделать подстроку ссылкой
- `fmtItalicAt(int offsetUtf16, int lengthUtf16)` - сделать фрагмент курсивом
- `fmtUnderlineAt(int offsetUtf16, int lengthUtf16)` - сделать фрагмент подчеркнутым
- `fmtUrlAt(int offsetUtf16, int lengthUtf16, const String& href)` - сделать фрагмент ссылкой
- `fmtMerge(const String& a, const String& b)` - объединить несколько `format_data` JSON

Важно:
- `offset` и `length` должны быть в **UTF-16 code units** (требование VK API).
- Методы `fmtBold/fmtItalic/fmtUnderline/fmtUrl` форматируют **первое найденное вхождение** `part`.

Пример:

```cpp
String text = "Привет! Открой vk.com";

// Удобный вариант: без ручного подсчета offset/length
String italic = bot.fmtItalic(text, "Привет!");
String link = bot.fmtUrl(text, "vk.com", "https://vk.com");
String fmt = bot.fmtMerge(italic, link);

bot.sendMessage(text, peer_id, fmt);
```

Пример: весь ответ курсивом

```cpp
String reply = "Эхо: " + text;
String fmt = bot.fmtItalicAll(reply);
bot.sendMessage(reply, peer_id, fmt);
```

### Управление временем

- `setTimezone(int hours)` - установить таймзону (например, 3 для UTC+3)
- `syncTime()` - синхронизировать время с сервером VK
- `getCurrentTimeString()` - получить текущее время как строку
- `getSecondsFromMidnight()` - секунды с начала дня

## Примеры

В папке `examples` находятся примеры:

1. **EchoBot** - простой эхо-бот
2. **LEDControl** - управление светодиодом через команды
3. **DHT11Sensor** - получение данных с датчика DHT11
4. **ShortPollEchoBot** - эхо-бот в режиме Short Poll (`messages.getHistory`)

## Поддержка платформ

Библиотека автоматически определяет платформу:
- ESP8266
- ESP32

Для других платформ будет ошибка компиляции.

## Настройка VK

1. Создайте группу в VK
2. Получите токен группы с правами на сообщения
3. Получите ID группы (с минусом)
4. Настройте Long Poll в настройках группы

## Лицензия

 Apache License Version 2.0
