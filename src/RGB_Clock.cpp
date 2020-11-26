/*
   WS2812 RGB Digital Clock for ESP8266
   (C) 2016-2020 Julian Metzler
*/

#include <TimeLib.h> // https://github.com/PaulStoffregen/Time
#include <NtpClientLib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <FS.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <PubSubClient.h> // MQTT_MAX_PACKET_SIZE needs to be increased from the default for Home Assistant discovery to work

#include "settings.h"

/*
   TYPEDEFS
*/

enum ColorMapType {
  MT_DIG_POSITION,  // Color for each possible digit position
  MT_DIG_VALUE,     // Color for each possible digit value
  MT_SEG_POSITION,  // Color for each possible segment position
  MT_SEG_RANDOM,    // (Pseudo-)Random choice for each segment
};

struct ColorMap {
  ColorMapType mapType;
  const unsigned long* cMap;
  byte numColors;
};

enum ControlSource {
  CS_STANDALONE,
  CS_MQTT,
};

/*
   CONSTANTS
*/

#define LDR_PIN A0
#define DATA_PIN 13
#define LEDS_PER_SEGMENT 3
#define NUM_LEDS (4 * 7 * LEDS_PER_SEGMENT)

// Mapping of indexes to segment combinations. This is the link between SEG_BUF and DIG_BUF.
const byte SEG_CONF[12] = {
  //            ID  Val
  0b1110111, // 0   0
  0b0100100, // 1   1
  0b1011101, // 2   2
  0b1101101, // 3   3
  0b0101110, // 4   4
  0b1101011, // 5   5
  0b1111011, // 6   6
  0b0100101, // 7   7
  0b1111111, // 8   8
  0b1101111, // 9   9
  0b0001000, // 10  -
  0b0000000, // 11  [Off]
};

const unsigned long cMapValuesAllWhite[4] = {
  0xFFFFFF, // Digit 1
  0xFFFFFF, // Digit 2
  0xFFFFFF, // Digit 3
  0xFFFFFF, // Digit 4
};

const unsigned long cMapValuesDigitPosition[4] = {
  0xFF0000, // Digit 1
  0x00FF00, // Digit 2
  0x0000FF, // Digit 3
  0xFFFFFF, // Digit 4
};

const unsigned long cMapValuesDefault[12] = {
  // See SEG_CONF for mapping of indexes to values
  0x00FF00,
  0xFF0000,
  0x0000FF,
  0x00FFCC,
  0xFF00FF,
  0xFFFF00,
  0x00FF80,
  0xFF0080,
  0xFF8000,
  0x0080FF,
  0x8000FF,
  0x000000,
};

unsigned long cMapValuesCustom1[4] = {
  0xFFFFFF, // Digit 1
  0xFFFFFF, // Digit 2
  0xFFFFFF, // Digit 3
  0xFFFFFF, // Digit 4
};

unsigned long cMapValuesCustom2[4] = {
  0xFFFFFF, // Digit 1
  0xFFFFFF, // Digit 2
  0xFFFFFF, // Digit 3
  0xFFFFFF, // Digit 4
};

unsigned long cMapValuesMQTT[4] = {
  0xFFFFFF, // Digit 1
  0xFFFFFF, // Digit 2
  0xFFFFFF, // Digit 3
  0xFFFFFF, // Digit 4
};

const ColorMap cmAllWhite = {MT_DIG_POSITION, cMapValuesAllWhite, 4};
const ColorMap cmDigitPosition = {MT_DIG_POSITION, cMapValuesDigitPosition, 4};
const ColorMap cmDigitValue = {MT_DIG_VALUE, cMapValuesDefault, 12};
const ColorMap cmSegmentPosition = {MT_SEG_POSITION, cMapValuesDefault, 12};
const ColorMap cmSegmentRandom = {MT_SEG_RANDOM, cMapValuesDefault, 12};
const ColorMap cmCustom1 = {MT_DIG_POSITION, cMapValuesCustom1, 4};
const ColorMap cmCustom2 = {MT_DIG_POSITION, cMapValuesCustom2, 4};
const ColorMap cmMQTT = {MT_DIG_POSITION, cMapValuesMQTT, 4};

const ColorMap* COLOR_MAPS[7] = {
  &cmAllWhite,
  &cmDigitPosition,
  &cmDigitValue,
  &cmSegmentPosition,
  &cmSegmentRandom,
  &cmCustom1,
  &cmCustom2,
};

