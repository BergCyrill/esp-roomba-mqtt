#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Roomba.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
extern "C" {
#include "user_interface.h"
}

// Remote debugging over telnet. Just run:
// `telnet roomba.local` OR `nc roomba.local 23`
#if LOGGING
#include <RemoteDebug.h>
#define DLOG(msg, ...) if(Debug.isActive(Debug.DEBUG)){Debug.printf(msg, ##__VA_ARGS__);}
#define VLOG(msg, ...) if(Debug.isActive(Debug.VERBOSE)){Debug.printf(msg, ##__VA_ARGS__);}
RemoteDebug Debug;
#else
#define DLOG(msg, ...)
#endif

// Roomba setup
Roomba roomba(&Serial, Roomba::Baud115200);

// Roomba state
typedef struct {
  // Sensor values
  int16_t distance;
  uint8_t chargingState;
  uint16_t voltage;
  int16_t current;
  // Supposedly unsigned according to the OI docs, but I've seen it
  // underflow to ~65000mAh, so I think signed will work better.
  int16_t charge;
  uint16_t capacity;
  int16_t temp;
  uint8_t chargingSourcesAvailable;
  uint8_t OIMode;

  int16_t leftencodercounts;
  int16_t rightencodercounts;
  uint8_t stasis;

  // Derived state
  bool cleaning;
  bool docked;
  bool returning;

  int timestamp;
  bool sent;
} RoombaState;

RoombaState roombaState = {};

// Roomba sensor packet
uint8_t roombaPacket[150];
uint8_t sensors[] = {
  Roomba::SensorDistance, // PID 19, 2 bytes, mm, signed
  Roomba::SensorChargingState, // PID 21, 1 byte
  Roomba::SensorVoltage, // PID 22, 2 bytes, mV, unsigned
  Roomba::SensorCurrent, // PID 23, 2 bytes, mA, signed
  Roomba::SensorBatteryTemperature, // PID 24, 1 byte, signed
  Roomba::SensorBatteryCharge, // PID 25, 2 bytes, mAh, unsigned
  Roomba::SensorBatteryCapacity, // PID 26, 2 bytes, mAh, unsigned
  Roomba::SensorChargingSourcesAvailable, // PID 34, 1 byte, unsigned
  Roomba::SensorOIMode, // PID 35, 1 byte, unsigned
  Roomba::SensorLeftEncoderCounts, // PID 43, 2 bytes, signed
  Roomba::SensorRightEncoderCounts, // PID 44, 2 bytes, signed
  Roomba::SensorStasis // PID 58, 1 byte, unsigned
};

// Network setup
WiFiClient wifiClient;
bool OTAStarted;

// MQTT setup
PubSubClient mqttClient(wifiClient);
const PROGMEM char *commandTopic = MQTT_COMMAND_TOPIC;
const PROGMEM char *statusTopic = MQTT_STATE_TOPIC;
const PROGMEM char *statusHATopic = MQTT_STATE_HA_TOPIC;
const PROGMEM char *infoTopic = MQTT_INFO_TOPIC;
const PROGMEM char *lwtTopic = MQTT_LWT_TOPIC;
const PROGMEM char *lwtMessage = "ONLINE";
const PROGMEM char *debugTopic = MQTT_DEBUG_TOPIC;

// miscellanous
int32_t distanceSum;
bool stop_wakeup = false;

void wakeup() {
  DLOG("Wakeup Roomba\n");
  pinMode(BRC_PIN,OUTPUT);
  digitalWrite(BRC_PIN,LOW);
  delay(1000);
  pinMode(BRC_PIN,INPUT);
  delay(1000);
  if (roombaState.OIMode == 0){
    DLOG("OIMode is Off. Send Start command\n");
    Serial.write(128); // Start - CB
  }
  else if (roombaState.OIMode == 1) {
    DLOG("OIMode is not off. Try to keep alive by sending Start command\n");
    //Serial.write(131);
    //delay(100);
    Serial.write(128);
    //Serial.write(130);
  }
  else {
    DLOG("OIMode is neither 0 nor 1; do nothing\n");
  }
}

