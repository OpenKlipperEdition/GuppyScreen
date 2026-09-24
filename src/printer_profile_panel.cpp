#include "printer_profile_panel.h"
#include "utils.h"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"

#include <algorithm>
#include <fstream>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;
namespace sp = subprocess;

LV_IMG_DECLARE(back);
LV_IMG_DECLARE(print);

static const char *DEFAULT_SEEDS_DIR = "/opt/openke-seeds/printer_profiles";
static const char *USER_PROFILES_DIR = "/usr/data/openke/printer_profiles";
static const char *ACTIVE_MARKER_PATH = "/usr/data/openke/system/active-profile.json";

PrinterProfilePanel::PrinterProfilePanel(KWebSocketClient &c)
  : ws(c)
  , cont(lv_obj_create(lv_scr_act()))
  , top_bar(lv_obj_create(cont))
  , title_label(lv_label_create(top_bar))
  , list_cont(lv_obj_create(cont))
  , back_btn(top_bar, &back, "Back", &PrinterProfilePanel::_handle_callback, this)
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

  lv_label_set_text(title_label, "Select Printer Model");
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
  lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_align(back_btn.get_container(), LV_ALIGN_RIGHT_MID, 0, 0);

  // Scrollable list container
  lv_obj_set_size(list_cont, LV_PCT(100), LV_PCT(82));
  lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list_cont, 4, 0);
  lv_obj_set_style_pad_gap(list_cont, 6, 0);
  lv_obj_add_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list_cont, 0, 0);
}

PrinterProfilePanel::~PrinterProfilePanel() {
  if (cont != nullptr) {
    lv_obj_del(cont);
    cont = nullptr;
  }
}

void PrinterProfilePanel::foreground() {
  refresh_profiles();
  build_profile_list();
  lv_obj_move_foreground(cont);
}

void PrinterProfilePanel::background() {
  lv_obj_move_background(cont);
}

void PrinterProfilePanel::handle_callback(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *target = lv_event_get_current_target(event);
    if (target == back_btn.get_container()) {
      background();
    }
  }
}

void PrinterProfilePanel::refresh_profiles() {
  profiles.clear();
  active_profile_id = "creality-ender3-v3-ke";

  // Read active marker if present
  if (fs::exists(ACTIVE_MARKER_PATH)) {
    try {
      std::ifstream f(ACTIVE_MARKER_PATH);
      json j;
      f >> j;
      if (j.contains("id") && j["id"].is_string()) {
        active_profile_id = j["id"].get<std::string>();
      }
    } catch (const std::exception &e) {
      spdlog::warn("Failed to read active profile marker: {}", e.what());
    }
  }

  std::vector<std::string> search_dirs;
  if (fs::exists(USER_PROFILES_DIR)) search_dirs.push_back(USER_PROFILES_DIR);

  if (fs::exists(DEFAULT_SEEDS_DIR)) search_dirs.push_back(DEFAULT_SEEDS_DIR);

  for (const auto &sdir : search_dirs) {
    try {
      for (const auto &entry : fs::directory_iterator(sdir)) {
        if (!fs::is_directory(entry.path())) continue;
        auto meta_path = entry.path() / "profile.json";
        auto cfg_path = entry.path() / "printer.cfg";
        if (fs::exists(meta_path) && fs::exists(cfg_path)) {
          try {
            std::ifstream mf(meta_path.string());
            json j;
            mf >> j;

            bool is_enabled = j.value("enabled", true) && !j.value("hidden", false);
            std::string pid = j.value("id", entry.path().filename().string());
            bool is_active = (pid == active_profile_id);
            if (!is_enabled && !is_active) {
              continue;
            }

            PrinterProfileItem item;
            item.id = pid;
            item.name = j.value("name", item.id);
            item.manufacturer = j.value("manufacturer", "Creality");
            item.kinematics = j.value("kinematics", "cartesian");
            item.description = j.value("description", "");
            item.probe = j.value("probe", "");
            item.mcu = j.value("mcu", "");
            item.path = entry.path().string();
            item.is_active = is_active;

            // Check machinephoto.png
            auto photo = entry.path() / "machinephoto.png";
            if (fs::exists(photo)) {
              item.photo_path = photo.string();
            } else {
              auto seed_base = DEFAULT_SEEDS_DIR;
              auto seed_photo = fs::path(seed_base) / pid / "machinephoto.png";
              if (fs::exists(seed_photo)) {
                item.photo_path = seed_photo.string();
              }
            }

            if (j.contains("bed_size") && j["bed_size"].is_array()) {
              for (const auto &dim : j["bed_size"]) {
                if (dim.is_number()) item.bed_size.push_back(dim.get<int>());
              }
            }

            // Deduplicate by ID
            bool exists = false;
            for (const auto &p : profiles) {
              if (p.id == item.id) { exists = true; break; }
            }
            if (!exists) {
              profiles.push_back(item);
            }
          } catch (const std::exception &e) {
            spdlog::warn("Error parsing profile {}: {}", meta_path.string(), e.what());
          }
        }
      }
    } catch (const std::exception &e) {
      spdlog::warn("Error scanning profiles in {}: {}", sdir, e.what());
    }
  }

  // Sort: Active profile first, then alphabetically by name
  std::sort(profiles.begin(), profiles.end(), [](const PrinterProfileItem &a, const PrinterProfileItem &b) {
    if (a.is_active != b.is_active) return a.is_active > b.is_active;
    return a.name < b.name;
  });
}