/*
   GLOBAL VARIABLES
*/

WiFiClient client;
PubSubClient mqttClient(client);
ESP8266WebServer server(80);

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NUM_LEDS, DATA_PIN, NEO_GRB + NEO_KHZ800);

// HIGH LEVEL INTERFACE TO DISPLAY CONTENTS
// Current digit values. Similar to SEG_BUF, but contains the index of the value displayed.
// (e.g. 0-9 are the digits 0-9, 10 is a - sign etc.
// This way, you don't have to map all 128 possible segment combinations for a value-based color map,
// but only a subset that makes sense.
byte DIG_BUF[4] = {11, 11, 11, 11};

// LOW LEVEL INTERFACE TO DISPLAY CONTENTS
// Current segment buffer. Contains the bit combinations of the active segments.
byte SEG_BUF[4] = {0x00, 0x00, 0x00, 0x00};

// The current time
int curTime = 0;

// The display brightness
byte curBrightness = 255;
byte dayBrightness = 255;
byte nightBrightness = 64;
byte mqttBrightness = 255;

// The selected colormap
byte curColorMapId = 0;
const ColorMap* curColorMap = &cmAllWhite;
byte dayColorMapId = 0;
const ColorMap* dayColorMap = &cmAllWhite;
byte nightColorMapId = 0;
const ColorMap* nightColorMap = &cmAllWhite;

// The current mode
int nightModeStartTime = 0;
int nightModeEndTime = 0;
bool nightMode = false;
// Bit order:
// 0 - Forcing disabled (0) or enabled (1)
// 1 - Force night (0) or day (1) mode
// 2 - Force until next switch (0) or permanently (1)
byte forceMode = 0x00;

// Control source
ControlSource ctrlSrc = CS_STANDALONE;

// MQTT variables
#define MQTT_PAYLOAD_ARR_LEN 256
char mqttPayload[MQTT_PAYLOAD_ARR_LEN] = {0x00};
bool mqttOnState = true;
unsigned long mqttColorR = 255;
unsigned long mqttColorG = 255;
unsigned long mqttColorB = 255;

/*
   CONFIGURATION SAVE & RECALL (EEPROM)
*/

void EEPROMWriteByte(int address, byte value) {
  EEPROM.write(address, value);
}

void EEPROMWriteInt(int address, unsigned int value) {
  EEPROM.write(address, value);
  EEPROM.write(address + 1, value >> 8);
}

void EEPROMWriteLong(int address, unsigned long value) {
  EEPROM.write(address, value);
  EEPROM.write(address + 1, value >> 8);
  EEPROM.write(address + 2, value >> 16);
  EEPROM.write(address + 3, value >> 24);
}

byte EEPROMReadByte(int address) {
  return EEPROM.read(address);
}

unsigned int EEPROMReadInt(int address) {
  return (unsigned int)EEPROM.read(address) | (unsigned int)EEPROM.read(address + 1) << 8;
}

unsigned long EEPROMReadLong(int address) {
  return (unsigned long)EEPROM.read(address) | (unsigned long)EEPROM.read(address + 1) << 8 | (unsigned long)EEPROM.read(address + 2) << 16 | (unsigned long)EEPROM.read(address + 3) << 24;
}

void saveConfiguration() {
  EEPROMWriteInt(0, nightModeStartTime);
  EEPROMWriteInt(2, nightModeEndTime);
  EEPROMWriteByte(4, forceMode);
  EEPROMWriteByte(5, ctrlSrc);

  EEPROMWriteByte(10, dayColorMapId);
  EEPROMWriteByte(11, dayBrightness);

  EEPROMWriteByte(20, nightColorMapId);
  EEPROMWriteByte(21, nightBrightness);

  EEPROMWriteLong(30, cMapValuesCustom1[0]);
  EEPROMWriteLong(34, cMapValuesCustom1[1]);
  EEPROMWriteLong(38, cMapValuesCustom1[2]);
  EEPROMWriteLong(42, cMapValuesCustom1[3]);

  EEPROMWriteLong(60, cMapValuesCustom2[0]);
  EEPROMWriteLong(64, cMapValuesCustom2[1]);
  EEPROMWriteLong(68, cMapValuesCustom2[2]);
  EEPROMWriteLong(72, cMapValuesCustom2[3]);

  EEPROM.commit();
}