void wakeOnDock() {
  DLOG("Wakeup Roomba on dock\n");
  wakeup();
#ifdef ROOMBA_650_SLEEP_FIX
  // Some black magic from @AndiTheBest to keep the Roomba awake on the dock
  // See https://github.com/johnboiles/esp-roomba-mqtt/issues/3#issuecomment-402096638
  delay(10);
  Serial.write(135); // Clean
  delay(150);
  Serial.write(143); // Dock
#endif
}

void wakeOffDock() {
  DLOG("Wakeup Roomba off Dock\n");
  Serial.write(131); // Safe mode
  delay(300);
  Serial.write(130); // Passive mode
}

void setOIModePassive() {
  DLOG("Set OI Mode to Passive\n");
  Serial.write(128); // Passive mode
}

void setOIModeSafe() {
  DLOG("Set OI Mode to Safe\n");
  Serial.write(131); // Safe mode
}

void setOIModeFull() {
  DLOG("Set OI Mode to Full\n");
  Serial.write(132); // Full mode
}

void sendPacket(char *packetPayload) {
  DLOG("Prepare to send packet %s\n", packetPayload);
  char* command = strtok(packetPayload, " ");
  while (command != 0){
    DLOG("Set serial to %d\n", atoi(command));
    Serial.write(atoi(command));
    command = strtok(0, " ");
  }
}

bool performCommand(const char *cmdchar) {
  wakeup();

  // Char* string comparisons dont always work
  String cmd(cmdchar);

  // MQTT protocol commands
  if (cmd == "clean") {
    if (roombaState.cleaning) {
      DLOG("Already cleaning!\n");
    }
    else {
      DLOG("Start cleaning!\n");
      roombaState.cleaning = true;
      roomba.cover();
    }
    roombaState.returning = false;
  } else if (cmd == "turn_off") {
    DLOG("Turning off\n");
    roomba.power();
    roombaState.cleaning = false;
    roombaState.returning = false;
  } else if (cmd == "toggle" || cmd == "start_pause") {
    DLOG("Toggling\n");
    if (roombaState.cleaning){
      DLOG("Stop cleaning ...\n");
      roomba.power();
      roombaState.cleaning = false;
      roombaState.returning = false;
    }
    else {
      DLOG("Start cleaning ...\n");
      roomba.cover();
      roombaState.cleaning = true;
      roombaState.returning = false;
    }
    
    roomba.cover();
  } else if (cmd == "stop") {
    if (roombaState.cleaning || roombaState.returning) {
      DLOG("Stopping\n");
      roombaState.cleaning = false;
      roombaState.returning = false;
      roomba.cover();
    } else {
      DLOG("Not cleaning, can't stop\n");
    }
  } else if (cmd == "clean_spot") {
    DLOG("Cleaning Spot\n");
    roombaState.cleaning = true;
    roombaState.returning = false;
    roomba.spot();
  } else if (cmd == "locate") {
    if (roombaState.cleaning || roombaState.returning){
      DLOG("Not locating - currently cleaning/returning\n");
    } else {
      DLOG("Locating\n");
      // Set Song Number 1 and play it directly - still a little buggy
      char commandArray[39];
      String s = "140 1 3 57 8 75 8 73 16 0 131 0 141 1";
      s.toCharArray(commandArray, 39);
      sendPacket(commandArray);
      delay(750);
      Serial.write(128); // Start command
    }
  } else if (cmd == "return_to_base") {
    DLOG("Returning to Base\n");
    roombaState.returning = true;
    roomba.dock();
  } else if (cmd == "send_status") {
    DLOG("Send status through MQTT\n");
    //sendStatus();
  } else if (cmd.substring(0,6) == "packet") {
    DLOG("Received packet command\n");
    char commandArray[(cmd.length() - 6)];
    cmd.substring(7).toCharArray(commandArray, (cmd.length() - 6));
    sendPacket(commandArray);
  } else if (cmd == "sleep"){
    DLOG("Received sleep command, will sleep 10 seconds\n");
    //ESP.deepSleep(10000000); - disabled due to not connected GPIO16 to RST
  } else if (cmd == "reboot"){
    DLOG("Reboot ESP...");
    ESP.restart();
  } else {
    return false;
  }
  return true;
}
//MQTT callback for receiving submitted commands & messages
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  DLOG("Received mqtt callback for topic %s with payload %s\n", topic, payload);
  if (strcmp(commandTopic, topic) == 0) {
    // turn payload into a null terminated string
    char *cmd = (char *)malloc(length + 1);
    memcpy(cmd, payload, length);
    cmd[length] = 0;

    if(!performCommand(cmd)) {
      DLOG("Unknown command %s\n", cmd);
    }
    free(cmd);
  }
}

