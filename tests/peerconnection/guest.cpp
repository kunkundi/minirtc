#include <iostream>

#include "minirtc.h"

void GuestReceiveBuffer(const char* data, size_t size, const char* user_id,
                        size_t user_id_size) {
  // std::string msg(data, size);
  // std::string user(user_id, user_id_size);

  // std::cout << "Receive: [" << user << "] " << msg << std::endl;
}

int main(int argc, char** argv) {
  Params params;
  params.cfg_path = "../../../../config/config.ini";
  params.on_receive_buffer = GuestReceiveBuffer;

  std::string transmission_id = "000000";
  std::string user_id = argv[1];
  // std::cout << "Please input which transmisson want to join: ";
  // std::cin >> transmission_id;

  PeerPtr* peer = CreatePeer(&params);
  JoinConnection(peer, transmission_id.c_str(), user_id.c_str());

  std::string msg = "Hello world";

  int i = 100;
  // while (i--) {
  //   getchar();
  //   std::cout << "Send msg: " << msg << std::endl;
  //   SendData(peer, DATA_TYPE::USER, msg.data(), msg.size());
  // }

  // getchar();

  while (1) {
  }
  return 0;
}