void loadConfiguration() {
  nightModeStartTime = EEPROMReadInt(0);
  nightModeEndTime = EEPROMReadInt(2);
  forceMode = EEPROMReadByte(4);
  ctrlSrc = (ControlSource)EEPROMReadByte(5);

  dayColorMapId = EEPROMReadByte(10);
  dayBrightness = EEPROMReadByte(11);

  nightColorMapId = EEPROMReadByte(20);
  nightBrightness = EEPROMReadByte(21);

  cMapValuesCustom1[0] = EEPROMReadLong(30);
  cMapValuesCustom1[1] = EEPROMReadLong(34);
  cMapValuesCustom1[2] = EEPROMReadLong(38);
  cMapValuesCustom1[3] = EEPROMReadLong(42);

  cMapValuesCustom2[0] = EEPROMReadLong(60);
  cMapValuesCustom2[1] = EEPROMReadLong(64);
  cMapValuesCustom2[2] = EEPROMReadLong(68);
  cMapValuesCustom2[3] = EEPROMReadLong(72);

  dayColorMap = COLOR_MAPS[dayColorMapId];
  nightColorMap = COLOR_MAPS[nightColorMapId];
}

/*
   HELPER FUNCTIONS
*/

void reverseArray(unsigned long* a, int sz) {
  int i, j;
  for (i = 0, j = sz; i < j; i++, j--) {
    unsigned long tmp = a[i];
    a[i] = a[j];
    a[j] = tmp;
  }
}

void rotateArray(unsigned long* array, int size, int amt) {
  if (amt < 0) amt = size + amt;
  reverseArray(array, size - amt - 1);
  reverseArray(array + size - amt, amt - 1);
  reverseArray(array, size - 1);
}

bool timeInRange(int time, int rangeStart, int rangeEnd) {
  if (rangeEnd >= rangeStart) {
    return time >= rangeStart && time < rangeEnd;
  } else {
    return time >= rangeStart || time < rangeEnd;
  }
}

void updateCurrentMode() {
  switch(ctrlSrc) {
    case CS_MQTT: {
      nightMode = false;
      curBrightness = mqttOnState ? mqttBrightness : 0;
      curColorMap = &cmMQTT;
      break;
    }

    default:
    case CS_STANDALONE: {
      bool shouldBeNightMode = timeInRange(curTime, nightModeStartTime, nightModeEndTime);
      if (forceMode & 1) {
        // Forcing enabled
        nightMode = !(forceMode & 2);
        if (!(forceMode & 4)) {
          // Temporary forcing
          if (nightMode == shouldBeNightMode) {
            // Disable forcing if we are in the right time again
            forceMode &= ~1;
          }
        }
      } else {
        // No forcing
        nightMode = shouldBeNightMode;
      }
      curBrightness = nightMode ? nightBrightness : dayBrightness;
      curColorMap = nightMode ? nightColorMap : dayColorMap;
      curColorMapId = nightMode ? nightColorMapId : dayColorMapId;
      break;
    }
  }
}

int str2int(char* str, int len) {
  int i;
  int ret = 0;
  for (i = 0; i < len; ++i)
  {
    ret = ret * 10 + (str[i] - '0');
  }
  return ret;
}

/*
   MQTT FUNCTIONS
*/

