#include <Statistic.h>
#include <ArduinoJson.h>
#include <HX711.h>
//#include <queue.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#define DATPIN 32
#define CLKPIN 12
#define SAMPLES 8
#define MEASUREFREQ 80.f
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 2
#define MSGBUF 200

const char* ssid = "WIFI-SSID";
const char* wifipassword =  "WIFI-PASSWORD";
const char* hostname = "ESP32-hx711";
const char* mqtthost = "MQTT-IP";
const char* mqttuser = "MQTT-USERNAME";
const char* mqttpass = "MQTT-PASSWORD";
//const UBaseType_t notif_chan = 0;
WiFiClient espClient;
PubSubClient mqclient(espClient);
HX711 loadcell;

struct SensorMessage
{
  bool filtered;
  uint64_t ts;
  float grams;
};

QueueHandle_t msgq = NULL;
QueueHandle_t cmdq = NULL;
TaskHandle_t weightthread;
TaskHandle_t sendthread;
TaskHandle_t controltask_t;
TimerHandle_t hx711Timer;
Preferences preferences;
int32_t hx_offset;
float hx_div;
SemaphoreHandle_t sensorsem = NULL;
StaticSemaphore_t xSemBuf;

void setup() {
  sensorsem = xSemaphoreCreateBinaryStatic(&xSemBuf);
  configASSERT(sensorsem);
  xSemaphoreGive(sensorsem);
  Serial.begin(115200);
  cmdq = xQueueCreate(5,sizeof(char)*MSGBUF);
  msgq = xQueueCreate(80,sizeof(struct SensorMessage));
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, wifipassword);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi..");
  }
  Serial.println("Connected to the WiFi network");
  mqclient.setServer(mqtthost, 1883);
  mqclient.setCallback(&callme);
  preferences.begin("hx711config", true);
  loadcell.begin(DATPIN,CLKPIN,64);
  hx_offset = preferences.getLong("offset",0);
  hx_div = preferences.getFloat("div",0.0);
  preferences.end();
  if ((hx_offset == 0) || (hx_div == 0.0))
  {
    calibrate();
  }
  //calibrate();
  loadcell.set_scale(hx_div);
  loadcell.set_offset(hx_offset);
  Serial.print("offset ");
  Serial.println(hx_offset);
  Serial.print("divider ");
  Serial.println(hx_div);
  xTaskCreatePinnedToCore(hx711val,"HX reader", 4096, NULL,2,&weightthread,tskNO_AFFINITY);
  configASSERT(weightthread);
  xTaskCreatePinnedToCore(sendert,"MQTT sender", 4096, NULL,1,&sendthread,tskNO_AFFINITY);
  configASSERT(sendthread);
  xTaskCreatePinnedToCore(ctrltask,"MQTT remote control", 4096, NULL,1,&controltask_t,tskNO_AFFINITY);
  configASSERT(controltask_t);
  //hx711Timer = xTimerCreate("HX Timer", pdMS_TO_TICKS(1000.f/MEASUREFREQ), pdTRUE, (void*)0, hx_notif);
}

void loop() {
  // put your main code here, to run repeatedly:
  if (!mqclient.connected()) {
    reconnect();
  }
  mqclient.loop();
}

void hx711val(void * pvParameters)
{
  const TickType_t waitTime = pdMS_TO_TICKS(2);
  const TickType_t maxBlockTime = pdMS_TO_TICKS(50);
  struct SensorMessage tmpmsg;
  tmpmsg.filtered = false;
  tmpmsg.grams = 0.0;
  tmpmsg.ts = 0;
  float avg = 0.0;
  float value = 0.0;
  uint8_t n = 0;
  float elapsed = 0.0;
  auto measure_start = esp_timer_get_time();
  auto measure_end = measure_start;
  float stat_varia = 0.0;
  statistic::Statistic<float,uint16_t,true> stats;
  while (true)
  {
    uint8_t m = 0;
    while (m < SAMPLES)
    {
      measure_start = esp_timer_get_time();
      if (xSemaphoreTake(sensorsem,(TickType_t)10) == pdTRUE)
      {
        value = loadcell.get_units(1);
        xSemaphoreGive(sensorsem);
      }
      stats.add(value);
      tmpmsg.grams = value;
      tmpmsg.ts = measure_start;
      xQueueSend(msgq,(void*)&tmpmsg,waitTime);
      //n = (n + 1) % SAMPLES;
      m++;
      measure_end = esp_timer_get_time();
      float sleep_dur = (1000000.f/MEASUREFREQ)-(measure_start-measure_end);
      if (sleep_dur >= 0.0)
      {
        vTaskDelay(pdMS_TO_TICKS(sleep_dur/1000));
      }
    }
    avg = stats.average();
    stat_varia = stats.variance();
    if (stat_varia <= 5.0)
    {
      tmpmsg.grams = avg;
      tmpmsg.ts = esp_timer_get_time();
      tmpmsg.filtered = true;
      xQueueSend(msgq,(void*)&tmpmsg,waitTime);
    }
    stats.clear();
    tmpmsg.filtered = false;
  }
}

