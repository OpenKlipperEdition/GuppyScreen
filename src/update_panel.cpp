#include "update_panel.h"
#include "utils.h"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"

#include "hv/json.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <cstdlib>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;
namespace sp = subprocess;
using json = nlohmann::json;

LV_IMG_DECLARE(back);
LV_IMG_DECLARE(refresh_img);
LV_IMG_DECLARE(update_img);


// SWUpdate struct progress_msg definition
struct swupdate_progress_msg {
  unsigned int magic;
  unsigned int status;
  unsigned int dwl_percent;
  unsigned int nsteps;
  unsigned int cur_step;
  unsigned int cur_percent;
  char cur_image[256];
  char info[2048];
  unsigned int inval;
};

UpdatePanel::UpdatePanel(KWebSocketClient &c)
  : ws(c)
  , cont(lv_obj_create(lv_scr_act()))
  , top_bar(lv_obj_create(cont))
  , title_label(lv_label_create(top_bar))
  , list_cont(lv_obj_create(cont))
  , back_btn(top_bar, &back, "Back", &UpdatePanel::_handle_callback, this)
  , scan_btn(top_bar, &refresh_img, "Scan USB", &UpdatePanel::_handle_callback, this)
{
  lv_obj_move_background(cont);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(cont, 4, 0);
  lv_obj_set_style_pad_gap(cont, 4, 0);

  // Top bar configuration
  lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(top_bar, 2, 0);
  lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(top_bar, 1, 0);
  lv_obj_set_style_border_color(top_bar, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);

  lv_label_set_text(title_label, "Firmware Update");
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
  lv_obj_set_width(title_label, 300);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
  lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_align(scan_btn.get_container(), LV_ALIGN_RIGHT_MID, -70, 0);
  lv_obj_align(back_btn.get_container(), LV_ALIGN_RIGHT_MID, 0, 0);

  // Scrollable list container
  lv_obj_set_size(list_cont, LV_PCT(100), LV_PCT(82));
  lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list_cont, 4, 0);
  lv_obj_set_style_pad_gap(list_cont, 6, 0);
  lv_obj_add_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list_cont, 0, 0);

  update_timer = lv_timer_create(UpdatePanel::_timer_callback, 200, this);
}

UpdatePanel::~UpdatePanel() {
  if (update_timer != nullptr) {
    lv_timer_del(update_timer);
    update_timer = nullptr;
  }
  close_whats_new_popup();
  close_usb_detect_popup();
  close_modal();
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
  if (cont != nullptr) {
    lv_obj_del(cont);
    cont = nullptr;
  }
}

void UpdatePanel::foreground() {
  close_whats_new_popup();
  close_usb_detect_popup();
  scan_updates(true);
  build_package_list();
  lv_obj_move_foreground(cont);
}

void UpdatePanel::background() {
  if (state == UpdateState::FLASHING) {
    // Do not allow exiting panel while flashing
    return;
  }
  close_modal();
  lv_obj_move_background(cont);
}

void UpdatePanel::handle_callback(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *target = lv_event_get_current_target(event);
    if (target == back_btn.get_container()) {
      background();
    } else if (target == scan_btn.get_container()) {
      scan_updates(true);
      build_package_list();
    }
  }
}

static std::string get_current_os_version() {
  std::ifstream vfile("/etc/openke-version");
  if (vfile.is_open()) {
    std::string v;
    if (std::getline(vfile, v) && !v.empty()) {
      return v;
    }
  }
  std::ifstream swfile("/etc/sw-versions");
  if (swfile.is_open()) {
    std::string name, ver;
    if (swfile >> name >> ver && !ver.empty()) {
      return ver;
    }
  }
  std::ifstream osfile("/etc/os-release");
  if (osfile.is_open()) {
    std::string line;
    while (std::getline(osfile, line)) {
      if (line.rfind("VERSION=", 0) == 0 || line.rfind("VERSION_ID=", 0) == 0) {
        std::string v = line.substr(line.find('=') + 1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
          v = v.substr(1, v.size() - 2);
        }
        return v;
      }
    }
  }
  return "1.0.0";
}

static void parse_swu_metadata(const std::string &swu_path, const std::string &filename, std::string &version_out, std::string &changelog_out) {
  version_out = "";
  changelog_out = "";

  std::ifstream f(swu_path, std::ios::binary);
  if (f.is_open()) {
    std::vector<char> buf(32768, 0);
    f.read(buf.data(), buf.size() - 1);
    std::string content(buf.data(), f.gcount());

    // 1. Extract version from sw-description
    auto pos = content.find("version = \"");
    if (pos != std::string::npos) {
      pos += 11;
      auto endpos = content.find('"', pos);
      if (endpos != std::string::npos) {
        version_out = content.substr(pos, endpos - pos);
      }
    }

    // 2. Extract changelog from sw-description
    auto cl_pos = content.find("changelog = \"");
    if (cl_pos != std::string::npos) {
      cl_pos += 13;
      size_t end_cl = cl_pos;
      while (end_cl < content.size()) {
        if (content[end_cl] == '"' && content[end_cl - 1] != '\\') {
          break;
        }
        end_cl++;
      }
      if (end_cl < content.size()) {
        std::string raw_cl = content.substr(cl_pos, end_cl - cl_pos);
        std::string unescaped;
        for (size_t i = 0; i < raw_cl.size(); ++i) {
          if (raw_cl[i] == '\\' && i + 1 < raw_cl.size()) {
            if (raw_cl[i + 1] == 'n') {
              unescaped += '\n';
              i++;
            } else if (raw_cl[i + 1] == '"' || raw_cl[i + 1] == '\\') {
              unescaped += raw_cl[i + 1];
              i++;
            } else {
              unescaped += raw_cl[i];
            }
          } else {
            unescaped += raw_cl[i];
          }
        }
        changelog_out = unescaped;
      }
    }
  }

  // Fallback to filename version if sw-description version is absent
  if (version_out.empty() && filename.rfind("openke-update-", 0) == 0) {
    std::string v = filename.substr(14);
    if (v.size() > 4 && v.substr(v.size() - 4) == ".swu") {
      version_out = v.substr(0, v.size() - 4);
    }
  }
}

