#include "update_panel.h"
#include "utils.h"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"

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
  close_usb_detect_popup();
  scan_updates();
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
      scan_updates();
      build_package_list();
    }
  }
}

static bool is_slot2_active() {
  std::ifstream cmdline("/proc/cmdline");
  if (cmdline.is_open()) {
    std::string line;
    std::getline(cmdline, line);
    if (line.find("root=/dev/mmcblk0p8") != std::string::npos ||
        line.find("rootfs2") != std::string::npos) {
      return true;
    }
  }
  return false;
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

static std::string parse_swu_version(const std::string &swu_path, const std::string &filename) {
  std::ifstream f(swu_path, std::ios::binary);
  if (f.is_open()) {
    std::vector<char> buf(8192, 0);
    f.read(buf.data(), buf.size() - 1);
    std::string content(buf.data());
    auto pos = content.find("version = \"");
    if (pos != std::string::npos) {
      pos += 11;
      auto endpos = content.find('"', pos);
      if (endpos != std::string::npos) {
        return content.substr(pos, endpos - pos);
      }
    }
  }
  if (filename.rfind("openke-update-", 0) == 0) {
    std::string v = filename.substr(14);
    if (v.size() > 4 && v.substr(v.size() - 4) == ".swu") {
      return v.substr(0, v.size() - 4);
    }
  }
  return "";
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
  if (v1.empty() || v2.empty()) return 0;
  if (v1 == v2) return 0;
  if (!is_valid_semver(v1) || !is_valid_semver(v2)) return 0;
  auto nums1 = parse_version_nums(v1);
  auto nums2 = parse_version_nums(v2);
  size_t max_len = std::max(nums1.size(), nums2.size());
  for (size_t i = 0; i < max_len; ++i) {
    int n1 = i < nums1.size() ? nums1[i] : 0;
    int n2 = i < nums2.size() ? nums2[i] : 0;
    if (n1 > n2) return 1;
    if (n1 < n2) return -1;
  }
  return 0;
}

void UpdatePanel::scan_updates() {
  found_packages.clear();
  std::string current_ver = get_current_os_version();

  std::vector<std::pair<std::string, std::string>> search_paths;

  // 1. Dynamic scan from /proc/mounts for any connected USB storage (/dev/sd*)
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

  // 2. Standard filesystem mount and storage paths
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
            item.version = parse_swu_version(item.file_path, item.file_name);

            if (!item.version.empty() && !current_ver.empty()) {
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
            for (const auto &f : found_packages) {
              if (f.file_path == item.file_path) {
                is_duplicate = true;
                break;
              }
            }
            if (!is_duplicate) {
              found_packages.push_back(item);
            }
          }
        }
      }
    } catch (const std::exception &e) {
      spdlog::warn("Error scanning path {}: {}", dir, e.what());
    }
  }

  spdlog::info("SWUpdate scanner found {} package(s)", found_packages.size());
}

void UpdatePanel::build_package_list() {
  lv_obj_clean(list_cont);

  std::string current_ver = get_current_os_version();
  std::string slot_name = is_slot2_active() ? "Slot 2" : "Slot 1";
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
    if (!pkg.version.empty()) {
      lv_obj_t *badge = lv_obj_create(card);
      lv_obj_set_size(badge, LV_SIZE_CONTENT, 20);
      lv_obj_set_style_pad_hor(badge, 6, 0);
      lv_obj_set_style_pad_ver(badge, 1, 0);
      lv_obj_set_style_radius(badge, 4, 0);
      lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 24);

      if (pkg.version_diff > 0) {
        lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_GREEN), 0);
      } else if (pkg.version_diff == 0) {
        lv_obj_set_style_bg_color(badge, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
      } else {
        lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_ORANGE), 0);
      }

      lv_obj_t *badge_lbl = lv_label_create(badge);
      std::string b_text = "v" + pkg.version + " (" + pkg.status_badge + ")";
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

    // Flash / Install Button
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
    lv_label_set_text(btn_lbl, "Install");
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
    lv_msgbox_close(usb_detect_mbox);
    usb_detect_mbox = nullptr;
  }
}

