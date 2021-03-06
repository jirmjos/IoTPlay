/*
Read all the DS18B20 temperature sensors on the wire interface and
- publish to MQTT topic.
- publish to thingspeak
- publish on a local web server.


Requires the following libraries (in the platformio terminal):
1. PubSubClient: install with platformio lib install 89
2. DallasTemperature: install with platformio lib install 54
3. OneWire: install  with platformio lib install 1
4. WifiManager: istall with platformio lib install 567
5. Json: platformio lib install 64
*/

#define USEOLED 1
#define USEDHTSENSOR 1

extern "C"
{
    #include "user_interface.h"
}

#include <FS.h>                   // filesstem: this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>

#ifdef USEOLED
#include "SSD1306Spi.h"
#include "fonts.h"
// Initialize the OLED display using SPI
// D5 -> CLK
// D7 -> MOSI (DOUT)
// D0 -> RES
// D2 -> DC
// D8 -> CS
SSD1306Spi        display(D0, D2, D8);
#endif


//start OTA block
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
//end OTA block

//start FTP server block
#include <ESP8266FtpServer.h>
FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP8266FtpServer.h to see ftp verbose on serial
//end FTP server block



#define INT6 6
#define INT18 18
#define INT40 40
#define INT64 64

#ifdef USEDHTSENSOR
//begin DHT11 sensor
// Uncomment whatever type you're using!
#define DHTTYPE DHT11   // DHT 11
//#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
//#define DHTTYPE DHT21   // DHT 21 (AM2301)
#include <DHT.h>
#define PinDHT11 12  //DHT11 is on GPIO00, or D3 on the NodeMCU
//http://www.esp8266.com/viewtopic.php?f=29&t=3249
//https://github.com/gonium/esp8266-dht22-sensor/blob/master/src/main.cpp
// Initialize DHT sensor
// NOTE: For working with a faster than ATmega328p 16 MHz Arduino chip, like an ESP8266,
// you need to increase the threshold for cycle counts considered a 1 or 0.
// You can do this by passing a 3rd parameter for this threshold.  It's a bit
// of fiddling to find the right value, but in general the faster the CPU the
// higher the value.  The default for a 16mhz AVR is a value of 6.  For an
// Arduino Due that runs at 84mhz a value of 30 works.
// This is for the ESP8266 processor on ESP-01
DHT dht(PinDHT11, DHTTYPE, 11);
float humidity;
float temperatureDHT;
//end DHT11 sensor
#endif

//begin wifi manager block
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>
#include "WiFiManager.h"  //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#define CAPTIVEPORTALSSID "AutoConnectAP"
#define CAPTIVEPORTALPASSWD "Bug35*able"
bool shouldSaveConfig = false; //flag for saving data to file system
//end wifi manager block

//start http client GET block
#include <ESP8266HTTPClient.h>
//end http client GET block

//start time of day block
//https://github.com/PaulStoffregen/Time
#include <timelib.h>
//my timezone is 2 hours ahead of GMT
#define TZHOURS 2.0
// Update interval in seconds
#define NTPSYNCINTERVAL 300
bool timeofDayValid;

//end time of day block

//start web server
#define HTTPPORT 80
//WiFiServer wifiserver(HTTPPORT);
ESP8266WebServer * httpserver;
//end web server

WiFiClient espClient;

//start mqtt block
#include <PubSubClient.h>
PubSubClient mqttclient(espClient);
char mqtt_server[INT40];
char mqtt_user[INT40];
char mqtt_password[INT40];
char mqtt_port[INT6];
char mqtt_topic[INT64];
//end mqtt block

//start thingspeak block
char tspeak_host[INT40];
char tspeak_key[INT18];
//end thingspeak block

//start DS18B20 sensor
#include <OneWire.h>
#include <DallasTemperature.h>
#define DS18GPIO02 2  //DS18B20 is on GPIO02
OneWire oneWire(DS18GPIO02);
DallasTemperature tempsensor(&oneWire);
#define TEMPERATURE_PRECISION 11
// arrays to hold device addresses
DeviceAddress * ds18b20Addr;
float* ds18b20Temp;
int numSensors=0;
String httpString;
//end DS18B20 sensor

