# PantrySense (ESP32)
This is a part of my broader smart pantry project Rikōtodana. The project is currently in the prototype stage.

This repository includes software for the sensors (LDR, DHT22 and a HX711 combined with a load cell) that are directly connected to an ESP32.
While the LDR and the DHT22 share one ESP32, the HX711 gets its own ESP32. The source code for the HX711 related software can be found in the `weight` directory, while the source code for utilizing the LDR and DHT22 can be found in the `ldr-dht22` directory.  
All the measured data gets forwarded with an MQTT broker. You need to set one up and change the WiFi and MQTT credentials and MQTT broker address in the source code. Then you can compile it and upload the software to the ESP32.  
The source code was written with the Arduino IDE and therefore relies on the [arduino-esp32 library](https://github.com/espressif/arduino-esp32).
This source code also relies on the [PubSubClient](https://github.com/knolleary/pubsubclient) from Nick O'Leary for MQTT connectivity, the [DHT library](https://github.com/beegee-tokyo/DHTesp) from Bernd Giesecke, the [HX711 library](https://github.com/bogde/HX711) from Bogdan Necula and the [Statistics library](https://github.com/RobTillaart/Statistic) from Rob Tillaart and the [ArduinoJson library](https://github.com/bblanchon/ArduinoJson) from Benoît Blanchon.
### Security warning
Since this is still a prototype, many security measures are missing! Transport encryption for the MQTT messages has not been implemented yet.