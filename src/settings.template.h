#define MQTT_SERVER "123.123.123.123"
#define MQTT_PORT 1883
#define MQTT_UID "RGBClock"
#define MQTT_USER "username"
#define MQTT_PASSWORD "password"

#define NTP_HOST "pool.ntp.org"
#define NTP_UPDATE_INTERVAL_MS 5000

// MQTT integration is like a RGB light in Home Assistant
#define MQTT_TOPIC_SET "home/rgb_clock/set"
#define MQTT_TOPIC_STATE "home/rgb_clock/state"
#define MQTT_TOPIC_SET_BRT "home/rgb_clock/set_brightness"
#define MQTT_TOPIC_BRT "home/rgb_clock/brightness"
#define MQTT_TOPIC_SET_COLOR "home/rgb_clock/set_color_rgb"
#define MQTT_TOPIC_COLOR "home/rgb_clock/color_rgb"

#define MQTT_DISCOVERY_TOPIC "homeassistant/light/rgb_clock/config"
#define MQTT_DISCOVERY_NAME "RGB Clock"
#define MQTT_DISCOVERY_UID "rgb_clock"
#define MQTT_DISCOVERY_DEVICE_NAME "RGB Clock"
#define MQTT_DISCOVERY_DEVICE_UID "rgb_clock"
#define MQTT_DISCOVERY_DEVICE_MANUFACTURER "xatLabs"
#define MQTT_DISCOVERY_DEVICE_DESCRIPTION "7-Segment RGB clock with WS2812 LEDs"
#define MQTT_DISCOVERY_INTERVAL_MS 60000

// Variables for Station WiFi
const char* STA_SSID = "WiFi SSID";
const char* STA_PASS = "WiFI Password";