void UpdatePanel::show_usb_detect_popup(const UpdatePackageItem &pkg) {
  close_usb_detect_popup();

  pending_usb_package = pkg;

  static const char *btns[] = {"View Update", "Dismiss", ""};
  std::string body = "Found firmware update package on USB:\n\n" +
                     pkg.file_name + " (" + pkg.file_size + ")\n\n"
                     "Open the System Update panel to view details and install?";

  usb_detect_mbox = lv_msgbox_create(NULL, "USB Firmware Update Detected",
                                     body.c_str(), btns, false);
  KUtils::style_lock_mbox(usb_detect_mbox, 90);

  auto cb = [](lv_event_t *e) {
    auto *self = static_cast<UpdatePanel*>(e->user_data);
    lv_obj_t *mbox = lv_obj_get_parent(lv_event_get_target(e));
    uint16_t btn_idx = lv_msgbox_get_active_btn(mbox);
    if (btn_idx == 0) { // "View Update"
      self->foreground();
    }
    self->usb_detect_mbox = nullptr;
    lv_msgbox_close(mbox);
  };

  lv_obj_add_event_cb(usb_detect_mbox, cb, LV_EVENT_VALUE_CHANGED, this);
}

void UpdatePanel::check_usb_auto_detect() {
  if (state != UpdateState::IDLE) return;
  if (KUtils::is_printing()) return;

  scan_updates();

  std::set<std::string> current_paths;
  for (const auto &p : found_packages) {
    current_paths.insert(p.file_path);
  }

  for (auto it = prompted_packages.begin(); it != prompted_packages.end(); ) {
    if (current_paths.find(*it) == current_paths.end()) {
      it = prompted_packages.erase(it);
    } else {
      ++it;
    }
  }

  if (usb_detect_mbox != nullptr &&
      current_paths.find(pending_usb_package.file_path) == current_paths.end()) {
    close_usb_detect_popup();
  }

  for (const auto &pkg : found_packages) {
    if (pkg.location_tag.find("USB") != std::string::npos) {
      if (prompted_packages.find(pkg.file_path) == prompted_packages.end()) {
        prompted_packages.insert(pkg.file_path);
        show_usb_detect_popup(pkg);
        break;
      }
    }
  }
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

  bool slot2_active = is_slot2_active();
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
  lv_label_set_text(title, "Confirm System Firmware Update");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *desc = lv_label_create(modal_cont);
  std::string current_ver = get_current_os_version();
  std::string pkg_ver_str = pkg.version.empty() ? "Unspecified" : ("v" + pkg.version + " (" + pkg.status_badge + ")");
  std::string info_text = "Package: " + pkg.file_name + " (" + pkg.file_size + ")\n"
                          "• Version: " + pkg_ver_str + "\n"
                          "• Installed: OpenKE v" + current_ver + " (" + active_slot + ")\n"
                          "• Target Slot: " + target_slot + "\n"
                          "• Preflight: Verifies hardware revision & SHA256 hashes\n"
                          "• Safety: Automatic backup of user config prior to write\n"
                          "• Reboot required upon completion.";
  if (printing) {
    info_text += "\n\n[LOCKED] Printer is currently active! Flashing is blocked.";
  }
  lv_label_set_text(desc, info_text.c_str());
  lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
  lv_obj_align(desc, LV_ALIGN_TOP_LEFT, 0, 30);

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
  lv_label_set_text(confirm_lbl, printing ? "Busy (Printing)" : "Flash Update");
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
  lv_label_set_text(title, "Flashing Firmware Update...");
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
  lv_label_set_text(status_label, "Initializing SWUpdate...");
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
  status_message = "Starting SWUpdate worker...";

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread(&UpdatePanel::execute_update_thread, this, pkg.file_path);
}

void UpdatePanel::execute_update_thread(std::string swu_path) {
  spdlog::info("SWUpdate worker executing for {}", swu_path);

  std::string target_slot = is_slot2_active() ? "slot1" : "slot2";
  std::string selection_arg = "stable," + target_slot;

  {
    std::lock_guard<std::mutex> lock(status_mutex);
    status_message = "Targeting inactive " + target_slot + "... invoking swupdate";
  }

  std::string log_file = "/tmp/swupdate.log";
  std::string cmd;
  if (fs::exists("/usr/bin/swupdate")) {
    cmd = "/usr/bin/swupdate -i '" + swu_path + "' -e '" + selection_arg + "' -v > " + log_file + " 2>&1";
  } else {
    cmd = "swupdate -i '" + swu_path + "' -e '" + selection_arg + "' -v > " + log_file + " 2>&1";
  }

  int rc = -1;
  try {
    auto p = sp::Popen(cmd, sp::shell{true});

    while (p.poll() == -1) {
      usleep(100000); // 100ms
      progress_percent = std::min(progress_percent + 2, 95);
    }

    rc = p.retcode();

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
        // Keep last 300 characters
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
    std::lock_guard<std::mutex> lock(status_mutex);
    status_message = std::string("Exception executing swupdate: ") + e.what();
    state = UpdateState::FAILED;
  }

  spdlog::info("SWUpdate worker finished with state {}", static_cast<int>(state.load()));
}

void UpdatePanel::timer_tick() {
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