float readADC(int samples) {
  // Basic code to read from the ADC
  int adc = 0;
  for (int i = 0; i < samples; i++) {
    delay(1);
    adc += analogRead(A0);
  }
  adc = adc / samples;
  float mV = adc * ADC_VOLTAGE_DIVIDER;
  VLOG("ADC for %d is %.1fmV with %d samples\n", adc, mV, samples);
  return mV;
}

void debugCallback() {
  String cmd = Debug.getLastCommand();

  // Debugging commands via telnet
  if (performCommand(cmd.c_str())) {
  } else if (cmd == "quit") {
    DLOG("Stopping Roomba\n");
    Serial.write(173);
  } else if (cmd == "rreset") {
    DLOG("Resetting Roomba\n");
    roomba.reset();
  } else if (cmd == "mqtthello") {
    mqttClient.publish("vacuum/hello", "hello there");
  } else if (cmd == "version") {
    const char compile_date[] = __DATE__ " " __TIME__;
    DLOG("Compiled on: %s\n", compile_date);
  } else if (cmd == "baud115200") {
    DLOG("Setting baud to 115200\n");
    Serial.begin(115200);
    delay(100);
  } else if (cmd == "baud19200") {
    DLOG("Setting baud to 19200\n");
    Serial.begin(19200);
    delay(100);
  } else if (cmd == "baud57600") {
    DLOG("Setting baud to 57600\n");
    Serial.begin(57600);
    delay(100);
  } else if (cmd == "baud38400") {
    DLOG("Setting baud to 38400\n");
    Serial.begin(38400);
    delay(100);
  } else if (cmd == "sleep5") {
    DLOG("Going to sleep for 5 seconds\n");
    delay(100);
    ESP.deepSleep(5e6);
  } else if (cmd == "wake") {
    DLOG("Toggle BRC pin\n");
    wakeup();
  } else if (cmd == "wake2") {
    DLOG("wakeOnDock\n");
    wakeOnDock();
  } else if (cmd == "wake3") {
    DLOG("wakeOffDock\n");
    wakeOffDock();
  } else if (cmd == "OIPassive") {
    DLOG("OIPassive\n");
    setOIModePassive();
  } else if (cmd == "OISafe") {
    DLOG("OISafe\n");
    setOIModeSafe();
  } else if (cmd == "OIFull") {
    DLOG("OIFull\n");
    setOIModeFull();
  } else if (cmd == "EnableSoftAP") {
    DLOG("Enable Soft AP\n");
    WiFi.softAP("roombaESPWiFi");
  } else if (cmd == "DisableSoftAP"){
    DLOG("Disable Soft AP\n");
    WiFi.softAPdisconnect(true);
  } else if (cmd == "readadc") {
    float adc = readADC(10);
    DLOG("ADC voltage is %.1fmV\n", adc);
  } else if (cmd == "streamresume") {
    DLOG("Resume streaming\n");
    roomba.streamCommand(Roomba::StreamCommandResume);
  } else if (cmd == "streampause") {
    DLOG("Pause streaming\n");
    roomba.streamCommand(Roomba::StreamCommandPause);
  } else if (cmd == "stream") {
    DLOG("Requesting stream\n");
    roomba.stream(sensors, sizeof(sensors));
  } else if (cmd == "streamreset") {
    DLOG("Resetting stream\n");
    roomba.stream({}, 0);
  } else if (cmd == "esprestart") {
    DLOG("Reboot ESP...");
    ESP.restart();
  } else {
    DLOG("Unknown command %s\n", cmd.c_str());
  }
}

