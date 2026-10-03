#include "appstore_panel.h"
#include "utils.h"
#include "state.h"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;
namespace sp = subprocess;

LV_IMG_DECLARE(back);
LV_IMG_DECLARE(refresh_img);
LV_IMG_DECLARE(layers_img);
LV_IMG_DECLARE(spoolman_img);
LV_IMG_DECLARE(clock_img);
LV_IMG_DECLARE(network_img);
LV_IMG_DECLARE(fine_tune_img);
LV_IMG_DECLARE(power_devices_img);
LV_IMG_DECLARE(print);

static const void *get_app_icon(const AppItem &app) {
  const std::string &icon = app.icon;
  const std::string &id = app.id;
  const std::string &cat = app.category;

  if (icon == "spoolman" || id == "spoolman" || icon == "spool") {
    return &spoolman_img;
  }
  if (icon == "timelapse" || id == "timelapse" || icon == "clock" || icon == "camera") {
    return &clock_img;
  }
  if (icon == "mobileraker" || id == "mobileraker" || icon == "mobile" || icon == "bell") {
    return &network_img;
  }
  if (icon == "mainsail" || id == "mainsail") {
    return &network_img;
  }
  if (icon == "fluidd" || id == "fluidd") {
    return &fine_tune_img;
  }
  if (icon == "guppyscreen" || id == "guppyscreen") {
    return &print;
  }
  if (icon == "helixscreen" || id == "helixscreen") {
    return &layers_img;
  }

  // Generic category fallbacks
  if (cat == "web_ui") {
    return &network_img;
  }
  if (cat == "touch_ui") {
    return &layers_img;
  }
  if (cat == "plugin") {
    return &power_devices_img;
  }
  if (cat == "tool") {
    return &fine_tune_img;
  }

  return &layers_img;
}

static const char *DEFAULT_CATALOG_PATHS[] = {
  "/usr/data/openke/apps.json",
  "/usr/data/openke-seeds/apps.json",
  "/opt/openke-seeds/apps.json",
  "/opt/openke/apps.json",
  "/etc/openke/apps.json",
  "/etc/openke-apps.json",
  nullptr
};