//Interrupt and timer callbacks and flags
char alive_interval[INT18] = "5000";// in milliseconds
volatile bool aliveTick;   //flag set by ISR, must be volatile
os_timer_t aliveTimer; // send alive signals every so often
void aliveTimerCallback(void *pArg){aliveTick = true;} // when alive timer elapses
char clock_interval[INT18] = "1000";// in milliseconds
volatile bool clockTick;   //flag set by ISR, must be volatile
os_timer_t clockTimer; // send clock signals every so often
void clockTimerCallback(void *pArg){clockTick = true;} // when clock timer elapses


//begin wifi manager block
//default custom static IP
char static_ip[16] = "196.0.0.99";
char static_gw[16] = "196.0.0.2";
char static_sn[16] = "255.255.255.0";
//end wifi manager block

////////////////////////////////////////////////////////////////////////////////
// adapted from https://github.com/switchdoclabs/OurWeatherWeatherPlus/blob/master/Utils.h
#define countof(a) (sizeof(a) / sizeof(a[0]))
String strDateTime(time_t t)
{
  char strn[20];

  snprintf_P(strn,
             countof(strn),
             "%04u-%02u-%02uT%02u:%02u:%02u",
             year(t),
             month(t),
             day(t),
             hour(t),
             minute(t),
             second(t) );
  return String(strn);
}


////////////////////////////////////////////////////////////////////////////////
// adapted from https://github.com/switchdoclabs/OurWeatherWeatherPlus/blob/master/Utils.h
String strDateTimeURL(time_t t)
{
  char strn[25];

  snprintf_P(strn,
             countof(strn),
             "%04u-%02u-%02u+%02u%%3A%02u%%3A%02u",
             year(t),
             month(t),
             day(t),
             hour(t),
             minute(t),
             second(t) );
  return String(strn);
}


////////////////////////////////////////////////////////////////////////////////
//start DS18B20 sensor
//return a string representation of the device address
String stringAddress(DeviceAddress deviceAddress)
{
  String str="";
  for (uint8_t i = 0; i < 8; i++)
  {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16) str = String(str + "0");
    str = String(str + String(deviceAddress[i], HEX));
  }
  return (str);
}
//end DS18B20 sensor


// begin NTP server time update
////////////////////////////////////////////////////////////////////////////////
/*-------- NTP code ----------*/
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
// NTP Servers:
static const char ntpServerName[] = "us.pool.ntp.org";

//https://github.com/PaulStoffregen/Time/blob/master/examples/TimeNTP_ESP8266WiFi/TimeNTP_ESP8266WiFi.ino

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

////////////////////////////////////////////////////////////////////////////////
time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Received NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + TZHOURS * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}
// end NTP server time update


////////////////////////////////////////////////////////////////////////////////
// a little wrapper function to print to serial if publish failed
void publishMQTT(const char* topic, const char* payload)
{
    if (!mqttclient.publish(topic, payload))
    {
        Serial.print("Publish failed: ");
        Serial.print(topic);
        Serial.println(payload);
    }
}


bool mqttReconnect()
{
    int cnt = 0;
    bool mqttConnected = false;
    while (!mqttclient.connected() && cnt<2)
    {
      cnt++;
        Serial.print("Attempting MQTT connection...");
        // If you do not want to use a username and password, change next line to
        // if ()
        bool connectstatus = false;
        if (strlen(mqtt_user)>0  && strlen(mqtt_password)>0)
        {
          connectstatus = mqttclient.connect("ESP8266Client", mqtt_user, mqtt_password);
        }
        else
        {
          connectstatus = mqttclient.connect("ESP8266Client");
        }

        if (connectstatus)
        {
            Serial.println("connected");
            publishMQTT(mqtt_topic, "online");
            mqttConnected = true;
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttclient.state());
            Serial.println(" try again in 1 seconds");
            delay(1000);
        }
    }
    return(mqttConnected);
}


////////////////////////////////////////////////////////////////////////////////
//begin wifi manager block
//callback notifying us of the need to save config
void saveConfigCallback ()
{
  Serial.println("saveConfigCallback: should save config");
  shouldSaveConfig = true;
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager)
{
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
}

