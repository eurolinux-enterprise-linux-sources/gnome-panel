/* Gnome panel: general applet functionality
 * (C) 1997 the Free Software Foundation
 *
 * Authors:  George Lebl
 *           Federico Mena
 *           Miguel de Icaza
 */

#include <config.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <gdk/gdkx.h>

#include <libpanel-util/panel-show.h>

#include "button-widget.h"
#include "drawer.h"
#include "launcher.h"
#include "panel-addto.h"
#include "panel-gconf.h"
#include "panel-config-global.h"
#include "panel-applet-frame.h"
#include "panel-action-button.h"
#include "panel-menu-bar.h"
#include "panel-separator.h"
#include "panel-compatibility.h"
#include "panel-toplevel.h"
#include "panel-util.h"
#include "panel-profile.h"
#include "panel-menu-button.h"
#include "panel-globals.h"
#include "panel-properties-dialog.h"
#include "panel-lockdown.h"

#define SMALL_ICON_SIZE 20

static GSList *registered_applets = NULL;
static GSList *queued_position_saves = NULL;
static guint   queued_position_source = 0;


static void
panel_applet_set_dnd_enabled (AppletInfo *info,
			      gboolean    dnd_enabled)
{
	switch (info->type) {
	case PANEL_OBJECT_DRAWER:
		panel_drawer_set_dnd_enabled (info->data, dnd_enabled);
		break;
	case PANEL_OBJECT_MENU:
		panel_menu_button_set_dnd_enabled (PANEL_MENU_BUTTON (info->widget),
						   dnd_enabled);
		break;
	case PANEL_OBJECT_LAUNCHER:
		panel_launcher_set_dnd_enabled (info->data, dnd_enabled);
		break;
	case PANEL_OBJECT_BONOBO:
		break;
	case PANEL_OBJECT_LOGOUT:
	case PANEL_OBJECT_LOCK:
	case PANEL_OBJECT_ACTION:
		panel_action_button_set_dnd_enabled (PANEL_ACTION_BUTTON (info->widget),
						     dnd_enabled);
		break;
	case PANEL_OBJECT_MENU_BAR:
	case PANEL_OBJECT_SEPARATOR:
		break;
	default:
		g_assert_not_reached ();
		break;
	}

}

gboolean
panel_applet_toggle_locked (AppletInfo *info)
{
	PanelWidget *panel_widget;
	gboolean     locked;

	panel_widget = PANEL_WIDGET (info->widget->parent);
	
	locked = panel_widget_toggle_applet_locked (panel_widget, info->widget);

	panel_applet_save_position (info, info->id, TRUE);
	panel_applet_set_dnd_enabled (info, !locked);

	return locked;
}

static void
panel_applet_lock (GtkCheckMenuItem *menuitem,
		   AppletInfo       *info)
{
	gboolean locked;

	locked = panel_applet_toggle_locked (info);

	gtk_check_menu_item_set_active (menuitem, locked);

	if (info->move_item)
		gtk_widget_set_sensitive (info->move_item, !locked);
}

static void
move_applet_callback (GtkWidget *widget, AppletInfo *info)
{
	PanelWidget *panel;

	g_return_if_fail (info != NULL);
	g_return_if_fail (info->widget != NULL);
	g_return_if_fail (info->widget->parent != NULL);
	g_return_if_fail (PANEL_IS_WIDGET (info->widget->parent));

	panel = PANEL_WIDGET (info->widget->parent);

	panel_widget_applet_drag_start (panel, info->widget,
					PW_DRAG_OFF_CENTER,
					GDK_CURRENT_TIME);
}

/* permanently remove an applet - all non-permanent
 * cleanups should go in panel_applet_destroy()
 */
void
panel_applet_clean (AppletInfo *info)
{
	g_return_if_fail (info != NULL);

	if (info->type == PANEL_OBJECT_LAUNCHER)
		panel_launcher_delete (info->data);

	if (info->widget) {
		GtkWidget *widget = info->widget;

		info->widget = NULL;
		gtk_widget_destroy (widget);
	}
}

static void
panel_applet_recreate_menu (AppletInfo	*info)
{
	GList *l;

	if (!info->menu)
		return;

	for (l = info->user_menu; l; l = l->next) {
		AppletUserMenu *menu = l->data;

		menu->menuitem =NULL;
		menu->submenu =NULL;
	}

	g_object_unref (info->menu);
	info->menu = panel_applet_create_menu (info);
}

static void
panel_applet_locked_change_notify (GConfClient *client,
				   guint        cnxn_id,
				   GConfEntry  *entry,
				   GtkWidget   *applet)
{
	GConfValue  *value;
	gboolean     locked;
	gboolean     applet_locked;
	AppletInfo  *info;
	PanelWidget *panel_widget;

	g_assert (applet != NULL);

	info = (AppletInfo  *) g_object_get_data (G_OBJECT (applet),
						  "applet_info");
	if (info == NULL)
		return;

	value = gconf_entry_get_value (entry);
	if (value == NULL || value->type != GCONF_VALUE_BOOL)
		return;

	locked = gconf_value_get_bool (value);

	panel_widget = PANEL_WIDGET (info->widget->parent);
	applet_locked = panel_widget_get_applet_locked (panel_widget,
							info->widget);

	if ((locked && applet_locked) || !(locked || applet_locked))
		return;

	panel_applet_toggle_locked (info);

	if (info->type == PANEL_OBJECT_BONOBO)
		panel_applet_frame_sync_menu_state (PANEL_APPLET_FRAME (info->widget));
	else
		panel_applet_recreate_menu (info);
}

