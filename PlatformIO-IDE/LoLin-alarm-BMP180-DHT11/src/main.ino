//See the documentation for this code here:
//https://github.com/NelisW/myOpenHab/blob/master/docs/421-ESP-PIR-alarm.md

//some blocks of code are encapsulated by begin and end comments to delineate some
//sections that achieve a particular objective. These can be removed if not required.
// These sections are
// - time of day
// - environmental measurement
// - DS18B20 temperature sensor
// - BMP085 pressure/temperature sensor
// - Over the air (OTA) updates
// - fixed IP


extern "C"
{
    #include "user_interface.h"
}

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
// start OTA block
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
// end OTA block
//start time of day block
#include <time.h>
//end time of day block
//start DS18B20 sensor
//#include <OneWire.h>
//#include <DallasTemperature.h>
//end DS18B20 sensor
//begin BMP085 sensor
#define SENSORS_BMP085_ATTACHED
#include <Sensors.h>
#include <Wire.h>
//end BMP085 sensor
//begin DHT11 sensor
// Uncomment whatever type you're using!
#define DHTTYPE DHT11   // DHT 11
//#define DHTTYPE DHT22   // DHT 22  (AM2302), AM2321
//#define DHTTYPE DHT21   // DHT 21 (AM2301)
#include <DHT.h>
//end DHT11 sensor


// start fixed IP block
//put the following in platformio.ini:
//upload_port = 10.0.0.30
//this will tell platformio to use the wifi to upload
//uncomment the line in platformio.ini to disable OTA and use USB
// end fixed IP block

#include "../../../../openHABsysfiles/password.h"
//#define wifi_ssid "yourwifiSSID"
//#define wifi_password "yourwifipassword"
//#define mqtt_server "yourmqttserverIP"
//#define mqtt_user "yourmqttserverusername"
//#define mqtt_password "yourmqttserverpassword"



// all times in milliseconds
const int aliveTimout  = 5000; // heartbeat
const int AlarmTimeOn  = 100; // raise alarm pulse width
const int LEDPIRTimeOn = 1000; // duration light is on after movement
const int LEDCtlTimeOn = 3000;  // duration light is on after external ON
const int timFAsamples = 10000;  // time window for movement detection
const int numFAsamples = 2; // must get this number of triggers
time_t timeoutFA[numFAsamples]; //keep count of alarm triggers
int timeoutFAcounter;

//start environmental
const int environmentalTimout  = 30000;//millisecs
//end environmental

#define PIR0GPIO12D6 12  //PIR0 is on GPIO12, or D6 on the NodeMCU
#define PIR1GPIO13D7 13  //PIR1 is on GPIO13, or D7 on the NodeMCU
#define PIR2GPIO14D5 14  //PIR2 is on GPIO14, or D5 on the NodeMCU
#define LEDGPIO02D4 2  //LED is on GPIO02, or D4 on the NodeMCU
//#define DS18GPIO00D3 0  //DS18B20 is on GPIO00, or D3 on the NodeMCU
#define DHT1GPIO00D3 0  //DHT11 is on GPIO00, or D3 on the NodeMCU
#define BMP180SDAGPIO04D2 4  //pins SDA(4, D2 on nodeMCU)
#define BMP180SCLGPIO05D1 5  //pins SCL(5, D1 on nodeMCU).

// start fixed IP block
//If you do OTA then also set the target IP address in platform.ini
//[env:esp12e]
//upload_port = 10.0.0.30
IPAddress ipLocal(10, 0, 0, 30);
IPAddress ipGateway(10, 0, 0, 2);
IPAddress ipSubnetMask(255, 255, 255, 0);
// end fixed IP block

//start web server
WiFiServer wifiserver(80);
//end web server

WiFiClient espClient;
PubSubClient mqttclient(espClient);

//start DS18B20 sensor
//OneWire oneWire(DS18GPIO00D3);
//DallasTemperature tempsensor(&oneWire);
//end DS18B20 sensor

//begin DHT11 sensor
//http://www.esp8266.com/viewtopic.php?f=29&t=3249
DHT dht(DHT1GPIO00D3, DHTTYPE);
//end DHT11 sensor


float humidity;
float pressure;
float temperatureBMP;
float temperatureDHT;


//Interrupt and timer callbacks and flags
volatile bool aliveTick;   //flag set by ISR, must be volatile
volatile bool PIR0Occured; //flag set by ISR, must be volatile
volatile bool PIR1Occured; //flag set by ISR, must be volatile
volatile bool PIR2Occured; //flag set by ISR, must be volatile
volatile bool LEDCtlOnChanged ; // to signal change in the light switch status
volatile bool resetAlarm; // to signal that alarm pulse must be reset
char msg0[4] = "   ";
char msg1[4] = "   ";
char msg2[4] = "   ";

