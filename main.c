#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

#define ALERT_LED   2       // LED de alerta (GPIO2)
#define BTN_TOGGLE  4       // Botão para alternar política (whitelist estrita)
#define TAG         "WOKWI_MON"

// -------- Estruturas / Globais --------
typedef struct {
  bool authorized;
  char ssid[33];
} wifi_check_t;

static QueueHandle_t alert_queue;   // FILA
static SemaphoreHandle_t wl_mutex;  // SEMÁFORO (mutex)

// Whitelist (>=5) protegida por mutex
static const char* default_whitelist[] = {
  "Wokwi-GUEST",
  "CorpNet-5G",
  "CorpNet-2G",
  "Guest-VLAN-10",
  "Lab-SSID"
};
static char whitelist[8][33];
static size_t whitelist_len = 0;

volatile bool policy_strict = false; // alterna remoção de "Wokwi-GUEST" para forçar alerta

// -------- Helpers --------
bool is_ssid_authorized(const char *ssid) {
  bool ok = false;
  if (xSemaphoreTake(wl_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    for (size_t i=0; i<whitelist_len; i++) {
      if (strncmp(whitelist[i], ssid, 32) == 0) { ok = true; break; }
    }
    xSemaphoreGive(wl_mutex);
  }
  return ok;
}

void rebuild_whitelist() {
  if (xSemaphoreTake(wl_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    whitelist_len = 0;
    for (size_t i=0; i<sizeof(default_whitelist)/sizeof(default_whitelist[0]); i++) {
      // Em modo estrito, removemos "Wokwi-GUEST" para forçar ALERTA
      if (policy_strict && strcmp(default_whitelist[i], "Wokwi-GUEST") == 0) continue;
      strncpy(whitelist[whitelist_len], default_whitelist[i], 32);
      whitelist[whitelist_len][32] = '\0';
      whitelist_len++;
    }
    xSemaphoreGive(wl_mutex);
  }
  Serial.printf("[%s] Whitelist atualizada (strict=%d). Itens=%d\n", TAG, policy_strict, (int)whitelist_len);
}

// -------- Tarefas --------
// 1) Conexão + timeout/retry (robustez #1)
void task_wifi_connect(void *pv) {
  esp_task_wdt_add(NULL); // WDT
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[%s] Não conectado. Tentando...\n", TAG);
      WiFi.disconnect();
      vTaskDelay(pdMS_TO_TICKS(200));
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(2000)); // timeout simples
    } else {
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
    esp_task_wdt_reset();
  }
}

// 2) Monitora SSID e envia para a FILA
void task_wifi_monitor(void *pv) {
  esp_task_wdt_add(NULL); // WDT
  for (;;) {
    wifi_check_t msg;
    memset(&msg, 0, sizeof(msg));
    if (WiFi.status() == WL_CONNECTED) {
      String s = WiFi.SSID();
      strncpy(msg.ssid, s.c_str(), 32);
      msg.ssid[32] = '\0';
      msg.authorized = is_ssid_authorized(msg.ssid);
    } else {
      strncpy(msg.ssid, "<disconnected>", 32);
      msg.authorized = false;
    }
    xQueueSend(alert_queue, &msg, 0);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1500));
  }
}

// 3) Consome FILA, gera LOG e pisca LED em ALERTA
void task_alert_handler(void *pv) {
  esp_task_wdt_add(NULL); // WDT
  wifi_check_t msg;
  for (;;) {
    if (xQueueReceive(alert_queue, &msg, pdMS_TO_TICKS(2000))) {
      if (!msg.authorized) {
        // LOG de alerta (requisito)
        Serial.printf("[%s] ALERTA: rede NAO AUTORIZADA: '%s'\n", TAG, msg.ssid);
        // LED pisca 3x
        for (int i=0; i<3; i++) {
          digitalWrite(ALERT_LED, HIGH);
          vTaskDelay(pdMS_TO_TICKS(150));
          digitalWrite(ALERT_LED, LOW);
          vTaskDelay(pdMS_TO_TICKS(150));
        }
      } else {
        Serial.printf("[%s] OK: rede autorizada: '%s'\n", TAG, msg.ssid);
      }
    }
    esp_task_wdt_reset();
  }
}

// -------- Setup / Loop --------
unsigned long lastButtonPoll = 0;

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(ALERT_LED, OUTPUT);
  digitalWrite(ALERT_LED, LOW);
  pinMode(BTN_TOGGLE, INPUT_PULLUP);

  wl_mutex = xSemaphoreCreateMutex();
  alert_queue = xQueueCreate(8, sizeof(wifi_check_t));
  rebuild_whitelist();

  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", ""); // Wokwi Wi-Fi público (sem senha)

  // Robustez #2: WDT (API IDF v5)
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_cfg = {
    .timeout_ms = 8000,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_cfg);
#else
  esp_task_wdt_init(8, true);
#endif

  // 3 tarefas com prioridades diferentes
  xTaskCreatePinnedToCore(task_wifi_connect,  "t_wifi_connect",  4096, NULL, 3, NULL, APP_CPU_NUM);
  xTaskCreatePinnedToCore(task_wifi_monitor,  "t_wifi_monitor",  4096, NULL, 4, NULL, APP_CPU_NUM);
  xTaskCreatePinnedToCore(task_alert_handler, "t_alert_handler", 4096, NULL, 5, NULL, APP_CPU_NUM);

  Serial.printf("[%s] Setup concluído. IDF v%d.%d\n", TAG, ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR);
}

void loop() {
  // Alterna política com o botão (GPIO4) a cada 200ms
  unsigned long now = millis();
  if (now - lastButtonPoll >= 200) {
    lastButtonPoll = now;
    static int last = HIGH;
    int cur = digitalRead(BTN_TOGGLE);
    if (last == HIGH && cur == LOW) {
      policy_strict = !policy_strict;
      rebuild_whitelist();
      Serial.printf("[%s] Policy toggled. strict=%d\n", TAG, policy_strict);
    }
    last = cur;
  }
  delay(10);
}