static std::string parse_swu_version(const std::string &swu_path, const std::string &filename) {
  std::string v, cl;
  parse_swu_metadata(swu_path, filename, v, cl);
  return v;
}

static bool is_valid_semver(const std::string &v) {
  if (v.empty()) return false;
  bool has_digit = false;
  for (char c : v) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      has_digit = true;
    } else if (c != '.') {
      return false;
    }
  }
  return has_digit;
}

static std::vector<int> parse_version_nums(const std::string &v) {
  std::vector<int> nums;
  std::stringstream ss(v);
  std::string item;
  while (std::getline(ss, item, '.')) {
    try {
      nums.push_back(std::stoi(item));
    } catch (...) {
      nums.push_back(0);
    }
  }
  return nums;
}

static int compare_versions(const std::string &v1, const std::string &v2) {
  auto n1 = parse_version_nums(v1);
  auto n2 = parse_version_nums(v2);
  size_t max_len = std::max(n1.size(), n2.size());
  while (n1.size() < max_len) n1.push_back(0);
  while (n2.size() < max_len) n2.push_back(0);
  for (size_t i = 0; i < max_len; ++i) {
    if (n1[i] > n2[i]) return 1;
    if (n1[i] < n2[i]) return -1;
  }
  return 0;
}

static bool is_git_commit_hash(const std::string &v) {
  if (v.empty() || v.length() < 7 || v.length() > 40) return false;
  for (char c : v) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

static std::string get_remote_manifest_url() {
  std::vector<std::string> conf_paths = {
    "/usr/data/nebulaos/openke-update.conf",
    "/etc/openke-update.conf"
  };
  for (const auto &cp : conf_paths) {
    std::ifstream f(cp);
    if (f.is_open()) {
      std::string line;
      while (std::getline(f, line)) {
        if (line.rfind("remote_manifest_url=", 0) == 0) {
          std::string u = line.substr(20);
          if (!u.empty()) return u;
        }
      }
    }
  }
  return "https://raw.githubusercontent.com/OpenKlipperEdition/OpenKE/main/manifests/releases.json";
}

static void scan_remote_repo(std::vector<UpdatePackageItem> &packages, const std::string &current_ver) {
  std::string manifest_url = get_remote_manifest_url();
  std::string tmp_manifest = "/tmp/openke-remote-releases.json";
  unlink(tmp_manifest.c_str());

  std::string cmd = "curl -s -k -m 4 -L '" + manifest_url + "' -o '" + tmp_manifest + "' 2>/dev/null";
  int rc = system(cmd.c_str());
  if (rc != 0 || !fs::exists(tmp_manifest) || fs::file_size(tmp_manifest) == 0) {
    return;
  }

  std::ifstream f(tmp_manifest);
  if (!f.is_open()) return;

  try {
    json j = json::parse(f);

    // Format 1: OpenKE manifests/releases.json
    if (j.contains("releases") && j["releases"].is_array()) {
      for (const auto &rel : j["releases"]) {
        if (!rel.contains("url") || !rel.contains("version")) continue;

        UpdatePackageItem item;
        item.file_path = rel.value("url", "");
        item.file_name = rel.value("filename", "openke-update.swu");
        item.version = rel.value("version", "");
        item.expected_sha256 = rel.value("sha256", "");
        item.release_notes = rel.value("release_notes", "");
        item.changelog = rel.value("changelog", "");
        if (item.changelog.empty()) {
          item.changelog = item.release_notes;
        }
        item.location_tag = "Web Repo (OTA)";
        item.is_remote = true;

        std::string rel_type = rel.value("type", "stable");
        item.is_nightly = (rel_type == "nightly" || is_git_commit_hash(item.version) || item.file_name.find("nightly") != std::string::npos);

        uint64_t sz_bytes = rel.value("size_bytes", (uint64_t)0);
        if (sz_bytes > 0) {
          std::stringstream ss;
          ss << std::fixed << std::setprecision(1) << (static_cast<double>(sz_bytes) / (1024.0 * 1024.0)) << " MB";
          item.file_size = ss.str();
        } else {
          item.file_size = "Remote";
        }

        if (item.is_nightly) {
          item.status_badge = "Nightly (" + (item.version.empty() ? "Build" : item.version.substr(0, 7)) + ")";
          item.version_diff = 0;
        } else if (!item.version.empty() && !current_ver.empty()) {
          if (is_valid_semver(item.version) && is_valid_semver(current_ver)) {
            item.version_diff = compare_versions(item.version, current_ver);
            if (item.version_diff > 0) {
              item.status_badge = "Newer (Upgrade)";
            } else if (item.version_diff == 0) {
              item.status_badge = "Current Version";
            } else {
              item.status_badge = "Older (Downgrade)";
            }
          } else if (item.version == current_ver) {
            item.version_diff = 0;
            item.status_badge = "Current Version";
          } else {
            item.version_diff = 0;
            item.status_badge = "Firmware Package";
          }
        } else {
          item.status_badge = "Firmware Package";
        }

        bool dup = false;
        for (const auto &p : packages) {
          if (p.file_path == item.file_path || (p.is_remote && p.version == item.version)) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          packages.push_back(item);
        }
      }
    }
  } catch (const std::exception &e) {
    spdlog::warn("Remote update manifest parse error: {}", e.what());
  }

  unlink(tmp_manifest.c_str());
}

static std::vector<std::string> get_dev_server_urls() {
  std::vector<std::string> urls;
  std::vector<std::string> conf_paths = {
    "/usr/data/nebulaos/openke-update.conf",
    "/usr/data/printer_data/config/openke-update.conf",
    "/etc/openke-update.conf",
    "/tmp/openke-dev-server.conf",
    "/tmp/openke-dev-url",
    "/usr/data/openke-dev-url"
  };

  for (const auto &cp : conf_paths) {
    std::ifstream f(cp);
    if (!f.is_open()) continue;
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;

      std::string val;
      if (line.rfind("dev_server_url=", 0) == 0) {
        val = line.substr(15);
      } else if (line.rfind("dev_manifest_url=", 0) == 0) {
        val = line.substr(17);
      } else if (line.rfind("local_dev_url=", 0) == 0) {
        val = line.substr(14);
      } else if (line.rfind("dev_url=", 0) == 0) {
        val = line.substr(8);
      } else if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
        val = line;
      }

      if (!val.empty()) {
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'')) {
          val = val.substr(1, val.size() - 2);
        }
        if (std::find(urls.begin(), urls.end(), val) == urls.end()) {
          urls.push_back(val);
        }
      }
    }
  }
  return urls;
}