static void
applet_remove_callback (GtkWidget  *widget,
			AppletInfo *info)
{

	if (info->type == PANEL_OBJECT_DRAWER)
		drawer_query_deletion (info->data);
	else
		panel_profile_delete_object (info);
}

static inline GdkScreen *
applet_user_menu_get_screen (AppletUserMenu *menu)
{
	PanelWidget *panel_widget;

	panel_widget = PANEL_WIDGET (menu->info->widget->parent);

	return gtk_window_get_screen (GTK_WINDOW (panel_widget->toplevel));
}

static void
applet_callback_callback (GtkWidget      *widget,
			  AppletUserMenu *menu)
{
	GdkScreen *screen;

	g_return_if_fail (menu->info != NULL);

	screen = applet_user_menu_get_screen (menu);

	switch (menu->info->type) {
	case PANEL_OBJECT_LAUNCHER:
		if (!strcmp (menu->name, "launch"))
			launcher_launch (menu->info->data, widget);
		else if (!strcmp (menu->name, "properties"))
			launcher_properties (menu->info->data);
		break;
	case PANEL_OBJECT_DRAWER: 
		if (strcmp (menu->name, "add") == 0) {
			Drawer *drawer = menu->info->data;

			panel_addto_present (GTK_MENU_ITEM (widget),
					     panel_toplevel_get_panel_widget (drawer->toplevel));
		} else if (strcmp (menu->name, "properties") == 0) {
			Drawer *drawer = menu->info->data;

			panel_properties_dialog_present (drawer->toplevel);
		} else if (strcmp (menu->name, "help") == 0) {
			panel_show_help (screen,
					 "user-guide", "gospanel-18", NULL);
		}
		break;
	case PANEL_OBJECT_MENU:
		panel_menu_button_invoke_menu (
			PANEL_MENU_BUTTON (menu->info->widget), menu->name);
		break;
	case PANEL_OBJECT_ACTION:
	case PANEL_OBJECT_LOGOUT:
	case PANEL_OBJECT_LOCK:
		panel_action_button_invoke_menu (
			PANEL_ACTION_BUTTON (menu->info->widget), menu->name);
		break;
	case PANEL_OBJECT_MENU_BAR:
		panel_menu_bar_invoke_menu (
			PANEL_MENU_BAR (menu->info->widget), menu->name);
		break;

	case PANEL_OBJECT_BONOBO:
		/*
		 * Applet's menu's are handled differently
		 */
		break;
	case PANEL_OBJECT_SEPARATOR:
		break;
	default:
		g_assert_not_reached ();
		break;
	}
}

static void
applet_menu_show (GtkWidget *w,
		  AppletInfo *info)
{
	PanelWidget *panel_widget;

	panel_widget = PANEL_WIDGET (info->widget->parent);

	panel_toplevel_push_autohide_disabler (panel_widget->toplevel);
}


static void
applet_menu_deactivate (GtkWidget *w,
			AppletInfo *info)
{
	PanelWidget *panel_widget;

	panel_widget = PANEL_WIDGET (info->widget->parent);

	panel_toplevel_pop_autohide_disabler (panel_widget->toplevel);
}

AppletUserMenu *
panel_applet_get_callback (GList      *user_menu,
			   const char *name)
{
	GList *l;

	for (l = user_menu; l; l = l->next) {
		AppletUserMenu *menu = l->data;

		if (strcmp (menu->name, name) == 0)
			return menu;
	}

	return NULL;	
}

void
panel_applet_add_callback (AppletInfo          *info,
			   const char          *callback_name,
			   const char          *stock_item,
			   const char          *menuitem_text,
			   CallbackEnabledFunc  is_enabled_func)
{
	AppletUserMenu *menu;

	g_return_if_fail (info != NULL);
	g_return_if_fail (panel_applet_get_callback (info->user_menu,
						     callback_name) == NULL);

	menu                  = g_new0 (AppletUserMenu, 1);
	menu->name            = g_strdup (callback_name);
	menu->stock_item      = g_strdup (stock_item);
	menu->text            = g_strdup (menuitem_text);
	menu->is_enabled_func = is_enabled_func;
	menu->sensitive       = TRUE;
	menu->info            = info;
	menu->menuitem        = NULL;
	menu->submenu         = NULL;

	info->user_menu = g_list_append (info->user_menu, menu);

	panel_applet_recreate_menu (info);
}