////////////////////////////////////////////////////////////////////////////////
void setup_wifimanager(void)
{
  Serial.println("entering setup_wifimanager() ...");
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  wifiManager.setDebugOutput(true);

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("mqttserver", "mqtt server", mqtt_server, INT40);
  WiFiManagerParameter custom_mqtt_port("mqttport", "mqtt port", mqtt_port, INT6);
  WiFiManagerParameter custom_mqtt_user("mqttuser", "mqtt user", mqtt_user, INT40);
  WiFiManagerParameter custom_mqtt_password("mqttpassword", "mqtt password", mqtt_password, INT40);
  WiFiManagerParameter custom_mqtt_topic("mqtttopic", "mqtt topic", mqtt_topic, INT64);
  WiFiManagerParameter custom_tspeak_host("tspeak_host", "thingspeak host", tspeak_host, INT40);
  WiFiManagerParameter custom_tspeak_key("tspeak_key", "thingspeak key", tspeak_key, INT18);
  WiFiManagerParameter custom_alive_interval("alive_interval", "alive interval", alive_interval, INT18);

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set static ip
  IPAddress _ip,_gw,_sn;
  _ip.fromString(static_ip);
  _gw.fromString(static_gw);
  _sn.fromString(static_sn);

  wifiManager.setSTAStaticIPConfig(_ip, _gw, _sn);

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_password);
  wifiManager.addParameter(&custom_mqtt_topic);
  wifiManager.addParameter(&custom_tspeak_host);
  wifiManager.addParameter(&custom_tspeak_key);
  wifiManager.addParameter(&custom_alive_interval);

  //reset saved settings
  //wifiManager.resetSettings();

  //set minimum quality of signal so it ignores AP's under that quality defaults to 8%
  //wifiManager.setMinimumSignalQuality();

  //sets timeout until configuration portal gets turned off, useful to make it all retry or go to sleep. in seconds
  //wifiManager.setTimeout(120);


  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //and go into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect(CAPTIVEPORTALSSID,CAPTIVEPORTALPASSWD))
      {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        //reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(5000);
      }

  //if you get here you have connected to the WiFi
  Serial.println("connected...)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());
  strcpy(tspeak_host, custom_tspeak_host.getValue());
  strcpy(tspeak_key, custom_tspeak_key.getValue());
  strcpy(alive_interval, custom_alive_interval.getValue());


  //save the custom parameters to FS
  Serial.print("---- shouldSaveConfig");
  Serial.println(shouldSaveConfig);
  if (shouldSaveConfig)
    {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_password"] = mqtt_password;
    json["mqtt_topic"] = mqtt_topic;
    json["tspeak_host"] = tspeak_host;
    json["tspeak_key"] = tspeak_key;
    json["alive_interval"] = alive_interval;
    json["ip"] = WiFi.localIP().toString();
    json["gateway"] = WiFi.gatewayIP().toString();
    json["subnet"] = WiFi.subnetMask().toString();

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile)
    {
      Serial.println("failed to open config file for writing");
    }

    json.prettyPrintTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  Serial.println("leaving setup_wifimanager() ...");

}
//end wifi manager block