static void scan_dev_servers(std::vector<UpdatePackageItem> &packages, const std::string &current_ver) {
  std::vector<std::string> dev_urls = get_dev_server_urls();
  if (dev_urls.empty()) return;

  for (std::string base_url : dev_urls) {
    while (!base_url.empty() && base_url.back() == '/') {
      base_url.pop_back();
    }
    if (base_url.empty()) continue;

    std::string manifest_url;
    if (base_url.rfind(".json") != std::string::npos) {
      manifest_url = base_url;
    } else if (base_url.rfind(".swu") != std::string::npos) {
      UpdatePackageItem item;
      item.file_path = base_url;
      item.file_name = base_url.substr(base_url.find_last_of('/') + 1);
      item.version = "dev";
      item.status_badge = "Dev Build";
      item.location_tag = "Dev Server (LAN)";
      item.is_remote = true;
      item.is_nightly = true;
      item.file_size = "Local Server";

      std::string head_file = "/tmp/openke-dev-head.txt";
      unlink(head_file.c_str());
      std::string hcmd = "curl -s -k -I -m 2 '" + base_url + "' -o '" + head_file + "' 2>/dev/null";
      if (system(hcmd.c_str()) == 0 && fs::exists(head_file)) {
        std::ifstream hf(head_file);
        std::string hline;
        while (std::getline(hf, hline)) {
          if (hline.rfind("Content-Length:", 0) == 0 || hline.rfind("content-length:", 0) == 0) {
            try {
              uint64_t cl = std::stoull(hline.substr(15));
              if (cl > 0) {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(1) << (static_cast<double>(cl) / (1024.0 * 1024.0)) << " MB";
                item.file_size = ss.str();
              }
            } catch (...) {}
          }
        }
        unlink(head_file.c_str());
        packages.push_back(item);
      }
      continue;
    } else {
      manifest_url = base_url + "/releases.json";
    }

    std::string tmp_manifest = "/tmp/openke-dev-manifest.json";
    unlink(tmp_manifest.c_str());
    std::string cmd = "curl -s -k -m 2 -L '" + manifest_url + "' -o '" + tmp_manifest + "' 2>/dev/null";
    int rc = system(cmd.c_str());

    bool manifest_ok = (rc == 0 && fs::exists(tmp_manifest) && fs::file_size(tmp_manifest) > 0);
    if (!manifest_ok && manifest_url == (base_url + "/releases.json")) {
      manifest_url = base_url + "/manifest.json";
      cmd = "curl -s -k -m 2 -L '" + manifest_url + "' -o '" + tmp_manifest + "' 2>/dev/null";
      rc = system(cmd.c_str());
      manifest_ok = (rc == 0 && fs::exists(tmp_manifest) && fs::file_size(tmp_manifest) > 0);
    }

    if (manifest_ok) {
      std::ifstream f(tmp_manifest);
      if (f.is_open()) {
        try {
          json j = json::parse(f);
          if (j.contains("releases") && j["releases"].is_array()) {
            for (const auto &rel : j["releases"]) {
              if (!rel.contains("url") && !rel.contains("filename")) continue;

              std::string rel_url = rel.value("url", "");
              std::string fname = rel.value("filename", "openke-update.swu");
              if (rel_url.empty()) rel_url = fname;

              if (rel_url.rfind("http://", 0) != 0 && rel_url.rfind("https://", 0) != 0) {
                if (rel_url.front() == '/') {
                  rel_url = base_url + rel_url;
                } else {
                  rel_url = base_url + "/" + rel_url;
                }
              }

              UpdatePackageItem item;
              item.file_path = rel_url;
              item.file_name = fname;
              item.version = rel.value("version", "dev");
              item.expected_sha256 = rel.value("sha256", "");
              item.release_notes = rel.value("release_notes", "");
              item.changelog = rel.value("changelog", "");
              if (item.changelog.empty()) {
                item.changelog = item.release_notes;
              }
              item.location_tag = "Dev Server (LAN)";
              item.is_remote = true;
              item.is_nightly = true;
              item.status_badge = "Dev Build";

              uint64_t sz_bytes = rel.value("size_bytes", (uint64_t)0);
              if (sz_bytes > 0) {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(1) << (static_cast<double>(sz_bytes) / (1024.0 * 1024.0)) << " MB";
                item.file_size = ss.str();
              } else {
                item.file_size = "Dev Server";
              }

              bool dup = false;
              for (const auto &p : packages) {
                if (p.file_path == item.file_path) {
                  dup = true;
                  break;
                }
              }
              if (!dup) {
                packages.push_back(item);
              }
            }
          }
        } catch (const std::exception &e) {
          spdlog::warn("Dev server manifest parse error: {}", e.what());
        }
      }
      unlink(tmp_manifest.c_str());
    } else {
      std::vector<std::string> swu_probes = { "openke-update.swu", "openke-update-1.0.0.swu" };
      for (const auto &swu_name : swu_probes) {
        std::string test_url = base_url + "/" + swu_name;
        std::string head_file = "/tmp/openke-dev-probe-head.txt";
        unlink(head_file.c_str());
        std::string pcmd = "curl -s -k -I -m 2 '" + test_url + "' -o '" + head_file + "' 2>/dev/null";
        if (system(pcmd.c_str()) == 0 && fs::exists(head_file)) {
          std::ifstream hf(head_file);
          std::string first_line;
          std::getline(hf, first_line);
          if (first_line.find("200") != std::string::npos) {
            UpdatePackageItem item;
            item.file_path = test_url;
            item.file_name = swu_name;
            item.version = "dev";
            item.status_badge = "Dev Build";
            item.location_tag = "Dev Server (LAN)";
            item.is_remote = true;
            item.is_nightly = true;
            item.file_size = "Local Server";

            std::string hline;
            while (std::getline(hf, hline)) {
              if (hline.rfind("Content-Length:", 0) == 0 || hline.rfind("content-length:", 0) == 0) {
                try {
                  uint64_t cl = std::stoull(hline.substr(15));
                  if (cl > 0) {
                    std::stringstream ss;
                    ss << std::fixed << std::setprecision(1) << (static_cast<double>(cl) / (1024.0 * 1024.0)) << " MB";
                    item.file_size = ss.str();
                  }
                } catch (...) {}
              }
            }
            unlink(head_file.c_str());
            packages.push_back(item);
            break;
          }
        }
        unlink(head_file.c_str());
      }
    }
  }
}

