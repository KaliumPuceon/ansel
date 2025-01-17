/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <gdk/gdkkeysyms.h>

#include "common/collection.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/culling.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *zoom;
  GtkWidget *zoom_entry;
  GtkWidget *layout_box;
  GtkWidget *layout_filemanager;
  GtkWidget *layout_culling_dynamic;
  GtkWidget *layout_preview;
  dt_lighttable_layout_t layout, base_layout;
  int current_zoom;
  gboolean fullpreview;
  gboolean fullpreview_focus;
  gboolean combo_evt_reset;
  GtkWidget *over_popup, *thumbnails_box;
  GtkWidget *over_r0, *over_r1, *over_r2;
  gboolean disable_over_events;
  GtkWidget *grouping_button, *overlays_button;
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);
static gint _lib_lighttable_get_zoom(dt_lib_module_t *self);

/* get/set layout proxy function */
static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self);

/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data);
/* zoom entry change callback */
static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event,
                                                   dt_lib_module_t *self);

static void _set_zoom(dt_lib_module_t *self, int zoom);

/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("lighttable");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static void _main_icons_register_size(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{

  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  /* get the css geometry properties */
  GtkBorder margin, border, padding;
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);

  /* we first remove css margin border and padding from allocation */
  int width = allocation->width - margin.left - margin.right - border.left - border.right - padding.left - padding.right;

  GtkStyleContext *ccontext = gtk_widget_get_style_context(DTGTK_BUTTON(widget)->canvas);
  GtkBorder cmargin;
  gtk_style_context_get_margin(ccontext, state, &cmargin);

  /* we remove the extra room for optical alignment */
  width = round((float)width * (1.0 - (cmargin.left + cmargin.right) / 100.0f));

  // we store the icon size in order to keep in sync thumbtable overlays
  darktable.gui->icon_size = width;
}

static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data)
{

  darktable.gui->grouping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(widget, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(widget, _("collapse grouped images"));
  dt_conf_set_bool("ui_last/grouping", darktable.gui->grouping);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING, NULL);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","global_toolbox-grouping_toggle",
      LUA_ASYNC_TYPENAME,"bool",darktable.gui->grouping,
      LUA_ASYNC_DONE);
#endif // USE_LUA
}

static void _overlays_toggle_button(GtkWidget *w, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->disable_over_events) return;

  dt_thumbnail_overlay_t over = DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r0)))
    over = DT_THUMBNAIL_OVERLAYS_NONE;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r1)))
    over = DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r2)))
    over = DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL;

  dt_thumbtable_set_overlays_mode(dt_ui_thumbtable(darktable.gui->ui), over);
  dt_view_lighttable_culling_preview_reload_overlays(darktable.view_manager);

#ifdef USE_LUA
  gboolean show = (over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL);
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper, 0, NULL, NULL, LUA_ASYNC_TYPENAME, "const char*",
                          "global_toolbox-overlay_toggle", LUA_ASYNC_TYPENAME, "bool", show, LUA_ASYNC_DONE);
#endif // USE_LUA
}

static void _overlays_show_popup(GtkWidget *button, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  d->disable_over_events = TRUE;

  gboolean show = FALSE;

  // thumbnails part
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  gboolean thumbs_state;
  if(g_strcmp0(cv->module_name, "slideshow") == 0)
  {
    thumbs_state = FALSE;
  }
  else if(g_strcmp0(cv->module_name, "lighttable") == 0)
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
    {
      thumbs_state = dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM);
    }
    else
    {
      thumbs_state = TRUE;
    }
  }
  else
  {
    thumbs_state = dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM);
  }


  if(thumbs_state)
  {
    // we get and set the current value
    dt_thumbnail_overlay_t mode = sanitize_overlays(dt_ui_thumbtable(darktable.gui->ui)->overlays);
    if(mode == DT_THUMBNAIL_OVERLAYS_NONE)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r0), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r1), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r2), TRUE);

    gtk_widget_show_all(d->thumbnails_box);
    show = TRUE;
  }
  else
  {
    gtk_widget_hide(d->thumbnails_box);
  }

  if(show)
  {
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

    int x, y;
    GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
    gpointer   pointer_widget = NULL;
    if(pointer_window)
      gdk_window_get_user_data(pointer_window, &pointer_widget);

    GdkRectangle rect = { gtk_widget_get_allocated_width(button) / 2,
                          0, 1, 1 };

    if(pointer_widget && button != pointer_widget)
      gtk_widget_translate_coordinates(pointer_widget, button, x, y, &rect.x, &rect.y);

    gtk_popover_set_pointing_to(GTK_POPOVER(d->over_popup), &rect);

    gtk_widget_show(d->over_popup);
  }
  else
    dt_control_log(_("overlays not available here..."));

  d->disable_over_events = FALSE;
}

