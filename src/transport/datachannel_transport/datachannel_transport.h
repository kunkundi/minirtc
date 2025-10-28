/*
 * @Author: DI JUNKUN
 * @Date: 2025-10-28
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATACHANNEL_TRANSPORT_H_
#define _DATACHANNEL_TRANSPORT_H_

#include "rtc/rtc.hpp"
#include "rtc/websocket.hpp"

class DataChannelTransport {
 public:
  DataChannelTransport();
  ~DataChannelTransport();

  int InitDataChannelTransport(std::string& stun_ip, int stun_port,
                               std::string& turn_ip, int turn_port,
                               std::string& turn_username,
                               std::string& turn_password);
  int CreateDataChannelTransport();
  int DestroyDataChannelTransport();
};

#endif