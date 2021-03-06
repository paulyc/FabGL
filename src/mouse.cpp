/*
  Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com) - <http://www.fabgl.com>
  Copyright (c) 2019 Fabrizio Di Vittorio.
  All rights reserved.

  This file is part of FabGL Library.

  FabGL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FabGL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with FabGL.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "Arduino.h"

#include "freertos/FreeRTOS.h"

#include "mouse.h"
#include "ps2controller.h"
#include "vgacontroller.h"


fabgl::MouseClass Mouse;



namespace fabgl {


MouseClass::MouseClass()
  : m_mouseAvailable(false), m_mouseType(LegacyMouse), m_prevDeltaTime(0),
    m_movementAcceleration(180), m_wheelAcceleration(60000), m_absoluteUpdateTimer(NULL),
    m_absoluteQueue(NULL), m_updateVGAController(false)
{
}


MouseClass::~MouseClass()
{
  if (m_absoluteUpdateTimer)
    xTimerDelete(m_absoluteUpdateTimer, portMAX_DELAY);
  if (m_absoluteQueue)
    vQueueDelete(m_absoluteQueue);
}


void MouseClass::begin(int PS2Port)
{
  PS2DeviceClass::begin(PS2Port);
  reset();
}


void MouseClass::begin(gpio_num_t clkGPIO, gpio_num_t dataGPIO)
{
  PS2Controller.begin(clkGPIO, dataGPIO);
  begin(0);
}


bool MouseClass::reset()
{
  // tries up to six times for mouse reset
  for (int i = 0; i < 6; ++i) {
    m_mouseAvailable = send_cmdReset();
    if (m_mouseAvailable)
      break;
    delay(500);
  }

  // negotiate compatibility and default parameters
  if (m_mouseAvailable) {
    // try Intellimouse (three buttons + scroll wheel, 4 bytes packet)
    if (send_cmdSetSampleRate(200) && send_cmdSetSampleRate(100) && send_cmdSetSampleRate(80) && identify() == PS2Device::MouseWithScrollWheel) {
      // Intellimouse ok!
      m_mouseType = Intellimouse;
    }

    setSampleRate(60);
  }

  return m_mouseAvailable;
}


int MouseClass::getPacketSize()
{
  return (m_mouseType == Intellimouse ? 4 : 3);
}



int MouseClass::deltaAvailable()
{
  return dataAvailable() / getPacketSize();
}


bool MouseClass::getNextDelta(MouseDelta * delta, int timeOutMS, bool requestResendOnTimeOut)
{
  // receive packet
  int packetSize = getPacketSize();
  int rcv[packetSize];
  for (int i = 0; i < packetSize; ++i) {
    while (true) {
      rcv[i] = getData(timeOutMS);
      if (rcv[i] == -1 && requestResendOnTimeOut) {
        requestToResendLastByte();
        continue;
      }
      break;
    }
    if (rcv[i] < 0)
      return false;  // timeout
  }

  // decode packet
  m_status.buttons.left   = (rcv[0] & 0x01 ? 1 : 0);
  m_status.buttons.middle = (rcv[0] & 0x04 ? 1 : 0);
  m_status.buttons.right  = (rcv[0] & 0x02 ? 1 : 0);
  if (delta) {
    delta->deltaX    = (int16_t)(rcv[0] & 0x10 ? 0xFF00 | rcv[1] : rcv[1]);
    delta->deltaY    = (int16_t)(rcv[0] & 0x20 ? 0xFF00 | rcv[2] : rcv[2]);
    delta->deltaZ    = (int8_t)(packetSize > 3 ? rcv[3] : 0);
    delta->overflowX = (rcv[0] & 0x40 ? 1 : 0);
    delta->overflowY = (rcv[0] & 0x80 ? 1 : 0);
    delta->buttons   = m_status.buttons;
  }
  
  return true;
}


void MouseClass::setupAbsolutePositioner(int width, int height, bool createAbsolutePositionsQueue, bool updateVGAController)
{
  m_area                  = Size(width, height);
  m_status.X              = width >> 1;
  m_status.Y              = height >> 1;
  m_status.wheelDelta     = 0;
  m_status.buttons.left   = 0;
  m_status.buttons.middle = 0;
  m_status.buttons.right  = 0;

  m_updateVGAController = updateVGAController;

  if (createAbsolutePositionsQueue) {
    m_absoluteQueue = xQueueCreate(FABGLIB_MOUSE_EVENTS_QUEUE_SIZE, sizeof(MouseStatus));
  }

  if (m_updateVGAController) {
    // setup initial position
    VGAController.setMouseCursorPos(m_status.X, m_status.Y);
  }

  if (m_updateVGAController || createAbsolutePositionsQueue) {
    // create and start the timer
    m_absoluteUpdateTimer = xTimerCreate("", pdMS_TO_TICKS(10), pdTRUE, this, absoluteUpdateTimerFunc);
    xTimerStart(m_absoluteUpdateTimer, portMAX_DELAY);
  }
}


void MouseClass::updateAbsolutePosition(MouseDelta * delta)
{
  const int maxDeltaTimeUS = 500000; // after 0.5s doesn't consider acceleration

  int dx = delta->deltaX;
  int dy = delta->deltaY;
  int dz = delta->deltaZ;

  int64_t now   = esp_timer_get_time();
  int deltaTime = now - m_prevDeltaTime; // time in microseconds

  if (deltaTime < maxDeltaTimeUS) {

    // calcualte movement acceleration
    if (dx != 0 || dy != 0) {
      int   deltaDist    = isqrt(dx * dx + dy * dy);                 // distance in mouse points
      float vel          = (float)deltaDist / deltaTime;             // velocity in mousepoints/microsecond
      float newVel       = vel + m_movementAcceleration * vel * vel; // new velocity
      int   newDeltaDist = newVel * deltaTime;                       // new distance
      dx = dx * newDeltaDist / deltaDist;
      dy = dy * newDeltaDist / deltaDist;
    }

    // calculate wheel acceleration
    if (dz != 0) {
      int   deltaDist    = abs(dz);                                  // distance in wheel points
      float vel          = (float)deltaDist / deltaTime;             // velocity in mousepoints/microsecond
      float newVel       = vel + m_wheelAcceleration * vel * vel;    // new velocity
      int   newDeltaDist = newVel * deltaTime;                       // new distance
      dz = dz * newDeltaDist / deltaDist;
    }

  }

  m_status.X           = tclamp((int)m_status.X + dx, 0, m_area.width  - 1);
  m_status.Y           = tclamp((int)m_status.Y - dy, 0, m_area.height - 1);
  m_status.wheelDelta  = dz;
  m_prevDeltaTime      = now;
}


void MouseClass::absoluteUpdateTimerFunc(TimerHandle_t xTimer)
{
  MouseClass * mouse = (MouseClass*) pvTimerGetTimerID(xTimer);
  MouseDelta delta;
  if (mouse->deltaAvailable() && mouse->getNextDelta(&delta, 0, false)) {
    mouse->updateAbsolutePosition(&delta);

    // VGA Controller
    if (mouse->m_updateVGAController)
      VGAController.setMouseCursorPos(mouse->m_status.X, mouse->m_status.Y);

    if (mouse->m_absoluteQueue) {
      xQueueSend(mouse->m_absoluteQueue, &mouse->m_status, 0);
    }
  }
}


int MouseClass::availableStatus()
{
  return m_absoluteQueue ? uxQueueMessagesWaiting(m_absoluteQueue) : 0;
}


MouseStatus MouseClass::getNextStatus(int timeOutMS)
{
  MouseStatus status;
  if (m_absoluteQueue)
    xQueueReceive(m_absoluteQueue, &status, timeOutMS < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeOutMS));
  return status;
}



} // end of namespace