bool parseRoombaStateFromStreamPacket(uint8_t *packet, int length, RoombaState *state) {
  state->timestamp = millis();
  //DLOG("Parse new packet ...\n");
  int j = 0;
  while (j < length) {
    //DLOG("%d,",packet[j]);
    j += 1;
  }
  //DLOG("\n");
  int i = 0;
  while (i < length) {
    switch(packet[i]) {
      case Roomba::Sensors7to26: // 0
        i += 27;
        break;
      case Roomba::Sensors7to16: // 1
        i += 11;
        break;
      case Roomba::SensorVirtualWall: // 13
        i += 2;
        break;
      case Roomba::SensorDistance: // 19
        state->distance = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorChargingState: // 21
        state->chargingState = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorVoltage: // 22
        state->voltage = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorCurrent: // 23
        state->current = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryTemperature: //24
        state->temp = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorBatteryCharge: // 25
        state->charge = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorBatteryCapacity: //26
        state->capacity = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorChargingSourcesAvailable: //34
        state->chargingSourcesAvailable = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorOIMode: //35
        state->OIMode = packet[i+1];
        i += 2;
        break;
      case Roomba::SensorBumpsAndWheelDrops: // 7
        i += 2;
        break;
      case Roomba::SensorLeftEncoderCounts: //43
        state->leftencodercounts = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorRightEncoderCounts: //44
        state->rightencodercounts = packet[i+1] * 256 + packet[i+2];
        i += 3;
        break;
      case Roomba::SensorStasis: //58
        state->stasis = packet[i+1];
        i += 2;
        break;
      case 128: // Unknown
        i += 2;
        break;
      default:
        VLOG("Unhandled Packet ID %d\n", packet[i]);
        DLOG("Unhandled Packet ID %d\n", packet[i]);
        return false;
        break;
    }
  }
  return true;
}

void verboseLogPacket(uint8_t *packet, uint8_t length) {
    VLOG("Packet: ");
    for (int i = 0; i < length; i++) {
      VLOG("%d ", packet[i]);
    }
    VLOG("\n");
}

void readSensorPacket() {
  uint8_t packetLength;
  bool received = roomba.pollSensors(roombaPacket, sizeof(roombaPacket), &packetLength);
  if (received) {
    RoombaState rs = {};
    bool parsed = parseRoombaStateFromStreamPacket(roombaPacket, packetLength, &rs);
    verboseLogPacket(roombaPacket, packetLength);
    if (parsed && rs.temp != 0) {
      bool currentlyReturning = roombaState.returning;
      roombaState = rs;
      roombaState.returning = currentlyReturning;
      VLOG("Got Packet of len=%d! OIMode:%d Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh Stasis:%d\n", packetLength, roombaState.OIMode, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity, roombaState.stasis);
      //DLOG("Got Packet of len=%d! OIMode:%d Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", packetLength, roombaState.OIMode, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
      //char pkg[180];
      //sprintf(pkg, "Got Packet of len=%d! Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", packetLength, roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
      distanceSum += roombaState.distance;
      //roombaState.cleaning = false;
      //roombaState.docked = false;
      if (roombaState.current < -400 && !roombaState.returning) {
        roombaState.cleaning = true;
        roombaState.docked = false;
      } else if (roombaState.current > -50) {
        roombaState.docked = true;
        roombaState.cleaning = false;
        roombaState.returning = false;
      } else {
        roombaState.cleaning = false;
        roombaState.docked = false;
      }
    } else {
      VLOG("Failed to parse packet\n");
      DLOG("Failed to parse packet, packetLength:%d, Temperature:%d\n", packetLength, rs.temp);
      //mqttClient.publish(debugTopic,"Failed to parse packet");
    }
  }
}

void onOTAStart() {
  DLOG("Starting OTA session\n");
  DLOG("Pause streaming\n");
  roomba.streamCommand(Roomba::StreamCommandPause);
  OTAStarted = true;
}

void setup() {
  // High-impedence on the BRC_PIN
  pinMode(BRC_PIN,INPUT);

  // Sleep immediately if ENABLE_ADC_SLEEP and the battery is low
  // sleepIfNecessary();

  // Set Hostname.
  String hostname(HOSTNAME);
  WiFi.hostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();
  ArduinoOTA.onStart(onOTAStart);

  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(mqttCallback);

  #if LOGGING
  Debug.begin((const char *)hostname.c_str());
  Debug.setResetCmdEnabled(true);
  Debug.setCallBackProjectCmds(debugCallback);
  Debug.setSerialEnabled(false);
  #endif

  roomba.start();
  delay(100);

  // Reset stream sensor values
  roomba.stream({}, 0);
  delay(100);

  // Request sensor stream
  DLOG("SensorsSize:%d\n",sizeof(sensors));
  roomba.stream(sensors, sizeof(sensors));
}

