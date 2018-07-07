/*******************************************************************************
 * TTN Mapper and Test for single board ESP32 with LoRa radio
 * 
 * 
 * Derived from example code by Thomas Telkamp and Matthijs Kooijman
 * 
 * Set board and device specific settings in config.h
 * 
 * 
 * Requires:
 * TinyGPS++ https://github.com/mikalhart/TinyGPSPlus
 * Arduino LMIC https://github.com/matthijskooijman/arduino-lmic
 * U8g2 https://github.com/olikraus/u8g2
 *
 *******************************************************************************/

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <WiFi.h>
#include <U8x8lib.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>

#include "config.h"


/*
 * Do Not edit this file - edit conig.h instead
 */

#ifdef MODE_OTAA
// This EUI must be in little-endian format, so least-significant-byte
// first. When copying an EUI from ttnctl output, this means to reverse
// the bytes. For TTN issued EUIs the last bytes should be 0xD5, 0xB3,
// 0x70.
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8);}

// This should also be in little endian format, see above.
void os_getDevEui (u1_t* buf) { memcpy_P(buf, DEVEUI, 8);}

// This key should be in big endian format (or, since it is not really a
// number but a block of memory, endianness does not really apply). In
// practice, a key taken from ttnctl can be copied as-is.
// The key shown here is the semtech default key.

void os_getDevKey (u1_t* buf) {  memcpy_P(buf, APPKEY, 16);}

#else

// ABP Mode
void os_getArtEui (u1_t* buf) { memcpy_P(buf, APPEUI, 8);}

// This should also be in little endian format, see above.
void os_getDevEui (u1_t* buf) { memcpy_P(buf, DEVEUI, 8);}

// These callbacks are only used in over-the-air activation, so they are
// left empty here (we cannot leave them out completely unless
// DISABLE_JOIN is set in config.h, otherwise the linker will complain).

void os_getDevKey (u1_t* buf) { }

#endif

// LMIC job object for the packet sender
static osjob_t sendjob;

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);

inline int min(int a, int b)
{
  if (a<b) return a;
  return b;
}

// count how many packets we have seen
unsigned int counter_tx = 0;
unsigned int counter_rx = 0;

// outgoing packet transmit buffer
char txBuffer[51];
int txBufferLen = 0;

/* compose a packet using GPS data */
int build_gps_packet()
{
  uint32_t LatitudeBinary, LongitudeBinary;
  uint16_t altitudeGps;
  uint8_t hdopGps;
  uint8_t sats;

  if (!gps.location.isValid()) return 0;
  
  LatitudeBinary = ((gps.location.lat() + 90) / 180) * 16777215;
  LongitudeBinary = ((gps.location.lng() + 180) / 360) * 16777215;

  txBuffer[0] = ( LatitudeBinary >> 16 ) & 0xFF;
  txBuffer[1] = ( LatitudeBinary >> 8 ) & 0xFF;
  txBuffer[2] = LatitudeBinary & 0xFF;

  txBuffer[3] = ( LongitudeBinary >> 16 ) & 0xFF;
  txBuffer[4] = ( LongitudeBinary >> 8 ) & 0xFF;
  txBuffer[5] = LongitudeBinary & 0xFF;

  altitudeGps = gps.altitude.meters();
  txBuffer[6] = ( altitudeGps >> 8 ) & 0xFF;
  txBuffer[7] = altitudeGps & 0xFF;

  hdopGps = gps.hdop.value()*10;
  txBuffer[8] = hdopGps & 0xFF;

  sats = gps.satellites.value();
  txBuffer[9] = sats & 0xFF;
  
  txBufferLen = 10;
  // send this data type as 'port 2'
  return 2;  
}

struct info {
  String ssid;
  int8_t rssi;
  uint8_t bssid[6];
};

int rssi_comp(const void *a, const void *b)
{
  const struct info * aa = (const struct info *)a;
  const struct info * bb = (const struct info *)b;

  if (aa->rssi > bb->rssi) return -1;
  if (aa->rssi < bb->rssi) return 1;
  return 0;
}

