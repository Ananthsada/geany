/*
 *      splitwindow.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2008 The Geany contributors
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Split Window plugin. */

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include "geanyplugin.h"
#include "../src/notebook.h"
#include "gtkcompat.h"
#include <string.h>


PLUGIN_VERSION_CHECK(GEANY_API_VERSION)
PLUGIN_SET_INFO(_("Split Window"), _("Splits the editor view into two windows."),
	VERSION, _("The Geany developer team"))


GeanyData		*geany_data;
GeanyPlugin		*geany_plugin;


/* Keybinding(s) */
enum
{
	KB_SPLIT_HORIZONTAL,
	KB_SPLIT_VERTICAL,
	KB_SPLIT_UNSPLIT,
	KB_COUNT
};

enum State
{
	STATE_SPLIT_HORIZONTAL,
	STATE_SPLIT_VERTICAL,
	STATE_UNSPLIT,
	STATE_COUNT
};

static struct
{
	GtkWidget *main;
	GtkWidget *horizontal;
	GtkWidget *vertical;
	GtkWidget *unsplit;
}
menu_items;

static enum State plugin_state;


typedef struct EditWindow
{
	GeanyEditor		*editor;	/* original editor for split view */
	ScintillaObject	*sci;		/* new editor widget */
	GtkWidget		*vbox;
	GtkWidget		*name_label;
}
EditWindow;

static EditWindow edit_window = {NULL, NULL, NULL, NULL};
static gint document_main_page = -1;
static gboolean sci_notify_recvd = FALSE;
static guint sci_notify_timeout = 0;

static void on_refresh(void);
static void on_split_horizontally(GtkMenuItem *menuitem, gpointer user_data);
static void set_editor(EditWindow *editwin, GeanyEditor *editor);
static void on_unsplit(GtkMenuItem *menuitem, gpointer user_data);
static void split_view(gboolean horizontal, GeanyDocument* doc);

static void on_menu_item_clicked(GtkMenuItem *menuitem, gpointer user_data)
{
	on_split_horizontally(menuitem, NULL);
	on_refresh();
}

static void on_switch_splitview_page_clicked(GtkMenuItem *menuitem, gpointer user_data)
{
	GeanyDocument* doc = (GeanyDocument*)user_data;

	if (plugin_state != STATE_UNSPLIT)
	{
		if (doc->is_valid)
		{
			document_main_page = doc->index;
			set_editor(&edit_window, doc->editor);
		}
	}
	else
	{
		split_view(TRUE, doc);
	}
}

static void menu_plugin_callback(GtkWidget* menu, GeanyDocument* doc)
{
	GtkWidget* menu_item;

	if (doc)
	{
		menu_item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("M_ove to SplitPane"));
		gtk_widget_show(menu_item);
		gtk_container_add(GTK_CONTAINER(menu), menu_item);
		g_signal_connect(menu_item, "activate", G_CALLBACK(on_switch_splitview_page_clicked), doc);
	}
}

static gboolean on_sci_notify_timeout(G_GNUC_UNUSED gpointer data)
{
	if (sci_notify_recvd == TRUE)
	{
		sci_notify_recvd = FALSE;
		return TRUE;
	}
	else
	{
		set_splitmode_state(FALSE, -1);
		return FALSE;
	}
}

/* line numbers visibility */
static void set_line_numbers(ScintillaObject * sci, gboolean set)
{
	if (set)
	{
		gchar tmp_str[15];
		gint len = scintilla_send_message(sci, SCI_GETLINECOUNT, 0, 0);
		gint width;

		g_snprintf(tmp_str, 15, "_%d", len);
		width = scintilla_send_message(sci, SCI_TEXTWIDTH, STYLE_LINENUMBER, (sptr_t) tmp_str);
		scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 0, width);
		scintilla_send_message(sci, SCI_SETMARGINSENSITIVEN, 0, FALSE); /* use default behaviour */
	}
	else
	{
		scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 0, 0);
	}
}