void sendert(void * pvParameters)
{
  struct SensorMessage tmpmsg;
  tmpmsg.grams = 0.0;
  tmpmsg.filtered = false;
  tmpmsg.ts = 0;
  StaticJsonDocument<128> jsonmsg;
  char buf[128] = {0};
  while (true)
  {
    if (msgq != NULL)
    {
      if (xQueueReceive(msgq,&(tmpmsg),pdMS_TO_TICKS(2)) == pdPASS)
      {
        //send MQTT
        //Serial.println(tmpmsg.ts);
        //Serial.println(tmpmsg.grams);
        jsonmsg["ts"] = tmpmsg.ts;
        jsonmsg["g"] = tmpmsg.grams;
        serializeJson(jsonmsg, buf);
        //Serial.println(buf);
        while(!mqclient.connected())
        {
          vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (tmpmsg.filtered == true)
        {
          mqclient.publish("regal/weight/1/filtered",buf);
        }
        else
        {
          mqclient.publish("regal/weight/1/raw",buf);
        }
      }
      else
      {
        //empty queue
      }
    }
    else // Queue not initialized
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void reconnect()
{
  while (!mqclient.connected())
  {
    Serial.print("Connecting to MQTT");
    bool con = mqclient.connect("hx711-esp",mqttuser,mqttpass);
    if (con)
    {
      Serial.println(" succeded");
      mqclient.subscribe("regal/weight/1/cmd/");
    }
    else
    {
      Serial.print(" failed, reason=");
      Serial.print(mqclient.state());
      Serial.println(" retrying in 100ms");
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void calibrate()
{
    if (xSemaphoreTake(sensorsem,(TickType_t)10) == pdTRUE)
    {
      preferences.begin("hx711config", false);
      Serial.println("Recalibrating scale!");
      Serial.println("Please remove all weights from the scale and press any key");
      while (Serial.available() == 0) {
        }
      Serial.read();
      Serial.println("keypress detected");
      loadcell.set_scale();
      loadcell.tare();
      Serial.println("Please place a known weight on the scale and press any key");
      while (Serial.available() == 0) {
        }
      Serial.read();
      int rawread = loadcell.get_units(80);
      Serial.println("How many grams did you place on there?");
      while (Serial.available() == 0) {
        //delay(10);
        }
      int grams = Serial.parseInt();
      float scale_factor = (float)rawread / (float)grams;
      Serial.print("scale_factor ");
      Serial.println(scale_factor);
      loadcell.set_scale(scale_factor);
      preferences.putFloat("div",scale_factor);
      hx_div = scale_factor;
      hx_offset = loadcell.get_offset();
      preferences.putLong("offset",hx_offset);
      preferences.end();
      xSemaphoreGive(sensorsem);
    }
}

void callme (const char topic[], byte* content, unsigned int len)
{
  const TickType_t waitTime = pdMS_TO_TICKS(2);
  char buf[MSGBUF] = {0};
  if (len <= MSGBUF)
  {
    strncpy(buf,(char*)content,len);
    xQueueSend(cmdq,(void*)buf,waitTime);
  }
}

void ctrltask(void * pvParameters)
{
  char buf[MSGBUF] = {0};
  StaticJsonDocument<MSGBUF> msg_json;
  while (true)
  {
    if (xQueueReceive(cmdq,buf,pdMS_TO_TICKS(5)) == pdPASS)
    {
      if (deserializeJson(msg_json, buf) == DeserializationError::Ok)
      {
        String sent_command = msg_json["cmd"];
        if (sent_command == "calibrate")
        {
          Serial.println("starting calibration");
          calibrate();
        }
        if (sent_command == "div_add")
        {
          float addval = msg_json["param"];
          if (xSemaphoreTake(sensorsem,pdMS_TO_TICKS(5)) == pdTRUE)
          {
            hx_div = hx_div + addval;
            loadcell.set_scale(hx_div);
            preferences.begin("hx711config", false);
            preferences.putFloat("div",hx_div);
            preferences.end();
            xSemaphoreGive(sensorsem);
            Serial.print("new divider ");
            Serial.println(hx_div);
          }
        }
      }
    }
  }
}
