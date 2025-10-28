#include "datachannel_transport.h"

#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

DataChannelTransport::DataChannelTransport() {}

DataChannelTransport::~DataChannelTransport() {}

int DataChannelTransport::InitDataChannelTransport(
    std::string& stun_ip, int stun_port, std::string& turn_ip, int turn_port,
    std::string& turn_username, std::string& turn_password) {
  InitLogger(rtc::LogLevel::Debug);

  rtc::Configuration config;
  std::string stunServer = "stun:stun.l.google.com:19302";
  std::cout << "STUN server is " << stunServer << std::endl;
  config.iceServers.emplace_back(stunServer);
  config.disableAutoNegotiation = true;

  std::string localId = "server";
  std::cout << "The local ID is: " << localId << std::endl;

  auto ws = std::make_shared<rtc::WebSocket>();

  ws->onOpen([]() {
    std::cout << "WebSocket connected, signaling ready" << std::endl;
  });

  ws->onClosed([]() { std::cout << "WebSocket closed" << std::endl; });

  ws->onError([](const std::string& error) {
    std::cout << "WebSocket failed: " << error << std::endl;
  });

  ws->onMessage([&](std::variant<rtc::binary, std::string> data) {});

  const std::string ip_address = "127.0.0.1";
  const int port = 8080;
  const std::string url =
      "ws://" + ip_address + ":" + std::to_string(port) + "/" + localId;
  std::cout << "URL is " << url << std::endl;
  ws->open(url);

  return 0;
}

int DataChannelTransport::CreateDataChannelTransport() { return 0; }

int DataChannelTransport::DestroyDataChannelTransport() { return 0; }