void mqttConnect() {
  while (!mqttClient.connected()) {
    if (mqttClient.connect(MQTT_UID, MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe(MQTT_TOPIC_SET);
      mqttClient.subscribe(MQTT_TOPIC_SET_BRT);
      mqttClient.subscribe(MQTT_TOPIC_SET_COLOR);
    } else {
      delay(5000);
    }
  }
}

void mqttSendState() {
  mqttClient.publish(MQTT_TOPIC_STATE, mqttOnState ? "ON" : "OFF");
}

void mqttSendBrightness() {
  memset(mqttPayload, 0x00, MQTT_PAYLOAD_ARR_LEN);
  sprintf(mqttPayload, "%d", (int)mqttBrightness);
  mqttClient.publish(MQTT_TOPIC_BRT, mqttPayload);
}

void mqttSendColor() {
  memset(mqttPayload, 0x00, MQTT_PAYLOAD_ARR_LEN);
  sprintf(mqttPayload, "%d,%d,%d", (int)mqttColorR, (int)mqttColorG, (int)mqttColorB);
  mqttClient.publish(MQTT_TOPIC_COLOR, mqttPayload);
}

void updateAll(); // Defined further down
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_SET) ==  0) {
    if (strncmp((char*)payload, "ON", length) == 0) {
      mqttOnState = true;
      updateAll();
      mqttSendState();
    } else if (strncmp((char*)payload, "OFF", length) == 0) {
      mqttOnState = false;
      updateAll();
      mqttSendState();
    }
  } else if (strcmp(topic, MQTT_TOPIC_SET_BRT) ==  0) {
    mqttBrightness = (byte)str2int((char*)payload, length);
    updateAll();
    mqttSendBrightness();
  } else if (strcmp(topic, MQTT_TOPIC_SET_COLOR) ==  0) {
    memcpy(mqttPayload, (char*)payload, length);
    mqttPayload[length] = 0x00;
    sscanf(mqttPayload, "%d,%d,%d", &mqttColorR, &mqttColorG, &mqttColorB);
    unsigned long color = (mqttColorR << 16) | (mqttColorG << 8) | mqttColorB;
    cMapValuesMQTT[0] = color;
    cMapValuesMQTT[1] = color;
    cMapValuesMQTT[2] = color;
    cMapValuesMQTT[3] = color;
    updateAll();
    mqttSendColor();
  }
}

void mqttDiscovery() {
  String payload;
  payload += "{";
  payload += "\"name\": \"";
  payload += MQTT_DISCOVERY_NAME;
  payload += "\",";
  payload += "\"unique_id\": \"";
  payload += MQTT_DISCOVERY_UID;
  payload += "\",";
  payload += "\"command_topic\": \"";
  payload += MQTT_TOPIC_SET;
  payload += "\",";
  payload += "\"state_topic\": \"";
  payload += MQTT_TOPIC_STATE;
  payload += "\",";
  payload += "\"brightness_command_topic\": \"";
  payload += MQTT_TOPIC_SET_BRT;
  payload += "\",";
  payload += "\"brightness_state_topic\": \"";
  payload += MQTT_TOPIC_BRT;
  payload += "\",";
  payload += "\"rgb_command_topic\": \"";
  payload += MQTT_TOPIC_SET_COLOR;
  payload += "\",";
  payload += "\"rgb_state_topic\": \"";
  payload += MQTT_TOPIC_COLOR;
  payload += "\",";
  payload += "\"device\": {";
  payload += "\"name\": \"";
  payload += MQTT_DISCOVERY_DEVICE_NAME;
  payload += "\",";
  payload += "\"ids\": [\"";
  payload += MQTT_DISCOVERY_DEVICE_UID;
  payload += "\"],";
  payload += "\"mdl\": \"";
  payload += MQTT_DISCOVERY_DEVICE_DESCRIPTION;
  payload += "\",";
  payload += "\"mf\": \"";
  payload += MQTT_DISCOVERY_DEVICE_MANUFACTURER;
  payload += "\"}}";

  mqttClient.publish(MQTT_DISCOVERY_TOPIC, payload.c_str());
}

/*
   DISPLAY RELATED FUNCTIONS
*/

byte getSegmentIndex(byte segment) {
  // Segment order per digit: b a c f g e d
  switch (segment) {
    case 0: return 1; // a
    case 1: return 0; // b
    case 2: return 2; // c
    case 3: return 6; // d
    case 4: return 5; // e
    case 5: return 3; // f
    case 6: return 4; // g
    default: return 6;
  }
}

unsigned long applyBrightness(unsigned long color) {
  unsigned long red, green, blue;
  red = (color >> 16) & 0xFF;
  green = (color >> 8) & 0xFF;
  blue = color & 0xFF;

  // 0xFF * 0xFF = 65025 (0xFE01)
  red = map(red * curBrightness, 0, 65025, 0, 255);
  green = map(green * curBrightness, 0, 65025, 0, 255);
  blue = map(blue * curBrightness, 0, 65025, 0, 255);

  return red << 16 | green << 8 | blue;
}