void reconnect() {
  DLOG("Attempting MQTT connection...\n");
  // Attempt to connect
  //if (mqttClient.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD)) {
    if (mqttClient.connect(HOSTNAME, lwtTopic, 0, true, lwtMessage)) {
    DLOG("MQTT connected\n");
    mqttClient.subscribe(commandTopic);
    DLOG("MQTT command topic subscribed!\n");
    DLOG("Send info for roomba with MQTT\n");
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["Hostname"] = WiFi.hostname();
    root["MACAddress"] = WiFi.macAddress();
    root["IPAddress"] = wifiClient.localIP().toString();
    root["RSSI"] = WiFi.RSSI();
    root["SSID"] = WiFi.SSID();
    root["COMPILE_DATE"] = __DATE__ " " __TIME__;
    String jsonStr;
    root.printTo(jsonStr);
    mqttClient.publish(infoTopic, jsonStr.c_str());
  } else {
    DLOG("MQTT failed rc=%d try again in 5 seconds\n", mqttClient.state());
  }
}

void sendStatus() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  DLOG("Reporting packet Distance:%dmm ChargingState:%d Voltage:%dmV Current:%dmA Charge:%dmAh Capacity:%dmAh\n", roombaState.distance, roombaState.chargingState, roombaState.voltage, roombaState.current, roombaState.charge, roombaState.capacity);
  StaticJsonBuffer<300> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["cleaning"] = roombaState.cleaning;
  root["docked"] = roombaState.chargingSourcesAvailable == Roomba::ChargeAvailableDock;
  root["charging"] = roombaState.chargingState == Roomba::ChargeStateReconditioningCharging
  || roombaState.chargingState == Roomba::ChargeStateFullCharging
  || roombaState.chargingState == Roomba::ChargeStateTrickleCharging;
  root["chargingState"] = roombaState.chargingState;
  root["voltage"] = roombaState.voltage;
  root["current"] = roombaState.current;
  root["charge"] = roombaState.charge;
  root["capacity"] = roombaState.capacity;
  root["distance"] = roombaState.distance;
  root["distanceSum"] = distanceSum;
  root["batteryLevel"] = (int)(((float)roombaState.charge / (float)roombaState.capacity) * 100);
  root["batteryTemperature"] = roombaState.temp;
  root["chargingSourcesAvailable"] = roombaState.chargingSourcesAvailable;
  root["OIMode"] = roombaState.OIMode;
  root["stasis"] = roombaState.stasis;
  String jsonStr;
  root.printTo(jsonStr);
  mqttClient.publish(statusTopic, jsonStr.c_str());
}

void sendStatusHA() {
  if (!mqttClient.connected()) {
    DLOG("MQTT Disconnected, not sending status\n");
    return;
  }
  StaticJsonBuffer<300> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  
  if (roombaState.returning) {
    root["state"] = "returning";
  }
  else if (roombaState.cleaning){
    root["state"] = "cleaning";
  }
  else if (roombaState.chargingSourcesAvailable == Roomba::ChargeAvailableDock){
    root["state"] = "docked";
  }
  else {
    root["state"] = "idle"; // decided to go for state 'idle' since we cannot differ between standing around idling and having an error
  }
  root["battery_level"] = (int)(((float)roombaState.charge / (float)roombaState.capacity) * 100);
  String jsonStr;
  root.printTo(jsonStr);
  mqttClient.publish(statusHATopic, jsonStr.c_str(), true);
}