static void
setup_an_item (AppletUserMenu *menu,
	       GtkWidget      *submenu,
	       int             is_submenu)
{
	GtkWidget *image = NULL;

	menu->menuitem = gtk_image_menu_item_new_with_mnemonic (menu->text);
	if (menu->stock_item && menu->stock_item [0]) {
		image = gtk_image_new_from_stock (menu->stock_item,
						  GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menu->menuitem),
					       image);
	}
	gtk_widget_show (menu->menuitem);

	g_signal_connect (G_OBJECT (menu->menuitem), "destroy",
			  G_CALLBACK (gtk_widget_destroyed),
			  &menu->menuitem);

	if(submenu)
		gtk_menu_shell_append (GTK_MENU_SHELL (submenu), menu->menuitem);

	/*if an item not a submenu*/
	if (!is_submenu) {
		g_signal_connect (menu->menuitem, "activate",
				  G_CALLBACK (applet_callback_callback),
				  menu);
		g_signal_connect (submenu, "destroy",
				  G_CALLBACK (gtk_widget_destroyed),
				  &menu->submenu);
	/* if the item is a submenu and doesn't have it's menu
	   created yet*/
	} else if (!menu->submenu) {
		menu->submenu = gtk_menu_new ();
	}

	if(menu->submenu) {
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu->menuitem),
					  menu->submenu);
		g_signal_connect (G_OBJECT (menu->submenu), "destroy",
				    G_CALLBACK (gtk_widget_destroyed),
				    &menu->submenu);
	}
	
	gtk_widget_set_sensitive(menu->menuitem,menu->sensitive);
}

static void
add_to_submenus (AppletInfo *info,
		 const char *path,
		 const char *name,
		 AppletUserMenu *menu,
		 GtkWidget *submenu,
		 GList *user_menu)
{
	char *n = g_strdup (name);
	char *p = strchr (n, '/');
	char *t;
	AppletUserMenu *s_menu;

	/*this is the last one*/
	if (p == NULL) {
		g_free (n);
		setup_an_item (menu, submenu, FALSE);
		return;
	}
	
	/*this is the last one and we are a submenu, we have already been
	  set up*/
	if(p==(n + strlen(n) - 1)) {
		g_free(n);
		return;
	}
	
	*p = '\0';
	p++;
	
	t = g_strconcat (path, n, "/", NULL);
	s_menu = panel_applet_get_callback (user_menu, t);
	/*the user did not give us this sub menu, whoops, will create an empty
	  one then*/
	if (s_menu == NULL) {
		s_menu = g_new0 (AppletUserMenu,1);
		s_menu->name = g_strdup (t);
		s_menu->stock_item = NULL;
		s_menu->text = g_strdup (_("???"));
		s_menu->sensitive = TRUE;
		s_menu->info = info;
		s_menu->menuitem = NULL;
		s_menu->submenu = NULL;
		info->user_menu = g_list_append (info->user_menu,s_menu);
		user_menu = info->user_menu;
	}
	
	if (s_menu->submenu == NULL) {
		s_menu->submenu = gtk_menu_new ();
		/*a more elegant way to do this should be done
		  when I don't want to go to sleep */
		if (s_menu->menuitem != NULL) {
			gtk_widget_destroy (s_menu->menuitem);
			s_menu->menuitem = NULL;
		}
	}
	if (s_menu->menuitem == NULL)
		setup_an_item (s_menu, submenu, TRUE);
	
	add_to_submenus (info, t, p, menu, s_menu->submenu, user_menu);
	
	g_free(t);
	g_free(n);
}

GtkWidget *
panel_applet_create_menu (AppletInfo *info)
{
	GtkWidget   *menu;
	GtkWidget   *menuitem;
	GList       *l;
	PanelWidget *panel_widget;
	gboolean     added_anything = FALSE;

	panel_widget = PANEL_WIDGET (info->widget->parent);

	menu = g_object_ref_sink (gtk_menu_new ());

	/* connect the show & deactivate signal, so that we can "disallow" and
	 * "re-allow" autohide when the menu is shown/deactivated.
	 */
	g_signal_connect (menu, "show",
			  G_CALLBACK (applet_menu_show), info);
	g_signal_connect (menu, "deactivate",
			  G_CALLBACK (applet_menu_deactivate), info);

	for (l = info->user_menu; l; l = l->next) {
		AppletUserMenu *user_menu = l->data;

		if (user_menu->is_enabled_func && !user_menu->is_enabled_func ())
			continue;

		add_to_submenus (info, "", user_menu->name, user_menu, 
				 menu, info->user_menu);

		added_anything = TRUE;
	}

	if (!panel_lockdown_get_locked_down ()) {
		GtkWidget *image;
		gboolean   locked;
		gboolean   lockable;
		gboolean   movable;
		gboolean   removable;

		lockable = panel_applet_lockable (info);
		movable = panel_applet_can_freely_move (info);
		removable = panel_profile_id_lists_are_writable ();

		locked = panel_widget_get_applet_locked (panel_widget, info->widget);

		if (added_anything) {
			menuitem = gtk_separator_menu_item_new ();
			gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
			gtk_widget_show (menuitem);
		}

		menuitem = gtk_image_menu_item_new_with_mnemonic (_("_Remove From Panel"));
		image = gtk_image_new_from_stock (GTK_STOCK_REMOVE,
						  GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menuitem),
					       image);
		g_signal_connect (menuitem, "activate",
				  G_CALLBACK (applet_remove_callback), info);
		gtk_widget_show (menuitem);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
		gtk_widget_set_sensitive (menuitem, (!locked || lockable) && removable);
		
		menuitem = gtk_menu_item_new_with_mnemonic (_("_Move"));
		g_signal_connect (menuitem, "activate",
				  G_CALLBACK (move_applet_callback), info);
		gtk_widget_show (menuitem);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
		gtk_widget_set_sensitive (menuitem, !locked && movable);

		g_assert (info->move_item == NULL);

		info->move_item = menuitem;
		g_object_add_weak_pointer (G_OBJECT (menuitem),
					   (gpointer *) &info->move_item);

		menuitem = gtk_separator_menu_item_new ();
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
		gtk_widget_show (menuitem);

		menuitem = gtk_check_menu_item_new_with_mnemonic (_("Loc_k To Panel"));
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menuitem),
						locked);
		g_signal_connect (menuitem, "toggled",
				  G_CALLBACK (panel_applet_lock), info);
		gtk_widget_show (menuitem);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menuitem);
		gtk_widget_set_sensitive (menuitem, lockable);

		added_anything = TRUE;
	}

	if ( ! added_anything) {
		g_object_unref (menu);
		return NULL;
	}

	return menu;
}

