#include "secrets.h"

#define HOSTNAME "roomba" // e.g. roomba.local
#define BRC_PIN 14
#define ROOMBA_650_SLEEP_FIX 1

#define ADC_VOLTAGE_DIVIDER 44.551316985
//#define ENABLE_ADC_SLEEP

#define MQTT_SERVER "192.168.1.22"
#define MQTT_USER "homeassistant"
#define MQTT_COMMAND_TOPIC "vacuum/command"
#define MQTT_STATE_TOPIC "vacuum/STATUS"
#define MQTT_STATE_HA_TOPIC "vacuum/STATUSHA"
#define MQTT_INFO_TOPIC "vacuum/INFO"
#define MQTT_LWT_TOPIC "vacuum/LWT"
#define MQTT_DEBUG_TOPIC "vacuum/DEBUG"
