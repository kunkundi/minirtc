#ifndef _ICE_SERVER_RESOLVER_H_
#define _ICE_SERVER_RESOLVER_H_

#include <gio/gio.h>

#include <map>

#include "ice_server_config.h"
#include "log.h"

namespace minirtc {

// Resolve once per hostname and bound DNS fan-out before registering relays.
inline std::vector<std::string> ResolveTurnServerAddresses(
    const std::string& server) {
  constexpr size_t kMaxAddresses = 4;
  std::vector<std::string> addresses;

  if (server.empty()) {
    return addresses;
  }

  GInetAddress* literal = g_inet_address_new_from_string(server.c_str());
  if (literal != nullptr) {
    addresses.push_back(server);
    g_object_unref(literal);
    return addresses;
  }

  GResolver* resolver = g_resolver_get_default();
  GError* error = nullptr;
  GList* resolved =
      g_resolver_lookup_by_name(resolver, server.c_str(), nullptr, &error);

  for (GList* item = resolved;
       item != nullptr && addresses.size() < kMaxAddresses; item = item->next) {
    auto* address = G_INET_ADDRESS(item->data);
    gchar* numeric_address = g_inet_address_to_string(address);
    if (numeric_address == nullptr) {
      continue;
    }

    if (std::find(addresses.begin(), addresses.end(), numeric_address) ==
        addresses.end()) {
      addresses.emplace_back(numeric_address);
    }
    g_free(numeric_address);
  }

  if (resolved != nullptr) {
    g_resolver_free_addresses(resolved);
  }
  g_object_unref(resolver);

  if (error != nullptr) {
    LOG_ERROR("Failed to resolve TURN server [{}]: {}", server, error->message);
    g_error_free(error);
  }

  return addresses;
}

inline std::vector<IceServerEndpoint> ResolveTurnServerEndpoints(
    const IceServerConfiguration& config, MiniRtcTurnMode mode) {
  std::map<std::string, std::vector<std::string>> hosts;
  std::vector<const IceServerEndpoint*> endpoints;
  for (const auto& server : config.servers) {
    if (!config.Fresh()) return {};
    if (!IceTurnTransportAllowed(server, mode)) continue;
    auto [host, inserted] = hosts.try_emplace(server.host);
    if (inserted) host->second = ResolveTurnServerAddresses(server.host);
    endpoints.push_back(&server);
  }
  if (!config.Fresh()) return {};
  std::vector<IceServerEndpoint> result;
  // Give every configured URL its first address before adding DNS alternatives.
  // UDP and TCP each occupy one of libnice's eight TURN slots.
  for (size_t address = 0; address < 4 && result.size() < kMaxTurnEndpoints;
       ++address) {
    for (const auto* endpoint : endpoints) {
      const auto& addresses = hosts.at(endpoint->host);
      if (address >= addresses.size()) continue;
      auto server = *endpoint;
      server.host = addresses[address];
      result.push_back(std::move(server));
      if (result.size() == kMaxTurnEndpoints) break;
    }
  }
  return result;
}

}  // namespace minirtc
#endif
