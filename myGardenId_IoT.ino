///////////////////////////////////////////////////////////////////////////////
//                           Included Libraries
///////////////////////////////////////////////////////////////////////////////
#include <ESP8266WiFi.h> // wifi stack library
#include <ESP8266mDNS.h> // host broadcast discovery (Bonjour)
#include <ArduinoOTA.h> //support for Over The Air upgrade from Arduino ID
#include <FS.h> // SPI Flash File System (SPIFFS)
#include <Hash.h> // string hashing library
#include <ESPAsyncTCP.h> // Asynchronous TCP network protocol
#include <ESPAsyncWebServer.h> // web server - background request handling
#include <SPIFFSEditor.h> // file system edits - authenticates requests
#include <WiFiUdp.h> // UDP support on user defined port 
#include <TimeLib.h> // system time functions
#include <Wire.h> // i2c comms
#include <Adafruit_BMP085.h> // temperature and pressure sensor

///////////////////////////////////////////////////////////////////////////////
//    Globals, Constants / Board Configuration (ESP-201 as Generic ESP8266)
///////////////////////////////////////////////////////////////////////////////
// digital pin that will source voltage to enable analog read of soil moisture
const int soilPin = 9;
// SDA and SCL pins that will be used for i2c sensor comms
//const int SDA = 4;
//const int SCL = 5;
// pins for motor controller (STK681-332)
const int motorEnable = 14;
const int motorIn1 = 2;
const int motorIn2 = 0;
//assign NTP server URL for time sync
static const char ntpServerName[] = "us.pool.ntp.org";
//assign timezone as offset from GMT
const int timeZone = -7;  // Pacific Daylight Time (USA)
// configure local wifi network and admin authentication
const char* ssid = "your modem or router";
const char* password = "xxxxxxxxx";
const char * hostName = "esp-async";
const char* http_username = "xxxx";
const char* http_password = "xxxxxxxxx";
// assign UDP local port number
const int localPort = 8888;
// declare a time_t to track last sensor read
time_t lastTime = 0;

///////////////////////////////////////////////////////////////////////////////
//                    Instantiate Library Objects
///////////////////////////////////////////////////////////////////////////////
// instance of the bmp180 sensor
Adafruit_BMP085 bmp;
// web server instance on standard HTTP port
AsyncWebServer server(80);
// instance of the AsyncWebSocket plugin
AsyncWebSocket ws("/ws");
// events object to push web socket events
AsyncEventSource events("/events");
// instance of WiFiUDP class for NTP sync
WiFiUDP Udp;


///////////////////////////////////////////////////////////////////////////////
//                       Function Declarations
///////////////////////////////////////////////////////////////////////////////
// gets the unix time from NTP servers
time_t getNtpTime();
// a helper for getNtpTime() - constructs and sends the time request packet
void sendNTPpacket(IPAddress &address);
// gets a timestamp for logs - returns true on success
boolean getTimestamp(char *timestamp, int length) {
  //if time not set report error and return 0
  if (timeStatus() == timeNotSet) {
    Serial.println("Failed getTimestamp(). Time is not set.");
    return false;
  }
  // get current system time - is number of seconds since 1970
  time_t t = now();
  // declare an int to track the length our timestamp creation wants to be
  int x;
  // use snprintf so we dont overrun the buffer and convert time function results to c string
  x = snprintf(timestamp, 18, "%u%02u%02u %02u:%02u:%02u", year(t), month(t), day(t), hour(t), minute(t), second(t));
  // check the timestamp returned is of proper length
  if (x == length) {
    Serial.printf("Good timestamp: %s \n", timestamp);
    return true;
  } else {
    Serial.printf("Bad timestamp length: %u truncated to %s \n", x, timestamp);
    return false;
  }
}

