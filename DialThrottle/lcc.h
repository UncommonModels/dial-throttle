#pragma once

#include <Arduino.h>

// LCC / OpenLCB throttle backend speaking CAN frames over a GridConnect TCP link
// (JMRI's LCC GridConnect server, or a hardware hub such as an LCC/CAN gateway).
//
// Implements the parts of the standards a throttle needs:
//  - S-9.7.2.1 CAN frame transfer: alias allocation (CID/RID/AMD), alias conflict handling
//  - S-9.7.3   message network: initialization complete, verify/verified node ID
//  - S-9.7.3.1 traction control: assign controller, speed/direction, function, emergency stop
//  - S-9.7.3.2 DCC train node IDs (06.01.00.00.xx.xx)
namespace lcc {

void begin();
void service();
void setServer(const String& host, uint16_t port); // from the device's saved settings

bool connected();      // TCP up and our alias reserved
bool trainAssigned();  // train node found and this throttle assigned as its controller

void selectTrain(uint16_t address, bool isLong);
void releaseTrain();
void setSpeed(int step0to126, bool forward);
void setFunction(uint8_t fn, bool on);
void eStop();
void quit();
void turnout(const String& id);

void injectFrame(const String& frame); // testing: handle a frame as if it arrived
void printDiag();

} // namespace lcc