static void on_sci_notify(ScintillaObject *sci, gint param,
		SCNotification *nt, gpointer data)
{
	gint position,line;

	set_splitmode_state(TRUE, document_main_page);

	sci_notify_recvd = TRUE;
	if(sci_notify_timeout != 0)
	{
		g_source_remove(sci_notify_timeout);
	}
	sci_notify_timeout = g_timeout_add(600, on_sci_notify_timeout, NULL);

	position = sci_get_current_position(sci);
	switch (nt->nmhdr.code)
	{
		/* adapted from editor.c: on_margin_click() */
		case SCN_MARGINCLICK:
			/* left click to marker margin toggles marker */
			if (nt->margin == 1)
			{
				gboolean set;
				gint marker = 1;

				line = sci_get_line_from_position(sci, nt->position);
				set = sci_is_marker_set_at_line(sci, line, marker);
				if (!set)
					sci_set_marker_at_line(sci, line, marker);
				else
					sci_delete_marker_at_line(sci, line, marker);
			}
			/* left click on the folding margin to toggle folding state of current line */
			if (nt->margin == 2)
			{
				line = sci_get_line_from_position(sci, nt->position);
				scintilla_send_message(sci, SCI_TOGGLEFOLD, line, 0);
			}
			break;
		case SCN_PAINTED:
			position = sci_get_current_position(sci);
			line = sci_get_line_from_position(sci, position);
			sci_set_current_position(edit_window.editor->sci, position, TRUE);
			break;
		default: break;
	}
}


static void sync_to_current(ScintillaObject *sci, ScintillaObject *current)
{
	gpointer sdoc;
	gint pos;

	/* set the new sci widget to view the existing Scintilla document */
	sdoc = (gpointer) scintilla_send_message(current, SCI_GETDOCPOINTER, 0, 0);
	scintilla_send_message(sci, SCI_SETDOCPOINTER, 0, (sptr_t) sdoc);

	highlighting_set_styles(sci, edit_window.editor->document->file_type);
	pos = sci_get_current_position(current);
	sci_set_current_position(sci, pos, TRUE);

	/* override some defaults */
	set_line_numbers(sci, geany->editor_prefs->show_linenumber_margin);
	/* marker margin */
	scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 1,
		scintilla_send_message(current, SCI_GETMARGINWIDTHN, 1, 0));
	if (!geany->editor_prefs->folding)
		scintilla_send_message(sci, SCI_SETMARGINWIDTHN, 2, 0);
}


static void set_editor(EditWindow *editwin, GeanyEditor *editor)
{
	editwin->editor = editor;

	/* first destroy any widget, otherwise its signals will have an
	 * invalid document as user_data */
	if (editwin->sci != NULL)
		gtk_widget_destroy(GTK_WIDGET(editwin->sci));

	editwin->sci = editor_create_widget(editor);
	gtk_widget_show(GTK_WIDGET(editwin->sci));
	gtk_box_pack_start(GTK_BOX(editwin->vbox), GTK_WIDGET(editwin->sci), TRUE, TRUE, 0);

	sync_to_current(editwin->sci, editor->sci);

	scintilla_send_message(editwin->sci, SCI_USEPOPUP, 1, 0);
	/* for margin events */
	g_signal_connect(editwin->sci, "sci-notify",
			G_CALLBACK(on_sci_notify), NULL);

	gtk_label_set_text(GTK_LABEL(editwin->name_label), DOC_FILENAME(editor->document));
}


static void set_state(enum State id)
{
	gtk_widget_set_sensitive(menu_items.horizontal,
		(id != STATE_SPLIT_HORIZONTAL) && (id != STATE_SPLIT_VERTICAL));
	gtk_widget_set_sensitive(menu_items.vertical,
		(id != STATE_SPLIT_HORIZONTAL) && (id != STATE_SPLIT_VERTICAL));
	gtk_widget_set_sensitive(menu_items.unsplit,
		id != STATE_UNSPLIT);

	plugin_state = id;
}


/* Create a GtkToolButton with stock icon, label and tooltip.
 * @param label can be NULL to use stock label text. @a label can contain underscores,
 * which will be removed.
 * @param tooltip can be NULL to use label text (useful for GTK_TOOLBAR_ICONS). */