static void scan_local_storage(std::vector<UpdatePackageItem> &packages, const std::string &current_ver, bool usb_only) {
  std::vector<std::pair<std::string, std::string>> search_paths;

  // 1. Dynamic scan from /proc/mounts for any connected USB storage (/dev/sd* or /dev/mmcblk1*)
  std::ifstream mounts("/proc/mounts");
  if (mounts.is_open()) {
    std::string dev, mountpoint, fstype, opts;
    int d1, d2;
    while (mounts >> dev >> mountpoint >> fstype >> opts >> d1 >> d2) {
      if (dev.rfind("/dev/sd", 0) == 0 || dev.rfind("/dev/mmcblk1", 0) == 0) {
        std::string dev_name = dev.substr(dev.find_last_of('/') + 1);
        search_paths.push_back({mountpoint, "USB (" + dev_name + ")"});
      }
    }
  }

  if (usb_only && search_paths.empty()) {
    return;
  }

  // 2. Standard filesystem mount and storage paths (if not usb_only)
  if (!usb_only) {
    std::vector<std::pair<std::string, std::string>> fixed_paths = {
      {"/opt/printer_data/gcodes/USB", "USB Drive"},
      {"/tmp/udisk", "USB Drive"},
      {"/media", "USB Drive"},
      {"/mnt", "USB Drive"},
      {"/tmp/usb", "USB Drive"},
      {"/usr/data/deploy-staging", "Local Staging"},
      {"/usr/data", "Internal Storage"}
    };

    for (const auto &p : fixed_paths) {
      bool already_added = false;
      for (const auto &sp : search_paths) {
        if (sp.first == p.first) {
          already_added = true;
          break;
        }
      }
      if (!already_added) {
        search_paths.push_back(p);
      }
    }
  }

  for (const auto &sp : search_paths) {
    const std::string &dir = sp.first;
    const std::string &tag = sp.second;

    if (!fs::exists(dir)) continue;

    try {
      if (fs::is_directory(dir)) {
        for (const auto &entry : fs::directory_iterator(dir)) {
          if (entry.path().extension() == ".swu") {
            UpdatePackageItem item;
            item.file_path = entry.path().string();
            item.file_name = entry.path().filename().string();
            item.location_tag = tag;
            parse_swu_metadata(item.file_path, item.file_name, item.version, item.changelog);
            item.release_notes = item.changelog;
            item.is_remote = false;

            bool is_nightly_build = is_git_commit_hash(item.version) || (item.file_name.find("nightly") != std::string::npos);
            item.is_nightly = is_nightly_build;

            if (item.is_nightly) {
              item.status_badge = "Nightly (" + (item.version.empty() ? "Build" : item.version.substr(0, 7)) + ")";
              item.version_diff = 0;
            } else if (!item.version.empty() && !current_ver.empty()) {
              if (is_valid_semver(item.version) && is_valid_semver(current_ver)) {
                item.version_diff = compare_versions(item.version, current_ver);
                if (item.version_diff > 0) {
                  item.status_badge = "Newer (Upgrade)";
                } else if (item.version_diff == 0) {
                  item.status_badge = "Current Version";
                } else {
                  item.status_badge = "Older (Downgrade)";
                }
              } else if (item.version == current_ver) {
                item.version_diff = 0;
                item.status_badge = "Current Version";
              } else {
                item.version_diff = 0;
                item.status_badge = "Firmware Package";
              }
            } else {
              item.status_badge = "Firmware Package";
            }

            auto fsize = fs::file_size(entry.path());
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1) << (static_cast<double>(fsize) / (1024.0 * 1024.0)) << " MB";
            item.file_size = ss.str();

            bool is_duplicate = false;
            for (const auto &f : packages) {
              if (f.file_path == item.file_path) {
                is_duplicate = true;
                break;
              }
            }
            if (!is_duplicate) {
              packages.push_back(item);
            }
          }
        }
      }
    } catch (const std::exception &e) {
      spdlog::warn("Error scanning path {}: {}", dir, e.what());
    }
  }
}

void UpdatePanel::scan_updates(bool include_network) {
  found_packages.clear();
  std::string current_ver = get_current_os_version();

  scan_local_storage(found_packages, current_ver, false);

  if (include_network) {
    scan_dev_servers(found_packages, current_ver);
    scan_remote_repo(found_packages, current_ver);
  }

  spdlog::info("SWUpdate scanner found {} package(s)", found_packages.size());
}