////////////////////////////////////////////////////////////////////////////////
void setup_wifi()
{

    readConfigSpiffs();

    Serial.println("After readConfigSpiffs():");
    Serial.print("static_ip: ");    Serial.println(static_ip);
    Serial.print("mqtt_server: ");    Serial.println(mqtt_server);
    Serial.print("mqtt_port: ");    Serial.println(mqtt_port);
    Serial.print("mqtt_user: ");    Serial.println(mqtt_user);
    Serial.print("mqtt_password: ");    Serial.println(mqtt_password);
    Serial.print("mqtt_topic: ");    Serial.println(mqtt_topic);
    Serial.print("tspeak_host: ");    Serial.println(tspeak_host);
    Serial.print("tspeak_key: ");    Serial.println(tspeak_key);
    Serial.print("alive_interval: ");    Serial.println(alive_interval);

    setup_wifimanager();

    Serial.print("WiFi connected: ");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    //start OTA block
    Serial.println("Starting OTA");
    //careful here, lambda functions! Don't 'fix' the code!!!!
    ArduinoOTA.onStart([]() {
      Serial.println("Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("Finished OTA");
    //end OTA block

   //memory status
    Serial.print("Sketch size:  ");
    Serial.println(ESP.getSketchSize());
    Serial.print("Flash size:   ");
    Serial.println(ESP.getFlashChipSize());
    Serial.print("Free size: ** ");
    Serial.println(ESP.getFreeSketchSpace());
}




////////////////////////////////////////////////////////////////////////////////
void readConfigSpiffs()
{

  //clean FS, for testing
  //SPIFFS.format();

  //read configuration from FS json
  Serial.println("entering readConfigSpiffs...");

  if (SPIFFS.begin())
  {
      Serial.println("mounted file system");
      if (SPIFFS.exists("/config.json"))
      {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success())
        {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_user, json["mqtt_user"]);
          strcpy(mqtt_password, json["mqtt_password"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_topic, json["mqtt_topic"]);
          strcpy(tspeak_host, json["tspeak_host"]);
          strcpy(tspeak_key, json["tspeak_key"]);
          strcpy(alive_interval, json["alive_interval"]);

          if(json["ip"])
          {
            Serial.println("setting custom ip from config");
            strcpy(static_ip, json["ip"]);
            strcpy(static_gw, json["gateway"]);
            strcpy(static_sn, json["subnet"]);
            Serial.println(static_ip);
          }
          else
          {
            Serial.println("no custom ip in config");
          }
        }
        else
        {
          Serial.println("failed to load json config");
        }
      }
      else
      {
        Serial.println("unable to open config file /config.json");
      }
    }
    else
    {
      Serial.println("config file /config.json does not exist");
    }
  }
  else
  {
    Serial.println("failed to mount file system");
  }
  //end read
  Serial.println("leaving readConfigSpiffs...");
}

///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////
  void handleNotFound()
  {
    String page = FPSTR(HTTP_HEAD);
    page.replace("{v}", "handleNotFound");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += FPSTR(HTTP_HEAD_END);
    page += FPSTR("File Not Found<BR><BR>\nURI: ");
    page += FPSTR(httpserver->uri().c_str());
    page += FPSTR("<BR>\nMethod: ");
    page += FPSTR((httpserver->method() == HTTP_GET)?"GET":"POST");
    page += FPSTR("<BR>\nArguments: ");
    page += FPSTR(httpserver->args());
    page += FPSTR("<BR>\n");

    for (uint8_t i=0; i<httpserver->args(); i++)
    {
      page += FPSTR((String(" ") + httpserver->argName(i) + String(": ") + httpserver->arg(i) + String("<BR>\n")).c_str());
    }
    page += FPSTR(HTTP_END);
    httpserver->send(404, "text/html", page);
    }

    void  handleReset()
    {
      String page = FPSTR(HTTP_HEAD);
      page.replace("{v}", "Reset");
      page += FPSTR(HTTP_SCRIPT);
      page += FPSTR(HTTP_STYLE);
      page += FPSTR(HTTP_HEAD_END);
      page += F("Module will reset in a second...");
      page += FPSTR(HTTP_END);
      httpserver->send(200, "text/html", page);
    ESP.reset();
    }

  void  handleTemps()
  {
    char nowStr[120] ;
    time_t timenow = now();
    strcpy(nowStr, strDateTime(timenow).c_str()); // get rid of newline
    nowStr[strlen(nowStr)] = '\0';

    String strStatus = timeofDayValid ?String("True") : String("False");
    String tros = String("<tr>");
    String troe = String("</tr>");
    String tcos = String("<td>");
    String tcoe = String("</td>");
    String page = FPSTR(HTTP_HEAD);
    page.replace("{v}", String(static_ip)+" Environmental Status");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += FPSTR(HTTP_HEAD_END);
    page += String("<H1>"+String(static_ip)+" Environmental Status</H1>");
    page += String("<table border=""1"" cellpadding=""5"" cellspacing=""0"">");
    page += String("<tr><th>Time</th><th>Device</th><th>Value</th></tr>");

    for (uint8_t i = 0; i < numSensors; i++)
    {
        page += tros
                 + tcos + String(nowStr) + tcoe
                 + tcos + stringAddress(ds18b20Addr[i]) + tcoe
                 + tcos + String(ds18b20Temp[i],1)+" &deg;C" + tcoe
                 + troe + String("<BR>");
    }
#ifdef USEDHTSENSOR
    page += tros
                 + tcos + String(nowStr) + tcoe
                 + tcos + "DHT temperature" + tcoe
                 + tcos + String(temperatureDHT,1)+" &deg;C" + tcoe
                 + troe + String("<BR>");

    page += tros
                 + tcos + String(nowStr) + tcoe
                 + tcos + "DHT humidity" + tcoe
                 + tcos + String(humidity,0)+ "% RH" + tcoe
                 + troe + String("<BR>");
#endif

       page += String("</table><BR><BR>");
       page += String("Current time: ")+String(nowStr);
       page += String("<BR>NTP time sync status is "+strStatus);
       page += "<BR>";
       page += FPSTR(HTTP_END);
      httpserver->send(200, "text/html", page);
  }

  void  handleRoot()
  {
    //handleHelp();
    handleTemps();
  }

  void  handleInfo()
  {
    String page = FPSTR(HTTP_HEAD);
    page.replace("{v}", "Info");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += FPSTR(HTTP_HEAD_END);
    page += F("<dl>");
    page += F("<dt>Chip ID</dt><dd>");
    page += ESP.getChipId();
    page += F("</dd>");
    page += F("<dt>Flash Chip ID</dt><dd>");
    page += ESP.getFlashChipId();
    page += F("</dd>");
    page += F("<dt>IDE Flash Size</dt><dd>");
    page += ESP.getFlashChipSize();
    page += F(" bytes</dd>");
    page += F("<dt>Real Flash Size</dt><dd>");
    page += ESP.getFlashChipRealSize();
    page += F(" bytes</dd>");
    page += F("<dt>Soft AP IP</dt><dd>");
    page += WiFi.softAPIP().toString();
    page += F("</dd>");
    page += F("<dt>Soft AP MAC</dt><dd>");
    page += WiFi.softAPmacAddress();
    page += F("</dd>");
    page += F("<dt>Station MAC</dt><dd>");
    page += WiFi.macAddress();
    page += F("</dd>");
    page += F("</dl>");
    page += FPSTR(HTTP_END);
    httpserver->send(200, "text/html", page);
  }

     void  handleHelp()
     {
       String page = FPSTR(HTTP_HEAD);
       page.replace("{v}", "Help");
       page += FPSTR(HTTP_SCRIPT);
       page += FPSTR(HTTP_STYLE);
       page += FPSTR(HTTP_HEAD_END);
       page += F("/info : display ESP info<BR>\n");
       page += F("/help : display instructions<BR>\n");
       page += F("/temps : display temperatures<BR>\n");
       page += F("/reset : software reset<BR>\n");
       page += F("/config : show config page<BR>\n");
       page += FPSTR(HTTP_END);
       httpserver->send(200, "text/html", page);
     }

  void  handleConfigSave()
  {
    ;
  }

  void  handleConfig()
  {
    String page = FPSTR(HTTP_HEAD);
    page.replace("{v}", "Reset");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += FPSTR(HTTP_HEAD_END);
    page += F("Not yet available");
    page += FPSTR(HTTP_END);
    httpserver->send(200, "text/html", page);
   }



#ifdef USEOLED
void drawFontFaceDemo() {
    // Font Demo1
    // create more fonts at http://oleddisplay.squix.ch/
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    display.setFont(ArialMT_Plain_16);
    display.drawString(0, 13, "Hello world");
    display.drawString(0, 30, "Hello world");
    display.drawString(0, 45, "Hello world");
}
#endif




////////////////////////////////////////////////////////////////////////////////
void setup()
{
    Serial.begin(115200);
    Serial.println("");
    Serial.println("-------------------------------");
    Serial.println("ESP8266 with WiFiManager and DS18B20 sensors with MQTT notification");


#ifdef USEOLED
    // Initialising the UI will init the display too.
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
#endif

#ifdef USEDHTSENSOR
    //begin DHT11 sensor
    pinMode(PinDHT11, INPUT);
    dht.begin();
    humidity = NAN;
    temperatureDHT = NAN;
    //end DHT11 sensor
#endif

    setup_wifi();

    Serial.println("local ip");
    Serial.println(WiFi.localIP());
    Serial.println(WiFi.gatewayIP());
    Serial.println(WiFi.subnetMask());

    //set up a web server
    httpserver = new ESP8266WebServer(HTTPPORT);
    /* Setup web pages: root, wifi config pages, SO captive portal detectors and not found. */
    //handler functions must be void fn(void)
    httpserver->on("/", handleRoot);
    httpserver->on("/config", handleConfig);
    httpserver->on("/configsave", handleConfigSave);
    httpserver->on("/info", handleInfo);
    httpserver->on("/help", handleHelp);
    httpserver->on("/reset", handleReset);
    httpserver->on("/temps", handleTemps);
    httpserver->onNotFound(handleNotFound);
    httpserver->begin(); // Web server start
    Serial.println("HTTP server started");

    //set up mqtt and register the callback to subscribe
    Serial.print("Setting up MQTT to server """); Serial.print(mqtt_server);
    Serial.print(""" on port """);  Serial.print(mqtt_port);
    Serial.print(""" with topic """);  Serial.print(mqtt_topic); Serial.println("""");
    mqttclient.setServer(mqtt_server, String(mqtt_port).toInt());

    //start DS18B20 sensor
    pinMode(DS18GPIO02, INPUT);
    //https://github.com/milesburton/Arduino-Temperature-Control-Library/blob/master/examples/Multiple/Multiple.pde
    tempsensor.begin();
    // locate devices on the bus
    Serial.print("Locating devices...");
    Serial.print("Found ");
    Serial.print(tempsensor.getDeviceCount(), DEC);
    Serial.println(" devices.");

    httpString = "Temperature not currently available";

    // report parasite power requirements
    Serial.print("Parasite power is: ");
    if (tempsensor.isParasitePowerMode()) Serial.println("ON"); else Serial.println("OFF");

    ds18b20Addr = (DeviceAddress*) calloc(tempsensor.getDeviceCount(), sizeof(DeviceAddress));
    ds18b20Temp = (float*) calloc(tempsensor.getDeviceCount(), sizeof(float));
    numSensors = tempsensor.getDeviceCount();
    for (uint8_t i = 0; i < numSensors; i++)
    {
      tempsensor.getAddress(ds18b20Addr[i], i);
      if (ds18b20Addr[i])
      {
        tempsensor.setResolution(ds18b20Addr[i], TEMPERATURE_PRECISION);
        Serial.print("Device Address: ");
        Serial.print(stringAddress(ds18b20Addr[i]));
        Serial.print(" ");
        Serial.print("Resolution: ");
        Serial.print(tempsensor.getResolution(ds18b20Addr[i]), DEC);
        Serial.println();
      }
    }
    //end DS18B20 sensor

    //init values
    //Define a function to be called when the alive timer fires
    aliveTick = false;
    os_timer_disarm(&aliveTimer);
    os_timer_setfn(&aliveTimer, aliveTimerCallback, NULL);
    long alivetimer = String(alive_interval).toInt();
    os_timer_arm(&aliveTimer, alivetimer, true);
    //Define a function to be called when the clock timer fires
    clockTick = false;
    os_timer_disarm(&clockTimer);
    os_timer_setfn(&clockTimer, clockTimerCallback, NULL);
    long clocktimer = String(clock_interval).toInt();
    os_timer_arm(&clockTimer, clocktimer, true);


    // begin NTP server time update
    // set up to do updates
    timeofDayValid = false;
    Serial.println("Starting UDP");
    Udp.begin(localPort);
    Serial.print("Local port: ");
    Serial.println(Udp.localPort());
    Serial.println("waiting for sync");
    setSyncProvider(getNtpTime);
    setSyncInterval(NTPSYNCINTERVAL);
    // end NTP server time update

    //start FTP server block
    /////FTP Setup, ensure SPIFFS is started before ftp;  /////////
    if (SPIFFS.begin()) {
       Serial.println("SPIFFS opened!");
        ftpSrv.begin("esp8266","esp8266");
        //username, password for ftp.  set ports in ESP8266FtpServer.h  (default 21, 50009 for PASV)
    }
    //end FTP server block

    Serial.println("Setup completed!");
}


////////////////////////////////////////////////////////////////////////////////
void loop()
{
    //start OTA block
    ArduinoOTA.handle();
    //end OTA block

    if (!mqttclient.connected())
    {
      if (mqttReconnect())
      {
        mqttclient.loop();
      }
    }

    float temperature;
    temperature = 0. ;
    char nowStr[120] ;
    time_t timenow = now();
    strcpy(nowStr, strDateTime(timenow).c_str()); // get rid of newline
    nowStr[strlen(nowStr)] = '\0';
    String spc = String(" ");
    String sls = String("/");

    // timer interrupt woke us up again
    if (aliveTick == true )
    {

      // track the free heap space
      Serial.print("Free heap on ESP8266:");
      int heapSize = ESP.getFreeHeap();
      Serial.println(heapSize, DEC);

       if (timeStatus() == timeNotSet) {timeofDayValid = false;}
       else { timeofDayValid = true; }

        aliveTick = false;

#ifdef USEDHTSENSOR
        //begin DHT11 sensor
        humidity = dht.readHumidity();
        temperatureDHT = dht.readTemperature(false); //false means C, true means F
        //end DHT11 sensor
#endif

        //start DS18B20 sensor
        // call sensors.requestTemperatures() to issue a global temperature
        // request to all devices on the bus
        tempsensor.requestTemperatures();
        numSensors = tempsensor.getDeviceCount();
        delay (800); //wait 800 ms for result
        // for all devices
        for (uint8_t i = 0; i < numSensors; i++)
        {
            tempsensor.getAddress(ds18b20Addr[i], i);
            if (ds18b20Addr[i])
            {
                ds18b20Temp[i] = tempsensor.getTempC(ds18b20Addr[i]);
            }
        }
        //end DS18B20 sensor

        //start mqtt block
        if (timeofDayValid && strlen(mqtt_topic)>0)
        {
          for (uint8_t i = 0; i < numSensors; i++)
          {
            // short form: just temperature
            String strM = String(mqtt_topic)+sls+stringAddress(ds18b20Addr[i]);
            publishMQTT(strM.c_str(),String(ds18b20Temp[i]).c_str());
            //Long form: time+temp
            String strL = String(mqtt_topic)+"L"+sls+stringAddress(ds18b20Addr[i]);
            String strQ = String(nowStr)+spc+String(ds18b20Temp[i]);
            publishMQTT(strL.c_str(),strQ.c_str());

            Serial.println(stringAddress(ds18b20Addr[i])+spc+strQ);
          }
        }
        //end mqtt block

      //start thingspeak block
      if (timeofDayValid && strlen(tspeak_host)>0)
      {
        // String system_get_free_heap_size = String(system_get_free_heap_size());
        WiFiClient client;
        const int httpPort = 80;
        if (!client.connect(tspeak_host, httpPort))
        {
          Serial.print("Connecting to ");  Serial.print(tspeak_host);
          Serial.print(":");  Serial.println(httpPort);
          Serial.println("connection failed");
          return;
        }

        String url = String("/update?key=") + String(tspeak_key);
        for (uint8_t i = 0; i < numSensors; i++)
        {
          url += "&field" + String(i+1) + "=" + ds18b20Temp[i];
        }
        Serial.print("Requesting URL: ");  Serial.println(url);

        // This will send the request to the server
        client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                     "Host: " + tspeak_host + "\r\n" +
                     "Connection: close\r\n\r\n");
        delay(10);
        // Read all the lines of the reply from server and print them to Serial
        while(0 && client.available())
        {
          String line = client.readStringUntil('\r');
          Serial.print(line);
        }
      }
      //end thingspeak block


    } //if (aliveTick == true)


    #ifdef USEOLED
    if (clockTick == true )
    {
      clockTick = false;
      String tvalid = (timeofDayValid == 0? "*": " ");

          String Now = String(nowStr);
          String strOdate = Now.substring(11,19)+" "+Now.substring(2,4);
          strOdate += Now.substring(5,7)+Now.substring(8,10)+tvalid;
          display.clear();
          display.setTextAlignment(TEXT_ALIGN_LEFT);
          //display.setFont(DejaVu_Sans_Condensed_13);
          display.setFont(ArialMT_Plain_16);
          display.drawString(1,0,strOdate.c_str() );
          for (uint8_t i = 0; i < numSensors; i++)
          {
            String disStr = String(ds18b20Temp[i],1)+" °C";
            display.drawString(0, 13+15*i, disStr.c_str());
          }
#ifdef USEDHTSENSOR
          if (!isnan(humidity))
          {
            display.drawString(64, 13,  String(humidity,0)+"%RH");
          }
          if (!isnan(temperatureDHT))
          {
            display.drawString(64, 13+15, String(temperatureDHT,1)+" °C");
          }
#endif
          // write the buffer to the display
          display.display();
        } //if (clockTick == true)
#endif


    //start web server
    httpserver->handleClient();
    //end web server

    //start FTP server block
    ftpSrv.handleFTP();        //make sure in loop you call handleFTP()!!
    //end FTP server block

    //yield to wifi and other background tasks
    yield();  // or delay(0);
}