static GtkWidget *ui_tool_button_new(const gchar *stock_id, const gchar *label, const gchar *tooltip)
{
	GtkToolItem *item;
	gchar *dupl = NULL;

	if (stock_id && !label)
	{
		label = ui_lookup_stock_label(stock_id);
	}
	dupl = utils_str_remove_chars(g_strdup(label), "_");
	label = dupl;

	item = gtk_tool_button_new(NULL, label);
	if (stock_id)
		gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(item), stock_id);

	if (!tooltip)
		tooltip = label;
	if (tooltip)
		gtk_widget_set_tooltip_text(GTK_WIDGET(item), tooltip);

	g_free(dupl);
	return GTK_WIDGET(item);
}


static void on_refresh(void)
{
	GeanyDocument *doc = document_get_current();

	document_main_page = gtk_notebook_get_current_page(geany_data->main_widgets->notebook);
	g_return_if_fail(doc);
	g_return_if_fail(edit_window.sci);

	set_editor(&edit_window, doc->editor);
}


static void on_doc_menu_item_clicked(gpointer item, GeanyDocument *doc)
{
	if (doc->is_valid)
	{
		document_main_page = doc->index;
		set_editor(&edit_window, doc->editor);
	}
}


static void on_doc_show_menu(GtkMenuToolButton *button, GtkMenu *menu)
{
	/* clear the old menu items */
	gtk_container_foreach(GTK_CONTAINER(menu), (GtkCallback) gtk_widget_destroy, NULL);

	ui_menu_add_document_items(menu, edit_window.editor->document,
		G_CALLBACK(on_doc_menu_item_clicked));
}


#if GTK_CHECK_VERSION(3, 0, 0)
/* Blocks the ::show-menu signal if the menu's parent toggle button was inactive in the previous run.
 * This is a hack to workaround https://bugzilla.gnome.org/show_bug.cgi?id=769287
 * and should NOT be used for any other version than 3.15.9 to 3.21.4, although the code tries and
 * not block a legitimate signal in case the GTK version in use has been patched */
static void show_menu_gtk316_fix(GtkMenuToolButton *button, gpointer data)
{
	/* we assume only a single menu can popup at once, so reentrency isn't an issue.
	 * if it was, we could use custom data on the button, but it shouldn't be required */
	static gboolean block_next = FALSE;

	if (block_next)
	{
		g_signal_stop_emission_by_name(button, "show-menu");
		block_next = FALSE;
	}
	else
	{
		GtkWidget *menu = gtk_menu_tool_button_get_menu(button);
		GtkWidget *parent = gtk_menu_get_attach_widget(GTK_MENU(menu));

		if (parent && GTK_IS_TOGGLE_BUTTON(parent) && ! gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(parent)))
			block_next = TRUE;
	}
}
#endif


static GtkWidget *create_toolbar(void)
{
	GtkWidget *toolbar, *item;
	GtkToolItem *tool_item;

	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_MENU);
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);

	tool_item = gtk_menu_tool_button_new(NULL, NULL);
	gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(tool_item), GTK_STOCK_JUMP_TO);
	item = (GtkWidget*)tool_item;
	gtk_widget_set_tooltip_text(item, _("Show the current document"));
	gtk_container_add(GTK_CONTAINER(toolbar), item);
	g_signal_connect(item, "clicked", G_CALLBACK(on_refresh), NULL);

	item = gtk_menu_new();
	gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(tool_item), item);
#if GTK_CHECK_VERSION (3, 0, 0)
	/* hack for https://bugzilla.gnome.org/show_bug.cgi?id=769287 */
	if (! gtk_check_version(3, 15, 9) && gtk_check_version(3, 21, 4+1))
		g_signal_connect(tool_item, "show-menu", G_CALLBACK(show_menu_gtk316_fix), NULL);
#endif
	g_signal_connect(tool_item, "show-menu", G_CALLBACK(on_doc_show_menu), item);

	tool_item = gtk_tool_item_new();
	gtk_tool_item_set_expand(tool_item, TRUE);
	gtk_container_add(GTK_CONTAINER(toolbar), GTK_WIDGET(tool_item));

	item = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(item), PANGO_ELLIPSIZE_START);
	gtk_container_add(GTK_CONTAINER(tool_item), item);
	edit_window.name_label = item;

	item = ui_tool_button_new(GTK_STOCK_CLOSE, _("_Unsplit"), NULL);
	gtk_container_add(GTK_CONTAINER(toolbar), item);
	g_signal_connect(item, "clicked", G_CALLBACK(on_unsplit), NULL);

	return toolbar;
}

