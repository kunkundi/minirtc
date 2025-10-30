#ifndef _MINIRTC_INNER_H_
#define _MINIRTC_INNER_H_

#ifdef USE_LIBDATACHANNEL_CONNECTION
#include "datachannel_connection.h"
#else
#include "peer_connection.h"
#endif

using namespace minirtc;

struct Peer {
#ifdef USE_LIBDATACHANNEL_CONNECTION
  DataChannelConnection* peer_connection;
#else
  PeerConnection* peer_connection;
#endif
  PeerConnectionParams pc_params;
};

#endif