/* build a packet with WiFi scan data */
int build_wifi_packet()
{
  Serial.println("Scanning for WiFi networks");
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();

  struct info * list = (struct info *)calloc(n, sizeof(struct info));
  for (int i=0; i<n; i++) {
    list[i].ssid = WiFi.SSID(i);
    list[i].rssi = WiFi.RSSI(i);
    memcpy(list[i].bssid, WiFi.BSSID(i), 6);
  }

  qsort(list, n, sizeof(struct info), rssi_comp);

  int count = min(n, WIFI_LIST_LEN);

  // shrink list size to fix max packet size
  while (count * 7 > sizeof(txBuffer)) count--;
  
  Serial.printf("Found %d networks, choosing top %d:\n", n, count);
  
  txBufferLen = 0;
  for (int i=0; i<count; i++) {
    Serial.printf("%s %d %02X:%02X:%02X:%02X:%02X:%02X\n",
      list[i].ssid.c_str(), list[i].rssi,
      list[i].bssid[0],
      list[i].bssid[1],
      list[i].bssid[2],
      list[i].bssid[3],
      list[i].bssid[4],
      list[i].bssid[5]);

    // pack it into the tx buffer
    txBuffer[ txBufferLen++ ] = list[i].rssi & 0xFF;
    for (int q=0; q<6; q++)
      txBuffer[ txBufferLen++ ] = list[i].bssid[q];
  }
  free(list);

  // send this data type as 'port 3'
  return 3;
}

// compose a packet to send
int build_packet()
{
  int ret;

  ret = build_gps_packet();
  if (ret > 0) return ret;

  ret = build_wifi_packet();
  if (ret > 0) return ret;

  return 0;
}


// debugging info mostly
void onEvent (ev_t ev) {
    Serial.print(os_getTime());
    Serial.print(": ");
    switch(ev) {
        case EV_SCAN_TIMEOUT:
            Serial.println(F("EV_SCAN_TIMEOUT"));
            u8x8.drawString(0, 7, "EV_SCAN_TIMEOUT");
            break;
        case EV_BEACON_FOUND:
            Serial.println(F("EV_BEACON_FOUND"));
            u8x8.drawString(0, 7, "EV_BEACON_FOUND");
            break;
        case EV_BEACON_MISSED:
            Serial.println(F("EV_BEACON_MISSED"));
            u8x8.drawString(0, 7, "EV_BEACON_MISSED");
            break;
        case EV_BEACON_TRACKED:
            Serial.println(F("EV_BEACON_TRACKED"));
            u8x8.drawString(0, 7, "EV_BEACON_TRACKED");
            break;
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            u8x8.drawString(0, 7, "EV_JOINING    ");
            break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            u8x8.drawString(0, 7, "EV_JOINED    ");
            // Disable link check validation (automatically enabled
            // during join, but not supported by TTN at this time).
            LMIC_setLinkCheckMode(0);
            break;
        case EV_RFU1:
            Serial.println(F("EV_RFU1"));
            u8x8.drawString(0, 7, "EV_RFU1");
            break;
        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            u8x8.drawString(0, 7, "EV_JOIN_FAILED");
            break;
        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            u8x8.drawString(0, 7, "EV_REJOIN_FAILED");
            break;
        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
            u8x8.drawString(0, 7, "EV_TXCOMPLETE");
            counter_tx++;
            if (LMIC.txrxFlags & TXRX_ACK) {
                Serial.println(F("Received ACK"));
                u8x8.drawString(0, 7, "Received ACK");
            }
            if(LMIC.dataLen) {
                // data received in rx slot after tx
                Serial.print(F("Data Received: "));
                u8x8.drawString(0, 6, "RX ");
                u8x8.setCursor(4, 6);
                u8x8.printf("%i bytes", LMIC.dataLen);
                u8x8.setCursor(0, 7);
                u8x8.printf("RSSI %d SNR %d", LMIC.rssi, LMIC.snr);
                Serial.write(LMIC.frame+LMIC.dataBeg, LMIC.dataLen);
                Serial.println();
                counter_rx++;
            }
            // Schedule next transmission
            os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
            break;
        case EV_LOST_TSYNC:
            Serial.println(F("EV_LOST_TSYNC"));
            u8x8.drawString(0, 7, "EV_LOST_TSYNC");
            break;
        case EV_RESET:
            Serial.println(F("EV_RESET"));
            u8x8.drawString(0, 7, "EV_RESET");
            break;
        case EV_RXCOMPLETE:
            // data received in ping slot
            Serial.println(F("EV_RXCOMPLETE"));
            u8x8.drawString(0, 7, "EV_RXCOMPLETE");
            counter_rx++;
            break;
        case EV_LINK_DEAD:
            Serial.println(F("EV_LINK_DEAD"));
            u8x8.drawString(0, 7, "EV_LINK_DEAD");
            break;
        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            u8x8.drawString(0, 7, "EV_LINK_ALIVE");
            break;
         default:
            Serial.println(F("Unknown event"));
            u8x8.setCursor(0, 7);
            u8x8.printf("Unknown Event %d", ev);
            break;
    }
    u8x8.setCursor(0,6);
    u8x8.printf("RX:%d TX:%d", counter_rx, counter_tx);
}


