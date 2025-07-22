#ifndef _MINIRTC_INNER_H_
#define _MINIRTC_INNER_H_

#include "peer_connection.h"

using namespace minirtc;

struct Peer {
  PeerConnection *peer_connection;
  PeerConnectionParams pc_params;
};

#endif