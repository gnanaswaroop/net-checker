#include <ESP8266WiFi.h>          //ESP8266 Core WiFi Library (you most likely already have this in your sketch)

#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>
#include <FastLED.h>
#include <SSD1306.h>
#include <OLEDDisplayUi.h>

#include <Button.h>
#include <ButtonEventCallback.h>
#include <PushButton.h>
#include <Bounce2.h>

// DHT library to manage the temperature sensor
#include <SimpleDHT.h>

// enables UDP
#include <WiFiUdp.h>

// JSON management library
#include <ArduinoJson.h>

// time management libraries - uses millis() to manage time
#include <Time.h>
#include <TimeLib.h>

// Constants
#define BUTTON_LONG_PRESS_TIME_THRESHOLD 2000
#define BUTTON_SUPER_LONG_PRESS_FOR_RESET 10000

// Fast LED related properties
#define RGB_LED_PIN D7
#define FASTLED_ESP8266_RAW_PIN_ORDER
#define NUM_LEDS 1
#define COLOR_ORDER RGB
#define RGB_LED_BRIGHTNESS 30
#define RGB_LED_MODEL WS2811

#define DHT22_PIN D4

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define READ_STOCK_EVERY 30000

const size_t MAX_CONTENT_SIZE = 512;

#define HTTP_PORT 80

// initialize
SSD1306 display(0x3c, D2, D1);
OLEDDisplayUi ui ( &display );

int screenW = 128;
int screenH = 64;
int clockCenterX = screenW/2;
int clockCenterY = ((screenH-16)/2)+16;   // top yellow part is 16 px height
int clockRadius = 23;

WiFiManager wifiManager;
CRGB led_array[NUM_LEDS];

PushButton *leftButton;
PushButton *rightButton;

// timezonedb host & URLs
const char *timezonedbhost = "api.timezonedb.com";
const char *timezoneparams = "/v2/get-time-zone?key=CO5WO1XIHAFN&by=zone&zone=Asia/Kolkata&format=json";

unsigned int localPort = 2390;      // local port to listen for UDP packets
IPAddress timeServerIP; // time.nist.gov NTP server address
const char* ntpServerName = "time.nist.gov";
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets

// A UDP instance to let us send and receive packets over UDP
WiFiUDP udp;
time_t timeVal;

SimpleDHT22 dht22;

String cachedTemperature;
String cachedHumidity;

float cachedStockPrice;
String cachedStockPriceString = "--.--";

unsigned long cacheUpdatedTimeStampTemperature;
unsigned long cacheUpdatedTimeStampStock;

bool isAPModeON = false;

void initializeRGB() {
  FastLED.addLeds<RGB_LED_MODEL, RGB_LED_PIN, COLOR_ORDER>(led_array, NUM_LEDS);
  FastLED.setBrightness(  RGB_LED_BRIGHTNESS );
  Serial.println("RGB LED initialized");
}

void initializeTime() {
  // set up sync provider for getting time every 180 seconds
  setSyncProvider(getTime);
  setSyncInterval(600);
}

// duration reports back the total time that the button was held down
void onButtonReleasedLeft(Button& btn, uint16_t duration){

  Serial.print("Left button released after ");
  Serial.print(duration);
  Serial.println(" ms");

  leftButtonClick();
}

// duration reports back the total time that the button was held down
void onButtonReleasedRight(Button& btn, uint16_t duration){

  Serial.print("Right button released after ");
  Serial.print(duration);
  Serial.println(" ms");

  if(duration > BUTTON_SUPER_LONG_PRESS_FOR_RESET) {
    resetAndRestartDevice();
  } else if(duration > BUTTON_LONG_PRESS_TIME_THRESHOLD) {
    rightButtonLongPress();
  } else {
    rightButtonClick();
  }
}

void timeFrame(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(clockCenterX+x, 16+y, ((String) day()) + " " + monthStr(month()) + " " + ((String) year()));
  display->setFont(ArialMT_Plain_24);
  display->drawString(clockCenterX+x, 32+y, dayStr(weekday()));
}

int totalHTTPSCalls;
int failedHTTPSCalls;
int continousFailures;

void httpsStatusFrame(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(clockCenterX+x, 16+y, "Total " + String(totalHTTPSCalls));
  display->drawString(clockCenterX+x, 32+y, "Failed " + String(failedHTTPSCalls));
  display->drawString(clockCenterX+x, 48+y, "Continous " + String(continousFailures));
}

// utility function for digital clock display: prints leading 0
String twoDigits(int digits){
  if(digits < 10) {
    String i = '0'+ String(digits);
    return i;
  }
  else {
    return String(digits);
  }
}

String getFormattedTime() {
  String timenow = String(hour())+ (second()%2? ":" : " ")
  + twoDigits(minute())+ (second()%2? ":" : " ")
  + twoDigits(second());
  return timenow;
}

void clockOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {

  String timenow = getFormattedTime();
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(clockCenterX, 0, timenow );
}

boolean showHelloMsgOnce = false;
unsigned long helloStartTimeStamp;
int counter = 0;

void beforeConnectionOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  if(WiFi.SSID() == NULL && WiFi.status() != WL_CONNECTED) {
    display->drawString(clockCenterX, 0, "Configure");
  } else{
    display->drawString(clockCenterX, 0, "Booting");
  }
}

void helloFrame(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  display->clear();
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(clockCenterX+x, 16+y, "Hello World");
  if(WiFi.SSID() == NULL && WiFi.status() != WL_CONNECTED) {
    display->drawString(clockCenterX+x, 32+y, "Connect to WiFi ");
    display->drawString(clockCenterX+x, 48+y, "'InternetTester'");
  } else {
    display->drawString(clockCenterX+x, 32+y, "Connecting to WiFi ");
    display->drawString(clockCenterX+x, 48+y, WiFi.SSID());
  }
}

void beforeConnectionFrame(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  if (WiFi.status() != WL_CONNECTED) {
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(ArialMT_Plain_16);
    display->drawString(clockCenterX+x, 16+y, "Connect to Wifi");
    display->drawString(clockCenterX+x, 32+y, "'InternetTester'");
    display->drawString(clockCenterX+x, 48+y, "from phone");
  } else  {
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(ArialMT_Plain_16);
    display->drawString(clockCenterX+x, 16+y, "Connecting to ");
    display->drawString(clockCenterX+x, 32+y, WiFi.SSID());
  }
}

FrameCallback frames[] = {httpsStatusFrame };
FrameCallback beforeConnectionframes[] = { helloFrame };

// how many frames are there?
int frameCount = 1;
int overlaysCount = 1;

// Overlays are statically drawn on top of a frame eg. a clock
OverlayCallback overlays[] = { clockOverlay };
OverlayCallback beforeConnectionOverlays[] = { beforeConnectionOverlay };

void initializeDisplay()
{
  ui.setTargetFPS(60);
  ui.disableAllIndicators();

  ui.setFrames(beforeConnectionframes, 1);
  ui.setOverlays(beforeConnectionOverlays, 1);
  ui.init();
  display.flipScreenVertically();
  ui.update();

}

void initializeDisplayPostConnection() {
  ui.setFrameAnimation(SLIDE_LEFT);
  ui.setFrames(frames, frameCount);
  ui.setOverlays(overlays, overlaysCount);
  ui.init();

  display.flipScreenVertically();
}

// Time code borrowed from - https://github.com/adityarao/esplights/blob/master/temptime_04/temptime_04.ino
time_t getTime()
{
  return getTimeFromTimeZoneDB(timezonedbhost, timezoneparams);
}

boolean isNumber0(String line){
  return isDigit(line.charAt(0)) && line.toInt() == 0;
}

String getStockPrice(boolean overrideFlag) {

  unsigned long currentTimeStamp = millis();
  if(!overrideFlag && currentTimeStamp - cacheUpdatedTimeStampStock < READ_STOCK_EVERY) {
    return "";
  }

  cacheUpdatedTimeStampStock = millis();

  // https://www.alphavantage.co/query?function=BATCH_STOCK_QUOTES&symbols=NOW&apikey=9SJ1EOQEDZ0WCA4Y

  unsigned long startTime = millis();

  //https://httpbin.org/get
  String host = "httpbin.org";
  String url = "/get";

  totalHTTPSCalls++;

  WiFiClientSecure client;
  if (!client.connect(host, 443)) {
    Serial.print("Failed to connect to :");
    Serial.println(host);
    failedHTTPSCalls++;
    continousFailures++;
    blinkColor(255, 0, 0, 2);
    return "";
  }

  // Serial.println("connected !....");

  // send a GET request
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
  "Host: " + host + "\r\n" +
  "User-Agent: ESP8266\r\n" +
  "Accept: */*\r\n" +
  "Connection: close\r\n\r\n");

  // bypass HTTP headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    // Serial.print( "Header: ");
    // Serial.println(line);
    if (line == "\r") {
      break;
    }
  }

  // // get the length component
  // while (client.connected()) {
  //   String line = client.readStringUntil('\n');
  //   Serial.print( "Body Length: ");
  //   Serial.println(line);
  //   break;
  // }

  String json = "";

  int counter = 0;
  // get the actual body, which has the JSON content
  while (client.connected() && client.available()) {
    String line = client.readStringUntil('\n');
    if(isNumber0(line)) { // Quit when body length 0 is received.
      break;
    }

    json += line;
  }

  // Serial.println(json);

  // Use Arduino JSON libraries parse the JSON object
  const size_t BUFFER_SIZE =
  JSON_OBJECT_SIZE(8)    // the root object has 8 elements
  + 1000;    // additional space for strings

  // Allocate a temporary memory pool
  DynamicJsonBuffer jsonBuffer(BUFFER_SIZE);
  Serial.println("Preparse " + json);
  JsonObject& root = jsonBuffer.parseObject(json);

  if (!root.success()) {
    Serial.println("Stock JSON parsing failed!");
    failedHTTPSCalls++;
    continousFailures++;
    blinkColor(255, 0, 0, 2);
    return "";
  }

  continousFailures=0;

  return "";
}