os_timer_t aliveTimer; // send alive signals every so often
os_timer_t AlarmTimer; // how long must the raise alarm signal be before reset
os_timer_t LEDPIRTimer; // how long the light must be on after movement
os_timer_t LEDCtlTimer; // how long must the light be on if switched on from outside

//start environmental
os_timer_t environmentalTickTimer; //measure temp/pres regularly
volatile bool environmentalTick;   //flag set by ISR, must be volatile
//end environmental

void PIR0_ISR(){PIR0Occured = true;}
void PIR1_ISR(){PIR1Occured = true;}
void PIR2_ISR(){PIR2Occured = true;}
volatile bool LEDPIROn; //LED on via PIR status flag
volatile bool LEDCtlOn; //LED on via MQTT status flag

// when alive timer elapses: signal alive tick
void aliveTimerCallback(void *pArg){aliveTick = true;}

//start environmental
void environmentalTimerCallback(void *pArg){environmentalTick = true;}
//end environmental

// the various time period callbacks
void LEDPIRTimerCallback(void *pArg){LEDPIROn = false;}
void AlarmTimerCallback(void *pArg){resetAlarm = true; }
void LEDCtlTimerCallback(void *pArg){ LEDCtlOn = false;LEDCtlOnChanged=true;}


// when any PIR triggers: Led on & start one-shot timer to switch off later
void PIR_LED_ON()
{
    LEDPIROn = true;
    os_timer_arm(&LEDPIRTimer, LEDPIRTimeOn, false);
};