void UpdatePanel::build_package_list() {
  lv_obj_clean(list_cont);

  std::string current_ver = get_current_os_version();
  std::string slot_name = KUtils::get_active_slot_name();
  std::string title_str = "Firmware Update (v" + current_ver + " • " + slot_name + ")";
  lv_label_set_text(title_label, title_str.c_str());

  if (found_packages.empty()) {
    lv_obj_t *empty_card = lv_obj_create(list_cont);
    lv_obj_set_size(empty_card, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(empty_card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_radius(empty_card, 8, 0);

    lv_obj_t *msg = lv_label_create(empty_card);
    lv_label_set_text(msg, "No SWUpdate packages (.swu) found.\n\nInsert a USB flash drive containing an openke-update-*.swu\nfile and click 'Scan USB'.");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_center(msg);
    return;
  }

  for (size_t i = 0; i < found_packages.size(); ++i) {
    const auto &pkg = found_packages[i];

    lv_obj_t *card = lv_obj_create(list_cont);
    lv_obj_set_size(card, LV_PCT(100), 90);
    lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, pkg.file_name.c_str());
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Version Badge label (between filename and size/source)
    if (!pkg.version.empty() || pkg.is_nightly) {
      lv_obj_t *badge = lv_obj_create(card);
      lv_obj_set_size(badge, LV_SIZE_CONTENT, 20);
      lv_obj_set_style_pad_hor(badge, 6, 0);
      lv_obj_set_style_pad_ver(badge, 1, 0);
      lv_obj_set_style_radius(badge, 4, 0);
      lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 24);

      if (pkg.is_nightly) {
        lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_PURPLE), 0);
      } else if (pkg.version_diff > 0) {
        lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_GREEN), 0);
      } else if (pkg.version_diff == 0) {
        lv_obj_set_style_bg_color(badge, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
      } else {
        lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_ORANGE), 0);
      }

      lv_obj_t *badge_lbl = lv_label_create(badge);
      std::string b_text;
      if (pkg.is_nightly) {
        b_text = pkg.status_badge;
      } else {
        b_text = "v" + pkg.version + " (" + pkg.status_badge + ")";
      }
      lv_label_set_text(badge_lbl, b_text.c_str());
      lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
      lv_obj_center(badge_lbl);
    }

    // Meta label
    lv_obj_t *meta_lbl = lv_label_create(card);
    std::string meta_str = "Size: " + pkg.file_size + " | Source: " + pkg.location_tag;
    lv_label_set_text(meta_lbl, meta_str.c_str());
    lv_obj_set_style_text_font(meta_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(meta_lbl, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(meta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // Flash / Install / Download Button
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, 110, 42);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    if (pkg.version_diff > 0) {
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    }
    lv_obj_set_style_radius(btn, 6, 0);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    if (pkg.is_remote) {
      lv_label_set_text(btn_lbl, "Download");
    } else {
      lv_label_set_text(btn_lbl, "Install");
    }
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_lbl);

    auto click_handler = [](lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      auto *self = static_cast<UpdatePanel*>(e->user_data);
      if (KUtils::is_printing()) {
        KUtils::notify_locked();
        return;
      }
      size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
      if (idx < self->found_packages.size()) {
        self->show_confirmation_modal(self->found_packages[idx]);
      }
    };

    lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
    lv_obj_set_user_data(card, (void*)(uintptr_t)i);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, click_handler, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(card, click_handler, LV_EVENT_CLICKED, this);
  }
}

void UpdatePanel::close_usb_detect_popup() {
  if (usb_detect_mbox != nullptr) {
    lv_obj_t *m = usb_detect_mbox;
    usb_detect_mbox = nullptr;
    lv_msgbox_close(m);
  }
}

void UpdatePanel::show_usb_detect_popup(const UpdatePackageItem &pkg) {
  close_usb_detect_popup();

  pending_usb_package = pkg;

  static const char *btns[] = {"View Update", "Dismiss", ""};
  std::string body = "Found firmware update package on USB:\n" +
                     pkg.file_name + " (" + pkg.file_size + ")\n\n"
                     "Open System Update to view details and install?";

  usb_detect_mbox = lv_msgbox_create(NULL, "USB Update Detected",
                                     body.c_str(), btns, false);
  KUtils::style_dialog_msgbox(usb_detect_mbox);

  lv_obj_set_size(usb_detect_mbox, LV_PCT(88), LV_PCT(82));
  lv_obj_center(usb_detect_mbox);

  lv_obj_t *title = ((lv_msgbox_t *)usb_detect_mbox)->title;
  if (title != NULL) {
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  }

  lv_obj_t *txt = ((lv_msgbox_t *)usb_detect_mbox)->text;
  if (txt != NULL) {
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(txt, LV_PCT(100));
  }

  lv_obj_t *btnm = lv_msgbox_get_btns(usb_detect_mbox);
  if (btnm != NULL) {
    lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, -4);
    auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;
    lv_obj_set_size(btnm, LV_PCT(92), 44 * hscale);
  }

  auto cb = [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t btn_idx = lv_msgbox_get_active_btn(mbox);
    UpdatePackageItem pkg = self->pending_usb_package;

    self->close_usb_detect_popup();

    if (btn_idx == 0) { // "View Update"
      self->foreground();
      self->show_confirmation_modal(pkg);
    }
  };

  lv_obj_add_event_cb(usb_detect_mbox, cb, LV_EVENT_VALUE_CHANGED, this);
}