// read/write sensor data into log
/*void readSensors() {
  //enable soil sensor
  digitalWrite(soilPin, 1);
  // read soil moisture value and assign to soil
  int soil = analogRead(A0);
  //disable soil sensor
  //digitalWrite(soilPin, 0);
  // read the temperature in [F] and assign to temp
  int temp = static_cast<int>(bmp.readTemperature() * 9 / 5 + 32);
  // read the pressure in [mBar] and assign to mBar
  int mBar = static_cast<int>(bmp.readPressure() / 100);
  //debug output to serial
  Serial.printf("Sensors: Temperature is %u, Barometer is %u, and soil is %u \n", temp, mBar, soil);
  // declare a buffer sufficient to to hold timestamp formatted as yyyymmdd hh:mm:ss (17 chars and null)
  char timestamp[18];
  // get the timestamp
  if (getTimestamp(timestamp, sizeof(timestamp) - 1)) {
    // check if sensorLog.csv exists in SPIFFS
    if (!SPIFFS.exists("/sensorLog.csv")) {
      // if not, create file as csv
      File sLog = SPIFFS.open("/sensorLog.csv", "w");
      // write the header row
      sLog.printf("timestamp,temp[F],mBar,moisture\n");
      // close the file
      sLog.close();
    }
    //open the file for appending data
    File sLog = SPIFFS.open("/sensorLog.csv", "a");
    // write the current sensor readings
    sLog.printf("%s,%u,%u,%u\n", timestamp, temp, mBar, soil);
    // close the file
    sLog.close();
    // if we are in debug mode write readings to serial
    Serial.printf("%s temp: %u, Bar: %u, soil: %u \n",  timestamp, temp, mBar, soil);
  } else {
    // alert we have a problem with timestamp
    Serial.print("readSensors error: problem generating timestamp");
  }
  }
*/