// timer went off, prep a packet to send
void do_send(osjob_t* j){
    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND, not sending"));
        u8x8.drawString(0, 7, "OP_TXRXPEND busy");
    } else {
        // Prepare upstream data transmission at the next possible time.
        int port = build_packet();

        if (port == 0) {
          Serial.println("GPS location is not ready, waiting");
          u8x8.setCursor(0, 7);
          u8x8.printf("GPS Not Ready");
#ifdef FORCE_GPSLOCK
          os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
          return;
#else
          txBufferLen = sprintf(txBuffer, "DUMMY");
#endif
        }
        
        LMIC_setTxData2(port, (uint8_t *)txBuffer, txBufferLen, 0);
        
        Serial.println(F("Packet queued"));
        u8x8.setCursor(0, 7);
        u8x8.printf("PktQ N%d P%d   ", txBufferLen, port);
    }
    // Next TX is scheduled after TX_COMPLETE event.
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("Starting"));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    GPSSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    GPSSerial.setTimeout(2);
    
    u8x8.begin();
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(0, 0, "TTN Mapper");

    // LMIC init
    os_init();
    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

#ifndef MODE_OTAA
    uint8_t appskey[sizeof(APPSKEY)];
    uint8_t nwkskey[sizeof(NWKSKEY)];
    memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
    memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
    LMIC_setSession (0x1, DEVADDR, nwkskey, appskey);
    Serial.println("Starting ABP Mode");
#else
    Serial.println("Starting OTAA Mode");
#endif

#ifdef SINGLE_CHANNEL
    for (int i=0; i<9; i++) {
      if (i != SINGLE_CHANNEL)
        LMIC_disableChannel(i);
    }
#endif
    
#ifdef SINGLE_SF
    // Set data rate and transmit power for uplink 
    // txpower 0-15 normal, 16+ boosted
    LMIC_setDrTxpow(SINGLE_SF, 17);
#endif

    LMIC_setLinkCheckMode(0);

    // Start job
    do_send(&sendjob);
}

unsigned long lasttime= 0;

void loop() {
    os_runloop_once();

    // wherever there is data
    while (GPSSerial.available()) {
      gps.encode(GPSSerial.read());
    }

    // at most once per second
    unsigned long timenow = millis();
    if (timenow < lasttime || timenow > lasttime + 1000) {
      if (gps.time.isValid()) {
        u8x8.setCursor(0,1);
        u8x8.printf("Time: %02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
      } else {
        u8x8.drawString(0, 1, "Time: INVALID");
      }
      
      u8x8.setCursor(0,2);
      if (gps.satellites.isValid())
        u8x8.printf("Sat:%2d", gps.satellites.value());
      else
        u8x8.printf("Sat:? ");
       
      u8x8.setCursor(7,2);
      if (gps.hdop.isValid()) {
        float hdop = (int)(gps.hdop.value() * 10) / 10.0;
        u8x8.print("HDOP:");
        u8x8.print(hdop, 1);
      } else
        u8x8.printf("        ");

      u8x8.setCursor(0, 3);
      if (gps.location.isValid())
        u8x8.printf("%.3f %.3f", gps.location.lat(), gps.location.lng());
      else
        u8x8.printf("Loc Unknown");

      lasttime = timenow;
    }

}