void setup_wifi()
{
    delay(10);
    // We start by connecting to a WiFi network
    Serial.print("Connecting to WiFi network: ");
    Serial.println(wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);
    // start fixed IP block
    WiFi.config(ipLocal, ipGateway, ipSubnetMask);
    // end fixed IP block

    //start OTA block
    //careful here, lambda functions!
    ArduinoOTA.onStart([](){Serial.println("Start");});
    ArduinoOTA.onEnd([](){Serial.println("\nEnd"); });
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
    //end OTA block

    //get the wifi up
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    //start up the web server

    Serial.print("WiFi connected: ");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    //start web server
    wifiserver.begin();
    Serial.print("Server started. use this URL to connect: ");
    Serial.print("http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
    //end web server

    //memory status
    Serial.print("Sketch size:  ");
    Serial.println(ESP.getSketchSize());
    Serial.print("Flash size:   ");
    Serial.println(ESP.getFlashChipSize());
    Serial.print("Free size: ** ");
    Serial.println(ESP.getFreeSketchSpace());
}

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


void mqttReconnect()
{
    // Loop until we're reconnected
    while (!mqttclient.connected())
    {
        Serial.print("Attempting MQTT connection...");
        // Attempt to connect
        // If you do not want to use a username and password, change next line to
        if (mqttclient.connect("ESP8266Client"))
        {
            ///if (mqttclient.connect("ESP8266Client", mqtt_user, mqtt_password))
            Serial.println("connected");
            // Once connected, publish an announcement...
            publishMQTT("home/alarmW/alive", "hello world");
            // ... and resubscribe
            mqttclient.subscribe("home/alarmW/control/LEDCtlSwitchOn");
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.print(mqttclient.state());
            Serial.println(" try again in 5 seconds");
            // Wait 5 seconds before retrying
            delay(5000);
        }
    }
}

void synchroniseLocalTime()
{
    //start time of day block
    //get the current wall clock time from time servers
    //we are not overly concerned with the real time,
    //just do it as init values and once per day
    if (WiFi.status() == WL_CONNECTED)
    {
        //my timezone is 2 hours ahead of GMT
        configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("\nWaiting for time");
        while (!time(nullptr))
            {
              Serial.print(".");
              delay(1000);
            }
        time_t now = time(nullptr);
        publishMQTT("home/alarmW/timesynchronised", ctime(&now));
    }
    //end time of day block
}

void user_init(void)
{
    // Initialize the LED pin as an output
    pinMode(LEDGPIO02D4, OUTPUT);
    digitalWrite(LEDGPIO02D4, LOW);
    //set up PIR pins
    pinMode(PIR0GPIO12D6, INPUT);
    pinMode(PIR1GPIO13D7, INPUT);
    pinMode(PIR2GPIO14D5, INPUT);
    //start DS18B20 sensor
    //pinMode(DS18GPIO00D3, INPUT);
    //end DS18B20 sensor
    //begin DHT11 sensor
    pinMode(DHT1GPIO00D3, INPUT);
    //end DHT11 sensor
    //Define a function to be called when the timer fires
    os_timer_disarm(&aliveTimer);
    os_timer_setfn(&aliveTimer, aliveTimerCallback, NULL);
    os_timer_arm(&aliveTimer, aliveTimout, true);

    //start environmental
    os_timer_disarm(&environmentalTickTimer);
    os_timer_setfn(&environmentalTickTimer, environmentalTimerCallback, NULL);
    os_timer_arm(&environmentalTickTimer, environmentalTimout, true);
    environmentalTick = false;
    //end environmental

    //Timer to control how long the light will be on after trigger
    os_timer_disarm(&LEDPIRTimer);
    os_timer_setfn(&LEDPIRTimer, LEDPIRTimerCallback, NULL);
    //timer to control how long the light will be on after mqtt activation
    os_timer_disarm(&LEDCtlTimer);
    os_timer_setfn(&LEDCtlTimer, LEDCtlTimerCallback, NULL);
    //timer to control how long the alarm pulse must be high after alarm is raised
    os_timer_disarm(&AlarmTimer);
    os_timer_setfn(&AlarmTimer, AlarmTimerCallback, NULL);

    //init values
    aliveTick = false;
    PIR0Occured = false;
    PIR1Occured = false;
    PIR2Occured = false;
    LEDPIROn = false;
    LEDCtlOn = false;
    LEDCtlOnChanged = false;
    resetAlarm = false;
    //attach interrupt to pins - only for PIRs
    attachInterrupt(PIR0GPIO12D6, PIR0_ISR, RISING);
    attachInterrupt(PIR1GPIO13D7, PIR1_ISR, RISING);
    attachInterrupt(PIR2GPIO14D5, PIR2_ISR, RISING);

    //init the false alarm delay counters
    for(int i=0; i<numFAsamples;i++)
    {
        timeoutFA[i] = 1000 * (i + 1);
    }
    timeoutFAcounter = 0;

    // get wall clock time from the servers
    synchroniseLocalTime();

    //start DS18B20 sensor
    //tempsensor.begin();
    //end DS18B20 sensor

    //begin DHT11 sensor
    dht.begin();
    //end DHT11 sensor

    //begin BMP085 sensor
    //Wire.begin(int sda, int scl) //default to pins SDA(4, D2 on nodeMCU) and SCL(5, D1 on nodeMCU).
    // Activate i2c qand initialise device
    Wire.begin(BMP180SDAGPIO04D2, BMP180SCLGPIO05D1);
    Sensors::initialize();
    //end BMP085 sensor


    humidity = NAN;
    pressure = NAN;
    temperatureBMP = NAN;
    temperatureDHT = NAN;




}


void mqttCallback(const char* topic, const byte* payload, unsigned int length)
{
    //for some strange reason the payload sequence returned has extra chars appended at the end.
    //It seems that a long buffer was created and all of it sent, irrespective of the payload size.
    //In fact it appears that the extra chars is the date-time of the message.
    //the payload length is given so we can truncate with \0, but this does not work either, because
    //main.ino:331:17: error: assignment of read-only location '*(payload + ((sizetype)length))
    //so then use strncmp with the length specified. Why did it take me so long?
    // Serial.print("Message arrived [");
    // Serial.print(topic);
    // Serial.println("] ");
    // Serial.println((const char*)payload);
    // for (int i = 0; i < length; i++) {Serial.print((char)payload[i]);}
    // Serial.println();

    if (strcmp(topic, "home/alarmW/control/LEDCtlSwitchOn") == 0)
    {
        if (strncmp((char *) payload, "ON",length) == 0)// must be ON or OFF
        {
            LEDCtlOn = true;
            LEDCtlOnChanged = true;
            //activate time to switch off after some time
            os_timer_arm(&LEDCtlTimer, LEDCtlTimeOn, false);
        }
        else{LEDCtlOn = false;}
    }
    else if (false)
    {
        ;//later at more tests here
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("");
    Serial.println("-------------------------------");
    Serial.println("ESP8266 Alarm with MQTT warnings");

    setup_wifi();

    //set up mqtt and register the callback to subscribe
    mqttclient.setServer(mqtt_server, 1883);
    mqttclient.setCallback(mqttCallback);
    if (!mqttclient.connected())
    {
        mqttReconnect();
    }

    //do the mass of init settings
    user_init();

    //start with a defined state to OpenHab
    publishMQTT("home/alarmW/alarm", "0");
    publishMQTT("home/alarmW/control/LEDCtlOn", "0");

}

void loop()
{
    // Start OTA block
    ArduinoOTA.handle();
    // end OTA block

    if (!mqttclient.connected()){mqttReconnect();}
    mqttclient.loop();


    //start environmental
    //read temperature and pressure if flag is set, then reset flag
    if (environmentalTick == true)
    {
        char tmp[40] ;
        environmentalTick = false;

        //start DS18B20 sensor
        // tempsensor.requestTemperatures();
        // delay (200); //wait for result
        // time_t now = time(nullptr);
        // float temp = tempsensor.getTempCByIndex(0);
        // //Arduino don't have float formatting for sprintf
        // //so copy the time to buffer, format float and overwrite \r\n
        // strcpy(tmp, ctime(&now));
        // dtostrf(temp, 7, 1, &tmp[strlen(tmp)-1]);
        // publishMQTT("home/alarmW/temperatureDS18B20-C",tmp);
        // //also publish a shorter version with only the numeric
        // publishMQTT("home/alarmW/temperatureDS18B20-Cs",dtostrf(temp, 0, 1, tmp));
        //end DS18B20 sensor

        //begin DHT11 sensor
        humidity = dht.readHumidity();
        if (! isnan(humidity))
        {
            time_t now = time(nullptr);
            strcpy(tmp, ctime(&now));
            dtostrf(humidity, 7, 1, &tmp[strlen(tmp)-1]);
            publishMQTT("home/alarmW/humidityDHT11",tmp);
            //also publish a shorter version with only the numeric
            publishMQTT("home/alarmW/humidityDHT11s",dtostrf(humidity, 0, 0, tmp));
        }

        temperatureDHT = dht.readTemperature(false); //false means C, true means F
        if (! isnan(temperatureDHT))
        {
            time_t now = time(nullptr);
            strcpy(tmp, ctime(&now));
            dtostrf(temperatureDHT, 7, 1, &tmp[strlen(tmp)-1]);
            publishMQTT("home/alarmW/temperatureDHT11-C",tmp);
            //also publish a shorter version with only the numeric
            publishMQTT("home/alarmW/temperatureDHT11-Cs",dtostrf(temperatureDHT, 0, 1, tmp));
        }
        //end DHT11 sensor

        //begin BMP085 sensor
        Barometer *barometer = Sensors::getBarometer();
        if(barometer)
        {
            pressure = barometer->getPressure();
            time_t now = time(nullptr);
            strcpy(tmp, ctime(&now));
            dtostrf(pressure, 7, 1, &tmp[strlen(tmp)-1]);
            publishMQTT("home/alarmW/pressure-mB",tmp);
            //also publish a shorter version with only the numeric
            publishMQTT("home/alarmW/pressure-mBs",dtostrf(pressure, 0, 1, tmp));
        }
        else
        {
            pressure = NAN;
        }


        Thermometer *thermometer = Sensors::getThermometer();
        if(thermometer)
        {
            temperatureBMP = thermometer->getTemperature();
            time_t now = time(nullptr);
            strcpy(tmp, ctime(&now));
            dtostrf(temperatureBMP, 7, 1, &tmp[strlen(tmp)-1]);
            publishMQTT("home/alarmW/temperatureBMP085-C",tmp);
            //also publish a shorter version with only the numeric
            publishMQTT("home/alarmW/temperatureBMP085-Cs",dtostrf(temperatureBMP, 0, 1, tmp));
        }
        else
        {
            temperatureBMP = NAN;
        }
        //end BMP085 sensor
    }
    //end environmental

    //send message to confirm that we are still alive
    if (aliveTick == true)
    {
        aliveTick = false;
        publishMQTT("home/alarmW/alive", "1");

        //start time of day block
        //sync with the NTP server every day at noon
        time_t now = time(nullptr);
        struct tm* p_tm = localtime(&now);
        if (p_tm->tm_hour==12 && p_tm->tm_min==0 && p_tm->tm_sec<aliveTimout/1000)
        {
            synchroniseLocalTime();
        }
        //end time of day block
    }

    //here follows the PIR sensing and decision making
    if (PIR0Occured == true)
    {
        PIR0Occured = false;
        PIR_LED_ON();
        msg0[0] = '0';
        msg0[1] = digitalRead(PIR1GPIO13D7) ? '1':' ';
        msg0[2] = digitalRead(PIR2GPIO14D5) ? '2':' ';
        publishMQTT("home/alarmW/movement/PIR", msg0);
        if (msg0[0]+msg0[1] > 96)
        {
            publishMQTT("home/alarmW/movement/PIR", "01*");
            timeoutFA[(timeoutFAcounter+numFAsamples) % numFAsamples] = time(nullptr);
            timeoutFAcounter++;
        }
    }

    if (PIR1Occured == true)
    {
        PIR1Occured = false;
        PIR_LED_ON();
        msg1[0] = digitalRead(PIR0GPIO12D6) ? '0':' ';
        msg1[1] = '1';
        msg1[2] = digitalRead(PIR2GPIO14D5) ? '2':' ';
        publishMQTT("home/alarmW/movement/PIR", msg1);
        if (msg1[0]+msg1[1] > 96)
        {
            publishMQTT("home/alarmW/movement/PIR", "01*");
            timeoutFA[(timeoutFAcounter+numFAsamples) % numFAsamples] = time(nullptr);
            timeoutFAcounter++;
        }
    }

    //PIR2 must switch on the light but not raise the alarm
    if (PIR2Occured == true)
    {
        PIR2Occured = false;
        PIR_LED_ON();
        msg2[0] = digitalRead(PIR0GPIO12D6) ? '0':' ';
        msg2[1] = digitalRead(PIR1GPIO13D7) ? '1':' ';
        msg2[2] = '2';
        publishMQTT("home/alarmW/movement/PIR", msg2);
    }

    //prevent timeoutFAcounter from rolling over after long time
    if (timeoutFAcounter >= 2*numFAsamples) timeoutFAcounter=0;

    //count the number of too short intervals within alarm period
    int sum = 0;
    for (int i=0; i<numFAsamples; i++)
    {
        if (time(nullptr) - timeoutFA[i] < timFAsamples/1000) sum++;
    }
    //raise alarm if sufficient number of alarm events are present
    if (sum==numFAsamples)
    {
        publishMQTT("home/alarmW/movement/PIR", "Alarm!");
        publishMQTT("home/alarmW/alarm", "1");
        os_timer_arm(&AlarmTimer, AlarmTimeOn, false);//reset after raising
        for (int i=0; i<numFAsamples; i++)
        {
            timeoutFA[i] = 0;
        }
    }

    if (LEDPIROn == true || LEDCtlOn == true)
    {
        digitalWrite(LEDGPIO02D4, HIGH);
        if (LEDCtlOnChanged && LEDCtlOn)
        {
            publishMQTT("home/alarmW/control/LEDCtlOn", "1");
            LEDCtlOnChanged = false;
        }
     }
    else
    {
        digitalWrite(LEDGPIO02D4, LOW);
        if (LEDCtlOnChanged)
        {
            publishMQTT("home/alarmW/control/LEDCtlOn", "0");
            LEDCtlOnChanged = false;
        }

     }

     if (resetAlarm)
     {
         publishMQTT("home/alarmW/alarm", "0");
         resetAlarm = false;
     }

     //start web server
     // Check if a client has connected to our server
     WiFiClient client = wifiserver.available();
     if (client)
     {
         char tmp[120] ;
        // Return the response
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html");
        client.println(""); //  do not forget this one
        client.println("<!DOCTYPE HTML>");
        client.println("<html>");

        time_t now = time(nullptr);
        strcpy(tmp, ctime(&now));
        client.println(tmp);
        client.println("<br><br>");

        if (!isnan(humidity))
        {
            client.print("Humidity: ");
            client.print(dtostrf(humidity, 0, 0, tmp));
            client.println(" %<br/>\n");
        }
        if (!isnan(pressure))
        {
            client.print("Pressure: ");
            client.print(dtostrf(pressure, 0, 1, tmp));
            client.println(" mB<br/>\n");
        }
        if (!isnan(temperatureBMP))
        {
            client.print("Temperature (BMP): ");
            client.print(dtostrf(temperatureBMP, 0, 1, tmp));
            client.println(" C<br/>\n");
        }
        if (!isnan(temperatureDHT))
        {
            client.print("Temperature (DHT): ");
            client.print(dtostrf(temperatureDHT, 0, 1, tmp));
            client.println(" C<br/>\n");
        }
        // client.println("<br><br>");
        // client.println("<a href=\"/LED=ON\"\"><button>Turn On </button></a>");
        // client.println("<a href=\"/LED=OFF\"\"><button>Turn Off </button></a><br />");
        client.println("</html>");
     }
     //end web server



    //yield to wifi and other background tasks
    yield();  // or delay(0);
}
