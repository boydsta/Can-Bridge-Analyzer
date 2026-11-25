#ifndef CAN_DEVICE_H
#define CAN_DEVICE_H

#include "can_config.h"
#include <string.h>

#if CAN_CHIP == MCP2515
#include <mcp_can.h>
using CANController = MCP_CAN;
#else
#include <ACAN2517FD.h>
using CANController = ACAN2517FD;
#endif

struct CANFrame {
  uint32_t id = 0;
  bool ext = false;
  bool is_fd = false;
  bool bitrate_switch = false;
  bool remote = false;
  uint8_t len = 0;
  uint8_t data[64] = {};
};

inline bool can_controller_available(CANController& controller) {
#if CAN_CHIP == MCP2515
  return controller.checkReceive() == CAN_MSGAVAIL;
#else
  return controller.available();
#endif
}

inline bool can_controller_receive(CANController& controller, CANFrame& frame) {
#if CAN_CHIP == MCP2515
  if (controller.checkReceive() != CAN_MSGAVAIL) {
    return false;
  }

  uint8_t extended = 0;
  uint8_t length = 0;
  unsigned long id = 0;
  if (controller.readMsgBuf(&id, &extended, &length, frame.data) != CAN_OK) {
    return false;
  }
  frame.id = static_cast<uint32_t>(id);
  frame.ext = extended != 0;
  frame.is_fd = false;
  frame.bitrate_switch = false;
  frame.remote = false;
  frame.len = length > 8 ? 8 : length;
  return true;
#else
  CANFDMessage received;
  if (!controller.receive(received)) {
    return false;
  }
  if (received.len > sizeof(frame.data)) {
    return false;
  }

  frame.id = received.id;
  frame.ext = received.ext;
  frame.len = received.len;
  memcpy(frame.data, received.data, received.len);
  frame.remote = received.type == CANFDMessage::CAN_REMOTE;
  frame.is_fd = received.type == CANFDMessage::CANFD_NO_BIT_RATE_SWITCH ||
                received.type == CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
  frame.bitrate_switch = received.type == CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
  return true;
#endif
}

inline bool can_controller_send(CANController& controller, const CANFrame& frame) {
#if CAN_CHIP == MCP2515
  if (frame.is_fd || frame.remote || frame.len > 8) {
    return false;
  }
  uint8_t payload[8] = {};
  memcpy(payload, frame.data, frame.len);
  return controller.sendMsgBuf(frame.id, frame.ext ? 1 : 0, frame.len, payload) == CAN_OK;
#else
  if (frame.len > 64) {
    return false;
  }
  CANFDMessage outgoing;
  outgoing.id = frame.id;
  outgoing.ext = frame.ext;
  outgoing.len = frame.len;
  memcpy(outgoing.data, frame.data, frame.len);
  if (frame.remote) {
    outgoing.type = CANFDMessage::CAN_REMOTE;
  } else if (frame.is_fd) {
    outgoing.type = frame.bitrate_switch
        ? CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH
        : CANFDMessage::CANFD_NO_BIT_RATE_SWITCH;
  } else {
    outgoing.type = CANFDMessage::CAN_DATA;
  }
  return controller.tryToSend(outgoing);
#endif
}

inline void can_controller_recover(CANController& controller) {
#if CAN_CHIP != MCP2515
  controller.recoverFromRestrictedOperationMode();
#else
  (void)controller;
#endif
}

#endif