void setSegmentColor(byte digit, byte segment, unsigned long color) {
  byte segmentIndex = getSegmentIndex(segment);
  byte startPos = digit * (7 * LEDS_PER_SEGMENT) + segmentIndex * LEDS_PER_SEGMENT;
  for (byte i = 0; i < LEDS_PER_SEGMENT; i++) {
    pixels.setPixelColor(startPos + i, applyBrightness(color));
  }
}

void clearDisplay() {
  pixels.clear();
}

void updateDisplay() {
  pixels.show();
}

void setAllSegmentColors(unsigned long* colors) {
  // Set each segment to the specified color
  // Array order: abcdefg abcdefg abcdefg abcdefg
  for (byte i = 0; i < 28; i++) {
    byte digit = i / 7;
    byte segment = i % 7;
    setSegmentColor(digit, segment, colors[i]);
  }
}

unsigned long getColor(byte digit, byte segment, ColorMap cMap) {
  if (cMap.mapType == MT_DIG_POSITION) {
    return cMap.cMap[digit];
  } else if (cMap.mapType == MT_DIG_VALUE) {
    return cMap.cMap[DIG_BUF[digit]];
  } else if (cMap.mapType == MT_SEG_POSITION) {
    return cMap.cMap[segment];
  } else if (cMap.mapType == MT_SEG_RANDOM) {
    return cMap.cMap[random(0, cMap.numColors)];
  }
}

void setAllSegments(byte* segData) {
  // Set the segments as specified by segData using the colors specified by the color map
  // segData bit order: 0 g f e d c b a
  // segData order: Digit1 Digit2 Digit3 Digit4
  for (byte digit = 0; digit < 4; digit++) {
    for (byte segIndex = 0; segIndex < 7; segIndex++) {
      if (segData[digit] & (1 << segIndex)) {
        setSegmentColor(digit, segIndex, getColor(digit, segIndex, *curColorMap));
      } else {
        setSegmentColor(digit, segIndex, 0x000000);
      }
    }
  }
}

void formatInteger(byte* digBuf, int number, byte length) {
  // Format an integer into a digit buffer, cutting off the higher digits if necessary
  byte digits[4]; // 1s 10s 100s 1000s
  bool negative = number < 0;
  if (negative) number = -number;
  digits[3] = (number % 10000) / 1000;
  digits[2] = (number % 1000) / 100;
  digits[1] = (number % 100) / 10;
  digits[0] = number % 10;
  if (negative) digits[length - 1] = 10; // Hyphen, see SEG_CONF
  for (byte n = 0; n < length; n++) {
    digBuf[length - 1 - n] = digits[n];
  }
}

byte digitToSegments(byte digit) {
  // Get segment configuration for a digit (0 through 9)
  return SEG_CONF[digit];
}

void generateSegBuf(byte* segBuf, byte* digBuf) {
  // Generate the segment buffer from the digit buffer
  for (byte n = 0; n < 4; n++) {
    segBuf[n] = digitToSegments(digBuf[n]);
  }
}

void displayNumber(int number) {
  formatInteger(DIG_BUF, number, 4);
  generateSegBuf(SEG_BUF, DIG_BUF);
  setAllSegments(SEG_BUF);
  updateDisplay();
}

void updateAll() {
  updateCurrentMode();
  formatInteger(DIG_BUF, curTime, 4);
  generateSegBuf(SEG_BUF, DIG_BUF);
  setAllSegments(SEG_BUF);
  updateDisplay();
}

/*
   WEB SERVER
*/

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

String generateColorMapSelectMenu(byte colorMapId) {
  String page;
  page += "<select name='colormap'>";
  page += "<option value='0'";
  if (colorMapId == 0) page += " selected";
  page += ">All White</option>";
  page += "<option value='1'";
  if (colorMapId == 1) page += " selected";
  page += ">Per Digit</option>";
  page += "<option value='2'";
  if (colorMapId == 2) page += " selected";
  page += ">Per Number</option>";
  page += "<option value='3'";
  if (colorMapId == 3) page += " selected";
  page += ">Per Segment</option>";
  page += "<option value='4'";
  if (colorMapId == 4) page += " selected";
  page += ">Segment-Level Random</option>";
  page += "<option value='5'";
  if (colorMapId == 5) page += " selected";
  page += ">Custom 1</option>";
  page += "<option value='6'";
  if (colorMapId == 6) page += " selected";
  page += ">Custom 2</option>";
  page += "</select>";
  return page;
}