static void _lib_lighttable_update_btn(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  // which btn should be active ?
  GtkWidget *active = d->layout_filemanager;
  if(d->fullpreview)
    active = d->layout_preview;
  else if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    active = d->layout_culling_dynamic;

  GList *children = gtk_container_get_children(GTK_CONTAINER(d->layout_box));
  for(const GList *l = children; l; l = g_list_next(l))
  {
    GtkWidget *w = (GtkWidget *)l->data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), (w == active));
  }
  g_list_free(children);

  // and now we set the tooltips
  if(d->fullpreview)
    gtk_widget_set_tooltip_text(d->layout_preview, _("click to exit from full preview layout."));
  else
    gtk_widget_set_tooltip_text(d->layout_preview, _("click to enter full preview layout."));

  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC || d->fullpreview)
    gtk_widget_set_tooltip_text(d->layout_culling_dynamic, _("click to enter culling layout in dynamic mode."));
  else
    gtk_widget_set_tooltip_text(d->layout_culling_dynamic, _("click to exit culling layout."));
}

static void _lib_lighttable_set_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  // we deal with fullpreview first.
  if(!d->fullpreview && layout == DT_LIGHTTABLE_LAYOUT_PREVIEW)
  {
    // special case for preview : we don't change previous values, just show full preview
    d->fullpreview = TRUE;
    _lib_lighttable_update_btn(self);
    dt_view_lighttable_set_preview_state(darktable.view_manager, TRUE, d->fullpreview_focus);
    return;
  }
  else if(d->fullpreview && layout != DT_LIGHTTABLE_LAYOUT_PREVIEW)
  {
    d->fullpreview = FALSE;
    dt_view_lighttable_set_preview_state(darktable.view_manager, FALSE, FALSE);
    // and we continue to select the right layout...
  }

  const int current_layout = sanitize_lighttable_layout(dt_conf_get_int("plugins/lighttable/layout"));
  d->layout = layout;

  if(current_layout != layout)
  {
    if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
      d->current_zoom = MAX(1, MIN(30, dt_collection_get_selected_count(darktable.collection)));
      if(d->current_zoom == 1) d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
    else
    {
      d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    }

    gtk_widget_set_sensitive(d->zoom_entry, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && !d->fullpreview));
    gtk_widget_set_sensitive(d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && !d->fullpreview));
    gtk_range_set_value(GTK_RANGE(d->zoom), d->current_zoom);

    dt_conf_set_int("plugins/lighttable/layout", sanitize_lighttable_layout(layout));
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      d->base_layout = layout;
      dt_conf_set_int("plugins/lighttable/base_layout", layout);
    }

    dt_control_queue_redraw_center();
  }
  else
  {
    dt_control_queue_redraw_center();
  }

  _lib_lighttable_update_btn(self);
}