void UpdatePanel::check_usb_auto_detect() {
  if (state != UpdateState::IDLE) return;
  if (KUtils::is_printing() || KUtils::is_paused()) return;

  if (usb_detect_mbox != nullptr) {
    if (!fs::exists(pending_usb_package.file_path)) {
      close_usb_detect_popup();
    }
    return;
  }

  std::vector<UpdatePackageItem> usb_packages;
  std::string current_ver = get_current_os_version();
  scan_local_storage(usb_packages, current_ver, true /* usb_only */);

  if (usb_packages.empty()) {
    prompted_packages.clear();
    return;
  }

  std::set<std::string> current_paths;
  for (const auto &p : usb_packages) {
    current_paths.insert(p.file_path);
  }

  for (auto it = prompted_packages.begin(); it != prompted_packages.end(); ) {
    if (current_paths.find(*it) == current_paths.end()) {
      it = prompted_packages.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto &pkg : usb_packages) {
    if (prompted_packages.find(pkg.file_path) == prompted_packages.end()) {
      prompted_packages.insert(pkg.file_path);
      show_usb_detect_popup(pkg);
      break;
    }
  }
}

void UpdatePanel::close_whats_new_popup() {
  if (whats_new_mbox != nullptr) {
    lv_obj_t *m = whats_new_mbox;
    whats_new_mbox = nullptr;
    lv_obj_del(m);
  }
}

void UpdatePanel::show_first_boot_whats_new_popup(const std::string &version, const std::string &changelog_text) {
  close_whats_new_popup();

  whats_new_mbox = lv_obj_create(lv_scr_act());
  lv_obj_add_flag(whats_new_mbox, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(whats_new_mbox, LV_PCT(90), LV_PCT(88));
  lv_obj_center(whats_new_mbox);
  lv_obj_move_foreground(whats_new_mbox);
  lv_obj_set_style_bg_color(whats_new_mbox, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_bg_opa(whats_new_mbox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(whats_new_mbox, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_set_style_border_width(whats_new_mbox, 2, 0);
  lv_obj_set_style_radius(whats_new_mbox, 12, 0);
  lv_obj_set_style_pad_all(whats_new_mbox, 14, 0);
  lv_obj_clear_flag(whats_new_mbox, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(whats_new_mbox);
  std::string title_str = "Welcome to OpenKE v" + version;
  lv_label_set_text(title, title_str.c_str());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *sub_label = lv_label_create(whats_new_mbox);
  lv_label_set_text(sub_label, "Firmware update applied successfully! What's new:");
  lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(sub_label, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
  lv_obj_align(sub_label, LV_ALIGN_TOP_LEFT, 0, 24);

  // Scrollable changelog box
  lv_obj_t *cl_box = lv_obj_create(whats_new_mbox);
  lv_obj_set_size(cl_box, LV_PCT(100), LV_PCT(58));
  lv_obj_align(cl_box, LV_ALIGN_TOP_MID, 0, 44);
  lv_obj_set_style_bg_color(cl_box, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_set_style_radius(cl_box, 6, 0);
  lv_obj_set_style_pad_all(cl_box, 8, 0);
  lv_obj_add_flag(cl_box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *cl_text = lv_label_create(cl_box);
  lv_label_set_text(cl_text, changelog_text.c_str());
  lv_obj_set_style_text_font(cl_text, &lv_font_montserrat_12, 0);
  lv_obj_set_width(cl_text, LV_PCT(100));
  lv_label_set_long_mode(cl_text, LV_LABEL_LONG_WRAP);

  // Dismiss button
  lv_obj_t *btn = lv_btn_create(whats_new_mbox);
  lv_obj_set_size(btn, 160, 40);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_set_style_radius(btn, 6, 0);

  lv_obj_t *btn_lbl = lv_label_create(btn);
  lv_label_set_text(btn_lbl, "Get Started");
  lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(btn_lbl);

  lv_obj_add_event_cb(btn, [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    self->close_whats_new_popup();
  }, LV_EVENT_CLICKED, this);
}

void UpdatePanel::check_first_boot_whats_new() {
  if (first_boot_checked) return;
  first_boot_checked = true;

  std::string pending_file = "/usr/data/nebulaos/.pending_whats_new";
  if (!fs::exists(pending_file)) {
    pending_file = "/usr/data/nebulaos/pending_changelog.txt";
    if (!fs::exists(pending_file)) {
      return;
    }
  }

  std::string changelog_text;
  std::ifstream f(pending_file);
  if (f.is_open()) {
    std::stringstream buffer;
    buffer << f.rdbuf();
    changelog_text = buffer.str();
    f.close();
  }

  // Delete the pending marker file so it is never shown again on subsequent boots
  unlink(pending_file.c_str());

  if (changelog_text.empty()) {
    changelog_text = "• Welcome to OpenKE!\n• System update installed successfully.";
  }

  std::string current_ver = get_current_os_version();
  show_first_boot_whats_new_popup(current_ver, changelog_text);
}

void UpdatePanel::close_modal() {
  if (modal_cont != nullptr) {
    lv_obj_del(modal_cont);
    modal_cont = nullptr;
  }
}

void UpdatePanel::show_confirmation_modal(const UpdatePackageItem &pkg) {
  close_usb_detect_popup();
  close_modal();
  selected_package = pkg;
  state = UpdateState::CONFIRMING;

  bool slot2_active = KUtils::is_slot2_active();
  bool printing = KUtils::is_printing();
  std::string active_slot = slot2_active ? "Slot 2" : "Slot 1";
  std::string target_slot = slot2_active ? "Slot 1" : "Slot 2";

  modal_cont = lv_obj_create(cont);
  lv_obj_add_flag(modal_cont, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(modal_cont, LV_PCT(90), LV_PCT(88));
  lv_obj_center(modal_cont);
  lv_obj_move_foreground(modal_cont);
  lv_obj_set_style_bg_color(modal_cont, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_bg_opa(modal_cont, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(modal_cont, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_border_width(modal_cont, 2, 0);
  lv_obj_set_style_radius(modal_cont, 12, 0);
  lv_obj_set_style_pad_all(modal_cont, 16, 0);
  lv_obj_clear_flag(modal_cont, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(modal_cont);
  lv_label_set_text(title, pkg.is_remote ? "Confirm Remote Firmware Update" : "Confirm System Firmware Update");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *content_cont = lv_obj_create(modal_cont);
  lv_obj_set_size(content_cont, LV_PCT(100), LV_PCT(68));
  lv_obj_align(content_cont, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_flex_flow(content_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(content_cont, 4, 0);
  lv_obj_set_style_pad_gap(content_cont, 6, 0);
  lv_obj_add_flag(content_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(content_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content_cont, 0, 0);

  lv_obj_t *desc = lv_label_create(content_cont);
  std::string current_ver = get_current_os_version();
  std::string pkg_ver_str;
  if (pkg.is_nightly) {
    pkg_ver_str = pkg.status_badge;
  } else if (!pkg.version.empty()) {
    pkg_ver_str = "v" + pkg.version + " (" + pkg.status_badge + ")";
  } else {
    pkg_ver_str = "Unspecified";
  }
  std::string info_text = "• Package: " + pkg.file_name + " (" + pkg.file_size + ")\n"
                          "• Version: " + pkg_ver_str + "\n"
                          "• Installed: OpenKE v" + current_ver + " (" + active_slot + ")\n"
                          "• Target Slot: " + target_slot + "\n"
                          "• Source: " + pkg.location_tag;
  if (printing) {
    info_text += "\n\n[LOCKED] Printer is currently active! Flashing is blocked.";
  }
  lv_label_set_text(desc, info_text.c_str());
  lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);

  if (!pkg.changelog.empty()) {
    lv_obj_t *cl_box = lv_obj_create(content_cont);
    lv_obj_set_size(cl_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(cl_box, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_radius(cl_box, 6, 0);
    lv_obj_set_style_pad_all(cl_box, 6, 0);
    lv_obj_set_flex_flow(cl_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cl_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cl_hdr = lv_label_create(cl_box);
    lv_label_set_text(cl_hdr, "What's New in this Version:");
    lv_obj_set_style_text_font(cl_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cl_hdr, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);

    lv_obj_t *cl_text = lv_label_create(cl_box);
    lv_label_set_text(cl_text, pkg.changelog.c_str());
    lv_obj_set_style_text_font(cl_text, &lv_font_montserrat_12, 0);
    lv_obj_set_width(cl_text, LV_PCT(100));
    lv_label_set_long_mode(cl_text, LV_LABEL_LONG_WRAP);
  }

  // Cancel button
  lv_obj_t *cancel_btn = lv_btn_create(modal_cont);
  lv_obj_set_size(cancel_btn, 130, 44);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 10, 0);
  lv_obj_set_style_bg_color(cancel_btn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, "Cancel");
  lv_obj_center(cancel_lbl);

  lv_obj_add_event_cb(cancel_btn, [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    self->state = UpdateState::IDLE;
    self->close_modal();
  }, LV_EVENT_CLICKED, this);

  // Confirm button
  lv_obj_t *confirm_btn = lv_btn_create(modal_cont);
  lv_obj_set_size(confirm_btn, 160, 44);
  lv_obj_align(confirm_btn, LV_ALIGN_BOTTOM_RIGHT, -10, 0);
  lv_obj_set_style_bg_color(confirm_btn, printing ? lv_palette_darken(LV_PALETTE_GREY, 3) : lv_palette_main(LV_PALETTE_GREEN), 0);
  if (printing) {
    lv_obj_add_state(confirm_btn, LV_STATE_DISABLED);
  }
  lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
  if (printing) {
    lv_label_set_text(confirm_lbl, "Busy (Printing)");
  } else if (pkg.is_remote) {
    lv_label_set_text(confirm_lbl, "Download & Flash");
  } else {
    lv_label_set_text(confirm_lbl, "Flash Update");
  }
  lv_obj_center(confirm_lbl);

  lv_obj_add_event_cb(confirm_btn, [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    if (KUtils::is_printing()) {
      KUtils::notify_locked();
      return;
    }
    self->start_update(self->selected_package);
  }, LV_EVENT_CLICKED, this);
}

void UpdatePanel::show_progress_view(const UpdatePackageItem &pkg) {
  close_modal();

  modal_cont = lv_obj_create(cont);
  lv_obj_add_flag(modal_cont, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(modal_cont, LV_PCT(92), LV_PCT(88));
  lv_obj_center(modal_cont);
  lv_obj_move_foreground(modal_cont);
  lv_obj_set_style_bg_color(modal_cont, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_bg_opa(modal_cont, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(modal_cont, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_border_width(modal_cont, 2, 0);
  lv_obj_set_style_radius(modal_cont, 12, 0);
  lv_obj_set_style_pad_all(modal_cont, 16, 0);
  lv_obj_clear_flag(modal_cont, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(modal_cont);
  lv_label_set_text(title, pkg.is_remote ? "Downloading & Installing Update..." : "Flashing Firmware Update...");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  // Progress bar
  progress_bar = lv_bar_create(modal_cont);
  lv_obj_set_size(progress_bar, LV_PCT(100), 24);
  lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 40);
  lv_bar_set_range(progress_bar, 0, 100);
  lv_bar_set_value(progress_bar, 0, LV_ANIM_ON);

  progress_label = lv_label_create(modal_cont);
  lv_label_set_text(progress_label, "0%");
  lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_14, 0);
  lv_obj_align(progress_label, LV_ALIGN_TOP_MID, 0, 70);

  status_label = lv_label_create(modal_cont);
  lv_label_set_text(status_label, pkg.is_remote ? "Connecting to download server..." : "Initializing SWUpdate...");
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(status_label, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 0, 95);

  reboot_btn = lv_btn_create(modal_cont);
  lv_obj_set_size(reboot_btn, 160, 44);
  lv_obj_align(reboot_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(reboot_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_flag(reboot_btn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *reboot_lbl = lv_label_create(reboot_btn);
  lv_label_set_text(reboot_lbl, "Reboot Now");
  lv_obj_center(reboot_lbl);

  lv_obj_add_event_cb(reboot_btn, [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    if (self->state == UpdateState::FAILED) {
      self->state = UpdateState::IDLE;
      self->close_modal();
    } else if (self->state == UpdateState::SUCCESS) {
      spdlog::info("Reboot triggered by user after firmware update");
      sync();
      int rc = system("sync; reboot -f || /sbin/reboot -f || reboot || /sbin/reboot");
      (void)rc;
      reboot(RB_AUTOBOOT);
    }
  }, LV_EVENT_CLICKED, this);
}

void UpdatePanel::start_update(const UpdatePackageItem &pkg) {
  if (KUtils::is_printing()) {
    KUtils::notify_locked();
    close_modal();
    return;
  }

  show_progress_view(pkg);

  state = UpdateState::FLASHING;
  progress_percent = 0;
  current_step = 0;
  total_steps = 0;
  status_message = pkg.is_remote ? "Starting download..." : "Starting SWUpdate worker...";

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread(&UpdatePanel::execute_update_thread, this, pkg);
}

void UpdatePanel::execute_update_thread(UpdatePackageItem pkg) {
  spdlog::info("SWUpdate worker executing for {} (remote: {})", pkg.file_name, pkg.is_remote);

  std::string local_swu_path = pkg.file_path;
  bool is_temp_download = false;

  if (pkg.is_remote) {
    is_temp_download = true;
    std::string staging_dir = "/usr/data/deploy-staging";
    try {
      fs::create_directories(staging_dir);
    } catch (...) {}

    local_swu_path = staging_dir + "/openke-remote-update.swu";
    unlink(local_swu_path.c_str());

    {
      std::lock_guard<std::mutex> lock(status_mutex);
      status_message = "Downloading firmware package...";
    }
    progress_percent = 5;

    std::string curl_cmd = "curl -fSL -k --connect-timeout 20 -m 1200 '" + pkg.file_path + "' -o '" + local_swu_path + "' > /tmp/curl-download.log 2>&1";

    try {
      auto dl_proc = sp::Popen(curl_cmd, sp::shell{true});
      while (dl_proc.poll() == -1) {
        usleep(250000); // 250ms
        if (fs::exists(local_swu_path)) {
          uintmax_t cur_sz = fs::file_size(local_swu_path);
          double mb = static_cast<double>(cur_sz) / (1024.0 * 1024.0);
          std::stringstream ss;
          ss << "Downloading: " << std::fixed << std::setprecision(1) << mb << " MB";
          {
            std::lock_guard<std::mutex> lock(status_mutex);
            status_message = ss.str();
          }
          progress_percent = std::min(progress_percent + 1, 45);
        }
      }

      int dl_rc = dl_proc.retcode();
      if (dl_rc != 0 || !fs::exists(local_swu_path) || fs::file_size(local_swu_path) < 1024) {
        std::lock_guard<std::mutex> lock(status_mutex);
        status_message = "Download failed (curl error " + std::to_string(dl_rc) + "). Check network connection.";
        state = UpdateState::FAILED;
        unlink(local_swu_path.c_str());
        return;
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(status_mutex);
      status_message = std::string("Download error: ") + e.what();
      state = UpdateState::FAILED;
      unlink(local_swu_path.c_str());
      return;
    }

    progress_percent = 50;

    if (!pkg.expected_sha256.empty()) {
      {
        std::lock_guard<std::mutex> lock(status_mutex);
        status_message = "Verifying package checksum...";
      }

      std::string sha_cmd = "sha256sum '" + local_swu_path + "' | awk '{print $1}'";
      std::string computed_sha;
      try {
        auto sha_proc = sp::Popen(sha_cmd, sp::shell{true}, sp::output{sp::PIPE});
        auto out = sha_proc.communicate();
        if (out.first.length > 0 && out.first.buf.data() != nullptr) {
          computed_sha = std::string(out.first.buf.data(), out.first.length);
        }
        while (!computed_sha.empty() && (computed_sha.back() == '\n' || computed_sha.back() == '\r' || computed_sha.back() == ' ')) {
          computed_sha.pop_back();
        }
      } catch (...) {}

      if (!computed_sha.empty() && computed_sha != pkg.expected_sha256) {
        spdlog::error("SHA256 mismatch! Expected: {}, Got: {}", pkg.expected_sha256, computed_sha);
        std::lock_guard<std::mutex> lock(status_mutex);
        status_message = "Checksum verification failed! Package may be corrupted.";
        state = UpdateState::FAILED;
        unlink(local_swu_path.c_str());
        return;
      }
    }
  }

  std::string target_slot = KUtils::is_slot2_active() ? "slot1" : "slot2";
  std::string selection_arg = "stable," + target_slot;

  {
    std::lock_guard<std::mutex> lock(status_mutex);
    status_message = "Targeting inactive " + target_slot + "... invoking swupdate";
  }

  std::string log_file = "/tmp/swupdate.log";
  std::string cmd;
  if (fs::exists("/usr/bin/swupdate")) {
    cmd = "/usr/bin/swupdate -i '" + local_swu_path + "' -e '" + selection_arg + "' -v > " + log_file + " 2>&1";
  } else {
    cmd = "swupdate -i '" + local_swu_path + "' -e '" + selection_arg + "' -v > " + log_file + " 2>&1";
  }

  int rc = -1;
  try {
    auto p = sp::Popen(cmd, sp::shell{true});

    while (p.poll() == -1) {
      usleep(100000); // 100ms
      progress_percent = std::min(progress_percent + 2, 95);
    }

    rc = p.retcode();

    if (is_temp_download) {
      unlink(local_swu_path.c_str());
    }

    if (rc == 0) {
      progress_percent = 100;
      std::lock_guard<std::mutex> lock(status_mutex);
      status_message = "Update verified and written to " + target_slot + " successfully!";
      state = UpdateState::SUCCESS;
    } else {
      std::string err_output;
      std::ifstream lf(log_file);
      if (lf.is_open()) {
        std::stringstream ss;
        ss << lf.rdbuf();
        std::string full_log = ss.str();
        if (full_log.size() > 300) {
          err_output = full_log.substr(full_log.size() - 300);
        } else {
          err_output = full_log;
        }
      }
      std::lock_guard<std::mutex> lock(status_mutex);
      status_message = "SWUpdate failed (code " + std::to_string(rc) + "): " + err_output;
      state = UpdateState::FAILED;
    }
  } catch (const std::exception &e) {
    if (is_temp_download) {
      unlink(local_swu_path.c_str());
    }
    std::lock_guard<std::mutex> lock(status_mutex);
    status_message = std::string("Exception executing swupdate: ") + e.what();
    state = UpdateState::FAILED;
  }

  spdlog::info("SWUpdate worker finished with state {}", static_cast<int>(state.load()));
}

void UpdatePanel::timer_tick() {
  if (!first_boot_checked) {
    check_first_boot_whats_new();
  }
  if (state == UpdateState::IDLE) {
    scan_tick_counter = (scan_tick_counter + 1) % 15; // Every 3 seconds (15 * 200ms)
    if (scan_tick_counter == 0) {
      check_usb_auto_detect();
    }
  } else if (state == UpdateState::FLASHING) {
    if (progress_bar != nullptr) {
      lv_bar_set_value(progress_bar, progress_percent.load(), LV_ANIM_ON);
    }
    if (progress_label != nullptr) {
      std::string pct_str = std::to_string(progress_percent.load()) + "%";
      lv_label_set_text(progress_label, pct_str.c_str());
    }
    if (status_label != nullptr) {
      std::lock_guard<std::mutex> lock(status_mutex);
      lv_label_set_text(status_label, status_message.c_str());
    }
  } else if (state == UpdateState::SUCCESS) {
    if (progress_bar != nullptr) {
      lv_bar_set_value(progress_bar, 100, LV_ANIM_OFF);
    }
    if (progress_label != nullptr) {
      lv_label_set_text(progress_label, "100% - Done");
    }
    if (status_label != nullptr) {
      std::lock_guard<std::mutex> lock(status_mutex);
      lv_label_set_text(status_label, status_message.c_str());
    }
    if (reboot_btn != nullptr) {
      lv_obj_clear_flag(reboot_btn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(reboot_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_t *lbl = lv_obj_get_child(reboot_btn, 0);
      if (lbl) lv_label_set_text(lbl, "Reboot Now");
    }
  } else if (state == UpdateState::FAILED) {
    if (status_label != nullptr) {
      std::lock_guard<std::mutex> lock(status_mutex);
      lv_label_set_text(status_label, status_message.c_str());
      lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_RED), 0);
    }
    if (reboot_btn != nullptr) {
      lv_obj_clear_flag(reboot_btn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(reboot_btn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
      lv_obj_t *lbl = lv_obj_get_child(reboot_btn, 0);
      if (lbl) lv_label_set_text(lbl, "Close");
    }
  }
}