static void show_splitview_tab_bar_popup_menu(GdkEventButton *event)
{
	GtkWidget *menu_item;
	static GtkWidget *menu = NULL;

	if (menu == NULL)
		menu = gtk_menu_new();

	/* clear the old menu items */
	gtk_container_foreach(GTK_CONTAINER(menu), (GtkCallback)gtk_widget_destroy, NULL);

	menu_item = ui_image_menu_item_new(GTK_STOCK_CLOSE, _("Move File to _Other view"));
	gtk_widget_show(menu_item);
	gtk_container_add(GTK_CONTAINER(menu), menu_item);
	g_signal_connect(menu_item, "activate", G_CALLBACK(on_unsplit), NULL);

	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, event->button, event->time);
}

static gboolean splitview_notebook_tab_click(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	guint state;

	/* right-click is first handled here if it happened on a notebook tab */
	if (event->button == 3)
	{
		show_splitview_tab_bar_popup_menu(event);
		return TRUE;
	}

	return FALSE;
}


static void split_view(gboolean horizontal, GeanyDocument* doc_arg)
{
	GtkWidget *notebook = geany_data->main_widgets->notebook;
	GtkWidget *parent = gtk_widget_get_parent(notebook);
	GtkWidget *pane, *toolbar, *box, *splitwin_notebook;
	gint width = gtk_widget_get_allocated_width(notebook) / 2;
	gint height = gtk_widget_get_allocated_height(notebook) / 2;
	GeanyDocument *doc;

	if (!doc_arg)
	{
		doc = document_get_current();
	}
	else
	{
		doc = doc_arg;
	}

	g_return_if_fail(doc);
	g_return_if_fail(edit_window.editor == NULL);

	document_main_page = doc->index;;
	//document_main_page = gtk_notebook_get_current_page(notebook);

	set_state(horizontal ? STATE_SPLIT_HORIZONTAL : STATE_SPLIT_VERTICAL);

	g_object_ref(notebook);
	gtk_container_remove(GTK_CONTAINER(parent), notebook);

	pane = horizontal ? gtk_hpaned_new() : gtk_vpaned_new();
	gtk_container_add(GTK_CONTAINER(parent), pane);

	gtk_container_add(GTK_CONTAINER(pane), notebook);
	g_object_unref(notebook);

	box = gtk_vbox_new(FALSE, 0);
	toolbar = create_toolbar();
	gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);
	edit_window.vbox = box;

	/* used just to make the split window look the same as the main editor */
	splitwin_notebook = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(splitwin_notebook), FALSE);
	gtk_notebook_append_page(GTK_NOTEBOOK(splitwin_notebook), box, NULL);
	gtk_container_add(GTK_CONTAINER(pane), splitwin_notebook);

	g_signal_connect(G_OBJECT(splitwin_notebook), "button-press-event",
		G_CALLBACK(splitview_notebook_tab_click), NULL);

	set_editor(&edit_window, doc->editor);

	if (horizontal)
	{
		gtk_paned_set_position(GTK_PANED(pane), width);
	}
	else
	{
		gtk_paned_set_position(GTK_PANED(pane), height);
	}
	gtk_widget_show_all(pane);
}


static void on_split_horizontally(GtkMenuItem *menuitem, gpointer user_data)
{
	split_view(TRUE, NULL);
}


static void on_split_vertically(GtkMenuItem *menuitem, gpointer user_data)
{
	split_view(FALSE, NULL);
}


static void on_unsplit(GtkMenuItem *menuitem, gpointer user_data)
{
	GtkWidget *notebook = geany_data->main_widgets->notebook;
	GtkWidget *pane = gtk_widget_get_parent(notebook);
	GtkWidget *parent = gtk_widget_get_parent(pane);

	set_state(STATE_UNSPLIT);

	g_return_if_fail(edit_window.editor);

	g_object_ref(notebook);
	gtk_container_remove(GTK_CONTAINER(pane), notebook);

	gtk_widget_destroy(pane);
	edit_window.editor = NULL;
	edit_window.sci = NULL;

	gtk_container_add(GTK_CONTAINER(parent), notebook);
	g_object_unref(notebook);
}