bool isCustomColorMap(byte colorMapId) {
  return colorMapId == 5 || colorMapId == 6;
}

String generateCustomColorMapSettingsForm(byte colorMapId, const ColorMap* colorMap) {
  String page;
  char colorFmt[6];
  page += "<h4>Custom Colour Scheme</h4>";
  page += "<form action='/setcustomcolors";
  page += colorMapId == 5 ? "1" : "2";
  page += "' method='POST'>";
  page += "<input type='color' name='digit1' value='#";
  sprintf(colorFmt, "%06x", colorMap->cMap[0]);
  page += colorFmt;
  page += "' />";
  page += "<input type='color' name='digit2' value='#";
  sprintf(colorFmt, "%06x", colorMap->cMap[1]);
  page += colorFmt;
  page += "' />";
  page += "<input type='color' name='digit3' value='#";
  sprintf(colorFmt, "%06x", colorMap->cMap[2]);
  page += colorFmt;
  page += "' />";
  page += "<input type='color' name='digit4' value='#";
  sprintf(colorFmt, "%06x", colorMap->cMap[3]);
  page += colorFmt;
  page += "' />";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  return page;
}

void handleRoot() {
  String page;
  page += "<html>";
  page += "<head>";
  page += "<link rel='shortcut icon' href='/favicon.ico'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<link rel='stylesheet' href='/rgbclock.css'>";
  page += "<title>RGB Clock</title>";
  page += "</head>";
  page += "<body>";
  page += "<h1>RGB Clock</h1>";

  page += "<iframe class='simulation' src='/simulation.html'></iframe>";

  char startTimeStr[6], endTimeStr[6];
  sprintf(startTimeStr, "%02i:%02i", nightModeStartTime / 100, nightModeStartTime % 100);
  sprintf(endTimeStr, "%02i:%02i", nightModeEndTime / 100, nightModeEndTime % 100);
  page += "<div id='mode-settings'>";
  page += "<form action='/setmodetimes' method='POST'>";
  page += "Night mode from ";
  page += "<input type='time' name='start' value='";
  page += startTimeStr;
  page += "'/>";
  page += " to ";
  page += "<input type='time' name='end' value='";
  page += endTimeStr;
  page += "'/>";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  page += "</div>";
  page += "<div id='mode-force'>";
  page += "<form action='/setmodeforce' method='POST'>";
  page += "<label><input type='checkbox' name='force-enabled' value='true' ";
  page += (forceMode & 1) ? "checked" : "";
  page += "/> Force Mode</label>";
  page += "<br />";
  page += "<label><input type='radio' name='force-which' value='day'";
  page += (forceMode & 2) ? "checked" : "";
  page += "/> Day Mode</label>";
  page += "<br />";
  page += "<label><input type='radio' name='force-which' value='night'";
  page += !(forceMode & 2) ? "checked" : "";
  page += "/> Night Mode</label>";
  page += "<br />";
  page += "<label><input type='checkbox' name='force-permanent' value='true'";
  page += (forceMode & 4) ? "checked" : "";
  page += "/> Permanent</label>";
  page += "<br />";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  page += "</div>";

  page += "<div id='ctrl-src'>";
  page += "<form action='/setctrlsrc' method='POST'>";
  page += "<label><input type='radio' name='ctrl-src' value='standalone'";
  page += (ctrlSrc == CS_STANDALONE) ? "checked" : "";
  page += "/> Internal Control</label>";
  page += "<br />";
  page += "<label><input type='radio' name='ctrl-src' value='mqtt'";
  page += (ctrlSrc == CS_MQTT) ? "checked" : "";
  page += "/> MQTT Control</label>";
  page += "<br />";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  page += "</div>";

  page += "<hr />";

  page += "<h2>Day Settings</h2>";
  page += "<div id='day-settings'>";
  page += "<h3>Colour Scheme</h3>";
  page += "<form action='/setdaycolormap' method='POST'>";
  page += generateColorMapSelectMenu(dayColorMapId);
  page += "<input type='submit' value='Set'/>";
  page += "</form>";

  if (isCustomColorMap(dayColorMapId)) {
    page += generateCustomColorMapSettingsForm(dayColorMapId, dayColorMap);
  }

  char dayBrightnessStr[4];
  sprintf(dayBrightnessStr, "%i", dayBrightness);
  page += "<h3>Brightness</h3>";
  page += "<form action='/setdaybrightness' method='POST'>";
  page += "<input type='range' name='brightness' min='0' max='255' step='1' value='";
  page += dayBrightnessStr;
  page += "'/>";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  page += "</div>";

  page += "<hr />";

  page += "<h2>Night Settings</h2>";
  page += "<div id='night-settings'>";
  page += "<h3>Colour Scheme</h3>";
  page += "<form action='/setnightcolormap' method='POST'>";
  page += generateColorMapSelectMenu(nightColorMapId);
  page += "<input type='submit' value='Set'/>";
  page += "</form>";

  if (isCustomColorMap(nightColorMapId)) {
    page += generateCustomColorMapSettingsForm(nightColorMapId, nightColorMap);
  }

  char nightBrightnessStr[4];
  sprintf(nightBrightnessStr, "%i", nightBrightness);
  page += "<h3>Brightness</h3>";
  page += "<form action='/setnightbrightness' method='POST'>";
  page += "<input type='range' name='brightness' min='0' max='255' step='1' value='";
  page += nightBrightnessStr;
  page += "'/>";
  page += "<input type='submit' value='Set'/>";
  page += "</form>";
  page += "</div>";

  page += "</body>";
  page += "</html>";
  server.send(200, "text/html", page);
}

