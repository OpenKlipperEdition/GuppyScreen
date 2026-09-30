#include "appstore_panel.h"
#include "utils.h"
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
          app.version = item.value("version", "");
          app.author = item.value("author", "");
          app.description = item.value("description", "");
          app.is_builtin = item.value("is_builtin", false);
          app.is_installed = item.value("is_installed", false);
          app.is_active = item.value("is_active", false);
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
              app.version = item.value("version", "");
              app.author = item.value("author", "");
              app.description = item.value("description", "");
              app.is_builtin = item.value("is_builtin", false);
              app.is_installed = app.is_builtin;
              app.is_active = (app.id == "mainsail" || app.id == "guppyscreen");
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

  // Sort: Active first, then installed, then alphabetical
  std::sort(apps.begin(), apps.end(), [](const AppItem &a, const AppItem &b) {
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

    if (app.is_active) {
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

    // Left icon container
    lv_obj_t *icon_cont = lv_obj_create(card);
    lv_obj_set_size(icon_cont, 48, 48);
    lv_obj_set_style_radius(icon_cont, 6, 0);
    lv_obj_set_style_pad_all(icon_cont, 2, 0);
    lv_obj_set_style_bg_color(icon_cont, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_border_width(icon_cont, 0, 0);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_img = lv_img_create(icon_cont);
    lv_img_set_src(icon_img, &layers_img);
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
    if (app.is_active) {
      lv_obj_set_style_text_color(name_lbl, lv_palette_lighten(LV_PALETTE_GREEN, 1), 0);
    }

    // Subtitle (category, version, author)
    std::string meta = fmt::format("{} • v{} • {}", app.category, app.version, app.author);
    lv_obj_t *meta_lbl = lv_label_create(text_cont);
    lv_label_set_text(meta_lbl, meta.c_str());
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
    } else if (app.is_installed) {
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

void AppStorePanel::show_activate_confirm(const AppItem &app) {
  if (is_busy.load()) return;
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

void AppStorePanel::show_progress_modal(const std::string &title, const std::string &msg) {
  close_modal();
  static const char *btns[] = {""};
  modal_box = lv_msgbox_create(NULL, NULL, msg.c_str(), btns, false);
  KUtils::style_dialog_msgbox(modal_box);

  lv_obj_t *msg_obj = ((lv_msgbox_t *)modal_box)->text;
  lv_obj_set_style_text_align(msg_obj, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_recolor(msg_obj, true);
  lv_obj_set_width(msg_obj, LV_PCT(100));
  lv_obj_center(msg_obj);

  lv_obj_set_size(modal_box, LV_PCT(80), LV_PCT(45));
  lv_obj_center(modal_box);
}

void AppStorePanel::close_modal() {
  if (modal_box != nullptr) {
    lv_msgbox_close(modal_box);
    modal_box = nullptr;
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

  show_progress_modal("Updating", "Refreshing App Store catalog...\nPlease wait.");

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this]() {
    try {
      auto p = sp::Popen({"/usr/bin/openke-app", "update"}, sp::output{sp::PIPE}, sp::error{sp::PIPE});
      p.communicate();
      task_exit_code.store(0);
    } catch (const std::exception &e) {
      spdlog::warn("AppStore: update execution failed: {}", e.what());
      task_exit_code.store(0);
    }
    task_finished.store(true);
  });
}

void AppStorePanel::execute_install(const AppItem &app, bool activate) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Installing", fmt::format("Installing #2196F3 {}# ...\nPlease wait.", app.name));

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app, activate]() {
    std::string app_id = app.id;
    std::vector<std::string> args = {"/usr/bin/openke-app", "install", app_id};
    if (activate) {
      args.push_back("--activate");
    }

    try {
      auto p = sp::Popen(args, sp::output{sp::PIPE}, sp::error{sp::PIPE});
      auto res = p.communicate();
      task_exit_code.store(p.retcode());
      if (p.retcode() != 0) {
        std::string err_str;
        if (res.second.length > 0 && res.second.buf.data() != nullptr) {
          err_str = std::string(res.second.buf.data(), res.second.length);
        } else if (res.first.length > 0 && res.first.buf.data() != nullptr) {
          err_str = std::string(res.first.buf.data(), res.first.length);
        }
        task_error_msg = err_str;
      }
    } catch (const std::exception &e) {
      task_exit_code.store(1);
      task_error_msg = e.what();
    }
    task_finished.store(true);
  });
}

void AppStorePanel::execute_activate(const AppItem &app) {
  if (is_busy.load()) return;

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
    show_alert("Action Failed", fmt::format("#FF5252 Failed to activate {}#\n\nEnsure no print job is running or paused.", app.name));
    return;
  }

  refresh_apps();
  build_app_list();
}

void AppStorePanel::execute_remove(const AppItem &app) {
  if (is_busy.load()) return;
  is_busy.store(true);
  task_finished.store(false);

  show_progress_modal("Removing", fmt::format("Removing #FF5252 {}# ...\nPlease wait.", app.name));

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  worker_thread = std::thread([this, app]() {
    try {
      auto p = sp::Popen({"/usr/bin/openke-app", "remove", app.id}, sp::output{sp::PIPE}, sp::error{sp::PIPE});
      auto res = p.communicate();
      task_exit_code.store(p.retcode());
      if (p.retcode() != 0) {
        std::string err_str;
        if (res.second.length > 0 && res.second.buf.data() != nullptr) {
          err_str = std::string(res.second.buf.data(), res.second.length);
        } else if (res.first.length > 0 && res.first.buf.data() != nullptr) {
          err_str = std::string(res.first.buf.data(), res.first.length);
        }
        task_error_msg = err_str;
      }
    } catch (const std::exception &e) {
      task_exit_code.store(1);
      task_error_msg = e.what();
    }
    task_finished.store(true);
  });
}

void AppStorePanel::check_async_task() {
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
