/*
 * SoftRF(.ino) firmware
 * Copyright (C) 2016-2017 Linar Yusupov
 *
 * Author: Linar Yusupov, linar.r.yusupov@gmail.com
 *
 * Web: http://github.com/lyusupov/SoftRF
 *
 * Arduino core for ESP8266 is developed/supported by ESP8266 Community (support-esp8266@esp8266.com)
 * AVR/Arduino nRF905 Library/Driver is developed by Zak Kemble, contact@zakkemble.co.uk
 * flarm_decode is developed by Stanislaw Pusep, http://github.com/creaktive
 * Arduino Time Library is developed by Paul Stoffregen, http://github.com/PaulStoffregen
 * "Aircraft" and MAVLink Libraries are developed by Andy Little
 * TinyGPS++ and PString Libraries are developed by Mikal Hart
 * Adafruit NeoPixel Library is developed by Phil Burgess, Michael Miller and others
 * TrueRandom Library is developed by Peter Knight
 * IBM LMIC framework is maintained by Matthijs Kooijman
 * ESP8266FtpServer is developed by David Paiva
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*

   NodeMCU 1.0 GPIO pins:
   2 -> CE
   4 -> PWR
   16 -> TXE
   0 -> CD
   5 -> DR
   15 -> CSN
   12 -> SO
   13 -> SI
   14 -> SCK
*/

#include <SoftwareSerial.h>
extern "C" {
#include <user_interface.h>
}

#include "WiFiHelper.h"
#include "OTAHelper.h"
#include "WebHelper.h"
#include "TimeHelper.h"
#include "LEDHelper.h"
#include "GNSSHelper.h"
#include "RFHelper.h"
#include "SoundHelper.h"
#include "EEPROMHelper.h"
#include "BatteryHelper.h"
#include "MAVLinkHelper.h"
#include "GDL90Helper.h"

#include "SoftRF.h"

#if LOGGER_IS_ENABLED
#include "LogHelper.h"
#endif /* LOGGER_IS_ENABLED */

#define DEBUG 0


#define isTimeToDisplay() (millis() - LEDTimeMarker > 1000)
#define isTimeToExport() (millis() - ExportTimeMarker > 1000)
#define isValidFix() (gnss.location.isValid() && (gnss.location.age() <= 3000))

ufo_t ThisAircraft;

unsigned long LEDTimeMarker = 0;
unsigned long ExportTimeMarker = 0;

//ADC_MODE(ADC_VCC);

SoftwareSerial swSer(D3 /* 0 */, /* 5 */ 9 , false, 256);

void setup()
{
  rst_info *resetInfo;

  resetInfo = ESP.getResetInfoPtr();

  Serial.begin(38400);  
  //Misc_info();

#if LOGGER_IS_ENABLED
  Logger_setup();
#endif /* LOGGER_IS_ENABLED */

  Serial.println(""); Serial.print(F("Reset reason: ")); Serial.println(resetInfo->reason);
  Serial.println(ESP.getResetReason());
  Serial.print(F("Free heap size: ")); Serial.println(ESP.getFreeHeap());
  Serial.println(ESP.getResetInfo()); Serial.println("");

  EEPROM_setup();
  Battery_setup();

  ThisAircraft.addr = ESP.getChipId() & 0x00FFFFFF;

  RF_setup();
  delay(100);


  if (settings->mode == SOFTRF_MODE_UAV_BEACON) {
    Serial.begin(57600);
    MAVLink_setup();
    ThisAircraft.aircraft_type = AIRCRAFT_TYPE_UAV;  
  }  else {
    GNSS_setup();
    ThisAircraft.aircraft_type = AIRCRAFT_TYPE_GLIDER;  
  }
  
  LED_setup();

  WiFi_setup();
  OTA_setup();
  Web_setup();

  GDL90_setup();

#if 0
  GNSSserver.begin();
  GNSSserver.setNoDelay(true);
#endif

  LED_test();
  Sound_test(resetInfo->reason);
 
  if (settings->mode == SOFTRF_MODE_TX_TEST || settings->mode == SOFTRF_MODE_RX_TEST) {
    Time_setup();  
  }

}

void loop()
{
  switch (settings->mode)
  {
  case SOFTRF_MODE_TX_TEST:
    tx_test_loop();
    break;
  case SOFTRF_MODE_RX_TEST:
    rx_test_loop();
    break;
  case SOFTRF_MODE_UAV_BEACON:
    uav_loop();
    break;
  case SOFTRF_MODE_BRIDGE:
    bridge_loop();
    break;
  case SOFTRF_MODE_NORMAL:
  default:
    normal_loop();
    break;
  }   

  RF_loop();

  // Handle OTA update.
  OTA_loop();

  // Handle DNS
  WiFi_loop();

  // Handle Web
  Web_loop();

#if LOGGER_IS_ENABLED
  Logger_loop();
#endif /* LOGGER_IS_ENABLED */

  delay(0);
}

