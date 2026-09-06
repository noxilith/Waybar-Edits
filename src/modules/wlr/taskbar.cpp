#include "modules/wlr/taskbar.hpp"

#include <fmt/core.h>
#include <gdkmm/monitor.h>
#include <gio/gdesktopappinfo.h>
#include <giomm/desktopappinfo.h>
#include <gtkmm/icontheme.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>

#include "gdkmm/general.h"
#include "glibmm/error.h"
#include "glibmm/fileutils.h"
#include "glibmm/refptr.h"
#include "util/format.hpp"
#include "util/gtk_icon.hpp"
#include "util/rewrite_string.hpp"
#include "util/string.hpp"

namespace waybar::modules::wlr {

/* Task class implementation */
uint32_t Task::global_id = 0;

static void tl_handle_title(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                            const char* title) {
  return static_cast<Task*>(data)->handle_title(title);
}

static void tl_handle_app_id(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                             const char* app_id) {
  return static_cast<Task*>(data)->handle_app_id(app_id);
}

static void tl_handle_output_enter(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                                   struct wl_output* output) {
  return static_cast<Task*>(data)->handle_output_enter(output);
}

static void tl_handle_output_leave(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                                   struct wl_output* output) {
  return static_cast<Task*>(data)->handle_output_leave(output);
}

static void tl_handle_state(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                            struct wl_array* state) {
  return static_cast<Task*>(data)->handle_state(state);
}

static void tl_handle_done(void* data, struct zwlr_foreign_toplevel_handle_v1* handle) {
  return static_cast<Task*>(data)->handle_done();
}

static void tl_handle_parent(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                             struct zwlr_foreign_toplevel_handle_v1* parent) {
  /* This is explicitly left blank */
}

static void tl_handle_closed(void* data, struct zwlr_foreign_toplevel_handle_v1* handle) {
  return static_cast<Task*>(data)->handle_closed();
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_impl = {
    .title = tl_handle_title,
    .app_id = tl_handle_app_id,
    .output_enter = tl_handle_output_enter,
    .output_leave = tl_handle_output_leave,
    .state = tl_handle_state,
    .done = tl_handle_done,
    .closed = tl_handle_closed,
    .parent = tl_handle_parent,
};

static const std::vector<Gtk::TargetEntry> target_entries = {
    Gtk::TargetEntry("WAYBAR_TOPLEVEL", Gtk::TARGET_SAME_APP, 0)};

Task::Task(const waybar::Bar& bar, const Json::Value& config, Taskbar* tbar,
           struct zwlr_foreign_toplevel_handle_v1* tl_handle, struct wl_seat* seat)
    : bar_{bar},
      config_{config},
      tbar_{tbar},
      handle_{tl_handle},
      seat_{seat},
      id_{global_id++},
      content_{bar.orientation, 0} {
  zwlr_foreign_toplevel_handle_v1_add_listener(handle_, &toplevel_handle_impl, this);

  button.set_relief(Gtk::RELIEF_NONE);

  content_.add(text_before_);
  content_.add(icon_);
  content_.add(text_after_);

  content_.show();
  button.add(content_);

  format_before_.clear();
  format_after_.clear();

  if (config_["format"].isString()) {
    auto format = config_["format"].asString();

    if (format.find("{name}") != std::string::npos) {
      with_name_ = true;
    }

    auto parts = split(format, "{icon}", 1);
    format_before_ = parts[0];

    if (parts.size() > 1) {
      with_icon_ = true;
      format_after_ = parts[1];
    }
  } else {
    with_icon_ = true;
  }

  if (app_id_.empty()) {
    handle_app_id("unknown");
  }

  format_tooltip_.clear();

  if (!config_["tooltip"].isBool() || config_["tooltip"].asBool()) {
    if (config_["tooltip-format"].isString())
      format_tooltip_ = config_["tooltip-format"].asString();
    else
      format_tooltip_ = "{title}";
  }

  button.add_events(Gdk::BUTTON_PRESS_MASK);

  button.signal_button_release_event().connect(
      sigc::mem_fun(*this, &Task::handle_clicked), false);

  button.signal_motion_notify_event().connect(
      sigc::mem_fun(*this, &Task::handle_motion_notify), false);

  button.drag_source_set(
      target_entries, Gdk::BUTTON1_MASK, Gdk::ACTION_MOVE);

  button.drag_dest_set(
      target_entries, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_MOVE);

  button.signal_drag_data_get().connect(
      sigc::mem_fun(*this, &Task::handle_drag_data_get), false);

  button.signal_drag_data_received().connect(
      sigc::mem_fun(*this, &Task::handle_drag_data_received), false);
}

Task::~Task() {
  if (handle_) {
    zwlr_foreign_toplevel_handle_v1_destroy(handle_);
    handle_ = nullptr;
  }

  if (button_visible_) {
    tbar_->remove_button(button);
    button_visible_ = false;
  }
}

std::string Task::repr() const {
  std::stringstream ss;

  ss << "Task (" << id_ << ") " << title_ << " [" << app_id_ << "] <"
     << (active() ? "A" : "a")
     << (maximized() ? "M" : "m")
     << (minimized() ? "I" : "i")
     << (fullscreen() ? "F" : "f")
     << ">";

  return ss.str();
}

std::string Task::state_string(bool shortened) const {
  std::stringstream ss;

  if (shortened)
    ss << (minimized() ? "m" : "")
       << (maximized() ? "M" : "")
       << (active() ? "A" : "")
       << (fullscreen() ? "F" : "");
  else
    ss << (minimized() ? "minimized " : "")
       << (maximized() ? "maximized " : "")
       << (active() ? "active " : "")
       << (fullscreen() ? "fullscreen " : "");

  std::string res = ss.str();

  if (shortened || res.empty())
    return res;
  else
    return res.substr(0, res.size() - 1);
}

void Task::handle_title(const char* title) {
  if (title_.empty()) {
    spdlog::debug(fmt::format(
        "Task ({}) setting title to {}", id_, title_));
  } else {
    spdlog::debug(fmt::format(
        "Task ({}) overwriting title '{}' with '{}'",
        id_, title_, title));
  }

  title_ = title;
  hide_if_ignored();

  if ((!with_icon_ && !with_name_) || app_info_) {
    return;
  }

  app_info_ = IconLoader::get_app_info_from_app_id_list(title_);
  name_ = app_info_ ? app_info_->get_display_name() : title;

  if (!with_icon_) {
    return;
  }

  int icon_size =
      config_["icon-size"].isInt()
          ? config_["icon-size"].asInt()
          : 16;

  if (tbar_->icon_loader().image_load_icon(
          icon_, app_info_, icon_size)) {
    icon_.show();
  } else {
    spdlog::debug("Couldn't find icon for {}", title_);
  }
}

void Task::set_minimize_hint() {
  zwlr_foreign_toplevel_handle_v1_set_rectangle(
      handle_,
      bar_.surface,
      minimize_hint.x,
      minimize_hint.y,
      minimize_hint.w,
      minimize_hint.h);
}

void Task::hide_if_ignored() {
  if (tbar_->ignore_list().count(app_id_) ||
      tbar_->ignore_list().count(title_)) {

    ignored_ = true;

    if (button_visible_) {
      auto output =
          gdk_wayland_monitor_get_wl_output(
              bar_.output->monitor->gobj());

      handle_output_leave(output);
    }

  } else {

    bool is_was_ignored = ignored_;
    ignored_ = false;

    if (is_was_ignored) {
      auto output =
          gdk_wayland_monitor_get_wl_output(
              bar_.output->monitor->gobj());

      handle_output_enter(output);
    }
  }
}

void Task::handle_app_id(const char* app_id) {
  if (app_id_.empty()) {
    spdlog::debug(fmt::format(
        "Task ({}) setting app_id to {}", id_, app_id));
  } else {
    spdlog::debug(fmt::format(
        "Task ({}) overwriting app_id '{}' with '{}'",
        id_, app_id_, app_id));
  }

  app_id_ = app_id;
  hide_if_ignored();

  auto ids_replace_map = tbar_->app_ids_replace_map();

  if (ids_replace_map.count(app_id_)) {
    auto replaced_id = ids_replace_map[app_id_];

    spdlog::debug(fmt::format(
        "Task ({}) [{}] app_id was replaced with {}",
        id_, app_id_, replaced_id));

    app_id_ = replaced_id;
  }

  if (!with_icon_ && !with_name_) {
    return;
  }

  app_info_ =
      IconLoader::get_app_info_from_app_id_list(app_id_);

  name_ =
      app_info_
          ? app_info_->get_display_name()
          : app_id;

  if (!with_icon_) {
    return;
  }

  int icon_size =
      config_["icon-size"].isInt()
          ? config_["icon-size"].asInt()
          : 16;

  if (tbar_->icon_loader().image_load_icon(
          icon_, app_info_, icon_size)) {
    icon_.show();
  } else {
    spdlog::debug(
        "Couldn't find icon for {}", app_id_);
  }
}

void Task::on_button_size_allocated(Gtk::Allocation& alloc) {
  gtk_widget_translate_coordinates(
      GTK_WIDGET(button.gobj()),
      GTK_WIDGET(bar_.window.gobj()),
      0,
      0,
      &minimize_hint.x,
      &minimize_hint.y);

  minimize_hint.w = button.get_width();
  minimize_hint.h = button.get_height();
}

void Task::handle_output_enter(struct wl_output* output) {
  if (ignored_) {
    spdlog::debug("{} is ignored", repr());
    return;
  }

  spdlog::debug(
      "{} entered output {}",
      repr(),
      (void*)output);

  if (!button_visible_ &&
      (tbar_->all_outputs() ||
       tbar_->show_output(output))) {

    button.signal_size_allocate().connect_notify(
        sigc::mem_fun(
            this,
            &Task::on_button_size_allocated));

    tbar_->add_button(button);
    button.show();

    button_visible_ = true;

    spdlog::debug(
        "{} now visible on {}",
        repr(),
        bar_.output->name);
  }
}

void Task::handle_output_leave(struct wl_output* output) {
  spdlog::debug(
      "{} left output {}",
      repr(),
      (void*)output);

  if (button_visible_ &&
      (!tbar_->all_outputs() &&
       tbar_->show_output(output))) {

    tbar_->remove_button(button);
    button.hide();
    button_visible_ = false;

    spdlog::debug(
        "{} now invisible on {}",
        repr(),
        bar_.output->name);
  }
}

/*
 * Hyprland workspace visibility.
 *
 * This deliberately removes the button directly instead of using
 * handle_output_leave(), because workspace filtering is independent
 * from output filtering.
 */
void Task::set_workspace_visible(bool visible) {
  if (ignored_) {
    return;
  }

  if (visible) {
    if (!button_visible_) {
      auto output =
          gdk_wayland_monitor_get_wl_output(
              bar_.output->monitor->gobj());

      handle_output_enter(output);
    }
  } else {
    if (button_visible_) {
      tbar_->remove_button(button);
      button.hide();
      button_visible_ = false;
    }
  }
}

void Task::handle_state(struct wl_array* state) {
  state_ = 0;

  size_t size =
      state->size / sizeof(uint32_t);

  for (size_t i = 0; i < size; ++i) {
    auto entry =
        static_cast<uint32_t*>(state->data)[i];

    if (entry ==
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED)
      state_ |= MAXIMIZED;

    if (entry ==
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
      state_ |= MINIMIZED;

    if (entry ==
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
      state_ |= ACTIVE;

    if (entry ==
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN)
      state_ |= FULLSCREEN;
  }
}

void Task::handle_done() {
  spdlog::debug("{} changed", repr());

  if (state_ & MAXIMIZED)
    button.get_style_context()->add_class("maximized");
  else
    button.get_style_context()->remove_class("maximized");

  if (state_ & MINIMIZED)
    button.get_style_context()->add_class("minimized");
  else
    button.get_style_context()->remove_class("minimized");

  if (state_ & ACTIVE)
    button.get_style_context()->add_class("active");
  else
    button.get_style_context()->remove_class("active");

  if (state_ & FULLSCREEN)
    button.get_style_context()->add_class("fullscreen");
  else
    button.get_style_context()->remove_class("fullscreen");

  if (config_["active-first"].isBool() &&
      config_["active-first"].asBool() &&
      active()) {
    tbar_->move_button(button, 0);
  }

  tbar_->dp.emit();
}

void Task::handle_closed() {
  spdlog::debug("{} closed", repr());

  zwlr_foreign_toplevel_handle_v1_destroy(handle_);
  handle_ = nullptr;

  if (button_visible_) {
    tbar_->remove_button(button);
    button_visible_ = false;
  }

  tbar_->remove_task(id_);
}

bool Task::handle_clicked(GdkEventButton* bt) {
  if (bt->type == GDK_BUTTON_PRESS) {
    drag_start_button = bt->button;
    drag_start_x = bt->x;
    drag_start_y = bt->y;
  }

  std::string action;

  if (config_["on-click"].isString() &&
      bt->button == 1)
    action = config_["on-click"].asString();

  else if (config_["on-click-middle"].isString() &&
           bt->button == 2)
    action = config_["on-click-middle"].asString();

  else if (config_["on-click-right"].isString() &&
           bt->button == 3)
    action = config_["on-click-right"].asString();

  if (action.empty())
    return true;

  if (action == "activate")
    activate();

  else if (action == "minimize") {
    set_minimize_hint();
    minimize(!minimized());
  }

  else if (action == "minimize-raise") {
    set_minimize_hint();

    if (minimized())
      minimize(false);
    else if (active())
      minimize(true);
    else
      activate();

  } else if (action == "maximize")
    maximize(!maximized());

  else if (action == "fullscreen")
    fullscreen(!fullscreen());

  else if (action == "close")
    close();

  else
    spdlog::warn(
        "Unknown action {}",
        action);

  drag_start_button = -1;

  return true;
}

bool Task::handle_motion_notify(GdkEventMotion* mn) {
  if (drag_start_button == -1)
    return false;

  if (button.drag_check_threshold(
          drag_start_x,
          drag_start_y,
          mn->x,
          mn->y)) {

    auto target_list =
        Gtk::TargetList::create(target_entries);

    auto refptr =
        Glib::RefPtr<Gtk::TargetList>(target_list);

    auto drag_context =
        button.drag_begin(
            refptr,
            Gdk::DragAction::ACTION_MOVE,
            drag_start_button,
            (GdkEvent*)mn);
  }

  return false;
}

void Task::handle_drag_data_get(
    const Glib::RefPtr<Gdk::DragContext>& context,
    Gtk::SelectionData& selection_data,
    guint info,
    guint time) {

  spdlog::debug("drag_data_get");

  void* button_addr =
      (void*)&this->button;

  selection_data.set(
      "WAYBAR_TOPLEVEL",
      32,
      (const guchar*)&button_addr,
      sizeof(gpointer));
}

void Task::handle_drag_data_received(
    const Glib::RefPtr<Gdk::DragContext>& context,
    int x,
    int y,
    Gtk::SelectionData selection_data,
    guint info,
    guint time) {

  spdlog::debug("drag_data_received");

  gpointer handle =
      *(gpointer*)selection_data.get_data();

  auto dragged_button =
      (Gtk::Button*)handle;

  if (dragged_button == &this->button)
    return;

  auto parent_of_dragged =
      dragged_button->get_parent();

  auto parent_of_dest =
      this->button.get_parent();

  if (parent_of_dragged != parent_of_dest)
    return;

  auto box =
      (Gtk::Box*)parent_of_dragged;

  auto position_prop =
      box->child_property_position(this->button);

  auto position =
      position_prop.get_value();

  box->reorder_child(
      *dragged_button,
      position);
}

bool Task::operator==(const Task& o) const {
  return o.id_ == id_;
}

bool Task::operator!=(const Task& o) const {
  return o.id_ != id_;
}

void Task::update() {
  bool markup =
      config_["markup"].isBool()
          ? config_["markup"].asBool()
          : false;

  std::string title = title_;
  std::string name = name_;
  std::string app_id = app_id_;

  if (markup) {
    title = Glib::Markup::escape_text(title);
    name = Glib::Markup::escape_text(name);
    app_id = Glib::Markup::escape_text(app_id);
  }

  if (!format_before_.empty()) {
    auto txt =
        fmt::format(
            fmt::runtime(format_before_),
            fmt::arg("title", title),
            fmt::arg("name", name),
            fmt::arg("app_id", app_id),
            fmt::arg("state", state_string()),
            fmt::arg("short_state", state_string(true)));

    txt =
        waybar::util::rewriteString(
            txt,
            config_["rewrite"]);

    if (markup)
      text_before_.set_markup(txt);
    else
      text_before_.set_label(txt);

    text_before_.show();
  }

  if (!format_after_.empty()) {
    auto txt =
        fmt::format(
            fmt::runtime(format_after_),
            fmt::arg("title", title),
            fmt::arg("name", name),
            fmt::arg("app_id", app_id),
            fmt::arg("state", state_string()),
            fmt::arg("short_state", state_string(true)));

    txt =
        waybar::util::rewriteString(
            txt,
            config_["rewrite"]);

    if (markup)
      text_after_.set_markup(txt);
    else
      text_after_.set_label(txt);

    text_after_.show();
  }

  if (!format_tooltip_.empty()) {
    auto txt =
        fmt::format(
            fmt::runtime(format_tooltip_),
            fmt::arg("title", title),
            fmt::arg("name", name),
            fmt::arg("app_id", app_id),
            fmt::arg("state", state_string()),
            fmt::arg("short_state", state_string(true)));

    txt =
        waybar::util::rewriteString(
            txt,
            config_["rewrite"]);

    if (markup)
      button.set_tooltip_markup(txt);
    else
      button.set_tooltip_text(txt);
  }
}

void Task::maximize(bool set) {
  if (set)
    zwlr_foreign_toplevel_handle_v1_set_maximized(handle_);
  else
    zwlr_foreign_toplevel_handle_v1_unset_maximized(handle_);
}

void Task::minimize(bool set) {
  if (set)
    zwlr_foreign_toplevel_handle_v1_set_minimized(handle_);
  else
    zwlr_foreign_toplevel_handle_v1_unset_minimized(handle_);
}

void Task::activate() {
  zwlr_foreign_toplevel_handle_v1_activate(
      handle_,
      seat_);
}

void Task::fullscreen(bool set) {
  if (zwlr_foreign_toplevel_handle_v1_get_version(handle_) <
      ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_SET_FULLSCREEN_SINCE_VERSION) {

    spdlog::warn(
        "Foreign toplevel manager server does not support "
        "for set/unset fullscreen.");

    return;
  }

  if (set)
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(
        handle_,
        nullptr);
  else
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(
        handle_);
}

void Task::close() {
  zwlr_foreign_toplevel_handle_v1_close(handle_);
}

/* Taskbar class implementation */

static void handle_global(
    void* data,
    struct wl_registry* registry,
    uint32_t name,
    const char* interface,
    uint32_t version) {

  if (std::strcmp(
          interface,
          zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {

    static_cast<Taskbar*>(data)->register_manager(
        registry,
        name,
        version);

  } else if (std::strcmp(
                 interface,
                 wl_seat_interface.name) == 0) {

    static_cast<Taskbar*>(data)->register_seat(
        registry,
        name,
        version);
  }
}

static void handle_global_remove(
    void* data,
    struct wl_registry* registry,
    uint32_t name) {
  /* Nothing to do here */
}

static const wl_registry_listener registry_listener_impl = {
    .global = handle_global,
    .global_remove = handle_global_remove};

Taskbar::Taskbar(
    const std::string& id,
    const waybar::Bar& bar,
    const Json::Value& config)
    : waybar::AModule(
          config,
          "taskbar",
          id,
          false,
          false),
      bar_(bar),
      box_{bar.orientation, 0},
      manager_{nullptr},
      seat_{nullptr} {

  box_.set_name("taskbar");

  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }

  box_.get_style_context()->add_class(MODULE_CLASS);
  box_.get_style_context()->add_class("empty");

  event_box_.add(box_);

  struct wl_display* display =
      Client::inst()->wl_display;

  struct wl_registry* registry =
      wl_display_get_registry(display);

  wl_registry_add_listener(
      registry,
      &registry_listener_impl,
      this);

  wl_display_roundtrip(display);

  if (!manager_) {
    spdlog::error(
        "Failed to register as toplevel manager");
    return;
  }

  if (!seat_) {
    spdlog::error(
        "Failed to get wayland seat");
    return;
  }

  if (config_["icon-theme"].isArray()) {
    for (auto& c : config_["icon-theme"]) {
      icon_loader_.add_custom_icon_theme(
          c.asString());
    }
  } else if (config_["icon-theme"].isString()) {
    icon_loader_.add_custom_icon_theme(
        config_["icon-theme"].asString());
  }

  if (config_["ignore-list"].isArray()) {
    for (auto& app_name : config_["ignore-list"]) {
      ignore_list_.emplace(
          app_name.asString());
    }
  }

  if (config_["app_ids-mapping"].isObject()) {
    const Json::Value& mapping =
        config_["app_ids-mapping"];

    const std::vector<std::string> app_ids =
        config_["app_ids-mapping"]
            .getMemberNames();

    for (auto& app_id : app_ids) {
      app_ids_replace_map_.emplace(
          app_id,
          mapping[app_id].asString());
    }
  }

  /*
   * Poll Hyprland for workspace/client information.
   *
   * 250 ms keeps the taskbar responsive without
   * constantly hammering hyprctl.
   */
  workspace_timer_ =
      Glib::signal_timeout().connect(
          sigc::mem_fun(
              *this,
              &Taskbar::update_workspace_filter),
          250);
}

Taskbar::~Taskbar() {
  if (workspace_timer_.connected()) {
    workspace_timer_.disconnect();
  }

  if (manager_) {
    struct wl_display* display =
        Client::inst()->wl_display;

    zwlr_foreign_toplevel_manager_v1_stop(
        manager_);

    wl_display_roundtrip(display);

    if (manager_) {
      spdlog::warn(
          "Foreign toplevel manager destroyed "
          "before .finished event");

      zwlr_foreign_toplevel_manager_v1_destroy(
          manager_);

      manager_ = nullptr;
    }
  }
}

void Taskbar::update() {
  for (auto& t : tasks_) {
    t->update();
  }

  update_workspace_filter();

  if (config_["sort-by-app-id"].asBool()) {
    std::stable_sort(
        tasks_.begin(),
        tasks_.end(),
        [](const std::unique_ptr<Task>& a,
           const std::unique_ptr<Task>& b) {
          return a->app_id() < b->app_id();
        });

    for (unsigned long i = 0;
     i < tasks_.size();
     i++) {
     move_button(tasks_[i]->button, i);
    }
  }

  AModule::update();
}

/*
 * Hyprland workspace filter.
 *
 * Hyprland's foreign-toplevel protocol tells us about
 * windows but does not give Waybar a direct workspace
 * association.
 *
 * We therefore query Hyprland IPC and associate each
 * Waybar task with its workspace using class + title.
 */
bool Taskbar::update_workspace_filter() {
  FILE* pipe =
      popen(
          "hyprctl -j activeworkspace 2>/dev/null",
          "r");

  if (!pipe) {
    return true;
  }

  std::string active_workspace_json;
  char buffer[4096];

  while (fgets(buffer, sizeof(buffer), pipe)) {
    active_workspace_json += buffer;
  }

  pclose(pipe);

  Json::CharReaderBuilder active_builder;
  Json::Value active_root;
  std::string active_errors;
  std::istringstream active_stream(active_workspace_json);

  if (!Json::parseFromStream(
          active_builder,
          active_stream,
          &active_root,
          &active_errors)) {
    return true;
  }

  if (!active_root["id"].isInt64() &&
      !active_root["id"].isInt()) {
    return true;
  }

  current_workspace_id_ =
      active_root["id"].asInt64();

  pipe =
      popen(
          "hyprctl -j clients 2>/dev/null",
          "r");

  if (!pipe) {
    return true;
  }

  std::string clients_json;

  while (fgets(buffer, sizeof(buffer), pipe)) {
    clients_json += buffer;
  }

  pclose(pipe);

  Json::CharReaderBuilder clients_builder;
  Json::Value clients_root;
  std::string clients_errors;
  std::istringstream clients_stream(clients_json);

  if (!Json::parseFromStream(
          clients_builder,
          clients_stream,
          &clients_root,
          &clients_errors)) {
    return true;
  }

  if (!clients_root.isArray()) {
    return true;
  }

  // Keep track of which Hyprland clients have already
  // been assigned to a Waybar task.
  std::unordered_set<size_t> used_clients;

  for (auto& task : tasks_) {
    bool matched = false;

    for (Json::ArrayIndex i = 0;
         i < clients_root.size();
         ++i) {

      if (used_clients.count(i)) {
        continue;
      }

      const auto& client = clients_root[i];

      if (!client.isObject()) {
        continue;
      }

      const std::string client_class =
          client["class"].asString();

      const std::string client_initial_class =
          client["initialClass"].asString();

      const std::string client_title =
          client["title"].asString();

      const bool class_match =
          task->app_id() == client_class ||
          task->app_id() == client_initial_class;

      const bool title_match =
          task->title() == client_title;

      if (!class_match || !title_match) {
        continue;
      }

      if (!client["workspace"]["id"].isInt64() &&
          !client["workspace"]["id"].isInt()) {
        continue;
      }

      task->set_workspace_id(
          client["workspace"]["id"].asInt64());

      used_clients.insert(i);
      matched = true;
      break;
    }

    if (!matched) {
      task->set_workspace_id(-1);
    }

    const bool visible =
        matched &&
        task->workspace_id() ==
            current_workspace_id_;

    task->set_workspace_visible(visible);
  }

  return true;
}

static void tm_handle_toplevel(
    void* data,
    struct zwlr_foreign_toplevel_manager_v1* manager,
    struct zwlr_foreign_toplevel_handle_v1* tl_handle) {

  return static_cast<Taskbar*>(data)
      ->handle_toplevel_create(tl_handle);
}

static void tm_handle_finished(
    void* data,
    struct zwlr_foreign_toplevel_manager_v1* manager) {

  return static_cast<Taskbar*>(data)
      ->handle_finished();
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
    toplevel_manager_impl = {
        .toplevel = tm_handle_toplevel,
        .finished = tm_handle_finished,
};

void Taskbar::register_manager(
    struct wl_registry* registry,
    uint32_t name,
    uint32_t version) {

  if (manager_) {
    spdlog::warn(
        "Register foreign toplevel manager again "
        "although already existing!");

    return;
  }

  if (version <
      ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_SET_FULLSCREEN_SINCE_VERSION) {

    spdlog::warn(
        "Foreign toplevel manager server does not "
        "have the appropriate version. To be able "
        "to use all features, you need at least "
        "version 2, but server is version {}",
        version);
  }

  version =
      std::min<uint32_t>(
          version,
          zwlr_foreign_toplevel_manager_v1_interface.version);

  manager_ =
      static_cast<
          struct zwlr_foreign_toplevel_manager_v1*>(
          wl_registry_bind(
              registry,
              name,
              &zwlr_foreign_toplevel_manager_v1_interface,
              version));

  if (manager_) {
    zwlr_foreign_toplevel_manager_v1_add_listener(
        manager_,
        &toplevel_manager_impl,
        this);
  } else {
    spdlog::debug(
        "Failed to register manager");
  }
}

void Taskbar::register_seat(
    struct wl_registry* registry,
    uint32_t name,
    uint32_t version) {

  if (seat_) {
    spdlog::warn(
        "Register seat again although already existing!");

    return;
  }

  version =
      std::min<uint32_t>(
          version,
          wl_seat_interface.version);

  seat_ =
      static_cast<wl_seat*>(
          wl_registry_bind(
              registry,
              name,
              &wl_seat_interface,
              version));
}

void Taskbar::handle_toplevel_create(
    struct zwlr_foreign_toplevel_handle_v1* tl_handle) {

  tasks_.push_back(
      std::make_unique<Task>(
          bar_,
          config_,
          this,
          tl_handle,
          seat_));

  /*
   * Immediately try to assign the new window
   * to its Hyprland workspace.
   */
  update_workspace_filter();
}

void Taskbar::handle_finished() {
  zwlr_foreign_toplevel_manager_v1_destroy(
      manager_);

  manager_ = nullptr;
}

void Taskbar::add_button(Gtk::Button& bt) {
  box_.pack_start(
      bt,
      false,
      false);

  box_.get_style_context()
      ->remove_class("empty");
}

void Taskbar::move_button(
    Gtk::Button& bt,
    int pos) {

  box_.reorder_child(
      bt,
      pos);
}

void Taskbar::remove_button(
    Gtk::Button& bt) {

  box_.remove(bt);

  if (box_.get_children().empty()) {
    box_.get_style_context()
        ->add_class("empty");
  }
}

void Taskbar::remove_task(uint32_t id) {
  auto it =
      std::find_if(
          std::begin(tasks_),
          std::end(tasks_),
          [id](const TaskPtr& p) {
            return p->id() == id;
          });

  if (it == std::end(tasks_)) {
    spdlog::warn(
        "Can't find task with id {}",
        id);

    return;
  }

  tasks_.erase(it);
}

bool Taskbar::show_output(
    struct wl_output* output) const {

  return output ==
      gdk_wayland_monitor_get_wl_output(
          bar_.output->monitor->gobj());
}

bool Taskbar::all_outputs() const {
  return config_["all-outputs"].isBool() &&
         config_["all-outputs"].asBool();
}

const IconLoader& Taskbar::icon_loader() const {
  return icon_loader_;
}

const std::unordered_set<std::string>&
Taskbar::ignore_list() const {
  return ignore_list_;
}

const std::map<std::string, std::string>&
Taskbar::app_ids_replace_map() const {
  return app_ids_replace_map_;
}

} /* namespace waybar::modules::wlr */