static gboolean _lib_lighttable_layout_btn_release(GtkWidget *w, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(d->combo_evt_reset) return FALSE;

  const gboolean active
      = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)); // note : this is the state before the change
  dt_lighttable_layout_t new_layout = DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
  if(!active)
  {
    // that means we want to activate the button
    if(w == d->layout_preview)
    {
      d->fullpreview_focus = dt_modifier_is(event->state, GDK_CONTROL_MASK);
      new_layout = DT_LIGHTTABLE_LAYOUT_PREVIEW;
    }
    else if(w == d->layout_culling_dynamic)
      new_layout = DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC;
  }
  else
  {
    // that means we want to deactivate the button
    if(w == d->layout_preview)
      new_layout = d->layout;
    else if(w == d->layout_culling_dynamic)
      new_layout = d->base_layout;
    else
    {
      // we can't exit from filemanager or zoomable
      return TRUE;
    }
  }

  _lib_lighttable_set_layout(self, new_layout);
  return TRUE;
}

static void _lib_lighttable_key_accel_toggle_filemanager(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
}

static void _lib_lighttable_key_accel_toggle_preview(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->fullpreview)
    _lib_lighttable_set_layout(self, d->layout);
  else
  {
    d->fullpreview_focus = FALSE;
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_PREVIEW);
  }
}

static void _lib_lighttable_key_accel_toggle_preview_focus(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->fullpreview)
    _lib_lighttable_set_layout(self, d->layout);
  else
  {
    d->fullpreview_focus = TRUE;
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_PREVIEW);
  }
}

static void _lib_lighttable_key_accel_toggle_culling_dynamic_mode(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  // if we are already in any culling layout, we return to the base layout
  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
  else
    _lib_lighttable_set_layout(self, d->base_layout);

  dt_control_queue_redraw_center();
}

