/*
  AeroQuad v3.0 - May 2011
  www.AeroQuad.com
  Copyright (c) 2011 Ted Carancho.  All rights reserved.
  An Open Source Arduino based multicopter.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GpsDataType.h"

#define UseGPSUBLOX

struct gpsData gpsData; // This is accessed by the parser functions directly !

// default port
// remove?



// default to all protocols
#if (!defined(UseGPSUBLOX) && !defined(UseGPSNMEA) && !defined(UseGPSMTK16))
  #define UseGPSUBLOX
  #define UseGPSNMEA
  #define UseGPSMTK16
#endif


#ifdef UseGPSUBLOX
#include "ublox.h"
#endif

#ifdef UseGPSNMEA
#include <nmea.h>
#endif

#ifdef UseGPSMTK16
#include <mtk16.h>
#endif

#define MIN_NB_SATS_IN_USE 6

GeodeticPosition currentPosition;

float cosLatitude = 0.7; // @ ~ 45 N/S, this will be adjusted to home loc

struct gpsType {
  const char *name;
  void (*init)();
  int  (*processData)(unsigned char);
};

byte  gpsConfigsSent;  // number of cfg msgs sent
byte  gpsConfigTimer = 1;  // 0 = no more work, 1 = send now, >1 wait

const unsigned long gpsBaudRates[] = { 9600L, 19200L, 38400L, 57600L, 115200L};
const struct gpsType gpsTypes[] = {
#ifdef UseGPSUBLOX
  { "UBlox", ubloxInit, ubloxProcessData },
#endif
#ifdef UseGPSNMEA
  { "NMEA", nmeaInit, nmeaProcessData },
#endif
#ifdef UseGPSMTK16
  { "MTK16", mtk16Init, mtk16ProcessData },
#endif
};

#define GPS_NUMBAUDRATES (sizeof(gpsBaudRates)/sizeof(gpsBaudRates[0]))
#define GPS_NUMTYPES     (sizeof(gpsTypes)/sizeof(gpsTypes[0]))

// Timeout for GPS
#define GPS_MAXIDLE_DETECTING 200 // 2 seconds at 100Hz
#define GPS_MAXIDLE 500           // 5 seconds at 100Hz

void initializeGpsData() {
  gpsData.lat = GPS_INVALID_ANGLE;
  gpsData.lon = GPS_INVALID_ANGLE;
  gpsData.fixage = GPS_INVALID_AGE;
  gpsData.state = GPS_DETECTING;
  gpsData.sentences = 0;
  gpsData.sats = 0;
  gpsData.fixtime = 0xFFFFFFFF;
}

struct gpsConfigEntry gpsConfigEntries[] = {
#ifdef UseGPSMTK16
  MTK_CONFIGS,
#endif
#ifdef UseGPSUBLOX
  UBLOX_CONFIGS,
#endif
  { NULL, 0 }
};

// Send initialization strings to GPS one by one,
// it supports both string and binary packets
void gpsSendConfig() {
  if (gpsConfigEntries[gpsConfigsSent].data) {
    if (gpsConfigEntries[gpsConfigsSent].len) {
      for(uint8_t i=0; i<gpsConfigEntries[gpsConfigsSent].len; i++) {                        // send configuration data in UBX protocol
        GPS_SERIAL.write(gpsConfigEntries[gpsConfigsSent].data[i]);
        delay(5); //simulating a 38400baud pace (or less), otherwise commands are not accepted by the device.
      }
      gpsConfigTimer=gpsConfigEntries[gpsConfigsSent].len;
    }
    else {
      GPS_SERIAL.print((char*)gpsConfigEntries[gpsConfigsSent].data);
      gpsConfigTimer=strlen((char*)gpsConfigEntries[gpsConfigsSent].data);
    }
    if (gpsConfigTimer<10) {
      gpsConfigTimer=10;
    }
    gpsConfigsSent++;
  }
}

// Initialize GPS subsystem (called once after powerup)
void initializeGps() {
    gpsData.baudrate = 0;
    GPS_SERIAL.begin(gpsBaudRates[gpsData.baudrate]);
    for (gpsData.type=0; (gpsData.type < GPS_NUMTYPES); gpsData.type++) {
      gpsTypes[gpsData.type].init();
    }
    initializeGpsData();
 }

 
void resetGpsPort(void) {
	GPS_SERIAL.begin(gpsBaudRates[gpsData.baudrate]);
}
 
// Read data from GPS, this should be called at 100Hz to make sure no data is lost
// due to overflowing serial input buffer
void updateGps() {

  gpsData.idlecount++;

  while (GPS_SERIAL.available()) {
    unsigned char c = GPS_SERIAL.read();
    int ret=0;

    // If we are detecting run all parsers, stopping if any reports success
    if (gpsData.state == GPS_DETECTING) {
      for (gpsData.type=0; (gpsData.type < GPS_NUMTYPES); gpsData.type++) {
        ret = gpsTypes[gpsData.type].processData(c);
        if (ret) {
          // found GPS device start sending configuration
          gpsConfigsSent = 0;
          gpsConfigTimer = 1;
          break;
        }
      }
    }
    else {
      // Normal operation just execute the detected parser
      ret = gpsTypes[gpsData.type].processData(c);
    }

    // Upon a successfully parsed sentence, zero the idlecounter and update position data
    if (ret) {
      if (gpsData.state == GPS_DETECTING) {
         gpsData.state = GPS_NOFIX; // make sure to lose detecting state (state may not have been updated by parser)
      }
      gpsData.idlecount=0;
      currentPosition.latitude=gpsData.lat;
      currentPosition.longitude=gpsData.lon;
    }
  }

  // Schedule confg sending if needed
  if (gpsConfigTimer) {
    if (gpsConfigTimer==1) {
      gpsSendConfig();
    }
    gpsConfigTimer--;
  }

  // Check for inactivity, we have two timeouts depending on scan status
  if (gpsData.idlecount > ((gpsData.state == GPS_DETECTING) ? GPS_MAXIDLE_DETECTING : GPS_MAXIDLE)) {
    gpsData.idlecount=0;
    if (gpsData.state == GPS_DETECTING) {
      // advance baudrate
      gpsData.baudrate++;
      if (gpsData.baudrate >= GPS_NUMBAUDRATES) {
	      gpsData.baudrate = 0;
      }

      GPS_SERIAL.begin(gpsBaudRates[gpsData.baudrate]);
    }

    // ensure detection state (if we lost connection to GPS)
	gpsData.state = GPS_DETECTING;
	gpsConfigsSent = 0;
	gpsConfigTimer = 1;
    //  reinitialize all parsers
    for (gpsData.type=0; (gpsData.type < GPS_NUMTYPES); gpsData.type++) {
      gpsTypes[gpsData.type].init();
    }

    // zero GPS state
    initializeGpsData();
  }
}
boolean haveAGpsLock() {
  return (gpsData.state > GPS_NOFIX) && (gpsData.sats >= MIN_NB_SATS_IN_USE);
}
