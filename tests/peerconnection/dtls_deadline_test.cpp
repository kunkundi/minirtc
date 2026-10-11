// Run with: xmake test "dtls_deadline_test/*"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "ice_agent.h"

namespace {

using minirtc::IceAgent;
using minirtc::IceServerConfiguration;
using namespace std::chrono_literals;

std::atomic<int> disposed_deadlines{0};

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
void WaitFor(Predicate predicate, std::chrono::seconds timeout,
             const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    Require(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(10ms);
  }
}

// Inspect sources on their owning context, while the agent is still alive.
void OnContext(IceAgent& agent, std::function<void()> action) {
  auto* task = new std::packaged_task<void()>(std::move(action));
  auto result = task->get_future();
  g_main_context_invoke_full(
      agent.gcontext_, G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        (*static_cast<std::packaged_task<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      task,
      [](gpointer data) {
        delete static_cast<std::packaged_task<void()>*>(data);
      });
  result.get();
}

IceServerConfiguration LocalConfig() {
  IceServerConfiguration config;
  config.id = "dtls-deadline-test";
  config.expires_at = minirtc::IceUnixTime() + 3600;
  return config;
}

struct Peer {
  explicit Peer(bool offer)
      : offer(offer),
        ice(offer, false, false, TurnMode::TurnDisabled, true, LocalConfig()) {}

  void Start() {
    gathered = false;
    ready = false;
    failures = 0;
    authenticated = false;
    ice.has_audio_stream_ = false;
    ice.has_data_stream_ = false;
    ice.enable_srtp_ = true;
    Require(
        ice.CreateIceAgent(
            [](NiceAgent*, guint, guint, NiceComponentState state,
               gpointer data) {
              auto& peer = *static_cast<Peer*>(data);
              if (state == NICE_COMPONENT_STATE_READY) {
                peer.ice.StartDtls(peer.offer);
                peer.ready = true;
              }
              if (state == NICE_COMPONENT_STATE_FAILED) ++peer.failures;
            },
            [](NiceAgent*, NiceCandidate*, gpointer) {},
            [](NiceAgent*, guint, gpointer data) {
              static_cast<Peer*>(data)->gathered = true;
            },
            [](NiceAgent*, guint, guint, const char*, const char*, gpointer) {},
            [](NiceAgent*, guint, guint, guint, gchar*, gpointer) {},
            [](gpointer data) {
              static_cast<Peer*>(data)->authenticated = true;
            },
            this) == 0,
        "ICE initialization failed");
    // Only loopback UDP candidates: no signaling, STUN or TURN server needed.
    g_object_set(ice.agent_.load(), "upnp", false, "ice-tcp", false, nullptr);
    NiceAddress address;
    nice_address_init(&address);
    Require(nice_address_set_from_string(&address, "127.0.0.1"),
            "Invalid loopback address");
    Require(nice_agent_add_local_address(ice.agent_, &address),
            "Cannot restrict ICE to loopback");
    Require(ice.GatherCandidates() == 0, "Candidate gathering failed");
  }

  void Stop() { Require(ice.DestroyIceAgent() == 0, "ICE teardown failed"); }

  // Declare the agent last so it shuts down before callback state is destroyed.
  bool offer;
  std::atomic<bool> gathered{false}, ready{false}, authenticated{false};
  std::atomic<int> failures{0};
  IceAgent ice;
};

void Connect(Peer& client, Peer& server, bool silent_dtls_server) {
  client.Start();
  server.Start();
  WaitFor([&] { return client.gathered && server.gathered; }, 5s,
          "ICE gathering did not complete");
  const auto client_sdp = client.ice.GenerateLocalSdp();
  const auto server_sdp = server.ice.GenerateLocalSdp();
  // A silent peer still answers ICE checks, but does not negotiate DTLS itself.
  // Advertise its certificate to the client to exercise a stalled handshake.
  Require(server.ice.SetRemoteSdp(
              silent_dtls_server
                  ? client_sdp
                  : client.ice.AppendFingerprintLine(client_sdp)) >= 0,
          "Server rejected SDP");
  Require(client.ice.SetRemoteSdp(
              server.ice.AppendFingerprintLine(server_sdp)) >= 0,
          "Client rejected SDP");
  WaitFor([&] { return client.ready && server.ready; }, 5s,
          "Loopback ICE did not reach READY");
}

void ObserveDeadline(Peer& peer) {
  OnContext(peer.ice, [&] {
    // With punching disabled, the deadline is the only source using IceAgent
    // as callback data. A dispose hook detects a leaked caller reference even
    // after the source has been detached from its context.
    auto* source =
        g_main_context_find_source_by_user_data(peer.ice.gcontext_, &peer.ice);
    Require(source != nullptr, "DTLS deadline was not armed");
    g_source_set_dispose_function(source,
                                  [](GSource*) { ++disposed_deadlines; });
    const auto remaining =
        g_source_get_ready_time(source) - g_get_monotonic_time();
    Require(remaining > 15000000 && remaining <= 20000000,
            "DTLS deadline is not approximately 20 seconds");
  });
}

void TimeoutThenReconnect() {
  Peer client(true), server(false);
  const auto started = std::chrono::steady_clock::now();
  Connect(client, server, true);
  ObserveDeadline(client);
  const auto before = disposed_deadlines.load();
  WaitFor([&] { return client.failures.load() != 0; }, 25s,
          "Stalled DTLS handshake did not fail");
  Require(std::chrono::steady_clock::now() - started >= 20s,
          "DTLS failed before its deadline");
  client.Stop();
  server.Stop();
  Require(client.failures == 1, "Expected one terminal failure");
  Require(disposed_deadlines == before + 1, "Expired DTLS source leaked");

  // Reuse the agents to exercise teardown/reset, then authenticate a fresh
  // connection and exchange SRTP key material in both directions.
  Connect(client, server, false);
  WaitFor([&] { return client.authenticated && server.authenticated; }, 5s,
          "DTLS reconnect did not authenticate");
  OnContext(client.ice, [&] {
    Require(g_main_context_find_source_by_user_data(client.ice.gcontext_,
                                                    &client.ice) == nullptr,
            "Successful handshake left its deadline armed");
  });
  std::vector<uint8_t> client_key, client_salt, server_key, server_salt;
  std::vector<uint8_t> local_key, local_salt, remote_key, remote_salt;
  OnContext(client.ice, [&] {
    Require(client.ice.ExportSrtpKeys(client_key, client_salt, server_key,
                                      server_salt, true),
            "Client cannot export reconnected SRTP keys");
  });
  OnContext(server.ice, [&] {
    Require(server.ice.ExportSrtpKeys(local_key, local_salt, remote_key,
                                      remote_salt, false),
            "Server cannot export reconnected SRTP keys");
  });
  Require(local_key == server_key && local_salt == server_salt &&
              remote_key == client_key && remote_salt == client_salt,
          "Reconnected peers derived different SRTP keys");
  Require(client.failures == 0 && server.failures == 0,
          "Reconnect inherited a terminal failure");
  client.Stop();
  server.Stop();
  std::cout
      << "PASS: 20s timeout, source disposal and authenticated reconnect\n";
}

void CloseBeforeTimeout() {
  Peer client(true), server(false);
  Connect(client, server, true);
  ObserveDeadline(client);
  const auto before = disposed_deadlines.load();
  client.Stop();
  server.Stop();
  Require(disposed_deadlines == before + 1, "Cancelled DTLS source leaked");
  Require(client.failures == 0, "Closing emitted a DTLS timeout failure");
  std::cout << "PASS: early teardown cancels and releases the deadline\n";
}

}  // namespace

int main() {
  try {
    TimeoutThenReconnect();
    CloseBeforeTimeout();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
