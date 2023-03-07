#include <DHTesp.h>
//#include <queue.h>
#include <WiFi.h>
#include <PubSubClient.h>
#define LDRPIN 32
#define DHTPIN 22
#define LDRCACHE 100
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 2

const uint16_t ADCMAXVOLT = 2450;
const uint16_t ADCMAXVAL = 4096;
const uint16_t VMAX = 3300;
const char* ssid = "WIFI-SSID";
const char* wifipassword =  "WIFI-PASSWORD";
const char* hostname = "ESP32-temp-ldr";
const char* mqtthost = "MQTT-IP";
const char* mqttuser = "MQTT-USERNAME";
const char* mqttpass = "MQTT-PASSWORD";
const UBaseType_t notif_chan = 0;
DHTesp dht;
WiFiClient espClient;
PubSubClient mqclient(espClient);

struct SensorMessage
{
  char sType;
  float content;
};

QueueHandle_t msgq = NULL;
TaskHandle_t dhtthread;
TaskHandle_t ldrthread;
TaskHandle_t sendthread;
TimerHandle_t dhtTimer;
TimerHandle_t ldrTimer;

void setup() {
  Serial.begin(115200);
  dht.setup(DHTPIN, DHTesp::DHT22);
  msgq = xQueueCreate(20,sizeof(struct SensorMessage));
  Serial.println("DHT sensor initialized");
  analogSetAttenuation(ADC_11db);
  Serial.println("ADC attenuated");
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, wifipassword);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi..");
  }
  Serial.println("Connected to the WiFi network");
  mqclient.setServer(mqtthost, 1883);
  xTaskCreatePinnedToCore(dhtread,"DHT reader", 4096, NULL,1,&dhtthread,1);
  configASSERT(dhtthread);
  xTaskCreatePinnedToCore(ldrval,"LDR reader", 4096, NULL,1,&ldrthread,tskNO_AFFINITY);
  configASSERT(ldrthread);
  xTaskCreatePinnedToCore(sendert,"MQTT sender", 4096, NULL,1,&sendthread,tskNO_AFFINITY);
  configASSERT(sendthread);
  dhtTimer = xTimerCreate("DHT Timer", pdMS_TO_TICKS(2001), pdTRUE, (void*)0, dht_notif);
  ldrTimer = xTimerCreate("LDR Timer", pdMS_TO_TICKS(40), pdTRUE, (void*)0, ldr_notif);
}

void loop() {
  // put your main code here, to run repeatedly:
  if (!mqclient.connected()) {
    reconnect();
  }
  mqclient.loop();
}

void dhtread(void * pvParameters)
{
  TickType_t waitTime = pdMS_TO_TICKS(2); // 2/portTICK_PERIOD_MS;
  const TickType_t maxBlockTime = pdMS_TO_TICKS(2500);
  struct SensorMessage tmpmsg;
  uint32_t notif_val = 0;
  while (true)
  {
    //Serial.println("waiting for DHT timer signal");
    notif_val = ulTaskNotifyTakeIndexed(notif_chan, pdTRUE, maxBlockTime);
    //Serial.println("DHTactive");
    TempAndHumidity val = dht.getTempAndHumidity();
    if (dht.getStatus() != 0) {
      Serial.println("DHT22 error");
      Serial.println(dht.getStatusString());
    }
    tmpmsg.sType = 'T';
    tmpmsg.content = val.temperature;
    xQueueSend(msgq,(void*)&tmpmsg,waitTime);
    tmpmsg.sType = 'H';
    tmpmsg.content = val.humidity;
    xQueueSend(msgq,(void*)&tmpmsg,waitTime);
  }
}

void ldrval(void * pvParameters)
{
  const TickType_t waitTime = pdMS_TO_TICKS(2);
  const TickType_t maxBlockTime = pdMS_TO_TICKS(50);
  struct SensorMessage tmpmsg;
  tmpmsg.sType = 'L';
  tmpmsg.content = 0.0;
  float moving_avg = 0.0;
  float vals[LDRCACHE] = {0.0};
  uint8_t n = 0;
  float oldval = 0.0;
  uint32_t notif_val = 0;
  while (n < LDRCACHE)
  {
    vals[n] = read_ldr(LDRPIN);
    moving_avg = moving_avg + (vals[n] / (float)LDRCACHE);
    n = (n + 1);
  }
  //MQTT send
  tmpmsg.content = moving_avg;
  xQueueSend(msgq,(void*)&tmpmsg,waitTime);
  while (true)
  {
    //Serial.println("waiting for ADC timer signal");
    notif_val = ulTaskNotifyTakeIndexed(notif_chan, pdTRUE, maxBlockTime);
    //Serial.println("ADCactive");
    uint8_t m = 0;
    while (m < LDRCACHE)
    {
      oldval = vals[n];
      vals[n] = read_ldr(LDRPIN);
      moving_avg = moving_avg + (vals[n] / (float)LDRCACHE) - (oldval / (float)LDRCACHE);
      n = (n + 1) % LDRCACHE;
      m++;
    }
   tmpmsg.content = moving_avg;
   xQueueSend(msgq,(void*)&tmpmsg,waitTime);
   //Serial.println(moving_avg);
  }
}

float read_ldr (uint8_t pin)
{
  uint16_t value = analogRead(LDRPIN);
  //calculating ADC value and converting it to percents
  //since ADC maxes out at 2450 millivolt: adjust (lower) percentage to reflect reality
  //except when value is maxed, then percentage needs to be 100%
  if (value == ADCMAXVAL && ADCMAXVOLT < VMAX)
  {
    value = (ADCMAXVAL / ADCMAXVOLT) * VMAX;
  }
  float open = (((float)value / (float)ADCMAXVAL) / ((float)VMAX / (float)ADCMAXVOLT)) * 100;
  return open;
}

void sendert(void * pvParameters)
{
  struct SensorMessage tmpmsg;
  tmpmsg.content = 0.0;
  tmpmsg.sType = 'X';
  char tempString[8] = {0};
  while (true)
  {
    if (msgq != NULL && mqclient.connected())
    {
      if (xQueueReceive(msgq,&(tmpmsg),pdMS_TO_TICKS(2)) == pdPASS)
      {
        //send MQTT
        dtostrf(tmpmsg.content, 1, 2, tempString);
        if (tmpmsg.sType == 'L')
        {
          mqclient.publish("regal/light/1",tempString);
        }
        if (tmpmsg.sType == 'T')
        {
          mqclient.publish("regal/temp/1",tempString);
        }
        if (tmpmsg.sType == 'H')
        {
          mqclient.publish("regal/humid/1",tempString);
        }
      }
      else
      {
        //empty queue
      }
    }
    else // Queue not initialized or MQTT disconnected
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void dht_notif(TimerHandle_t xTimer)
{
  xTaskNotifyGiveIndexed(dhtthread,notif_chan);
}

void ldr_notif(TimerHandle_t xTimer)
{
  xTaskNotifyGiveIndexed(ldrthread,notif_chan);
}

void reconnect()
{
  while (!mqclient.connected())
  {
    Serial.print("Connecting to MQTT");
    bool con = mqclient.connect("ldr-dht-esp",mqttuser,mqttpass);
    if (con)
    {
      Serial.println(" succeded");
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