AppStorePanel::AppStorePanel(KWebSocketClient &c)
  : ws(c)
  , cont(lv_obj_create(lv_scr_act()))
  , top_bar(lv_obj_create(cont))
  , title_label(lv_label_create(top_bar))
  , category_cont(lv_obj_create(cont))
  , list_cont(lv_obj_create(cont))
  , back_btn(top_bar, &back, "Back", &AppStorePanel::_handle_callback, this)
  , refresh_btn(top_bar, &refresh_img, "Refresh", &AppStorePanel::_handle_callback, this)
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

  lv_label_set_text(title_label, "OpenKE App Store");
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
  lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_align(refresh_btn.get_container(), LV_ALIGN_RIGHT_MID, -70, 0);
  lv_obj_align(back_btn.get_container(), LV_ALIGN_RIGHT_MID, 0, 0);

  // Category filter row
  lv_obj_clear_flag(category_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(category_cont, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(category_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(category_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(category_cont, 2, 0);
  lv_obj_set_style_pad_gap(category_cont, 6, 0);
  lv_obj_set_style_bg_opa(category_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(category_cont, 0, 0);

  build_category_bar();

  // Scrollable list container
  lv_obj_set_width(list_cont, LV_PCT(100));
  lv_obj_set_flex_grow(list_cont, 1);
  lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list_cont, 4, 0);
  lv_obj_set_style_pad_gap(list_cont, 6, 0);
  lv_obj_add_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list_cont, 0, 0);

  poll_timer = lv_timer_create(AppStorePanel::_timer_callback, 300, this);
}

AppStorePanel::~AppStorePanel() {
  if (poll_timer != nullptr) {
    lv_timer_del(poll_timer);
    poll_timer = nullptr;
  }
  close_modal();
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
  if (cont != nullptr) {
    lv_obj_del(cont);
    cont = nullptr;
  }
}

void AppStorePanel::foreground() {
  refresh_apps();
  build_app_list();
  lv_obj_move_foreground(cont);
}

void AppStorePanel::background() {
  lv_obj_move_background(cont);
}

void AppStorePanel::handle_callback(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_obj_t *target = lv_event_get_current_target(event);
    if (target == back_btn.get_container()) {
      background();
    } else if (target == refresh_btn.get_container()) {
      execute_refresh();
    }
  }
}

void AppStorePanel::build_category_bar() {
  lv_obj_clean(category_cont);

  struct CatBtn {
    const char *id;
    const char *label;
  };

  static const CatBtn categories[] = {
    {"all", "All Apps"},
    {"web_ui", "Web UIs"},
    {"touch_ui", "Touch UIs"},
    {"plugin", "Plugins"},
    {"tool", "Tools"}
  };

  for (const auto &cat : categories) {
    lv_obj_t *btn = lv_btn_create(category_cont);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_pad_left(btn, 12, 0);
    lv_obj_set_style_pad_right(btn, 12, 0);
    lv_obj_set_style_pad_top(btn, 4, 0);
    lv_obj_set_style_pad_bottom(btn, 4, 0);

    bool is_sel = (current_category == cat.id);
    if (is_sel) {
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    } else {
      lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, cat.label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    auto cb = [](lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      auto *panel = static_cast<AppStorePanel*>(e->user_data);
      const char *cat_id = static_cast<const char*>(lv_obj_get_user_data(lv_event_get_current_target(e)));
      if (cat_id) {
        panel->filter_category(cat_id);
      }
    };

    lv_obj_set_user_data(btn, (void*)cat.id);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
  }
}

void AppStorePanel::filter_category(const std::string &category) {
  current_category = category;
  build_category_bar();
  build_app_list();
}

void AppStorePanel::refresh_apps() {
  apps.clear();

  // Try running `openke-app list --json`
  bool loaded_via_cli = false;
  try {
    auto p = sp::Popen({"/usr/bin/openke-app", "list", "--json"}, sp::output{sp::PIPE}, sp::error{sp::PIPE});
    auto res = p.communicate();
    std::string out_str;
    if (res.first.length > 0 && res.first.buf.data() != nullptr) {
      out_str = std::string(res.first.buf.data(), res.first.length);
    }
    if (p.retcode() == 0 && !out_str.empty()) {
      json j = json::parse(out_str);
      if (j.is_array()) {
        for (const auto &item : j) {
          AppItem app;
          app.id = item.value("id", "");
          app.name = item.value("name", app.id);
          app.category = item.value("category", "");
          app.icon = item.value("icon", "");
          app.version = item.value("version", "");
          app.author = item.value("author", "");
          app.description = item.value("description", "");
          app.is_builtin = item.value("is_builtin", false);
          app.is_installed = item.value("is_installed", false);
          app.is_active = item.value("is_active", false);
          app.installed_version = item.value("installed_version", "");
          app.has_update = item.value("has_update", false);
          app.has_service = item.value("has_service", false);
          app.service_enabled = item.value("service_enabled", true);
          app.service_status = item.value("service_status", "");
          app.status = item.value("status", "available");
          apps.push_back(app);
        }
        loaded_via_cli = true;
      }
    }
  } catch (const std::exception &e) {
    spdlog::warn("AppStore: openke-app execution failed: {}", e.what());
  }

  // Fallback direct JSON file load if CLI is unavailable
  if (!loaded_via_cli) {
    for (int i = 0; DEFAULT_CATALOG_PATHS[i] != nullptr; ++i) {
      const char *path = DEFAULT_CATALOG_PATHS[i];
      if (fs::exists(path)) {
        try {
          std::ifstream f(path);
          json j;
          f >> j;
          if (j.contains("apps") && j["apps"].is_array()) {
            for (const auto &item : j["apps"]) {
              AppItem app;
              app.id = item.value("id", "");
              app.name = item.value("name", app.id);
              app.category = item.value("category", "");
              app.icon = item.value("icon", "");
              app.version = item.value("version", "");
              app.installed_version = item.value("installed_version", "");
              app.author = item.value("author", "");
              app.description = item.value("description", "");
              app.is_builtin = item.value("is_builtin", false);
              app.is_installed = app.is_builtin;
              app.is_active = (app.id == "mainsail" || app.id == "guppyscreen");
              app.has_update = false;
              app.has_service = item.contains("service") || item.contains("service_init");
              app.service_enabled = true;
              app.service_status = app.is_installed && app.has_service ? "running" : (app.has_service ? "stopped" : "");
              app.status = app.is_active ? "active" : (app.is_installed ? "installed" : "available");
              apps.push_back(app);
            }
            break;
          }
        } catch (const std::exception &e) {
          spdlog::warn("AppStore: Failed reading fallback catalog {}: {}", path, e.what());
        }
      }
    }
  }

  // Sort: Updates first, then active, then installed, then alphabetical
  std::sort(apps.begin(), apps.end(), [](const AppItem &a, const AppItem &b) {
    if (a.has_update != b.has_update) return a.has_update > b.has_update;
    if (a.is_active != b.is_active) return a.is_active > b.is_active;
    if (a.is_installed != b.is_installed) return a.is_installed > b.is_installed;
    return a.name < b.name;
  });
}

void AppStorePanel::build_app_list() {
  lv_obj_clean(list_cont);

  std::vector<size_t> visible_indices;
  for (size_t i = 0; i < apps.size(); ++i) {
    if (current_category == "all" || apps[i].category == current_category) {
      visible_indices.push_back(i);
    }
  }

  if (visible_indices.empty()) {
    lv_obj_t *empty_lbl = lv_label_create(list_cont);
    lv_label_set_text(empty_lbl, "No apps found in this category.");
    lv_obj_center(empty_lbl);
    return;
  }

  for (size_t idx : visible_indices) {
    const auto &app = apps[idx];

    lv_obj_t *card = lv_obj_create(list_cont);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (app.has_update) {
      lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_ORANGE), 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    } else if (app.is_active) {
      lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    } else if (app.is_installed) {
      lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_BLUE), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    } else {
      lv_obj_set_style_border_color(card, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    }

    // Left icon container with category-themed subtle background
    lv_obj_t *icon_cont = lv_obj_create(card);
    lv_obj_set_size(icon_cont, 48, 48);
    lv_obj_set_style_radius(icon_cont, 8, 0);
    lv_obj_set_style_pad_all(icon_cont, 2, 0);
    if (app.category == "web_ui") {
      lv_obj_set_style_bg_color(icon_cont, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
    } else if (app.category == "touch_ui") {
      lv_obj_set_style_bg_color(icon_cont, lv_palette_darken(LV_PALETTE_PURPLE, 3), 0);
    } else if (app.category == "plugin") {
      lv_obj_set_style_bg_color(icon_cont, lv_palette_darken(LV_PALETTE_TEAL, 3), 0);
    } else {
      lv_obj_set_style_bg_color(icon_cont, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    }
    lv_obj_set_style_border_width(icon_cont, 0, 0);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_img = lv_img_create(icon_cont);
    lv_img_set_src(icon_img, get_app_icon(app));
    lv_obj_center(icon_img);

    // Middle text container
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
    lv_label_set_text(name_lbl, app.name.c_str());
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    if (app.has_update) {
      lv_obj_set_style_text_color(name_lbl, lv_palette_lighten(LV_PALETTE_ORANGE, 1), 0);
    } else if (app.is_active) {
      lv_obj_set_style_text_color(name_lbl, lv_palette_lighten(LV_PALETTE_GREEN, 1), 0);
    }

    // Subtitle (category, version, update status, author, service status)
    std::string svc_badge;
    if (app.is_installed && app.has_service) {
      if (!app.service_enabled) {
        svc_badge = " • #9E9E9E Service: Disabled#";
      } else if (app.service_status == "running") {
        svc_badge = " • #4CAF50 Service: Running#";
      } else {
        svc_badge = " • #FFA726 Service: Stopped#";
      }
    }

    std::string meta;
    if (app.has_update) {
      meta = fmt::format("{} • #FFA726 Update Available: v{} (installed: v{})#{}{} • {}",
                         app.category, app.version, app.installed_version.empty() ? "?" : app.installed_version,
                         svc_badge, app.author);
    } else {
      meta = fmt::format("{} • v{}{}{}", app.category, app.version, svc_badge, app.author.empty() ? "" : (" • " + app.author));
    }
    lv_obj_t *meta_lbl = lv_label_create(text_cont);
    lv_label_set_text(meta_lbl, meta.c_str());
    lv_label_set_recolor(meta_lbl, true);
    lv_obj_set_style_text_font(meta_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(meta_lbl, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);

    // Description
    if (!app.description.empty()) {
      lv_obj_t *desc_lbl = lv_label_create(text_cont);
      lv_label_set_text(desc_lbl, app.description.c_str());
      lv_obj_set_style_text_font(desc_lbl, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(desc_lbl, lv_palette_main(LV_PALETTE_GREY), 0);
    }

    // Right Action Buttons Container
    lv_obj_t *action_cont = lv_obj_create(card);
    lv_obj_set_size(action_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(action_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(action_cont, 0, 0);
    lv_obj_set_style_pad_gap(action_cont, 6, 0);
    lv_obj_set_style_bg_opa(action_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_cont, 0, 0);
    lv_obj_clear_flag(action_cont, LV_OBJ_FLAG_SCROLLABLE);

    auto upd_handler = [](lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      auto *panel = static_cast<AppStorePanel*>(e->user_data);
      size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
      if (i < panel->apps.size()) {
        panel->show_update_confirm(panel->apps[i]);
      }
    };

    if (app.is_active) {
      lv_obj_t *badge = lv_btn_create(action_cont);
      lv_obj_set_size(badge, LV_SIZE_CONTENT, 30);
      lv_obj_set_style_radius(badge, 15, 0);
      lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *b_lbl = lv_label_create(badge);
      lv_label_set_text(b_lbl, "ACTIVE");
      lv_obj_set_style_text_font(b_lbl, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(b_lbl, lv_color_white(), 0);
      lv_obj_center(b_lbl);

      if (app.has_update) {
        lv_obj_t *upd_btn = lv_btn_create(action_cont);
        lv_obj_set_size(upd_btn, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_radius(upd_btn, 15, 0);
        lv_obj_set_style_bg_color(upd_btn, lv_palette_main(LV_PALETTE_ORANGE), 0);

        lv_obj_t *upd_lbl = lv_label_create(upd_btn);
        lv_label_set_text(upd_lbl, "Update");
        lv_obj_set_style_text_font(upd_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(upd_lbl, lv_color_white(), 0);
        lv_obj_center(upd_lbl);

        lv_obj_set_user_data(upd_btn, (void*)(uintptr_t)idx);
        lv_obj_add_event_cb(upd_btn, upd_handler, LV_EVENT_CLICKED, this);
      }
    } else if (app.is_installed) {
      if (app.has_service) {
        if (!app.service_enabled) {
          lv_obj_t *en_btn = lv_btn_create(action_cont);
          lv_obj_set_size(en_btn, LV_SIZE_CONTENT, 30);
          lv_obj_set_style_radius(en_btn, 15, 0);
          lv_obj_set_style_bg_color(en_btn, lv_palette_main(LV_PALETTE_GREEN), 0);

          lv_obj_t *en_lbl = lv_label_create(en_btn);
          lv_label_set_text(en_lbl, "Enable");
          lv_obj_set_style_text_font(en_lbl, &lv_font_montserrat_12, 0);
          lv_obj_set_style_text_color(en_lbl, lv_color_white(), 0);
          lv_obj_center(en_lbl);

          auto en_handler = [](lv_event_t *e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            auto *panel = static_cast<AppStorePanel*>(e->user_data);
            size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
            if (i < panel->apps.size()) {
              panel->show_service_confirm(panel->apps[i], "enable");
            }
          };

          lv_obj_set_user_data(en_btn, (void*)(uintptr_t)idx);
          lv_obj_add_event_cb(en_btn, en_handler, LV_EVENT_CLICKED, this);
        } else {
          if (app.service_status == "running") {
            lv_obj_t *stop_btn = lv_btn_create(action_cont);
            lv_obj_set_size(stop_btn, LV_SIZE_CONTENT, 30);
            lv_obj_set_style_radius(stop_btn, 15, 0);
            lv_obj_set_style_bg_color(stop_btn, lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);

            lv_obj_t *stop_lbl = lv_label_create(stop_btn);
            lv_label_set_text(stop_lbl, "Stop");
            lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
            lv_obj_center(stop_lbl);

            auto stop_handler = [](lv_event_t *e) {
              if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
              auto *panel = static_cast<AppStorePanel*>(e->user_data);
              size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
              if (i < panel->apps.size()) {
                panel->show_service_confirm(panel->apps[i], "stop");
              }
            };

            lv_obj_set_user_data(stop_btn, (void*)(uintptr_t)idx);
            lv_obj_add_event_cb(stop_btn, stop_handler, LV_EVENT_CLICKED, this);
          } else {
            lv_obj_t *start_btn = lv_btn_create(action_cont);
            lv_obj_set_size(start_btn, LV_SIZE_CONTENT, 30);
            lv_obj_set_style_radius(start_btn, 15, 0);
            lv_obj_set_style_bg_color(start_btn, lv_palette_main(LV_PALETTE_GREEN), 0);

            lv_obj_t *start_lbl = lv_label_create(start_btn);
            lv_label_set_text(start_lbl, "Start");
            lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
            lv_obj_center(start_lbl);

            auto start_handler = [](lv_event_t *e) {
              if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
              auto *panel = static_cast<AppStorePanel*>(e->user_data);
              size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
              if (i < panel->apps.size()) {
                panel->show_service_confirm(panel->apps[i], "start");
              }
            };

            lv_obj_set_user_data(start_btn, (void*)(uintptr_t)idx);
            lv_obj_add_event_cb(start_btn, start_handler, LV_EVENT_CLICKED, this);
          }

          lv_obj_t *dis_btn = lv_btn_create(action_cont);
          lv_obj_set_size(dis_btn, LV_SIZE_CONTENT, 30);
          lv_obj_set_style_radius(dis_btn, 15, 0);
          lv_obj_set_style_bg_color(dis_btn, lv_palette_darken(LV_PALETTE_GREY, 3), 0);

          lv_obj_t *dis_lbl = lv_label_create(dis_btn);
          lv_label_set_text(dis_lbl, "Disable");
          lv_obj_set_style_text_font(dis_lbl, &lv_font_montserrat_12, 0);
          lv_obj_set_style_text_color(dis_lbl, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
          lv_obj_center(dis_lbl);

          auto dis_handler = [](lv_event_t *e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            auto *panel = static_cast<AppStorePanel*>(e->user_data);
            size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
            if (i < panel->apps.size()) {
              panel->show_service_confirm(panel->apps[i], "disable");
            }
          };

          lv_obj_set_user_data(dis_btn, (void*)(uintptr_t)idx);
          lv_obj_add_event_cb(dis_btn, dis_handler, LV_EVENT_CLICKED, this);
        }
      }

      if (app.has_update) {
        lv_obj_t *upd_btn = lv_btn_create(action_cont);
        lv_obj_set_size(upd_btn, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_radius(upd_btn, 15, 0);
        lv_obj_set_style_bg_color(upd_btn, lv_palette_main(LV_PALETTE_ORANGE), 0);

        lv_obj_t *upd_lbl = lv_label_create(upd_btn);
        lv_label_set_text(upd_lbl, "Update");
        lv_obj_set_style_text_font(upd_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(upd_lbl, lv_color_white(), 0);
        lv_obj_center(upd_lbl);

        lv_obj_set_user_data(upd_btn, (void*)(uintptr_t)idx);
        lv_obj_add_event_cb(upd_btn, upd_handler, LV_EVENT_CLICKED, this);
      }

      if (app.category == "web_ui" || app.category == "touch_ui") {
        lv_obj_t *act_btn = lv_btn_create(action_cont);
        lv_obj_set_size(act_btn, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_radius(act_btn, 15, 0);
        lv_obj_set_style_bg_color(act_btn, lv_palette_main(LV_PALETTE_BLUE), 0);

        lv_obj_t *act_lbl = lv_label_create(act_btn);
        lv_label_set_text(act_lbl, "Activate");
        lv_obj_set_style_text_font(act_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(act_lbl, lv_color_white(), 0);
        lv_obj_center(act_lbl);

        auto act_handler = [](lv_event_t *e) {
          if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
          auto *panel = static_cast<AppStorePanel*>(e->user_data);
          size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
          if (i < panel->apps.size()) {
            panel->show_activate_confirm(panel->apps[i]);
          }
        };

        lv_obj_set_user_data(act_btn, (void*)(uintptr_t)idx);
        lv_obj_add_event_cb(act_btn, act_handler, LV_EVENT_CLICKED, this);
      }

      if (!app.is_builtin) {
        lv_obj_t *rm_btn = lv_btn_create(action_cont);
        lv_obj_set_size(rm_btn, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_radius(rm_btn, 15, 0);
        lv_obj_set_style_bg_color(rm_btn, lv_palette_main(LV_PALETTE_RED), 0);

        lv_obj_t *rm_lbl = lv_label_create(rm_btn);
        lv_label_set_text(rm_lbl, "Remove");
        lv_obj_set_style_text_font(rm_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rm_lbl, lv_color_white(), 0);
        lv_obj_center(rm_lbl);

        auto rm_handler = [](lv_event_t *e) {
          if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
          auto *panel = static_cast<AppStorePanel*>(e->user_data);
          size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
          if (i < panel->apps.size()) {
            panel->show_remove_confirm(panel->apps[i]);
          }
        };

        lv_obj_set_user_data(rm_btn, (void*)(uintptr_t)idx);
        lv_obj_add_event_cb(rm_btn, rm_handler, LV_EVENT_CLICKED, this);
      }
    } else {
      // Uninstalled app: Show Install Button
      lv_obj_t *inst_btn = lv_btn_create(action_cont);
      lv_obj_set_size(inst_btn, LV_SIZE_CONTENT, 30);
      lv_obj_set_style_radius(inst_btn, 15, 0);
      lv_obj_set_style_bg_color(inst_btn, lv_palette_main(LV_PALETTE_BLUE), 0);

      lv_obj_t *inst_lbl = lv_label_create(inst_btn);
      lv_label_set_text(inst_lbl, "Install");
      lv_obj_set_style_text_font(inst_lbl, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(inst_lbl, lv_color_white(), 0);
      lv_obj_center(inst_lbl);

      auto inst_handler = [](lv_event_t *e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        auto *panel = static_cast<AppStorePanel*>(e->user_data);
        size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
        if (i < panel->apps.size()) {
          panel->show_install_confirm(panel->apps[i]);
        }
      };

      lv_obj_set_user_data(inst_btn, (void*)(uintptr_t)idx);
      lv_obj_add_event_cb(inst_btn, inst_handler, LV_EVENT_CLICKED, this);
    }
  }
}

void AppStorePanel::show_install_confirm(const AppItem &app) {
  if (is_busy.load()) return;
  pending_app = app;

  static const char *btns[] = {"Cancel", "Install", ""};
  std::string msg = fmt::format("Install\n#2196F3 {}# (v{}) ?\n\n{}", app.name, app.version, app.description);

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
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(65));

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *panel = static_cast<AppStorePanel*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      panel->execute_install(panel->pending_app);
    }
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_center(mbox);
}

void AppStorePanel::show_update_confirm(const AppItem &app) {
  if (is_busy.load()) return;
  pending_app = app;

  static const char *btns[] = {"Cancel", "Update", ""};
  std::string msg = fmt::format("Update\n#FFA726 {}#\nfrom #BDBDBD v{}# -> #4CAF50 v{}# ?\n\n{}",
                                app.name, app.installed_version.empty() ? "?" : app.installed_version, app.version, app.description);

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
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(65));

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *panel = static_cast<AppStorePanel*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      panel->execute_update(panel->pending_app);
    }
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_center(mbox);
}

void AppStorePanel::show_activate_confirm(const AppItem &app) {
  if (is_busy.load()) return;

  // 1. Printing safeguard (Web UI and Touch UI)
  if (KUtils::is_printing()) {
    show_alert("Cannot Switch Interface",
               "#FF5252 Print In Progress#\n\nA 3D print job is currently active or paused.\n\nPlease finish or cancel the print before switching interfaces.");
    return;
  }

  // 2. Thermal & Heater safeguard (Touch UI reload protection)
  auto s = State::get_instance();
  auto etarget_j = s->get_data("/printer_state/extruder/target"_json_pointer);
  auto etemp_j   = s->get_data("/printer_state/extruder/temperature"_json_pointer);
  auto btarget_j = s->get_data("/printer_state/heater_bed/target"_json_pointer);
  auto btemp_j   = s->get_data("/printer_state/heater_bed/temperature"_json_pointer);

  double etarget = etarget_j.is_number() ? etarget_j.get<double>() : 0.0;
  double etemp   = etemp_j.is_number()   ? etemp_j.get<double>()   : 0.0;
  double btarget = btarget_j.is_number() ? btarget_j.get<double>() : 0.0;
  double btemp   = btemp_j.is_number()   ? btemp_j.get<double>()   : 0.0;

  if (app.category == "touch_ui" && (etarget > 0.0 || btarget > 0.0)) {
    std::string h_info;
    if (etarget > 0.0 && btarget > 0.0) {
      h_info = fmt::format("Hotend: {:.0f}/{:.0f}°C\nBed: {:.0f}/{:.0f}°C", etemp, etarget, btemp, btarget);
    } else if (etarget > 0.0) {
      h_info = fmt::format("Hotend heating: {:.0f}/{:.0f}°C", etemp, etarget);
    } else {
      h_info = fmt::format("Bed heating: {:.0f}/{:.0f}°C", btemp, btarget);
    }
    show_alert("Heaters Currently Active",
               fmt::format("{}\n\n#FFA726 Thermal Safety:# Switching Touchscreen UI reloads the display service.\n\nPlease turn off heaters before switching.", h_info));
    return;
  }

  pending_app = app;

  static const char *btns[] = {"Cancel", "Activate", ""};
  std::string warning = "";
  if (app.category == "touch_ui") {
    warning = "\n\n#FFA726 Note:# Screen will reload to the selected Touch UI.";
  }

  std::string msg = fmt::format("Switch active {} to\n#2196F3 {}# ?{}",
                                app.category == "web_ui" ? "Web UI" : "Touchscreen UI",
                                app.name, warning);

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
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(65));

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *panel = static_cast<AppStorePanel*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      panel->execute_activate(panel->pending_app);
    }
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_center(mbox);
}

void AppStorePanel::show_remove_confirm(const AppItem &app) {
  if (is_busy.load()) return;

  if (app.is_active && KUtils::is_printing()) {
    show_alert("Cannot Remove Active UI",
               "#FF5252 Print In Progress#\n\nA 3D print job is currently active.\n\nPlease finish or cancel the print before removing the active interface.");
    return;
  }

  pending_app = app;

  static const char *btns[] = {"Cancel", "Remove", ""};
  std::string msg = fmt::format("Uninstall and remove\n#FF5252 {}# ?\n\nStored data will be deleted.", app.name);

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
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(65));

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *panel = static_cast<AppStorePanel*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      panel->execute_remove(panel->pending_app);
    }
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_center(mbox);
}

void AppStorePanel::show_service_confirm(const AppItem &app, const std::string &action) {
  if (is_busy.load()) return;
  pending_app = app;

  static const char *btns[] = {"Cancel", "Confirm", ""};
  std::string act_title, act_color, act_desc;
  if (action == "start") {
    act_title = "Start";
    act_color = "#4CAF50";
    act_desc = "The background service process will be started.";
  } else if (action == "stop") {
    act_title = "Stop";
    act_color = "#FFA726";
    act_desc = "The background service process will be stopped.";
  } else if (action == "enable") {
    act_title = "Enable";
    act_color = "#4CAF50";
    act_desc = "The service will be enabled for automatic startup on boot and started now.";
  } else if (action == "disable") {
    act_title = "Disable";
    act_color = "#FFA726";
    act_desc = "The service will be stopped and disabled from starting on boot.";
  }

  std::string msg = fmt::format("{} service for\n{} {}# ?\n\n{}",
                                act_title, act_color, app.name, act_desc);

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
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(65));

  struct ServiceCbData {
    AppStorePanel *panel;
    std::string action;
  };
  auto *cb_data = new ServiceCbData{this, action};

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    auto *data = static_cast<ServiceCbData*>(e->user_data);
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    if (lv_msgbox_get_active_btn(obj) == 1) {
      data->panel->execute_service_action(data->panel->pending_app, data->action);
    }
    delete data;
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, cb_data);

  lv_obj_center(mbox);
}

static int extract_appstore_percent(const std::string &line) {
  auto pct_pos = line.find('%');
  if (pct_pos != std::string::npos && pct_pos > 0) {
    size_t start = pct_pos - 1;
    while (start > 0 && (std::isdigit(line[start]) || line[start] == '.' || line[start] == ' ')) {
      start--;
    }
    std::string num_str = line.substr(start + 1, pct_pos - (start + 1));
    try {
      double p = std::stod(num_str);
      return std::max(0, std::min(100, static_cast<int>(p)));
    } catch (...) {}
  }
  return -1;
}

static std::string extract_appstore_suffix(const std::string &line) {
  auto bracket_pos = line.rfind(']');
  if (bracket_pos != std::string::npos && bracket_pos + 1 < line.size()) {
    std::string rem = line.substr(bracket_pos + 1);
    auto pct_pos = rem.find('%');
    if (pct_pos != std::string::npos && pct_pos + 1 < rem.size()) {
      std::string detail = rem.substr(pct_pos + 1);
      while (!detail.empty() && (detail.front() == ' ' || detail.front() == '\t')) detail.erase(0, 1);
      while (!detail.empty() && (detail.back() == ' ' || detail.back() == '\t' || detail.back() == '\r' || detail.back() == '\n')) detail.pop_back();
      return detail;
    }
    return rem;
  }
  return "";
}

void AppStorePanel::parse_appstore_output_line(const std::string &line) {
  if (line.find("FATAL:") != std::string::npos || line.find("ERROR:") != std::string::npos) {
    task_error_msg = line;
  }

  if (line.find("Downloading:") != std::string::npos) {
    int p = extract_appstore_percent(line);
    std::string detail = extract_appstore_suffix(line);
    if (p >= 0) {
      progress_percent.store(p);
    }
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = "Downloading package...";
    if (!detail.empty()) {
      progress_detail_str = detail;
    }
  } else if (line.find("SHA256 checksum verified") != std::string::npos) {
    progress_percent.store(100);
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = "Verifying package integrity...";
    progress_detail_str = "SHA256 verified";
  } else if (line.find("Installing:") != std::string::npos || line.find("Extracting package") != std::string::npos) {
    int p = extract_appstore_percent(line);
    std::string detail = extract_appstore_suffix(line);
    if (p >= 0) {
      progress_percent.store(p);
    }
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = "Installing files...";
    if (!detail.empty()) {
      progress_detail_str = detail;
    }
  } else if (line.find("dynamic service") != std::string::npos || line.find("Moonraker component") != std::string::npos || line.find("Added components") != std::string::npos) {
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = "Configuring system services...";
    progress_detail_str = "Registering components";
  } else if (line.find("Starting") != std::string::npos || line.find("Stopping") != std::string::npos || line.find("Enabling") != std::string::npos || line.find("Disabling") != std::string::npos) {
    progress_percent.store(50);
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = line;
    progress_detail_str = "Managing service";
  } else if (line.find("Successfully installed") != std::string::npos ||
             (line.find("OK") != std::string::npos && (line.find("service") != std::string::npos || line.find("Started") != std::string::npos || line.find("Stopped") != std::string::npos || line.find("Enabled") != std::string::npos || line.find("Disabled") != std::string::npos))) {
    progress_percent.store(100);
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = "Service operation complete!";
    progress_detail_str = "Done";
  }
}

void AppStorePanel::stream_process_output(const std::string &cmd, const std::string &initial_stage) {
  {
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_stage_str = initial_stage;
    progress_detail_str = "Starting...";
    task_error_msg.clear();
  }
  progress_percent.store(0);

  FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) {
    task_exit_code.store(1);
    task_error_msg = "Failed to launch openke-app process.";
    return;
  }

  std::string line;
  int c;
  while ((c = fgetc(pipe)) != EOF) {
    if (c == '\r' || c == '\n') {
      if (!line.empty()) {
        parse_appstore_output_line(line);
        line.clear();
      }
    } else {
      line += static_cast<char>(c);
    }
  }
  if (!line.empty()) {
    parse_appstore_output_line(line);
  }

  int status = pclose(pipe);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  task_exit_code.store(exit_code);
}

void AppStorePanel::show_progress_modal(const std::string &title, const std::string &initial_stage) {
  close_modal();

  // Full-screen floating backdrop
  modal_box = lv_obj_create(cont);
  lv_obj_add_flag(modal_box, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(modal_box, LV_PCT(100), LV_PCT(100));
  lv_obj_center(modal_box);
  lv_obj_move_foreground(modal_box);
  lv_obj_set_style_bg_color(modal_box, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(modal_box, LV_OPA_70, 0);
  lv_obj_set_style_border_width(modal_box, 0, 0);
  lv_obj_set_style_pad_all(modal_box, 0, 0);
  lv_obj_clear_flag(modal_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(modal_box, LV_OBJ_FLAG_CLICKABLE);

  // Dialog card
  lv_obj_t *card = lv_obj_create(modal_box);
  lv_obj_set_size(card, LV_PCT(88), LV_PCT(62));
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title_lbl = lv_label_create(card);
  lv_label_set_text(title_lbl, title.c_str());
  lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

  stage_label = lv_label_create(card);
  lv_label_set_text(stage_label, initial_stage.c_str());
  lv_obj_set_style_text_font(stage_label, &lv_font_montserrat_12, 0);
  lv_obj_align(stage_label, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_text_align(stage_label, LV_TEXT_ALIGN_CENTER, 0);

  progress_bar = lv_bar_create(card);
  lv_obj_set_size(progress_bar, LV_PCT(94), 22);
  lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 48);
  lv_bar_set_range(progress_bar, 0, 100);
  lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

  progress_label = lv_label_create(card);
  lv_label_set_text(progress_label, "0%");
  lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_12, 0);
  lv_obj_align(progress_label, LV_ALIGN_TOP_MID, 0, 74);

  detail_label = lv_label_create(card);
  lv_label_set_text(detail_label, "Please wait...");
  lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(detail_label, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
  lv_obj_align(detail_label, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
}

void AppStorePanel::close_modal() {
  if (modal_box != nullptr) {
    lv_obj_del(modal_box);
    modal_box = nullptr;
    progress_bar = nullptr;
    progress_label = nullptr;
    stage_label = nullptr;
    detail_label = nullptr;
  }
}

void AppStorePanel::show_alert(const std::string &title, const std::string &msg) {
  close_modal();
  static const char *btns[] = {"OK", ""};
  lv_obj_t *mbox = lv_msgbox_create(NULL, NULL, msg.c_str(), btns, false);
  KUtils::style_dialog_msgbox(mbox);

  lv_obj_t *msg_obj = ((lv_msgbox_t *)mbox)->text;
  lv_obj_set_style_text_align(msg_obj, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_recolor(msg_obj, true);
  lv_obj_set_width(msg_obj, LV_PCT(100));
  lv_obj_center(msg_obj);

  lv_obj_t *btnm = lv_msgbox_get_btns(mbox);
  lv_btnmatrix_set_btn_ctrl(btnm, 0, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_add_flag(btnm, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);

  auto hscale = (double)lv_disp_get_physical_ver_res(NULL) / 480.0;
  lv_obj_set_size(btnm, LV_PCT(90), 50 * hscale);
  lv_obj_set_size(mbox, LV_PCT(85), LV_PCT(55));

  lv_obj_add_event_cb(mbox, [](lv_event_t *e) {
    lv_obj_t *obj = lv_obj_get_parent(lv_event_get_target(e));
    lv_msgbox_close(obj);
  }, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_center(mbox);
}

void AppStorePanel::execute_refresh() {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Updating", "Refreshing App Store catalog...");

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this]() {
    stream_process_output("/usr/bin/openke-app update", "Checking for catalog updates...");
    task_finished.store(true);
  });
}

void AppStorePanel::execute_install(const AppItem &app, bool activate) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Installing " + app.name, "Initializing download...");

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app, activate]() {
    std::string cmd = "/usr/bin/openke-app install " + app.id;
    if (activate) {
      cmd += " --activate";
    }
    stream_process_output(cmd, "Connecting to download server...");
    task_finished.store(true);
  });
}

void AppStorePanel::execute_update(const AppItem &app) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Updating " + app.name, "Initializing update...");

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app]() {
    std::string cmd = "/usr/bin/openke-app install " + app.id;
    if (app.is_active) {
      cmd += " --activate";
    }
    stream_process_output(cmd, "Connecting to update server...");
    task_finished.store(true);
  });
}

void AppStorePanel::execute_activate(const AppItem &app) {
  if (is_busy.load()) return;

  if (KUtils::is_printing()) {
    show_alert("Cannot Switch Interface",
               "#FF5252 Print In Progress#\n\nA 3D print job is currently active or paused.\n\nPlease finish or cancel the print before switching interfaces.");
    return;
  }

  auto s = State::get_instance();
  auto etarget_j = s->get_data("/printer_state/extruder/target"_json_pointer);
  auto btarget_j = s->get_data("/printer_state/heater_bed/target"_json_pointer);
  double etarget = etarget_j.is_number() ? etarget_j.get<double>() : 0.0;
  double btarget = btarget_j.is_number() ? btarget_j.get<double>() : 0.0;

  if (app.category == "touch_ui" && (etarget > 0.0 || btarget > 0.0)) {
    show_alert("Heaters Active",
               "#FFA726 Thermal Safety:# Cannot switch Touchscreen UI while heaters are active.\n\nPlease turn off heaters first.");
    return;
  }

  std::string cmd;
  if (app.category == "web_ui") {
    cmd = fmt::format("/usr/bin/openke-app set-active-web {}", app.id);
  } else if (app.category == "touch_ui") {
    cmd = fmt::format("/usr/bin/openke-app set-active-touch {}", app.id);
  } else {
    return;
  }

  int rc = sp::call(cmd);
  if (rc != 0) {
    show_alert("Action Failed", fmt::format("#FF5252 Failed to activate {}#\n\nEnsure no print job is running or heaters active.", app.name));
    return;
  }

  refresh_apps();
  build_app_list();
}

void AppStorePanel::execute_remove(const AppItem &app) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Removing " + app.name, "Removing package files...");

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app]() {
    std::string cmd = "/usr/bin/openke-app remove " + app.id;
    stream_process_output(cmd, "Uninstalling...");
    task_finished.store(true);
  });
}

void AppStorePanel::execute_service_action(const AppItem &app, const std::string &action) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  std::string title;
  std::string initial_stage;
  if (action == "start") {
    title = "Starting " + app.name;
    initial_stage = "Starting service process...";
  } else if (action == "stop") {
    title = "Stopping " + app.name;
    initial_stage = "Stopping service process...";
  } else if (action == "enable") {
    title = "Enabling " + app.name;
    initial_stage = "Enabling service...";
  } else if (action == "disable") {
    title = "Disabling " + app.name;
    initial_stage = "Disabling service...";
  } else {
    title = "Managing " + app.name;
    initial_stage = "Processing service action...";
  }

  show_progress_modal(title, initial_stage);

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app, action, initial_stage]() {
    std::string cmd = fmt::format("/usr/bin/openke-app service {} {}", action, app.id);
    stream_process_output(cmd, initial_stage);
    task_finished.store(true);
  });
}

void AppStorePanel::check_async_task() {
  if (is_busy.load()) {
    if (progress_bar != nullptr && progress_label != nullptr && stage_label != nullptr) {
      int p = progress_percent.load();
      lv_bar_set_value(progress_bar, p, LV_ANIM_OFF);
      std::string plbl = std::to_string(p) + "%";
      lv_label_set_text(progress_label, plbl.c_str());

      std::string stage, detail;
      {
        std::lock_guard<std::mutex> lock(progress_mutex);
        stage = progress_stage_str;
        detail = progress_detail_str;
      }
      if (!stage.empty()) {
        lv_label_set_text(stage_label, stage.c_str());
      }
      if (detail_label != nullptr && !detail.empty()) {
        lv_label_set_text(detail_label, detail.c_str());
      }
    }
  }

  if (is_busy.load() && task_finished.load()) {
    is_busy.store(false);
    task_finished.store(false);
    close_modal();

    if (task_exit_code.load() == 0) {
      refresh_apps();
      build_app_list();
    } else {
      show_alert("Operation Failed", fmt::format("#FF5252 Operation failed:#\n\n{}", task_error_msg.empty() ? "Command returned error code." : task_error_msg));
    }
  }
}

