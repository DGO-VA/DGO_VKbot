// Пример: Эхо-бот (Short Poll)
// Опрашивает последнее сообщение у user_id через messages.getHistory
// Подходит, если не хочется long poll, и нужно контролируемое время в loop()

#include <DGO_VKbot.h>

// НАСТРОЙКИ
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
#define VK_TOKEN "your_vk_token_here"

// ВАЖНО:
// Short Poll в этом примере использует messages.getHistory с параметром user_id.
// Укажите ID пользователя, с которым вы переписываетесь.
#define USER_ID "123456789"

DGO_VKbot bot;

void onNewMessage(VkUpdate& update) {
  if (update.type != VK_MESSAGE_NEW) return;

  Serial.print("Новое сообщение от ");
  Serial.print(update.message.from_id);
  Serial.print(": ");
  Serial.println(update.message.text);

  bot.sendMessage("Эхо: " + update.message.text, update.message.peer_id);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Подключение к WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi подключен!");
  Serial.print("IP адрес: ");
  Serial.println(WiFi.localIP());

  bot.setToken(VK_TOKEN);
  bot.attach(onNewMessage);

  // Short Poll: интервал опроса и таймаут запроса настраиваются
  bot.setModeShortPoll(USER_ID, 2000, 5000);

  Serial.println("Запуск VK бота...");
  if (bot.begin()) {
    Serial.println("Бот успешно запущен!");
  } else {
    Serial.println("Ошибка запуска бота!");
  }
}

void loop() {
  bot.tick();
  delay(1);
}