void PrinterProfilePanel::build_profile_list() {
  lv_obj_clean(list_cont);

  if (profiles.empty()) {
    lv_obj_t *empty_lbl = lv_label_create(list_cont);
    lv_label_set_text(empty_lbl, "No printer profiles found");
    lv_obj_center(empty_lbl);
    return;
  }

  for (size_t i = 0; i < profiles.size(); ++i) {
    const auto &prof = profiles[i];

    lv_obj_t *card = lv_obj_create(list_cont);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (prof.is_active) {
      lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    } else {
      lv_obj_set_style_border_color(card, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
      lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    }

    // Left thumbnail image container
    lv_obj_t *img_cont = lv_obj_create(card);
    lv_obj_set_size(img_cont, 56, 56);
    lv_obj_set_style_radius(img_cont, 6, 0);
    lv_obj_set_style_pad_all(img_cont, 2, 0);
    lv_obj_set_style_bg_color(img_cont, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_border_width(img_cont, 0, 0);
    lv_obj_clear_flag(img_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_img_create(img_cont);
    if (!prof.photo_path.empty()) {
      std::string src = "A:" + prof.photo_path;
      lv_img_set_src(img, src.c_str());
    } else {
      lv_img_set_src(img, &print);
    }
    lv_obj_center(img);

    // Middle container for text details
    lv_obj_t *text_cont = lv_obj_create(card);
    lv_obj_set_flex_grow(text_cont, 1);
    lv_obj_set_height(text_cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(text_cont, 8, 0);
    lv_obj_set_style_pad_top(text_cont, 0, 0);
    lv_obj_set_style_pad_right(text_cont, 4, 0);
    lv_obj_set_style_pad_bottom(text_cont, 0, 0);
    lv_obj_set_style_bg_opa(text_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_cont, 0, 0);
    lv_obj_clear_flag(text_cont, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    lv_obj_t *name_lbl = lv_label_create(text_cont);
    lv_label_set_text(name_lbl, prof.name.c_str());
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    if (prof.is_active) {
      lv_obj_set_style_text_color(name_lbl, lv_palette_lighten(LV_PALETTE_GREEN, 1), 0);
    }

    // Subtitle / Specs label
    std::string specs;
    if (prof.bed_size.size() >= 3) {
      specs += fmt::format("Bed: {}x{}x{}mm", prof.bed_size[0], prof.bed_size[1], prof.bed_size[2]);
    }
    if (!prof.probe.empty()) {
      specs += " • Probe: " + prof.probe;
    }
    if (!prof.mcu.empty()) {
      specs += " • MCU: " + prof.mcu;
    }

    lv_obj_t *specs_lbl = lv_label_create(text_cont);
    lv_label_set_text(specs_lbl, specs.c_str());
    lv_obj_set_style_text_font(specs_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(specs_lbl, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);

    // Right container / badge
    if (prof.is_active) {
      lv_obj_t *badge = lv_btn_create(card);
      lv_obj_set_size(badge, LV_SIZE_CONTENT, 30);
      lv_obj_set_style_radius(badge, 15, 0);
      lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *b_lbl = lv_label_create(badge);
      lv_label_set_text(b_lbl, "ACTIVE");
      lv_obj_set_style_text_font(b_lbl, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(b_lbl, lv_color_white(), 0);
      lv_obj_center(b_lbl);
    } else {
      lv_obj_t *switch_btn = lv_btn_create(card);
      lv_obj_set_size(switch_btn, LV_SIZE_CONTENT, 30);
      lv_obj_set_style_radius(switch_btn, 15, 0);
      lv_obj_set_style_bg_color(switch_btn, lv_palette_main(LV_PALETTE_BLUE), 0);

      lv_obj_t *s_lbl = lv_label_create(switch_btn);
      lv_label_set_text(s_lbl, "Select");
      lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
      lv_obj_center(s_lbl);

      auto click_handler = [](lv_event_t *e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        auto *panel = static_cast<PrinterProfilePanel*>(e->user_data);
        size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
        if (idx < panel->profiles.size()) {
          panel->show_switch_confirm(panel->profiles[idx]);
        }
      };

      lv_obj_set_user_data(switch_btn, (void*)(uintptr_t)i);
      lv_obj_set_user_data(card, (void*)(uintptr_t)i);
      lv_obj_add_event_cb(switch_btn, click_handler, LV_EVENT_CLICKED, this);
      lv_obj_add_event_cb(card, click_handler, LV_EVENT_CLICKED, this);
    }
  }
}

void PrinterProfilePanel::show_switch_confirm(const PrinterProfileItem &profile) {
  pending_profile = profile;
  static const char *btns[] = {"Cancel", "Apply & Restart", ""};

  // Find current active profile MCU info
  std::string active_mcu = "";
  for (const auto &p : profiles) {
    if (p.is_active) {
      active_mcu = p.mcu;
      break;
    }
  }

  bool is_mcu_change = (!active_mcu.empty() && !profile.mcu.empty() && active_mcu != profile.mcu)
                       || (active_profile_id == "creality-ender3-v3-ke" && profile.id != "creality-ender3-v3-ke")
                       || (active_profile_id != "creality-ender3-v3-ke" && profile.id == "creality-ender3-v3-ke");

  std::string mcu_warning = "";
  if (is_mcu_change) {
    std::string fw_info;
    if (profile.id == "creality-ender3-v3-se") {
      fw_info = "Flash #FFA726 Ender3V3SE_klipper.bin# via SD card.";
    } else if (profile.id == "creality-ender3-v2-neo") {
      fw_info = "Flash #FFA726 Ender3V2Neo_klipper.bin# via SD card.";
    } else if (profile.id == "creality-ender3-v3-ke") {
      fw_info = "Built-in firmware (auto-upgraded).";
    } else {
      fw_info = fmt::format("Requires flashing matching {} firmware.", profile.mcu);
    }
    mcu_warning = fmt::format(
      "\n\n#FFA726 ⚠ MCU FIRMWARE NOTICE:#\n"
      "Mainboard MCU changed to {}\n{}",
      profile.mcu, fw_info
    );
  }

  std::string msg = fmt::format(
    "Switch printer profile to\n#2196F3 {}# ?\n\n"
    "This applies the printer.cfg for this model\n"
    "and restarts Klipper services.{}\n\n"
    "Existing configuration will be backed up.",
    profile.name,
    mcu_warning
  );

  lv_obj_t *mbox = lv_msgbox_create(NULL, NULL, msg.c_str(), btns, false);
  KUtils::style_dialog_msgbox(mbox);

  lv_obj_t *msg_obj = ((lv_msgbox_t *)mbox)->text;
  lv_obj_set_style_text_align(msg_obj, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_recolor(msg_obj, true);
  lv_obj_set_width(msg_obj, LV_PCT(100));
  lv_obj_center(msg_obj);

  lv_obj_t *btnm = lv_msgbox_get_btns(mbox);
  lv_btnmatrix_set_btn_ctrl(btnm, 0, LV_BTNMATRIX_CTRL_CHECKED);
  lv_btnmatrix_set_btn_ctrl(btnm, 1, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);

  auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;
  lv_obj_set_size(btnm, LV_PCT(90), 50 * hscale);
  lv_obj_set_size(mbox, LV_PCT(85), is_mcu_change ? LV_PCT(78) : LV_PCT(65));

  lv_obj_add_event_cb(btnm, [](lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part == LV_PART_ITEMS && dsc->id == 1) {
      dsc->rect_dsc->bg_color = lv_color_hex(0x1976D2);
      dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
      dsc->label_dsc->color   = lv_color_white();
    }
  }, LV_EVENT_DRAW_PART_BEGIN, NULL);

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *panel = static_cast<PrinterProfilePanel*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      panel->select_profile(panel->pending_profile);
    }
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_center(mbox);
}

void PrinterProfilePanel::select_profile(const PrinterProfileItem &profile) {
  spdlog::info("Applying printer profile: {} ({})", profile.name, profile.id);

  std::string cmd = fmt::format("/usr/bin/openke-profile set {} --restart", profile.id);
  int rc = sp::call(cmd);
  if (rc != 0) {
    spdlog::warn("openke-profile set exited with code {}", rc);
    static const char *err_btns[] = {"OK", ""};
    std::string err_msg = fmt::format(
      "#FF5252 Cannot Switch Profile#\n\n"
      "Profile change was blocked.\n"
      "Ensure no print job is currently running or paused."
    );
    lv_obj_t *mbox = lv_msgbox_create(NULL, NULL, err_msg.c_str(), err_btns, false);
    KUtils::style_dialog_msgbox(mbox);
    lv_obj_t *msg_obj = ((lv_msgbox_t *)mbox)->text;
    lv_obj_set_style_text_align(msg_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_recolor(msg_obj, true);
    lv_obj_set_width(msg_obj, LV_PCT(100));
    lv_obj_center(msg_obj);
    lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
      lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
      lv_msgbox_close(obj);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
    return;
  }

  // Request firmware / klipper restart via Moonraker
  ws.send_jsonrpc("printer.restart");

  // Refresh GUI state
  refresh_profiles();
  build_profile_list();
}