void normal_loop()
{
  unsigned long startTime = millis();
  bool success;

  PickGNSSFix();

  GNSSTimeSync();

  ThisAircraft.timestamp = now();
  if (isValidFix()) {
    ThisAircraft.latitude = gnss.location.lat();
    ThisAircraft.longitude = gnss.location.lng();
    ThisAircraft.altitude = gnss.altitude.meters();
    ThisAircraft.course = gnss.course.deg();

    RF_Transmit();
  }

  if (Import()) success = true;

  success = RF_Receive();

#if DEBUG
  success = true;
#endif

  if (success && isValidFix()) ParseData();

  if (isTimeToDisplay()) {
    if (isValidFix()) {
      LED_DisplayTraffic();  
    } else {
      LED_Clear();      
    }
    LEDTimeMarker = millis();  
  }

  if (isTimeToExport() && isValidFix()) {
    Export();
    ExportTimeMarker = millis(); 
  }

  ClearExpired();

}

#define MAVisValidFix() (the_aircraft.gps.fix_type == 3 /* 3D fix */ )

void uav_loop()
{
  bool success = false;

  PickMAVLinkFix();

  MAVLinkTimeSync();

  ThisAircraft.timestamp = now();

  if (MAVisValidFix()) {
    ThisAircraft.latitude = the_aircraft.location.gps_lat / 1e7;
    ThisAircraft.longitude = the_aircraft.location.gps_lon / 1e7;
    ThisAircraft.altitude = the_aircraft.location.gps_alt / 1000.0;
    ThisAircraft.course = the_aircraft.location.gps_cog;

    RF_Transmit();
  }

  success = RF_Receive();

  if (success && MAVisValidFix()) ParseData();

  if (isTimeToExport() && MAVisValidFix()) {
    MAVLinkShareTraffic();
    ExportTimeMarker = millis(); 
  }

  ClearExpired();
}

unsigned int pos_ndx = 0;
unsigned long TxPosUpdMarker = 0;

void tx_test_loop()
{
  bool success = false;

  ThisAircraft.timestamp = now();

  if (TxPosUpdMarker == 0 || (millis() - TxPosUpdMarker) > 4000 ) {
    ThisAircraft.latitude =  pgm_read_float( &tx_test_positions[pos_ndx][0]);
    ThisAircraft.longitude =  pgm_read_float( &tx_test_positions[pos_ndx][1]);
    pos_ndx = (pos_ndx + 1) % TX_TEST_NUM_POSITIONS;
    TxPosUpdMarker = millis();
  }
  ThisAircraft.altitude = TEST_ALTITUDE;
  ThisAircraft.course = 0;

  RF_Transmit();

  success = RF_Receive();

  if(success)
  {
    fo.raw = Bin2Hex(RxBuffer);

    if (settings->nmea_p) {
      StdOut.print(F("$PSRFI,")); StdOut.print(now()); StdOut.print(F(",")); StdOut.println(fo.raw);
    }
  }

  if (isTimeToExport()) {
    GDL90_Export();
    ExportTimeMarker = millis(); 
  }
}

const float rx_test_positions[][2] PROGMEM = { { 56.0092, 38.3710 } };

void rx_test_loop()
{
  bool success = false;

  ThisAircraft.timestamp = now();

  ThisAircraft.latitude = pgm_read_float( &rx_test_positions[0][0]);;
  ThisAircraft.longitude = pgm_read_float( &rx_test_positions[0][1]);
  ThisAircraft.altitude = TEST_ALTITUDE;
  ThisAircraft.course = 0;

  //RF_Transmit();

  success = RF_Receive();

  if (success) ParseData();

  if (isTimeToDisplay()) {
    LED_DisplayTraffic();  
    LEDTimeMarker = millis();  
  }

  if (isTimeToExport()) {
    Export();
    ExportTimeMarker = millis(); 
  }

  ClearExpired();
}


void bridge_loop()
{
  unsigned long startTime = millis();
  bool success;

  void *answer = WiFi_relay_from_android();
  if ((answer != NULL) && (settings->txpower != NRF905_TX_PWR_OFF) )
  {
    memcpy(TxBuffer, (unsigned char*) answer, PKT_SIZE);

    // Make data
    char *data = (char *) TxBuffer;

    // Set address of device to send to
    byte addr[] = TXADDR;
    nRF905_setTXAddress(addr);

    // Set payload data
    nRF905_setData(data, NRF905_PAYLOAD_SIZE );

    // Send payload (send fails if other transmissions are going on, keep trying until success)
    while (!nRF905_send()) {
      delay(0);
    } ;
    if (settings->nmea_p) {
      StdOut.print(F("$PSRFO,")); StdOut.print(now()); StdOut.print(F(",")); StdOut.println(Bin2Hex((byte *) data));
    }

    tx_packets_counter++;
    TxTimeMarker = millis();
  }

  success = RF_Receive();

  if(success)
  {

    fo.raw = Bin2Hex(RxBuffer);

    if (settings->nmea_p) {
      StdOut.print(F("$PSRFI,")); StdOut.print(now()); StdOut.print(F(",")); StdOut.println(fo.raw);
    }

    WiFi_relay_to_android();
  }

  if (isTimeToDisplay()) {
    LED_Clear();  
    LEDTimeMarker = millis();  
  }
}