static void _lib_lighttable_key_accel_toggle_culling_zoom_mode(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
  _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)g_malloc0(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->layout = d->base_layout = sanitize_lighttable_layout(dt_conf_get_int("plugins/lighttable/base_layout"));
  d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  // create the layouts icon list
  d->layout_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->layout_box, "lighttable-layouts-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->layout_box, TRUE, TRUE, 0);

  dt_action_t *ltv = &darktable.view_manager->proxy.lighttable.view->actions;
  dt_action_t *ac = NULL;

  d->layout_filemanager = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_grid, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle filemanager layout"), d->layout_filemanager, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_filemanager, 0, 0);
  dt_gui_add_help_link(d->layout_filemanager, dt_get_help_url("layout_filemanager"));
  gtk_widget_set_tooltip_text(d->layout_filemanager, _("click to enter filemanager layout."));
  g_signal_connect(G_OBJECT(d->layout_filemanager), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);
  gtk_box_pack_start(GTK_BOX(d->layout_box), d->layout_filemanager, TRUE, TRUE, 0);

  d->layout_culling_dynamic = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_dynamic, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle culling dynamic mode"), d->layout_culling_dynamic, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_culling_dynamic_mode, GDK_KEY_x, GDK_CONTROL_MASK);
  dt_gui_add_help_link(d->layout_culling_dynamic, dt_get_help_url("layout_culling"));
  g_signal_connect(G_OBJECT(d->layout_culling_dynamic), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);
  gtk_box_pack_start(GTK_BOX(d->layout_box), d->layout_culling_dynamic, TRUE, TRUE, 0);

  d->layout_preview = dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_fullpreview, 0, NULL);
  ac = dt_action_define(ltv, NULL, N_("toggle sticky preview mode"), d->layout_preview, NULL);
  dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_preview, 0, 0);
  dt_gui_add_help_link(d->layout_preview, dt_get_help_url("layout_preview"));
  g_signal_connect(G_OBJECT(d->layout_preview), "button-release-event",
                   G_CALLBACK(_lib_lighttable_layout_btn_release), self);
  gtk_box_pack_start(GTK_BOX(d->layout_box), d->layout_preview, TRUE, TRUE, 0);

  _lib_lighttable_update_btn(self);

  /* create horizontal zoom slider */
  d->zoom = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, DT_LIGHTTABLE_MAX_ZOOM, 1);
  gtk_widget_set_size_request(GTK_WIDGET(d->zoom), DT_PIXEL_APPLY_DPI(140), -1);
  gtk_scale_set_draw_value(GTK_SCALE(d->zoom), FALSE);
  gtk_range_set_increments(GTK_RANGE(d->zoom), 1, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom, TRUE, TRUE, 0);

  /* manual entry of the zoom level */
  d->zoom_entry = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(d->zoom_entry), 1.0);
  gtk_entry_set_max_length(GTK_ENTRY(d->zoom_entry), 2);
  gtk_entry_set_width_chars(GTK_ENTRY(d->zoom_entry), 3);
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->zoom_entry), 3);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->zoom), "value-changed", G_CALLBACK(_lib_lighttable_zoom_slider_changed), self);
  g_signal_connect(d->zoom_entry, "key-press-event", G_CALLBACK(_lib_lighttable_zoom_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->zoom), d->current_zoom);

  _lib_lighttable_zoom_slider_changed(GTK_RANGE(d->zoom), self); // the slider defaults to 1 and GTK doesn't
                                                                 // fire a value-changed signal when setting
                                                                 // it to 1 => empty text box

  gtk_widget_set_sensitive(d->zoom_entry, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && !d->fullpreview));
  gtk_widget_set_sensitive(d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && !d->fullpreview));

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
  darktable.view_manager->proxy.lighttable.get_zoom = _lib_lighttable_get_zoom;
  darktable.view_manager->proxy.lighttable.get_layout = _lib_lighttable_get_layout;
  darktable.view_manager->proxy.lighttable.set_layout = _lib_lighttable_set_layout;

  dt_action_register(ltv, N_("toggle culling zoom mode"), _lib_lighttable_key_accel_toggle_culling_zoom_mode,
                     GDK_KEY_less, 0);
  dt_action_register(ltv, N_("toggle sticky preview mode with focus detection"), _lib_lighttable_key_accel_toggle_preview_focus,
                     0, 0);

   /* create the grouping button */
  d->grouping_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_grouping, 0, NULL);
  dt_action_define(&darktable.control->actions_global, NULL, N_("grouping"), d->grouping_button, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(self->widget), d->grouping_button, FALSE, FALSE, 0);
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(d->grouping_button, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(d->grouping_button, _("collapse grouped images"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), darktable.gui->grouping);
  g_signal_connect(G_OBJECT(d->grouping_button), "clicked", G_CALLBACK(_lib_filter_grouping_button_clicked),
                   NULL);

  /* create the "show/hide overlays" button */
  d->overlays_button = dtgtk_button_new(dtgtk_cairo_paint_overlays, 0, NULL);
  dt_action_define(&darktable.control->actions_global, NULL, N_("thumbnail overlays options"), d->overlays_button, &dt_action_def_button);
  gtk_widget_set_tooltip_text(d->overlays_button, _("click to change the type of overlays shown on thumbnails"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->overlays_button, FALSE, FALSE, 0);
  d->over_popup = gtk_popover_new(d->overlays_button);
  gtk_widget_set_size_request(d->over_popup, 350, -1);
  g_object_set(G_OBJECT(d->over_popup), "transitions-enabled", FALSE, NULL);
  g_signal_connect(G_OBJECT(d->overlays_button), "clicked", G_CALLBACK(_overlays_show_popup), self);
  // we register size of overlay icon to keep in sync thumbtable overlays
  g_signal_connect(G_OBJECT(d->overlays_button), "size-allocate", G_CALLBACK(_main_icons_register_size), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_container_add(GTK_CONTAINER(d->over_popup), vbox);

#define NEW_RADIO(widget, box, callback, label)                                     \
  rb = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(rb), _(label)); \
  dt_action_define(ac, NULL, label, rb, &dt_action_def_button);                     \
  g_signal_connect(G_OBJECT(rb), "clicked", G_CALLBACK(callback), self);            \
  gtk_box_pack_start(GTK_BOX(box), rb, TRUE, TRUE, 0);                              \
  widget = rb;

  // thumbnails overlays
  d->thumbnails_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  ac = dt_action_section(&darktable.control->actions_global, N_("thumbnail overlays"));
  GtkWidget *rb = NULL;
  NEW_RADIO(d->over_r0, d->thumbnails_box, _overlays_toggle_button, N_("no overlays"));
  NEW_RADIO(d->over_r1, d->thumbnails_box, _overlays_toggle_button, N_("overlays on mouse hover"));
  NEW_RADIO(d->over_r2, d->thumbnails_box, _overlays_toggle_button, N_("permanent overlays"));

  gtk_box_pack_start(GTK_BOX(vbox), d->thumbnails_box, TRUE, TRUE, 0);
#undef NEW_RADIO

  gtk_widget_show_all(vbox);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _set_zoom(dt_lib_module_t *self, int zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
  {
    dt_conf_set_int("plugins/lighttable/images_in_row", zoom);
    dt_thumbtable_zoom_changed(dt_ui_thumbtable(darktable.gui->ui), d->current_zoom, zoom);
  }
}

static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int i = gtk_range_get_value(range);
  gchar *i_as_str = g_strdup_printf("%d", i);
  gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
  _set_zoom(self, i);
  d->current_zoom = i;
  g_free(i_as_str);
}

static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:
    {
      // reset
      int i = 0;
      if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        i = dt_conf_get_int("plugins/lighttable/culling_num_images");
      else
        i = dt_conf_get_int("plugins/lighttable/images_in_row");
      gchar *i_as_str = g_strdup_printf("%d", i);
      gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
      g_free(i_as_str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // apply zoom level
      const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->zoom_entry));
      int i = atoi(value);
      gtk_range_set_value(GTK_RANGE(d->zoom), i);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow 0 .. 9, left/right movement using arrow keys and del/backspace
    case GDK_KEY_0:
    case GDK_KEY_KP_0:
    case GDK_KEY_1:
    case GDK_KEY_KP_1:
    case GDK_KEY_2:
    case GDK_KEY_KP_2:
    case GDK_KEY_3:
    case GDK_KEY_KP_3:
    case GDK_KEY_4:
    case GDK_KEY_KP_4:
    case GDK_KEY_5:
    case GDK_KEY_KP_5:
    case GDK_KEY_6:
    case GDK_KEY_KP_6:
    case GDK_KEY_7:
    case GDK_KEY_KP_7:
    case GDK_KEY_8:
    case GDK_KEY_KP_8:
    case GDK_KEY_9:
    case GDK_KEY_KP_9:

    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Delete:
    case GDK_KEY_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->layout;
}