void sleepIfNecessary() {
  // Check the battery, if it's too low, sleep the ESP (so we don't murder the battery)
  //float mV = readADC(10);
  // According to this post, you want to stop using NiMH batteries at about 0.9V per cell
  // https://electronics.stackexchange.com/a/35879 For a 12 cell battery like is in the Roomba,
  // That's 10.8 volts.
  if ((roombaState.voltage < 10800 && roombaState.voltage > 0) || ((int)(((float)roombaState.charge / (float)roombaState.capacity) * 100) < 15)) {
    // Fire off a quick message with our most recent state, if MQTT is connected
    DLOG("Battery voltage is low (%.1fV). Sleeping for 10 minutes\n", (float)roombaState.voltage / 1000);
    if (roombaState.cleaning || roombaState.returning){
      roomba.cover();
    }
    if (mqttClient.connected()) { 
      sendStatus();
      sendStatusHA();
      StaticJsonBuffer<200> jsonBuffer;
      JsonObject& root = jsonBuffer.createObject();
      //root["warning"] = "low battery - sleep 10 minutes";
      root["warning"] = "low battery - disabled cleaning";
      root["voltage"] = roombaState.voltage;
      root["batteryLevel"] = (int)(((float)roombaState.charge / (float)roombaState.capacity) * 100);
      String jsonStr;
      root.printTo(jsonStr);
      mqttClient.publish(statusTopic, jsonStr.c_str(), true);
      delay(200);
      stop_wakeup = true; // added bool to allow Roomba to enter power_saving mode - work in progress
      //ESP.deepSleep(600e6); - disabled due to not connected GPIO16 to RST
    }
    //delay(200);

    // Sleep for 10 minutes - moved to mqtt section to force warning message
    //ESP.deepSleep(600e6); - disabled due to not connected GPIO16 to RST
  }
}

int lastStateMsgTime = 0;
int lastInfoMsgTime = 0;
int lastWakeupTime = 0;
int lastConnectTime = 0;

void loop() {
  // Important callbacks that _must_ happen every cycle
  ArduinoOTA.handle();
  yield();
  Debug.handle();

  // Skip all other logic if we're running an OTA update
  if (OTAStarted) {
    return;
  }

  long now = millis();
  // If MQTT client can't connect to broker, then reconnect every 30 seconds
  if (!mqttClient.connected() && (now - lastConnectTime) > 30000) {
    DLOG("Reconnecting MQTT\n");
    lastConnectTime = now;
    reconnect();
  }
  // Wakeup the roomba at fixed intervals - every 50 seconds
  if (now - lastWakeupTime > 50000) {
    DLOG("Wakeup Roomba now\n");
    lastWakeupTime = now;
    if (!roombaState.cleaning && !stop_wakeup && !roombaState.returning) {
      if (roombaState.docked) {
        //wakeOnDock(); - CB
      } else {
        wakeOffDock();
        wakeup();
      }
    } else {
      //wakeup(); - CB, no wakeup Roomba is cleaning!
    }
  }

  // Report INFO
  if(now - lastInfoMsgTime > 60000) {
    lastInfoMsgTime = now;
    DLOG("Send info for roomba with MQTT\n");
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    int updays = millis()/86400000;
    int uphours = millis()/3600000 - updays*24;
    int upminutes = millis()/60000 - updays*1440 - uphours*60;
    int upseconds = millis()/1000 - upminutes*60 - updays*86400 - uphours*3600;
    // String uptime = updays + "T" + uphours + ":" + upminutes + ":" + upseconds;
    char uptime[15];
    sprintf(uptime, "%dT%02d:%02d:%02d", updays, uphours, upminutes, upseconds);
    root["UPTIME"] = uptime;
    root["Hostname"] = WiFi.hostname();
    root["IPAddress"] = wifiClient.localIP().toString();
    root["RSSI"] = WiFi.RSSI();
    root["SSID"] = WiFi.SSID();
    root["COMPILE_DATE"] = __DATE__ " " __TIME__;
    String jsonStr;
    root.printTo(jsonStr);
    mqttClient.publish(infoTopic, jsonStr.c_str());
    //roomba.stream(sensors, sizeof(sensors));
    //readSensorPacket();
  }

  // Report the status over mqtt at fixed intervals
  if (now - lastStateMsgTime > 10000) {
    lastStateMsgTime = now;
    if (now - roombaState.timestamp > 30000 || roombaState.sent) {
      DLOG("Roomba state already sent (%.1fs old)\n", (now - roombaState.timestamp)/1000.0);
      DLOG("Request stream\n");
      DLOG("SensorsSize:%d\n",sizeof(sensors));
      roomba.stream(sensors, sizeof(sensors));
    } else {
      DLOG("send roomba status\n");
      sendStatus();
      sendStatusHA();
      roombaState.sent = true;
    }
    sleepIfNecessary();
  }

  readSensorPacket();
  mqttClient.loop();
}