// connect to timezone db !
time_t getTimeFromTimeZoneDB(const char* host, const char* params)
{
  Serial.print("Trying to connect to ");
  Serial.println(host);

  // Use WiFiClient for timezonedb
  WiFiClient client;
  if (!client.connect(host, HTTP_PORT)) {
    Serial.print("Failed to connect to :");
    Serial.println(host);
    blinkColor(255, 0, 0, 2);
    setSyncProvider(getTime);
    return 0;
  }

  Serial.println("connected !....");

  // send a GET request
  client.print(String("GET ") + timezoneparams + " HTTP/1.1\r\n" +
  "Host: " + host + "\r\n" +
  "User-Agent: ESP8266\r\n" +
  "Accept: */*\r\n" +
  "Connection: close\r\n\r\n");

  // bypass HTTP headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    // Serial.print( "Header: ");
    // Serial.println(line);
    if (line == "\r") {
      break;
    }
  }

  // get the length component
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    // Serial.print( "Body Length: ");
    // Serial.println(line);
    break;
  }

  String line = "";

  // get the actual body, which has the JSON content
  while (client.connected()) {
    line = client.readStringUntil('\n');
    // Serial.print( "Json body: ");
    // Serial.println(line);
    break;
  }

  // Use Arduino JSON libraries parse the JSON object
  const size_t BUFFER_SIZE =
  JSON_OBJECT_SIZE(8)    // the root object has 8 elements
  + MAX_CONTENT_SIZE;    // additional space for strings

  // Allocate a temporary memory pool
  DynamicJsonBuffer jsonBuffer(BUFFER_SIZE);

  JsonObject& root = jsonBuffer.parseObject(line);

  if (!root.success()) {
    Serial.println("JSON parsing failed!");
    blinkColor(255, 0, 0, 2);
    return -1;
  }

  // Serial.println("Parsing a success ! -- ");
  blinkColor(0,0, 255, 2);

  // 'timestamp' has the exact UNIX time for the zone specified by zone param
  Serial.println((long)root["timestamp"]);

  return (time_t)root["timestamp"];
}

void blinkColor(int red, int green, int blue, int frequency)
{
  while (frequency) {

    CRGB color = CRGB(green, red, blue);
    fill_solid(led_array, NUM_LEDS, color);
    FastLED.show();                                                           //This displays the color on the RGB
    delay(200);

    CRGB colorBlank = CRGB(0, 0, 0);
    fill_solid(led_array, NUM_LEDS, colorBlank);
    FastLED.show();

    delay(200);
    frequency--;
  }
}

void leftButtonClick() {
  // getStockPrice(true);
  Serial.println("Left Button Long press");
}

void rightButtonLongPress() {
  Serial.println("Right Button Long press");
}

void rightButtonClick() {
  Serial.println("Right Button press");
}

void initializeButtons() {

  leftButton = new PushButton(D3);
  rightButton = new PushButton(D6);

  leftButton->onRelease(onButtonReleasedLeft);
  rightButton->onRelease(onButtonReleasedRight);
}

void apModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("AP Mode ON");
  isAPModeON = true;
}

void saveConfigCallback() {
  Serial.println("AP Mode OFF");
  isAPModeON = false;
}

void resetAndRestartDevice() {
  Serial.println("Reset Device Success");
  wifiManager.resetSettings();
  ESP.eraseConfig();
  Serial.println("Restarting Device now");
  ESP.reset();
}

void setup() {
  Serial.begin(115200);
  initializeDisplay();
  initializeButtons();
  initializeRGB();
  wifiManager.setAPCallback(apModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.autoConnect("InternetTester");

  Serial.println(WiFi.localIP());
  Serial.println(WiFi.macAddress());

  initializeTime();
  getStockPrice(true);

  initializeDisplayPostConnection();
}

void loop() {
  int remainingTimeBudget = ui.update();

  leftButton->update();
  rightButton->update();

  if (remainingTimeBudget > 0) {
    // You can do some work here
    // Don't do stuff if you are below your
    // time budget.
    getStockPrice(false);
    delay(remainingTimeBudget);
  }
}