///////////////////////////////////////////////////////////////////////////////
//                        Web Server Handlers
///////////////////////////////////////////////////////////////////////////////
// websocket events
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->printf("Hello Client %u :)", client->id());
    client->ping();
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  } else if (type == WS_EVT_ERROR) {
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  } else if (type == WS_EVT_PONG) {
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char*)data : "");
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len) {
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);
      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for (size_t i = 0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n", msg.c_str());
      if (info->opcode == WS_TEXT)
        client->text("I got your text message");
      else
        client->binary("I got your binary message");
    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if (info->index == 0) {
        if (info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }
      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT) ? "text" : "binary", info->index, info->index + len);
      if (info->opcode == WS_TEXT) {
        for (size_t i = 0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for (size_t i = 0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n", msg.c_str());

      if ((info->index + len) == info->len) {
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if (info->final) {
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT) ? "text" : "binary");
          if (info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}

void setup() {
  // start serial output
  Serial.begin(115200);
  // write debug info to serial flag true or false
  Serial.setDebugOutput(true);
  // setup WiFi as station
  WiFi.hostname(hostName);
  WiFi.mode(WIFI_AP_STA);
  // setup WiFi host name for software access point fallback
  // WiFi.softAP(hostName);

  //try to connect with credentials
  WiFi.begin(ssid, password);
  //  report fail and try again
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.printf("STA: Failed!\n");
    WiFi.disconnect(false);
    delay(1000);
    WiFi.begin(ssid, password);
  }

  //Send OTA events to the administrator
  ArduinoOTA.onStart([]() {
    events.send("Update Start", "ota");
  });
  ArduinoOTA.onEnd([]() {
    // events.send("Update End", "ota");
    Serial.printf("Remaining OTA space: %u \n", ESP.getFreeSketchSpace());
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char p[32];
    sprintf(p, "Progress: %u%%\n", (progress / (total / 100)));
    events.send(p, "ota");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if (error == OTA_AUTH_ERROR) events.send("Auth Failed", "ota");
    else if (error == OTA_BEGIN_ERROR) events.send("Begin Failed", "ota");
    else if (error == OTA_CONNECT_ERROR) events.send("Connect Failed", "ota");
    else if (error == OTA_RECEIVE_ERROR) events.send("Recieve Failed", "ota");
    else if (error == OTA_END_ERROR) events.send("End Failed", "ota");
  });

  // set OTA hostname and start the OTA server
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.begin();

  // add MDNS service supporting http and tcp
  MDNS.addService("http", "tcp", 80);

  // start the file systen
  SPIFFS.begin();

  // listen for websocket events at onWsEvent
  ws.onEvent(onWsEvent);
  //  add a handler for web socket requests
  server.addHandler(&ws);

  // listen for client connect
  events.onConnect([](AsyncEventSourceClient * client) {
    client->send("hello!", NULL, millis(), 1000);
  });
  //add a handler to spew to client
  server.addHandler(&events);

  // add a handler for authenticating requests to view/modify the file system
  server.addHandler(new SPIFFSEditor(http_username, http_password));

  // handle memory heap requests
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  // handle all other URL requests from static file server (SPIFFS)
  //default root - index.html is the public sensor output page
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  //   handle not found - 404 response with request details

  server.onNotFound([](AsyncWebServerRequest * request) {
    Serial.printf("NOT_FOUND: ");
    if (request->method() == HTTP_GET)
      Serial.printf("GET");
    else if (request->method() == HTTP_POST)
      Serial.printf("POST");
    else if (request->method() == HTTP_DELETE)
      Serial.printf("DELETE");
    else if (request->method() == HTTP_PUT)
      Serial.printf("PUT");
    else if (request->method() == HTTP_PATCH)
      Serial.printf("PATCH");
    else if (request->method() == HTTP_HEAD)
      Serial.printf("HEAD");
    else if (request->method() == HTTP_OPTIONS)
      Serial.printf("OPTIONS");
    else
      Serial.printf("UNKNOWN");
    Serial.printf(" http://%s%s\n", request->host().c_str(), request->url().c_str());
    //if there was request for data, dump the type length details to serial
    if (request->contentLength()) {
      Serial.printf("_CONTENT_TYPE: %s\n", request->contentType().c_str());
      Serial.printf("_CONTENT_LENGTH: %u\n", request->contentLength());
    }
    // get the headers from request and dump to serial
    int headers = request->headers();
    int i;
    for (i = 0; i < headers; i++) {
      AsyncWebHeader* h = request->getHeader(i);
      Serial.printf("_HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
    }
    // get request params and dump to serial
    int params = request->params();
    for (i = 0; i < params; i++) {
      AsyncWebParameter* p = request->getParam(i);
      if (p->isFile()) {
        Serial.printf("_FILE[%s]: %s, size: %u\n", p->name().c_str(), p->value().c_str(), p->size());
      } else if (p->isPost()) {
        Serial.printf("_POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
      } else {
        Serial.printf("_GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
      }
    }
    request->send(404);
  });

  // handle file uploads - authenticated FS updates
  server.onFileUpload([](AsyncWebServerRequest * request, const String & filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index)
      Serial.printf("UploadStart: %s\n", filename.c_str());
    Serial.printf("%s", (const char*)data);
    if (final)
      Serial.printf("UploadEnd: %s (%u)\n", filename.c_str(), index + len);
  });
  server.onRequestBody([](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (!index)
      Serial.printf("BodyStart: %u\n", total);
    Serial.printf("%s", (const char*)data);
    if (index + len == total)
      Serial.printf("BodyEnd: %u\n", total);
  });

  // start the async web server
  server.begin();
  Serial.printf("Starting web server\n");

  // start the UDP server
  Serial.printf("Starting UDP\n");
  Udp.begin(localPort);
  Serial.printf("Local port: %u\n", Udp.localPort());

  //setup time syncing with previously defined getter at intervals in seconds
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  // start the i2c service
  Wire.begin(SDA, SCL);

  //start bmp sensors and report if fail
  /*  if (!bmp.begin()) {
      Serial.printf("No BMP180\n");
    }

    // setup soilPin - low when not in use saves power and prevents electrode corrosion
    pinMode(soilPin, OUTPUT);
    digitalWrite(soilPin, 0);
  */

  char temporaryStamp[18];
  getTimestamp(temporaryStamp, sizeof(temporaryStamp) - 1);

  aggregateDailySensorData();

}// end setup


void loop() {
  // handle an arduino IDE upgrade request
  ArduinoOTA.handle();
  //  set interval in seconds and update sensorLog
  /* int interval = 60;
    if (now() >= (lastTime + interval)) {
     readSensors();
     lastTime = now();
    }
  */
}


/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.printf("Transmit NTP Request\n");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.printf(ntpServerName);
  Serial.printf(": ");
  Serial.print(ntpServerIP);
  Serial.printf("\n");
  sendNTPpacket(ntpServerIP);
  long beginWait = millis();
  while (millis() - beginWait < 3000) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.printf("Receive NTP Response\n");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.printf("No NTP Response :-(\n");
  return 0; // return 0 if unable to get the time
}

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

// log daily daily sensor aggregation
void aggregateDailySensorData() {
  // calculate yesterday - now minus seconds in 24 hours
  time_t yesterday = now() - (60 * 60 * 24);
  // assign memory to hold the aggregation date string
 char aggregationDate[9];
  // format aggregation date yyyymmdd from time functions
  sprintf(aggregationDate, "%u%02u%02u", year(yesterday), month(yesterday), day(yesterday));
  // initialize memory to store daily high and low temperatures
  uint8_t hiTemp = 0;
  uint8_t loTemp = 100;
  // initialize storage for barometer reading
  int mBar = 0;
  // an enum for Barometer direction
int8_t bardir = 0; 
  // irrigation flag
  uint8_t irrigation = 0;
  // open the sensor log file for read only access
  File sensorLog = SPIFFS.open("/sensorLog.csv", "r");

  // gather info and report on file system for debug
  /* struct FSInfo {
     size_t totalBytes;
     size_t usedBytes;
     size_t blockSize;
     size_t pageSize;
     size_t maxOpenFiles;
     size_t maxPathLength;
    };

    FSInfo fs_info;
    SPIFFS.info(fs_info);
    Serial.printf("%u bytes used out of %u total.\n",totalBytes, usedBytes);
    Serial.printf("%u is block size and %u is the page size.\n",blockSize, pageSize);
    Serial.printf("%u is the max number of open files\n",maxOpenFiles);
  */
  // check success
  if (!sensorLog) {
    // report error and return
    Serial.printf("aggregateDailySensorData: failed to open sensorLog\n");
    return;
  }
  // report success
  Serial.printf("aggregateDailySensorData: reading sensorLog\n");
  // if we find the aggregationDate
  if (sensorLog.find(aggregationDate)) {
    // report the position in the file
    Serial.printf("%s at position %u of %u\n", aggregationDate, sensorLog.position(), sensorLog.size());
    // we will try to read 24 sensorLog entries - 1 for each hour - initialize a count
    int count = 0;
    // set up our buffer - largest field is timestamp holding 17 chars - pad for possible terminators fore and aft
    char buffr[19];
    // read into buffer until we see terminator (comma = ascii 44) or max buffer size (19) and report number of characters read
    int numChars = sensorLog.readBytesUntil(44, buffr, 19);
    // we just found the aggregatiobn date so we know we have at least 1 line of sensor data to process
    // do while each line starts with the date we wish to aggregate
    do {
      Serial.printf(">> entering DO WHILE >> the last number of Bytes read was: %u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_ buffer[1]:_%c_\n",buffr[0],buffr[1]);
      // sensorLog lines look something like (yyyymmdd hh:mm:ss,nnn,nnnn,nnnn,n\n) we think we are at the first comma, so read the temperature next
      numChars = sensorLog.readBytesUntil(44, buffr, 4);
      Serial.printf("trying to read TEMPERATURE (4 bytes max)\nNumber of Bytes read:%u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_\nbuffer[1]:_%c_\nbuffer[2]:_%c_\nbuffer[3]:_%c_\n",buffr[0],buffr[1],buffr[2],buffr[3]);
      int temp = 0;
      if (numChars == 3) {
        temp = 100 + (buffr[1]-48) * 10 + (buffr[2]-48);
      } else {
        temp = (buffr[0]-48) * 10 + (buffr[1]-48);
      }
      Serial.printf("the temperature value read is: %u\n", temp);
      if (temp >= hiTemp) { hiTemp = temp;}
      else {
        if (temp < loTemp) { loTemp = temp;}
      }
      // get next sensor reading (barometer)
      numChars = sensorLog.readBytesUntil(44, buffr, 5);
      Serial.printf("trying to read BAROMETER (5 bytes max)\nNumber of Bytes read:%u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_\nbuffer[1]:_%c_\nbuffer[2]:_%c_\nbuffer[3]:_%c_\nbuffer[4]:_%c_\n",buffr[0],buffr[1],buffr[2],buffr[3],buffr[4]);
      //of interest is hour 12 for capturing barometer - we will capture until 12th hour
      if (count < 13) {
        // initialize storsage for barometer reading
        int bar = 0;
        if (numChars == 4) {
          bar = 1000 + (buffr[2]-48) * 10 + (buffr[3]-48);
        } else if (numChars == 3){
          bar = 900 + (buffr[1]-48) * 10 + (buffr[2]-48);
        } else {
          Serial.printf("ERROR:  processing %u bytes from stream, expecting 3 or 4 max\n",numChars);
          return;
        }
        if (mBar == bar) {
          bardir = 0;
        } else {
          if ( mBar > bar ) {
            bardir = -1;
          } else {
            bardir = 1;
          }
        }
        Serial.printf("processing shows at hour %u, the barometer was %u and %d\n",count,bar,bardir);
        mBar = bar;
      }
      // get next sensor reading (soil moisture)
      numChars = sensorLog.readBytesUntil(44, buffr,5);
      Serial.printf("trying to read SOIL MOISTURE (5 bytes max)\nNumber of Bytes read:%u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_\nbuffer[1]:_%c_\nbuffer[2]:_%c_\nbuffer[3]:_%c_\nbuffer[4]:_%c_\n",buffr[0],buffr[1],buffr[2],buffr[3],buffr[4]);
     
      // this function does not do anything with soil moisture data. so read last bytes before new line (ascii 10)
      numChars = sensorLog.readBytesUntil(10, buffr, 3);
       Serial.printf("trying to read IRRIGATION (3 bytes max)\nNumber of Bytes read:%u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_\nbuffer[1]:_%c_\nbuffer[2]:_%c_\n",buffr[0],buffr[1],buffr[2]);
      if (irrigation != 1) {
        irrigation = (buffr[0]==49)?1:0;
        Serial.printf("the value assigned to irrigation: >>>>>>  %u  <<<<<<<\n",irrigation);
      } else {
      Serial.printf("the irrigation flag already set to %u for the day\n",irrigation);
      }
      // increment count for tracking the next hourly sensor reading
      count ++;
      Serial.printf("\n incrementing the hour counter to **** %u **** \n",count);    
      // read the beginning of next line into buffer until a comma (ascii 44) or max buffer length (19) and report number of characters read
      numChars = sensorLog.readBytesUntil(44, buffr, 19);
       Serial.printf("trying to read timestamp from new line (19 bytes max)\nNumber of Bytes read:%u\nBuffer holds:_%s_\n", numChars, buffr);
      Serial.printf("buffer[0]:_%c_\nbuffer[1]:_%c_\n",buffr[0],buffr[1]);
      // check that we have not yet read all 24 hourly readings and that we are still reading the right date 
    } while (count < 24 && match(buffr, aggregationDate));
    //close sensorLog
    sensorLog.close();
    // log the daily aggregation (daydata.csv)
    // check if daydata.csv exists in SPIFFS
    if (!SPIFFS.exists("/daydata.csv")) {
      // if not, create file as csv
      File aggregatedata = SPIFFS.open("/daydata.csv", "w");
      // write the header row
      aggregatedata.printf("Date,hiTemp,loTemp,mBar,direction,irrigate\n");
      // close the file
      aggregatedata.close();
    }
    //open the file for appending data
    File aggregatedata = SPIFFS.open("/daydata.csv", "a");
    //write aggregation data to file
    aggregatedata.printf("%s,%u,%u,%u,%d,%u\n",aggregationDate,hiTemp,loTemp,mBar,bardir,irrigation);
    //close the log file
    aggregatedata.close();
  } else {
    Serial.printf("Error: failed to find aggregationDate: %s\n", aggregationDate);
  }
   return;
}

// match function returns true if buffer contains the matchDate
boolean match (const char* buffr, const char* matchDate) {
  Serial.printf("Processing buffer contents:_%s_ for for a match with Aggregation Date of %s\n",buffr,matchDate);
  // check buffr and date alignment
  if (buffr[0] == matchDate[0]) {
    // verify the two timestamps have matching month and day
    for (int i = 4; i < 8; i++) {
    Serial.printf("\n buffer is ALIGNED >> trying to match %c and %c\n", buffr[i], matchDate[i]);
    if (buffr[i] == matchDate[i]) {
      continue;
    } else {
      // timestamp does not match
      return false;
    }
  }
  // timestamp date is a match
  return true;
} else {
  // buffr and matchDate are misaligned an offset is likely required to skip leading characters - find offset in buffr
    for (int offset = 0; offset < 4; offset ++) {
      if (buffr[offset] == matchDate[0]){
        for (int i=4; i <8; i++) {
          Serial.printf("\n buffer ALIGNMENT CORRECTED >>  trying to match %c and %c\n", buffr[i+1], matchDate[i]);
          if (buffr[i+offset] == matchDate[i]) {
            continue;
            } else {
              // timestamp does not match
              return false;
            }
            // timestamp is a match
         } return true;
      } else {
        continue;
      }
    }
    Serial.printf("Error: failed to correct buffer alignment.");
    return false;
}
}