static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  d->current_zoom = zoom;
}

static gint _lib_lighttable_get_zoom(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->current_zoom;
}

#ifdef USE_LUA

static int layout_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const dt_lighttable_layout_t tmp = _lib_lighttable_get_layout(self);
  if(lua_gettop(L) > 0){
    dt_lighttable_layout_t value;
    luaA_to(L, dt_lighttable_layout_t, &value, 1);
    _lib_lighttable_set_layout(self, value);
  }
  luaA_push(L, dt_lighttable_layout_t, &tmp);
  return 1;
}

static int zoom_level_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const gint tmp = _lib_lighttable_get_zoom(self);
  if(lua_gettop(L) > 0){
    int value;
    luaA_to(L, int, &value, 1);
    _lib_lighttable_set_zoom(self, value);
  }
  luaA_push(L, int, &tmp);
  return 1;
}

static int grouping_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->grouping);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->grouping != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), value);
    }
  }
  return 0;
}

static int show_overlays_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->show_overlays);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->show_overlays != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), value);
    }
  }
  return 0;
}


void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, layout_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "layout");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, zoom_level_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "zoom_level");

  luaA_enum(L,dt_lighttable_layout_t);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_FIRST);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_PREVIEW);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_LAST);

  lua_pushcfunction(L, grouping_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "grouping");
  lua_pushcfunction(L, show_overlays_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "show_overlays");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-grouping_toggle");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-overlay_toggle");
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