void handle_setdaycolormap() {
  byte choice = strtol(server.arg("colormap").c_str(), NULL, 10);
  dayColorMapId = choice;
  dayColorMap = COLOR_MAPS[choice];

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setnightcolormap() {
  byte choice = strtol(server.arg("colormap").c_str(), NULL, 10);
  nightColorMapId = choice;
  nightColorMap = COLOR_MAPS[choice];

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setcustomcolors1() {
  char color1_str[8];
  strcpy(color1_str, server.arg("digit1").c_str());
  cMapValuesCustom1[0] = strtol(color1_str + 1, NULL, 16);
  char color2_str[8];
  strcpy(color2_str, server.arg("digit2").c_str());
  cMapValuesCustom1[1] = strtol(color2_str + 1, NULL, 16);
  char color3_str[8];
  strcpy(color3_str, server.arg("digit3").c_str());
  cMapValuesCustom1[2] = strtol(color3_str + 1, NULL, 16);
  char color4_str[8];
  strcpy(color4_str, server.arg("digit4").c_str());
  cMapValuesCustom1[3] = strtol(color4_str + 1, NULL, 16);

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setcustomcolors2() {
  char color1_str[8];
  strcpy(color1_str, server.arg("digit1").c_str());
  cMapValuesCustom2[0] = strtol(color1_str + 1, NULL, 16);
  char color2_str[8];
  strcpy(color2_str, server.arg("digit2").c_str());
  cMapValuesCustom2[1] = strtol(color2_str + 1, NULL, 16);
  char color3_str[8];
  strcpy(color3_str, server.arg("digit3").c_str());
  cMapValuesCustom2[2] = strtol(color3_str + 1, NULL, 16);
  char color4_str[8];
  strcpy(color4_str, server.arg("digit4").c_str());
  cMapValuesCustom2[3] = strtol(color4_str + 1, NULL, 16);

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setdaybrightness() {
  dayBrightness = strtol(server.arg("brightness").c_str(), NULL, 10);

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setnightbrightness() {
  nightBrightness = strtol(server.arg("brightness").c_str(), NULL, 10);

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setmodetimes() {
  char startTimeHoursStr[3], startTimeMinutesStr[3];
  strncpy(startTimeHoursStr, server.arg("start").c_str(), 2);
  strncpy(startTimeMinutesStr, server.arg("start").c_str() + 3, 2);
  int startTimeHours = strtol(startTimeHoursStr, NULL, 10);
  int startTimeMinutes = strtol(startTimeMinutesStr, NULL, 10);
  nightModeStartTime = startTimeHours * 100 + startTimeMinutes;

  char endTimeHoursStr[3], endTimeMinutesStr[3];
  strncpy(endTimeHoursStr, server.arg("end").c_str(), 2);
  strncpy(endTimeMinutesStr, server.arg("end").c_str() + 3, 2);
  int endTimeHours = strtol(endTimeHoursStr, NULL, 10);
  int endTimeMinutes = strtol(endTimeMinutesStr, NULL, 10);
  nightModeEndTime = endTimeHours * 100 + endTimeMinutes;

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setmodeforce() {
  if (server.arg("force-enabled") == "true") {
    forceMode |= 1;
  } else {
    forceMode &= ~1;
  }

  if (server.arg("force-which") == "day") {
    forceMode |= 2;
  } else if (server.arg("force-which") == "night") {
    forceMode &= ~2;
  }

  if (server.arg("force-permanent") == "true") {
    forceMode |= 4;
  } else {
    forceMode &= ~4;
  }

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_setctrlsrc() {
  if (server.arg("ctrl-src") == "standalone") {
    ctrlSrc = CS_STANDALONE;
  } else if (server.arg("ctrl-src") == "mqtt") {
    ctrlSrc = CS_MQTT;
  } else {
    ctrlSrc = CS_STANDALONE;
  }

  saveConfiguration();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handle_getsegmentcolors() {
  String page;
  unsigned long color;
  char colorStr[6];
  for (byte digit = 0; digit < 4; digit++) {
    for (byte segment = 0; segment < 7; segment++) {
      if ((SEG_BUF[digit] >> segment) & 0x01) {
        color = getColor(digit, segment, *curColorMap);
      } else {
        color = 0x000000;
      }
      sprintf(colorStr, "%06x", color);
      page += colorStr;
      page += "\n";
    }
  }
  server.send(200, "text/plain", page);
}

/*
   MAIN PROGRAM
*/

void setup() {
  ArduinoOTA.setHostname("RGB-Clock");
  ArduinoOTA.begin();

  EEPROM.begin(512);
  SPIFFS.begin();

  pinMode(LDR_PIN, INPUT);

  pixels.begin();

  formatInteger(DIG_BUF, 8888, 4);
  generateSegBuf(SEG_BUF, DIG_BUF);

  for (int b = 0; b < 256; b++) {
    curBrightness = b;
    setAllSegments(SEG_BUF);
    updateDisplay();
    delay(10);
  }
  
  displayNumber(-100);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.hostname("RGB-Clock");
  WiFi.begin(STA_SSID, STA_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    displayNumber(-(WiFi.status() + 100));
    delay(1000);
  }

  displayNumber(-200);
  delay(100);

  NTP.begin(NTP_HOST, 1, true);
  NTP.setInterval(3600);

  displayNumber(-300);
  delay(100);

  server.onNotFound(handleNotFound);
  server.on("/", handleRoot);
  server.on("/setdaycolormap", handle_setdaycolormap);
  server.on("/setnightcolormap", handle_setnightcolormap);
  server.on("/setcustomcolors1", handle_setcustomcolors1);
  server.on("/setcustomcolors2", handle_setcustomcolors2);
  server.on("/setdaybrightness", handle_setdaybrightness);
  server.on("/setnightbrightness", handle_setnightbrightness);
  server.on("/setmodetimes", handle_setmodetimes);
  server.on("/setmodeforce", handle_setmodeforce);
  server.on("/setctrlsrc", handle_setctrlsrc);
  server.on("/getsegmentcolors", handle_getsegmentcolors);
  server.serveStatic("/rgbclock.css", SPIFFS, "/rgbclock.css");
  server.serveStatic("/simulation.html", SPIFFS, "/simulation.html");
  server.serveStatic("/simulation.js", SPIFFS, "/simulation.js");
  server.serveStatic("/simulation.svg", SPIFFS, "/simulation.svg");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
  server.begin();

  displayNumber(-400);
  delay(100);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  displayNumber(-500);
  delay(100);

  loadConfiguration();

  displayNumber(-600);

  delay(100); // To avoid displaying 00:00 for a moment on startup
}

unsigned long timeRefreshNow = 0;
void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (!mqttClient.connected()) {
    ArduinoOTA.handle();
    displayNumber(-700);
    mqttConnect();
    delay(100);
    displayNumber(-701);
    mqttClient.loop();
    delay(100);
    displayNumber(-702);
    mqttDiscovery();
    delay(100);
  }
  mqttClient.loop();

  if (millis() - timeRefreshNow > 5000) {
    timeRefreshNow = millis();
    time_t now = NTP.getTime();
    curTime = hour(now) * 100 + minute(now);
    updateAll();
  }
}
