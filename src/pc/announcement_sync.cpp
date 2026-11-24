#include "announcement_sync.h"

#include <openssl/sha.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace minirtc {
namespace {
using Json = nlohmann::json;
constexpr size_t kMaxDiskBytes = 16 * 1024 * 1024;
bool Integer(const Json& j, const char* key, int64_t minimum) {
  auto it = j.find(key);
  return it != j.end() && it->is_number_integer() && *it >= minimum &&
         *it <= INT64_MAX;
}
bool Text(const Json& j, const char* key, size_t limit) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() &&
         it->get_ref<const std::string&>().size() <= limit;
}
bool ValidRow(const Json& row) {
  return Integer(row, "id", 1) && Integer(row, "revision", 1) &&
         Integer(row, "updated_at", 1) && Text(row, "title", 240);
}
std::string Digest(const std::string& value) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(),
         hash);
  std::string result;
  for (auto byte : hash) {
    constexpr char hex[] = "0123456789abcdef";
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}
}  // namespace

AnnouncementSync::AnnouncementSync(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

void AnnouncementSync::Open(const std::string& endpoint,
                            const std::string& device) {
  const auto scope = Json::array({endpoint, device}).dump();
  if (scope == scope_) return;
  scope_ = scope;
  file_.clear();
  catalog_id_.clear();
  Install(-1, Json::array());
  if (device.empty()) return;
  file_ = directory_ / (Digest(scope) + ".json");
  std::error_code ec;
  if (std::filesystem::file_size(file_, ec) > kMaxDiskBytes || ec) return;
  std::ifstream input(file_, std::ios::binary);
  if (!input) return;
  auto saved = Json::parse(input, nullptr, false);
  if (!saved.is_object() || !Text(saved, "catalog_id", 32) ||
      saved["catalog_id"].get_ref<const std::string&>().size() != 32 ||
      !Integer(saved, "catalog_revision", 0) || !saved.contains("scope") ||
      saved["scope"] != scope_ || !saved.contains("items") ||
      !saved["items"].is_array())
    return;
  std::set<int64_t> ids;
  for (const auto& row : saved["items"]) {
    if (!ValidRow(row) || (row.contains("body") && !Text(row, "body", 8000)) ||
        !ids.insert(row["id"].get<int64_t>()).second)
      return;
  }
  catalog_id_ = saved["catalog_id"];
  Install(saved["catalog_revision"], std::move(saved["items"]));
}

void AnnouncementSync::Install(int64_t revision, Json rows) {
  revision_ = revision;
  rows_ = std::move(rows);
  positions_.clear();
  for (size_t i = 0; i < rows_.size(); ++i)
    positions_.emplace(rows_[i]["id"].get<int64_t>(), i);
}

void AnnouncementSync::Save() const {
  if (file_.empty() || revision_ < 0) return;
  Json saved = {{"scope", scope_},
                {"catalog_id", catalog_id_},
                {"catalog_revision", revision_},
                {"items", rows_}};
  auto bytes = saved.dump();
  if (bytes.size() > kMaxDiskBytes) {
    // Preserve a complete catalog on disk even when many bodies were opened.
    for (auto& row : saved["items"]) row.erase("body");
    bytes = saved.dump();
  }
  if (bytes.size() > kMaxDiskBytes) return;
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) return;
  auto tmp = file_;
  tmp += "." + std::to_string(std::random_device{}()) + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (out) {
#ifdef _WIN32
    if (MoveFileExW(tmp.c_str(), file_.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return;
#else
    std::filesystem::rename(tmp, file_, ec);
    if (!ec) return;
#endif
  }
  std::filesystem::remove(tmp, ec);
}

void AnnouncementSync::PrepareLogin(Json& login, const std::string& endpoint,
                                    const std::string& device) {
  std::lock_guard lock(mutex_);
  Open(endpoint, device);
  enabled_ = false;
  incoming_.clear();
  incoming_revision_ = -1;
  login["announcement_sync_version"] = 2;
  if (!catalog_id_.empty()) login["catalog_id"] = catalog_id_;
  login["catalog_revision"] = revision_;
}

void AnnouncementSync::LoggedIn(const Json& login, const std::string& endpoint,
                                const std::string& device) {
  std::lock_guard lock(mutex_);
  Open(endpoint, device);
  enabled_ = login.contains("announcement_sync_version") &&
             login["announcement_sync_version"] == 2 &&
             Integer(login, "catalog_revision", 0) &&
             Text(login, "catalog_id", 32) &&
             login["catalog_id"].get_ref<const std::string&>().size() == 32;
  if (enabled_ && catalog_id_ != login["catalog_id"]) {
    // A rebuilt server can reuse both row IDs and revision numbers.
    catalog_id_ = login["catalog_id"];
    Install(-1, Json::array());
  }
}

AnnouncementSync::Json AnnouncementSync::Page(const Json& request) const {
  const bool summary = request.value("summary_only", false);
  const int limit = summary ? 200 : 20;
  const int total = static_cast<int>(rows_.size());
  const int offset = std::clamp(request.value("offset", 0), 0,
                                total ? (total - 1) / limit * limit : 0);
  Json items = Json::array();
  for (int i = offset; i < std::min(total, offset + limit); ++i) {
    const auto& row = rows_[i];
    if (summary)
      items.push_back({{"id", row["id"]}, {"revision", row["revision"]}});
    else {
      items.push_back(row);
      if (!items.back().contains("body")) items.back()["body"] = "";
      items.back()["published"] = true;
    }
  }
  return {{"type", "announcements"},  {"request_id", request.at("request_id")},
          {"local_catalog", true},    {"catalog_revision", revision_},
          {"summary_only", summary},  {"limit", limit},
          {"total", total},           {"offset", offset},
          {"items", std::move(items)}};
}

bool AnnouncementSync::Request(const Json& message, Json& reply, Json& wire) {
  std::lock_guard lock(mutex_);
  if (!enabled_ || !message.is_object() || !message.contains("type") ||
      message["type"] != "announcements_list" ||
      !Text(message, "request_id", 80))
    return false;
  reply = {{"type", "announcements"}, {"request_id", message["request_id"]}};
  if (message.contains("detail_id")) {
    if (Integer(message, "detail_id", 1) && Integer(message, "revision", 1)) {
      auto it = positions_.find(message["detail_id"].get<int64_t>());
      if (it != positions_.end() &&
          rows_[it->second]["revision"] == message["revision"]) {
        const auto& row = rows_[it->second];
        if (row.contains("body"))
          reply["body_ready"] = true;
        else {
          wire = {{"type", "announcement_body"},
                  {"request_id", message["request_id"]},
                  {"id", row["id"]},
                  {"revision", row["revision"]}};
          reply = nullptr;
        }
        return true;
      }
    }
    reply["error"] = "announcement_unavailable";
  } else if (revision_ < 0) {
    reply["pending_catalog"] = true;
  } else if (Integer(message, "offset", 0) && message["offset"] <= INT32_MAX &&
             message.contains("summary_only") &&
             message["summary_only"].is_boolean()) {
    reply = Page(message);
  } else
    reply["error"] = "invalid_request";
  return true;
}

bool AnnouncementSync::Receive(const Json& message, Json& reply) {
  std::lock_guard lock(mutex_);
  if (!enabled_) return false;
  if (message["type"] == "announcement_body") {
    if (!Text(message, "request_id", 80)) return true;
    reply = {{"type", "announcements"}, {"request_id", message["request_id"]}};
    if (Integer(message, "id", 1) && Integer(message, "revision", 1) &&
        Text(message, "body", 8000)) {
      auto it = positions_.find(message["id"].get<int64_t>());
      if (it != positions_.end() &&
          rows_[it->second]["revision"] == message["revision"]) {
        rows_[it->second]["body"] = message["body"];
        Save();
        reply["body_ready"] = true;
        return true;
      }
    }
    reply["error"] = "announcement_unavailable";
    return true;
  }
  if (message["type"] != "announcement_catalog") return false;
  if (!Integer(message, "catalog_revision", 0) ||
      !Integer(message, "offset", 0) || !message.contains("last") ||
      !message["last"].is_boolean() || !message.contains("items") ||
      !message["items"].is_array() || message["items"].size() > 32)
    return true;
  if (message["offset"] == 0) {
    incoming_.clear();
    incoming_revision_ = message["catalog_revision"];
  }
  if (message["catalog_revision"] != incoming_revision_ ||
      message["offset"] != incoming_.size())
    return true;
  for (const auto& row : message["items"]) {
    if (!ValidRow(row)) return true;
    Json entry = {{"id", row["id"]},
                  {"revision", row["revision"]},
                  {"updated_at", row["updated_at"]},
                  {"title", row["title"]}};
    incoming_.push_back(std::move(entry));
  }
  if (message["last"] != true) return true;
  std::set<int64_t> ids;
  for (auto& row : incoming_) {
    const auto id = row["id"].get<int64_t>();
    if (!ids.insert(id).second) return true;
    auto it = positions_.find(id);
    if (it != positions_.end()) {
      const auto& old = rows_[it->second];
      if (old["revision"] == row["revision"] && old.contains("body"))
        row["body"] = old["body"];
    }
  }
  Install(incoming_revision_, std::move(incoming_));
  incoming_ = Json::array();
  incoming_revision_ = -1;
  Save();
  reply = {{"type", "announcements_changed"}, {"reset_pages", true}};
  return true;
}
}  // namespace minirtc
