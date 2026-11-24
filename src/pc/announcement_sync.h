/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-24
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANNOUNCEMENT_SYNC_H_
#define _ANNOUNCEMENT_SYNC_H_

#include <filesystem>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace minirtc {

// One identity peer. The application keeps its read/dismissed state; this
// cache holds only public content and is replaceable after a device restart.
class AnnouncementSync {
 public:
  using Json = nlohmann::json;
  explicit AnnouncementSync(std::filesystem::path directory);
  void PrepareLogin(Json& login, const std::string& endpoint,
                    const std::string& device);
  void LoggedIn(const Json& login, const std::string& endpoint,
                const std::string& device);
  // Return true if consumed. A non-null reply is delivered to the application
  // after releasing the lock, and a non-null wire request goes to the server.
  bool Request(const Json& message, Json& reply, Json& wire);
  bool Receive(const Json& message, Json& reply);

 private:
  void Open(const std::string& endpoint, const std::string& device);
  void Save() const;
  void Install(int64_t revision, Json rows);
  Json Page(const Json& request) const;
  std::mutex mutex_;
  std::filesystem::path directory_, file_;
  std::string scope_, catalog_id_;
  bool enabled_ = false;
  int64_t revision_ = -1, incoming_revision_ = -1;
  Json rows_ = Json::array(), incoming_ = Json::array();
  std::map<int64_t, size_t> positions_;
};

}  // namespace minirtc

#endif