void
panel_applet_menu_set_recurse (GtkMenu     *menu,
			       const gchar *key,
			       gpointer     data)
{
	GList *children;
	GList *l;

	g_object_set_data (G_OBJECT (menu), key, data);

	children = gtk_container_get_children (GTK_CONTAINER (menu));

	for (l = children; l; l = l->next) {
		GtkWidget *submenu = GTK_MENU_ITEM (l->data)->submenu;

		if (submenu)
			panel_applet_menu_set_recurse (
				GTK_MENU (submenu), key, data);
	}

	g_list_free (children);
}

void
panel_applet_position_menu (GtkMenu   *menu,
			    int       *x,
			    int       *y,
			    gboolean  *push_in,
			    GtkWidget *applet)
{
	GtkRequisition  requisition;
	GdkScreen      *screen;
	int             menu_x = 0;
	int             menu_y = 0;
	int             pointer_x;
	int             pointer_y;

	g_return_if_fail (PANEL_IS_WIDGET (applet->parent));

	screen = gtk_widget_get_screen (applet);

	gtk_widget_size_request (GTK_WIDGET (menu), &requisition);

	gdk_window_get_origin (applet->window, &menu_x, &menu_y);
	gtk_widget_get_pointer (applet, &pointer_x, &pointer_y);

	if (!gtk_widget_get_has_window (applet)) {
		menu_x += applet->allocation.x;
		menu_y += applet->allocation.y;
	}

	if (PANEL_WIDGET (applet->parent)->orient == GTK_ORIENTATION_HORIZONTAL) {
		if (gtk_widget_get_direction (GTK_WIDGET (menu)) != GTK_TEXT_DIR_RTL) {
			if (pointer_x < applet->allocation.width &&
			    requisition.width < pointer_x)
				menu_x += MIN (pointer_x,
					       applet->allocation.width - requisition.width);
		} else {
			menu_x += applet->allocation.width - requisition.width;
			if (pointer_x > 0 && pointer_x < applet->allocation.width &&
			    pointer_x < applet->allocation.width - requisition.width) {
				menu_x -= MIN (applet->allocation.width - pointer_x,
					       applet->allocation.width - requisition.width);
			}
		}
		menu_x = MIN (menu_x, gdk_screen_get_width (screen) - requisition.width);

		if (menu_y > gdk_screen_get_height (screen) / 2)
			menu_y -= requisition.height;
		else
			menu_y += applet->allocation.height;
	} else {
		if (pointer_y < applet->allocation.height &&
		    requisition.height < pointer_y)
			menu_y += MIN (pointer_y, applet->allocation.height - requisition.height);
		menu_y = MIN (menu_y, gdk_screen_get_height (screen) - requisition.height);

		if (menu_x > gdk_screen_get_width (screen) / 2)
			menu_x -= requisition.width;
		else
			menu_x += applet->allocation.width;
	}

	*x = menu_x;
	*y = menu_y;
	*push_in = TRUE;
}

static void
applet_show_menu (AppletInfo     *info,
		  GdkEventButton *event)
{
	PanelWidget *panel_widget;

	g_return_if_fail (info != NULL);

	panel_widget = PANEL_WIDGET (info->widget->parent);

	if (info->menu == NULL)
		info->menu = panel_applet_create_menu (info);

	if (info->menu == NULL)
		return;

	panel_applet_menu_set_recurse (GTK_MENU (info->menu),
				       "menu_panel",
				       info->widget->parent);

	gtk_menu_set_screen (GTK_MENU (info->menu),
			     gtk_window_get_screen (GTK_WINDOW (panel_widget->toplevel)));

	if (!gtk_widget_get_realized (info->menu))
		gtk_widget_show (info->menu);

	gtk_menu_popup (GTK_MENU (info->menu),
			NULL,
			NULL,
			(GtkMenuPositionFunc) panel_applet_position_menu,
			info->widget,
			event->button,
			event->time);
}

static gboolean
applet_do_popup_menu (GtkWidget      *widget,
		      GdkEventButton *event,
		      AppletInfo     *info)
{
	if (panel_applet_is_in_drag ())
		return FALSE;

	if (info->type == PANEL_OBJECT_BONOBO)
		return FALSE;

	applet_show_menu (info, event);

	return TRUE;
}

