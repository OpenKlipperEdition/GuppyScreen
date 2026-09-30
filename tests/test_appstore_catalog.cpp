// Offline, host-native unit test for OpenKE AppStore catalog parsing and data structures
#include "minitest.h"
#include "hv/json.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

struct TestAppItem {
  std::string id;
  std::string name;
  std::string category;
  std::string version;
  std::string author;
  std::string description;
  bool is_builtin{false};
  bool is_installed{false};
  bool is_active{false};
  std::string status;
};

TEST(AppStoreCatalog, parses_manifest_json_correctly) {
  const char *manifest_paths[] = {
    "../manifests/apps.json",
    "../../OpenKE/manifests/apps.json",
    "/root/Development/OpenKE/OpenKE/manifests/apps.json"
  };

  std::string found_path = "";
  for (const auto *p : manifest_paths) {
    std::ifstream check(p);
    if (check.good()) {
      found_path = p;
      break;
    }
  }

  ASSERT_TRUE(!found_path.empty());

  std::ifstream f(found_path);
  json data;
  f >> data;

  ASSERT_TRUE(data.contains("version"));
  ASSERT_TRUE(data.contains("apps"));
  ASSERT_TRUE(data["apps"].is_array());
  ASSERT_TRUE((int)data["apps"].size() >= 5);

  std::vector<TestAppItem> apps;
  for (const auto &item : data["apps"]) {
    TestAppItem app;
    app.id = item.value("id", "");
    app.name = item.value("name", app.id);
    app.category = item.value("category", "");
    app.version = item.value("version", "");
    app.author = item.value("author", "");
    app.description = item.value("description", "");
    app.is_builtin = item.value("is_builtin", false);
    app.is_installed = app.is_builtin;
    app.is_active = (app.id == "mainsail" || app.id == "guppyscreen");
    app.status = app.is_active ? "active" : (app.is_installed ? "installed" : "available");
    apps.push_back(app);
  }

  ASSERT_TRUE(std::any_of(apps.begin(), apps.end(), [](const TestAppItem &a) { return a.id == "mainsail" && a.is_builtin && a.is_active; }));
  ASSERT_TRUE(std::any_of(apps.begin(), apps.end(), [](const TestAppItem &a) { return a.id == "fluidd" && a.category == "web_ui"; }));
  ASSERT_TRUE(std::any_of(apps.begin(), apps.end(), [](const TestAppItem &a) { return a.id == "guppyscreen" && a.is_builtin && a.is_active; }));
  ASSERT_TRUE(std::any_of(apps.begin(), apps.end(), [](const TestAppItem &a) { return a.id == "helixscreen" && a.category == "touch_ui"; }));
  ASSERT_TRUE(std::any_of(apps.begin(), apps.end(), [](const TestAppItem &a) { return a.id == "spoolman" && a.category == "plugin"; }));
}

TEST(AppStoreCatalog, category_filtering_and_sorting) {
  std::vector<TestAppItem> apps = {
    {"mainsail", "Mainsail", "web_ui", "2.12.0", "Mainsail Crew", "", true, true, true, "active"},
    {"fluidd", "Fluidd", "web_ui", "1.30.0", "Fluidd Team", "", false, false, false, "available"},
    {"guppyscreen", "GuppyScreen", "touch_ui", "1.0.0", "OpenKE", "", true, true, true, "active"},
    {"helixscreen", "HelixScreen", "touch_ui", "0.5.0", "Helix", "", false, true, false, "installed"},
    {"spoolman", "Spoolman", "plugin", "0.20.0", "Donkie", "", false, false, false, "available"}
  };

  // Filter web_ui
  std::vector<TestAppItem> web_apps;
  for (const auto &a : apps) {
    if (a.category == "web_ui") web_apps.push_back(a);
  }
  ASSERT_EQ((int)web_apps.size(), 2);

  // Filter touch_ui
  std::vector<TestAppItem> touch_apps;
  for (const auto &a : apps) {
    if (a.category == "touch_ui") touch_apps.push_back(a);
  }
  ASSERT_EQ((int)touch_apps.size(), 2);

  // Sort: active first, then installed, then alphabetical
  std::sort(apps.begin(), apps.end(), [](const TestAppItem &a, const TestAppItem &b) {
    if (a.is_active != b.is_active) return a.is_active > b.is_active;
    if (a.is_installed != b.is_installed) return a.is_installed > b.is_installed;
    return a.name < b.name;
  });

  ASSERT_EQ(apps[0].status, std::string("active"));
  ASSERT_EQ(apps[1].status, std::string("active"));
  ASSERT_EQ(apps[2].status, std::string("installed"));
  ASSERT_EQ(apps[3].status, std::string("available"));
  ASSERT_EQ(apps[4].status, std::string("available"));
}

int main() {
  return minitest::run_all("appstore_catalog") > 0 ? 1 : 0;
}