static void kb_activate(guint key_id)
{
	switch (key_id)
	{
		case KB_SPLIT_HORIZONTAL:
			if (plugin_state == STATE_UNSPLIT)
				split_view(TRUE, NULL);
			break;
		case KB_SPLIT_VERTICAL:
			if (plugin_state == STATE_UNSPLIT)
				split_view(FALSE, NULL);
			break;
		case KB_SPLIT_UNSPLIT:
			if (plugin_state != STATE_UNSPLIT)
				on_unsplit(NULL, NULL);
			break;
	}
}


void plugin_init(GeanyData *data)
{
	GtkWidget *item, *menu;
	GeanyKeyGroup *key_group;

	menu_items.main = item = gtk_menu_item_new_with_mnemonic(_("_Split Window"));
	gtk_menu_shell_append(GTK_MENU_SHELL(geany_data->main_widgets->tools_menu), item);
	ui_add_document_sensitive(item);

	menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_items.main), menu);

	menu_items.horizontal = item =
		gtk_menu_item_new_with_mnemonic(_("_Side by Side"));
	g_signal_connect(item, "activate", G_CALLBACK(on_split_horizontally), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	menu_items.vertical = item =
		gtk_menu_item_new_with_mnemonic(_("_Top and Bottom"));
	g_signal_connect(item, "activate", G_CALLBACK(on_split_vertically), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	menu_items.unsplit = item =
		gtk_menu_item_new_with_mnemonic(_("_Unsplit"));
	g_signal_connect(item, "activate", G_CALLBACK(on_unsplit), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	gtk_widget_show_all(menu_items.main);

	set_state(STATE_UNSPLIT);

	/* setup keybindings */
	key_group = plugin_set_key_group(geany_plugin, "split_window", KB_COUNT, NULL);
	keybindings_set_item(key_group, KB_SPLIT_HORIZONTAL, kb_activate,
		0, 0, "split_horizontal", _("Side by Side"), menu_items.horizontal);
	keybindings_set_item(key_group, KB_SPLIT_VERTICAL, kb_activate,
		0, 0, "split_vertical", _("Top and Bottom"), menu_items.vertical);
	keybindings_set_item(key_group, KB_SPLIT_UNSPLIT, kb_activate,
		0, 0, "split_unsplit", _("_Unsplit"), menu_items.unsplit);
	
	register_menu_callback(menu_plugin_callback);
}


static gboolean do_select_current(gpointer data)
{
	GeanyDocument *doc;

	/* guard out for the unlikely case we get called after another unsplitting */
	if (plugin_state == STATE_UNSPLIT)
		return FALSE;

	doc = document_get_current();
	if (doc)
		set_editor(&edit_window, doc->editor);
	else
		on_unsplit(NULL, NULL);

	return FALSE;
}


static void on_document_close(GObject *obj, GeanyDocument *doc, gpointer user_data)
{
	if (doc->editor == edit_window.editor)
	{
		/* select current or unsplit in IDLE time, so the tab has changed */
		plugin_idle_add(geany_plugin, do_select_current, NULL);
	}
}


static void on_document_save(GObject *obj, GeanyDocument *doc, gpointer user_data)
{
	/* update filename */
	if (doc->editor == edit_window.editor)
		gtk_label_set_text(GTK_LABEL(edit_window.name_label), DOC_FILENAME(doc));
}


static void on_document_filetype_set(GObject *obj, GeanyDocument *doc,
	GeanyFiletype *filetype_old, gpointer user_data)
{
	/* update styles */
	if (edit_window.editor == doc->editor)
		sync_to_current(edit_window.sci, doc->editor->sci);
}


PluginCallback plugin_callbacks[] =
{
	{ "document-close", (GCallback) &on_document_close, FALSE, NULL },
	{ "document-save", (GCallback) &on_document_save, FALSE, NULL },
	{ "document-filetype-set", (GCallback) &on_document_filetype_set, FALSE, NULL },
	{ NULL, NULL, FALSE, NULL }
};


void plugin_cleanup(void)
{
	if (plugin_state != STATE_UNSPLIT)
		on_unsplit(NULL, NULL);

	gtk_widget_destroy(menu_items.main);
}