static gboolean
applet_popup_menu (GtkWidget      *widget,
		   AppletInfo     *info)
{
	GdkEventButton event;

	event.button = 3;
	event.time = GDK_CURRENT_TIME;

	return applet_do_popup_menu (widget, &event, info);
}

static gboolean
applet_button_press (GtkWidget      *widget,
		     GdkEventButton *event,
		     AppletInfo     *info)
{
	if (event->button == 3)
		return applet_do_popup_menu (widget, event, info);

	return FALSE;
}

static void
panel_applet_destroy (GtkWidget  *widget,
		      AppletInfo *info)
{
	GList *l;

	g_return_if_fail (info != NULL);

	info->widget = NULL;

	registered_applets = g_slist_remove (registered_applets, info);

	queued_position_saves =
		g_slist_remove (queued_position_saves, info);

	if (info->type == PANEL_OBJECT_DRAWER) {
		Drawer *drawer = info->data;

		if (drawer->toplevel) {
			PanelWidget *panel_widget;

			panel_widget = panel_toplevel_get_panel_widget (
							drawer->toplevel);
			panel_widget->master_widget = NULL;

			gtk_widget_destroy (GTK_WIDGET (drawer->toplevel));
			drawer->toplevel = NULL;
		}
	}

	if (info->type != PANEL_OBJECT_BONOBO)
		panel_lockdown_notify_remove (G_CALLBACK (panel_applet_recreate_menu),
					      info);

	if (info->menu)
		g_object_unref (info->menu);
	info->menu = NULL;

	if (info->data_destroy)
		info->data_destroy (info->data);
	info->data = NULL;

	for (l = info->user_menu; l != NULL; l = l->next) {
		AppletUserMenu *umenu = l->data;

		g_free (umenu->name);
		g_free (umenu->stock_item);
		g_free (umenu->text);

		g_free (umenu);
	}

	g_list_free (info->user_menu);
	info->user_menu = NULL;

	g_free (info->id);
	info->id = NULL;

	g_free (info);
}

typedef struct {
	char            *id;
	PanelObjectType  type;
	char            *toplevel_id;
	int              position;
	guint            right_stick : 1;
	guint            locked : 1;
} PanelAppletToLoad;

/* Each time those lists get both empty,
 * panel_applet_queue_initial_unhide_toplevels() should be called */
static GSList  *panel_applets_to_load = NULL;
static GSList  *panel_applets_loading = NULL;
/* We have a timeout to always unhide toplevels after a delay, in case of some
 * blocking applet */
#define         UNHIDE_TOPLEVELS_TIMEOUT_SECONDS 5
static guint    panel_applet_unhide_toplevels_timeout = 0;

static gboolean panel_applet_have_load_idle = FALSE;

static void
free_applet_to_load (PanelAppletToLoad *applet)
{
	g_free (applet->id);
	applet->id = NULL;

	g_free (applet->toplevel_id);
	applet->toplevel_id = NULL;

	g_free (applet);
}

gboolean
panel_applet_on_load_queue (const char *id)
{
	GSList *li;
	for (li = panel_applets_to_load; li != NULL; li = li->next) {
		PanelAppletToLoad *applet = li->data;
		if (strcmp (applet->id, id) == 0)
			return TRUE;
	}
	return FALSE;
}

/* This doesn't do anything if the initial unhide already happened */
static gboolean
panel_applet_queue_initial_unhide_toplevels (gpointer user_data)
{
	GSList *l;

	if (panel_applet_unhide_toplevels_timeout != 0) {
		g_source_remove (panel_applet_unhide_toplevels_timeout);
		panel_applet_unhide_toplevels_timeout = 0;
	}

	for (l = panel_toplevel_list_toplevels (); l != NULL; l = l->next)
		panel_toplevel_queue_initial_unhide ((PanelToplevel *) l->data);

	return FALSE;
}

void
panel_applet_stop_loading (const char *id)
{
	PanelAppletToLoad *applet;
	GSList *l;

	for (l = panel_applets_loading; l; l = l->next) {
		applet = l->data;

		if (strcmp (applet->id, id) == 0)
			break;
	}

	/* this can happen if we reload an applet after it crashed,
	 * for example */
	if (l != NULL) {
		panel_applets_loading = g_slist_delete_link (panel_applets_loading, l);
		free_applet_to_load (applet);
	}

	if (panel_applets_loading == NULL && panel_applets_to_load == NULL)
		panel_applet_queue_initial_unhide_toplevels (NULL);
}

static gboolean
panel_applet_load_idle_handler (gpointer dummy)
{
	PanelAppletToLoad *applet = NULL;
	PanelToplevel     *toplevel = NULL;
	PanelWidget       *panel_widget;
	GSList            *l;

	if (!panel_applets_to_load) {
		panel_applet_have_load_idle = FALSE;
		return FALSE;
	}

	for (l = panel_applets_to_load; l; l = l->next) {
		applet = l->data;

		toplevel = panel_profile_get_toplevel_by_id (applet->toplevel_id);
		if (toplevel)
			break;
	}

	if (!l) {
		/* All the rest of the applets don't have a panel */
		for (l = panel_applets_to_load; l; l = l->next)
			free_applet_to_load (l->data);
		g_slist_free (panel_applets_to_load);
		panel_applets_to_load = NULL;
		panel_applet_have_load_idle = FALSE;

		if (panel_applets_loading == NULL) {
			/* unhide any potential initially hidden toplevel */
			panel_applet_queue_initial_unhide_toplevels (NULL);
		}

		return FALSE;
	}

	panel_applets_to_load = g_slist_delete_link (panel_applets_to_load, l);
	panel_applets_loading = g_slist_append (panel_applets_loading, applet);

	panel_widget = panel_toplevel_get_panel_widget (toplevel);

	if (applet->right_stick) {
		if (!panel_widget->packed)
			applet->position = panel_widget->size - applet->position;
		else
			applet->position = -1;
	}

	switch (applet->type) {
	case PANEL_OBJECT_BONOBO:
		panel_applet_frame_load_from_gconf (
					panel_widget,
					applet->locked,
					applet->position,
					applet->id);
		break;
	case PANEL_OBJECT_DRAWER:
		drawer_load_from_gconf (panel_widget,
					applet->locked,
					applet->position,
					applet->id);
		break;
	case PANEL_OBJECT_MENU:
		panel_menu_button_load_from_gconf (panel_widget,
						   applet->locked,
						   applet->position,
						   TRUE,
						   applet->id);
		break;
	case PANEL_OBJECT_LAUNCHER:
		launcher_load_from_gconf (panel_widget,
					  applet->locked,
					  applet->position,
					  applet->id);
		break;
	case PANEL_OBJECT_LOGOUT:
	case PANEL_OBJECT_LOCK:
		panel_action_button_load_compatible (
				applet->type,
				panel_widget,
				applet->locked,
				applet->position,
				TRUE,
				applet->id);
		break;
	case PANEL_OBJECT_ACTION:
		panel_action_button_load_from_gconf (
				panel_widget,
				applet->locked,
				applet->position,
				TRUE,
				applet->id);
		break;
	case PANEL_OBJECT_MENU_BAR:
		panel_menu_bar_load_from_gconf (
				panel_widget,
				applet->locked,
				applet->position,
				TRUE,
				applet->id);
		break;
	case PANEL_OBJECT_SEPARATOR:
		panel_separator_load_from_gconf (panel_widget,
						 applet->locked,
						 applet->position,
						 applet->id);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* Only the bonobo applets will do a late stop_loading */
	if (applet->type != PANEL_OBJECT_BONOBO)
		panel_applet_stop_loading (applet->id);

	return TRUE;
}

void
panel_applet_queue_applet_to_load (const char      *id,
				   PanelObjectType  type,
				   const char      *toplevel_id,
				   int              position,
				   gboolean         right_stick,
				   gboolean         locked)
{
	PanelAppletToLoad *applet;

	if (!toplevel_id) {
		g_warning ("No toplevel on which to load object '%s'\n", id);
		return;
	}

	applet = g_new0 (PanelAppletToLoad, 1);

	applet->id          = g_strdup (id);
	applet->type        = type;
	applet->toplevel_id = g_strdup (toplevel_id);
	applet->position    = position;
	applet->right_stick = right_stick != FALSE;
	applet->locked      = locked != FALSE;

	panel_applets_to_load = g_slist_prepend (panel_applets_to_load, applet);
}

static int
panel_applet_compare (const PanelAppletToLoad *a,
		      const PanelAppletToLoad *b)
{
	int c;

	if ((c = strcmp (a->toplevel_id, b->toplevel_id)))
		return c;
	else if (a->right_stick != b->right_stick)
		return b->right_stick ? -1 : 1;
	else
		return a->position - b->position;
}

void
panel_applet_load_queued_applets (gboolean initial_load)
{
	if (!panel_applets_to_load) {
		panel_applet_queue_initial_unhide_toplevels (NULL);
		return;
	}

	if (initial_load && panel_applets_to_load) {
		/* Install a timeout to make sure we don't block the
		 * unhiding because of an applet that doesn't load */
		panel_applet_unhide_toplevels_timeout =
			g_timeout_add_seconds (UNHIDE_TOPLEVELS_TIMEOUT_SECONDS,
					       panel_applet_queue_initial_unhide_toplevels,
					       NULL);
	}

	panel_applets_to_load = g_slist_sort (panel_applets_to_load,
					      (GCompareFunc) panel_applet_compare);

	if ( ! panel_applet_have_load_idle) {
		/* on panel startup, we don't care about redraws of the
		 * toplevels since they are hidden, so we give a higher
		 * priority to loading of applets */
		if (initial_load)
			g_idle_add_full (G_PRIORITY_HIGH_IDLE,
					 panel_applet_load_idle_handler,
					 NULL, NULL);
		else
			g_idle_add (panel_applet_load_idle_handler, NULL);

		panel_applet_have_load_idle = TRUE;
	}
}

static G_CONST_RETURN char *
panel_applet_get_toplevel_id (AppletInfo *applet)
{
	PanelWidget *panel_widget;

	g_return_val_if_fail (applet != NULL, NULL);
	g_return_val_if_fail (GTK_IS_WIDGET (applet->widget), NULL);

	panel_widget = PANEL_WIDGET (applet->widget->parent);
	if (!panel_widget)
		return NULL;

	return panel_profile_get_toplevel_id (panel_widget->toplevel);
}

static gboolean
panel_applet_position_save_timeout (gpointer dummy)
{
	GSList *l;

	queued_position_source = 0;

	for (l = queued_position_saves; l; l = l->next) {
		AppletInfo *info = l->data;

		panel_applet_save_position (info, info->id, TRUE);
	}

	g_slist_free (queued_position_saves);
	queued_position_saves = NULL;

	return FALSE;
}

void
panel_applet_save_position (AppletInfo *applet_info,
			    const char *id,
			    gboolean    immediate)
{
	PanelGConfKeyType  key_type;
	GConfClient       *client;
	PanelWidget       *panel_widget;
	const char        *key;
	const char        *toplevel_id;
	char              *old_toplevel_id;
	gboolean           right_stick;
	gboolean           locked;
	int                position;

	g_return_if_fail (applet_info != NULL);

	if (!immediate) {
		if (!queued_position_source)
			queued_position_source =
				g_timeout_add_seconds (1,
						       (GSourceFunc) panel_applet_position_save_timeout,
						       NULL);

		if (!g_slist_find (queued_position_saves, applet_info))
			queued_position_saves =
				g_slist_prepend (queued_position_saves, applet_info);

		return;
	}

	if (!(toplevel_id = panel_applet_get_toplevel_id (applet_info)))
		return;

	client  = panel_gconf_get_client ();

	key_type = applet_info->type == PANEL_OBJECT_BONOBO ? PANEL_GCONF_APPLETS : PANEL_GCONF_OBJECTS;
	
	panel_widget = PANEL_WIDGET (applet_info->widget->parent);

	/* FIXME: Instead of getting keys, comparing and setting, there
	   should be a dirty flag */

	key = panel_gconf_full_key (key_type, id, "toplevel_id");
	old_toplevel_id = gconf_client_get_string (client, key, NULL);
	if (old_toplevel_id == NULL || strcmp (old_toplevel_id, toplevel_id) != 0)
		gconf_client_set_string (client, key, toplevel_id, NULL);
	g_free (old_toplevel_id);

	/* Note: changing some properties of the panel that may not be locked down
	   (e.g. background) can change the state of the "panel_right_stick" and
	   "position" properties of an applet that may in fact be locked down.
	   So check if these are writable before attempting to write them */

	right_stick = panel_is_applet_right_stick (applet_info->widget) ? 1 : 0;
	key = panel_gconf_full_key (
			key_type, id, "panel_right_stick");
	if (gconf_client_key_is_writable (client, key, NULL) &&
	    (gconf_client_get_bool (client, key, NULL) ? 1 : 0) != right_stick)
		gconf_client_set_bool (client, key, right_stick, NULL);

	position = panel_applet_get_position (applet_info);
	if (right_stick && !panel_widget->packed)
		position = panel_widget->size - position;

	key = panel_gconf_full_key (key_type, id, "position");
	if (gconf_client_key_is_writable (client, key, NULL) &&
	    gconf_client_get_int (client, key, NULL) != position)
		gconf_client_set_int (client, key, position, NULL);
	
	locked = panel_widget_get_applet_locked (panel_widget, applet_info->widget) ? 1 : 0;
	key = panel_gconf_full_key (key_type, id, "locked");
	if (gconf_client_get_bool (client, key, NULL) ? 1 : 0 != locked)
		gconf_client_set_bool (client, key, locked, NULL);
}

const char *
panel_applet_get_id (AppletInfo *info)
{
	if (!info)
		return NULL;

	return info->id;
}

const char *
panel_applet_get_id_by_widget (GtkWidget *applet_widget)
{
	GSList *l;

	if (!applet_widget)
		return NULL;

	for (l = registered_applets; l; l = l->next) {
		AppletInfo *info = l->data;

		if (info->widget == applet_widget)
			return info->id;
	}

	return NULL;
}

AppletInfo *
panel_applet_get_by_id (const char *id)
{
	GSList *l;

	for (l = registered_applets; l; l = l->next) {
		AppletInfo *info = l->data;

		if (!strcmp (info->id, id))
			return info;
	}

	return NULL;
}

GSList *
panel_applet_list_applets (void)
{
	return registered_applets;
}

AppletInfo *
panel_applet_get_by_type (PanelObjectType object_type, GdkScreen *screen)
{
	GSList *l;

	for (l = registered_applets; l; l = l->next) {
		AppletInfo *info = l->data;

		if (info->type == object_type) {
			if (screen) {
				if (screen == gtk_widget_get_screen (info->widget))
					return info;
			} else
				return info;
		}
	}

	return NULL;
}

AppletInfo *
panel_applet_register (GtkWidget       *applet,
		       gpointer         data,
		       GDestroyNotify   data_destroy,
		       PanelWidget     *panel,
		       gboolean         locked,
		       gint             pos,
		       gboolean         exactpos,
		       PanelObjectType  type,
		       const char      *id)
{
	AppletInfo *info;
	const char *key;
	
	g_return_val_if_fail (applet != NULL && panel != NULL, NULL);

	if (gtk_widget_get_has_window (applet))
		gtk_widget_set_events (applet, (gtk_widget_get_events (applet) |
						APPLET_EVENT_MASK) &
				       ~( GDK_POINTER_MOTION_MASK |
					  GDK_POINTER_MOTION_HINT_MASK));

	info = g_new0 (AppletInfo, 1);
	info->type         = type;
	info->widget       = applet;
	info->menu         = NULL;
	info->data         = data;
	info->data_destroy = data_destroy;
	info->user_menu    = NULL;
	info->move_item    = NULL;
	info->id           = g_strdup (id);

	g_object_set_data (G_OBJECT (applet), "applet_info", info);

	if (type != PANEL_OBJECT_BONOBO)
		panel_lockdown_notify_add (G_CALLBACK (panel_applet_recreate_menu),
					   info);

	key = panel_gconf_full_key ((type == PANEL_OBJECT_BONOBO) ?
				     PANEL_GCONF_APPLETS : PANEL_GCONF_OBJECTS,
				    id, "locked");
	panel_gconf_notify_add_while_alive (key,
					    (GConfClientNotifyFunc) panel_applet_locked_change_notify,
					    G_OBJECT (applet));

	if (type == PANEL_OBJECT_DRAWER) {
		Drawer *drawer = data;
		PanelWidget *assoc_panel;

		assoc_panel = panel_toplevel_get_panel_widget (drawer->toplevel);

		g_object_set_data (G_OBJECT (applet),
				   PANEL_APPLET_ASSOC_PANEL_KEY, assoc_panel);
		assoc_panel->master_widget = applet;
		g_object_add_weak_pointer (
			G_OBJECT (applet), (gpointer *) &assoc_panel->master_widget);
	}

	g_object_set_data (G_OBJECT (applet),
			   PANEL_APPLET_FORBIDDEN_PANELS, NULL);

	registered_applets = g_slist_append (registered_applets, info);

	if (panel_widget_add (panel, applet, locked, pos, exactpos) == -1 &&
	    panel_widget_add (panel, applet, locked, 0, TRUE) == -1) {
		GSList *l;

		for (l = panels; l; l = l->next) {
			panel = PANEL_WIDGET (l->data);

			if (panel_widget_add (panel, applet, locked, 0, TRUE) != -1)
				break;
		}

		if (!l) {
			g_warning (_("Cannot find an empty spot"));
			panel_profile_delete_object (info);
			return NULL;
		}
	}

	if (BUTTON_IS_WIDGET (applet) ||
	    gtk_widget_get_has_window (applet)) {
		g_signal_connect (applet, "button_press_event",
				  G_CALLBACK (applet_button_press),
				  info);

		g_signal_connect (applet, "popup_menu",
				  G_CALLBACK (applet_popup_menu),
				  info);
	}

	g_signal_connect (applet, "destroy",
			  G_CALLBACK (panel_applet_destroy),
			  info);

	panel_applet_set_dnd_enabled (info, !locked);

	gtk_widget_show_all (applet);

	orientation_change (info, panel);
	size_change (info, panel);
	back_change (info, panel);

	if (type != PANEL_OBJECT_BONOBO)
		gtk_widget_grab_focus (applet);
	else
		gtk_widget_child_focus (applet, GTK_DIR_TAB_FORWARD);

	return info;
}

int
panel_applet_get_position (AppletInfo *applet)
{
	AppletData *applet_data;

	g_return_val_if_fail (applet != NULL, 0);
	g_return_val_if_fail (G_IS_OBJECT (applet->widget), 0);

	applet_data = g_object_get_data (G_OBJECT (applet->widget), PANEL_APPLET_DATA);

	return applet_data->pos;
}

gboolean
panel_applet_can_freely_move (AppletInfo *applet)
{
	GConfClient       *client;
	PanelGConfKeyType  key_type;
	const char        *key;

	if (panel_lockdown_get_locked_down ())
		return FALSE;

	client  = panel_gconf_get_client ();
	
	key_type = (applet->type == PANEL_OBJECT_BONOBO) ? PANEL_GCONF_APPLETS : PANEL_GCONF_OBJECTS;
       
	key = panel_gconf_full_key (key_type, applet->id, "position");
	if (!gconf_client_key_is_writable (client, key, NULL))
		return FALSE;

	key = panel_gconf_full_key (key_type, applet->id, "toplevel_id");
	if (!gconf_client_key_is_writable (client, key, NULL))
		return FALSE;

	key = panel_gconf_full_key (key_type, applet->id, "panel_right_stick");
	if (!gconf_client_key_is_writable (client, key, NULL))
		return FALSE;

	return TRUE;
}

gboolean
panel_applet_lockable (AppletInfo *applet)
{
	GConfClient        *client;
	PanelGConfKeyType   key_type;
	const char         *key;

	if (panel_lockdown_get_locked_down ())
		return FALSE;
	
	client  = panel_gconf_get_client ();
	
	key_type = (applet->type == PANEL_OBJECT_BONOBO) ? PANEL_GCONF_APPLETS : PANEL_GCONF_OBJECTS;

	key = panel_gconf_full_key (key_type, applet->id, "locked");

	return gconf_client_key_is_writable (client, key, NULL);
}
