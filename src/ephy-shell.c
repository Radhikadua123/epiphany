/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2000-2004 Marco Pesenti Gritti
 *  Copyright © 2003, 2004, 2006 Christian Persch
 *  Copyright © 2011 Igalia S.L.
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "ephy-shell.h"

#include "ephy-debug.h"
#include "ephy-downloads-manager.h"
#include "ephy-embed-container.h"
#include "ephy-embed-utils.h"
#include "ephy-file-helpers.h"
#include "ephy-gui.h"
#include "ephy-header-bar.h"
#include "ephy-history-dialog.h"
#include "ephy-lockdown.h"
#include "ephy-prefs.h"
#include "ephy-session.h"
#include "ephy-settings.h"
#include "ephy-title-box.h"
#include "ephy-title-widget.h"
#include "ephy-type-builtins.h"
#include "ephy-web-view.h"
#include "ephy-window.h"
#include "prefs-dialog.h"
#include "window-commands.h"

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#ifdef ENABLE_SYNC
#include "ephy-notification.h"
#endif

struct _EphyShell {
  EphyEmbedShell parent_instance;

  EphySession *session;
#ifdef ENABLE_SYNC
  EphySyncService *sync_service;
#endif
  GList *windows;
  GObject *lockdown;
  EphyBookmarksManager *bookmarks_manager;
  GNetworkMonitor *network_monitor;
  GtkWidget *history_dialog;
  GObject *prefs_dialog;
  EphyShellStartupContext *local_startup_context;
  EphyShellStartupContext *remote_startup_context;
  GSList *open_uris_idle_ids;
};

static EphyShell *ephy_shell = NULL;

static void ephy_shell_dispose (GObject *object);
static void ephy_shell_finalize (GObject *object);

G_DEFINE_TYPE (EphyShell, ephy_shell, EPHY_TYPE_EMBED_SHELL)

/**
 * ephy_shell_startup_context_new:
 * @bookmarks_filename: A bookmarks file to import.
 * @session_filename: A session to restore.
 * @bookmark_url: A URL to be added to the bookmarks.
 * @arguments: A %NULL-terminated array of URLs and file URIs to be opened.
 * @user_time: The user time when the EphyShell startup was invoked.
 *
 * Creates a new startup context. All string parameters, including
 * @arguments, are copied.
 *
 * Returns: a newly allocated #EphyShellStartupContext
 **/
EphyShellStartupContext *
ephy_shell_startup_context_new (EphyStartupFlags startup_flags,
                                char *bookmarks_filename,
                                char *session_filename,
                                char *bookmark_url,
                                char **arguments,
                                guint32 user_time)
{
  EphyShellStartupContext *ctx = g_slice_new0 (EphyShellStartupContext);

  ctx->startup_flags = startup_flags;

  ctx->bookmarks_filename = g_strdup (bookmarks_filename);
  ctx->session_filename = g_strdup (session_filename);
  ctx->bookmark_url = g_strdup (bookmark_url);

  ctx->arguments = g_strdupv (arguments);

  ctx->user_time = user_time;

  return ctx;
}

static void
ephy_shell_startup_context_free (EphyShellStartupContext *ctx)
{
  g_assert (ctx != NULL);

  g_free (ctx->bookmarks_filename);
  g_free (ctx->session_filename);
  g_free (ctx->bookmark_url);

  g_strfreev (ctx->arguments);

  g_slice_free (EphyShellStartupContext, ctx);
}

static void
ephy_shell_startup_continue (EphyShell *shell, EphyShellStartupContext *ctx)
{
  EphySession *session = ephy_shell_get_session (shell);

  if (ctx->session_filename != NULL) {
    g_assert (session != NULL);
    ephy_session_load (session, (const char *)ctx->session_filename,
                       ctx->user_time, NULL, NULL, NULL);
  } else if (ctx->arguments || !session) {
    /* Don't queue any window openings if no extra arguments given, */
    /* since session autoresume will open one for us. */
    ephy_shell_open_uris (shell, (const char **)ctx->arguments,
                          ctx->startup_flags, ctx->user_time);
  }
}

static void
new_window (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  window_cmd_new_window (NULL, NULL, NULL);
}

static void
new_incognito_window (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
  window_cmd_new_incognito_window (NULL, NULL, NULL);
}

static void
reopen_closed_tab (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  window_cmd_reopen_closed_tab (NULL, NULL, NULL);
}

static void
import_bookmarks (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_import_bookmarks (NULL, NULL, EPHY_WINDOW (window));
}

static void
export_bookmarks (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_export_bookmarks (NULL, NULL, EPHY_WINDOW (window));
}

static void
show_history (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_show_history (NULL, NULL, EPHY_WINDOW (window));
}

static void
show_preferences (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_show_preferences (NULL, NULL, EPHY_WINDOW (window));
}

static void
show_shortcuts (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_show_shortcuts (NULL, NULL, EPHY_WINDOW (window));
}

static void
show_help (GSimpleAction *action,
           GVariant      *parameter,
           gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_show_help (NULL, NULL, GTK_WIDGET (window));
}

static void
show_about (GSimpleAction *action,
            GVariant      *parameter,
            gpointer       user_data)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (ephy_shell));

  window_cmd_show_about (NULL, NULL, GTK_WIDGET (window));
}

static void
quit_application (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  window_cmd_quit (NULL, NULL, NULL);
}

static GActionEntry app_entries[] = {
  { "new-window", new_window, NULL, NULL, NULL },
  { "new-incognito", new_incognito_window, NULL, NULL, NULL },
  { "import-bookmarks", import_bookmarks, NULL, NULL, NULL },
  { "export-bookmarks", export_bookmarks, NULL, NULL, NULL },
  { "history", show_history, NULL, NULL, NULL },
  { "preferences", show_preferences, NULL, NULL, NULL },
  { "shortcuts", show_shortcuts, NULL, NULL, NULL },
  { "help", show_help, NULL, NULL, NULL },
  { "about", show_about, NULL, NULL, NULL },
  { "quit", quit_application, NULL, NULL, NULL },
};

static GActionEntry app_mode_app_entries[] = {
  { "preferences", show_preferences, NULL, NULL, NULL },
  { "about", show_about, NULL, NULL, NULL },
  { "quit", quit_application, NULL, NULL, NULL },
};

static GActionEntry app_normal_mode_entries[] = {
  { "reopen-closed-tab", reopen_closed_tab, NULL, NULL, NULL },
};

static void
download_started_cb (WebKitWebContext *web_context,
                     WebKitDownload   *download,
                     EphyShell        *shell)
{
  EphyDownload *ephy_download;
  gboolean ephy_download_set;

  /* Is download locked down? */
  if (g_settings_get_boolean (EPHY_SETTINGS_LOCKDOWN,
                              EPHY_PREFS_LOCKDOWN_SAVE_TO_DISK)) {
    webkit_download_cancel (download);
    return;
  }

  /* Only create an EphyDownload for the WebKitDownload if it doesn't exist yet.
   * This can happen when the download has been started automatically by WebKit,
   * due to a context menu action or policy checker decision. Downloads started
   * explicitly by Epiphany are marked with ephy-download-set GObject data.
   */
  ephy_download_set = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (download), "ephy-download-set"));
  if (ephy_download_set)
    return;

  ephy_download = ephy_download_new (download);
  ephy_downloads_manager_add_download (ephy_embed_shell_get_downloads_manager (EPHY_EMBED_SHELL (shell)),
                                       ephy_download);
  g_object_unref (ephy_download);
}

#ifdef ENABLE_SYNC
static void
sync_tokens_load_finished_cb (EphySyncService *service,
                              GError          *error,
                              gpointer         user_data)
{
  EphyNotification *notification;

  g_assert (EPHY_IS_SYNC_SERVICE (service));

  /* If the tokens were successfully loaded, start the periodical sync.
   * Otherwise, notify the user to sign in again. */
  if (error == NULL) {
    ephy_sync_service_start_periodical_sync (service, TRUE);
  } else {
    notification = ephy_notification_new (error->message,
                                          _("Please visit Preferences and sign in "
                                            "again to continue the sync process."));
    ephy_notification_show (notification);
  }
}
#endif

static void
ephy_shell_startup (GApplication *application)
{
  EphyEmbedShell *embed_shell = EPHY_EMBED_SHELL (application);
  EphyEmbedShellMode mode;
  GtkBuilder *builder;

  G_APPLICATION_CLASS (ephy_shell_parent_class)->startup (application);

  /* We're not remoting; start our services */
  g_signal_connect (ephy_embed_shell_get_web_context (embed_shell),
                    "download-started",
                    G_CALLBACK (download_started_cb),
                    application);

  builder = gtk_builder_new ();
  gtk_builder_add_from_resource (builder,
                                 "/org/gnome/epiphany/gtk/application-menu.ui",
                                 NULL);

  mode = ephy_embed_shell_get_mode (embed_shell);
  if (mode != EPHY_EMBED_SHELL_MODE_APPLICATION) {
    g_action_map_add_action_entries (G_ACTION_MAP (application),
                                     app_entries, G_N_ELEMENTS (app_entries),
                                     application);

    if (mode != EPHY_EMBED_SHELL_MODE_INCOGNITO) {
      g_action_map_add_action_entries (G_ACTION_MAP (application),
                                       app_normal_mode_entries, G_N_ELEMENTS (app_normal_mode_entries),
                                       application);
      g_object_bind_property (G_OBJECT (ephy_shell_get_session (EPHY_SHELL (application))),
                              "can-undo-tab-closed",
                              g_action_map_lookup_action (G_ACTION_MAP (application),
                                                          "reopen-closed-tab"),
                              "enabled",
                              G_BINDING_SYNC_CREATE);
    }

#ifdef ENABLE_SYNC
    /* Create the sync service. */
    ephy_shell->sync_service = ephy_sync_service_new ();
    g_signal_connect (ephy_shell->sync_service,
                      "sync-tokens-load-finished",
                      G_CALLBACK (sync_tokens_load_finished_cb), NULL);
#endif

    gtk_application_set_app_menu (GTK_APPLICATION (application),
                                  G_MENU_MODEL (gtk_builder_get_object (builder, "app-menu")));
  } else {
    g_action_map_add_action_entries (G_ACTION_MAP (application),
                                     app_mode_app_entries, G_N_ELEMENTS (app_mode_app_entries),
                                     application);
    gtk_application_set_app_menu (GTK_APPLICATION (application),
                                  G_MENU_MODEL (gtk_builder_get_object (builder, "app-mode-app-menu")));
  }

  g_object_unref (builder);
}

static void
session_load_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  EphySession *session = EPHY_SESSION (object);
  EphyShellStartupContext *ctx = (EphyShellStartupContext *)user_data;

  ephy_session_resume_finish (session, result, NULL);
  ephy_shell_startup_continue (ephy_shell_get_default (), ctx);
}

static void
ephy_shell_activate (GApplication *application)
{
  EphyShell *shell = EPHY_SHELL (application);

  /*
   * We get here on each new instance (remote or not). Autoresume the
   * session and queue the commands if we are a secondary instance. Otherwise,
   * execute the commands immediately, before the remote startup context
   * can be invalidated by another remote instance.
   */
  if (shell->remote_startup_context == NULL) {
    EphySession *session = ephy_shell_get_session (shell);

    if (session) {
      ephy_session_resume (session,
                           shell->local_startup_context->user_time,
                           NULL, session_load_cb, shell->local_startup_context);
    } else
      ephy_shell_startup_continue (shell, shell->local_startup_context);
  } else {
    ephy_shell_startup_continue (shell, shell->remote_startup_context);
    g_clear_pointer (&shell->remote_startup_context, ephy_shell_startup_context_free);
  }
}

/*
 * We use this enumeration to conveniently fill and read from the
 * dictionary variant that is sent from the remote to the primary
 * instance.
 */
typedef enum {
  CTX_STARTUP_FLAGS,
  CTX_BOOKMARKS_FILENAME,
  CTX_SESSION_FILENAME,
  CTX_BOOKMARK_URL,
  CTX_ARGUMENTS,
  CTX_USER_TIME
} CtxEnum;

static void
ephy_shell_add_platform_data (GApplication    *application,
                              GVariantBuilder *builder)
{
  EphyShell *app;
  EphyShellStartupContext *ctx;
  GVariantBuilder *ctx_builder;
  static const char *empty_arguments[] = { "", NULL };
  const char * const *arguments;

  app = EPHY_SHELL (application);

  G_APPLICATION_CLASS (ephy_shell_parent_class)->add_platform_data (application,
                                                                    builder);

  if (app->local_startup_context) {
    /*
     * We create an array variant that contains only the elements in
     * ctx that are non-NULL.
     */
    ctx_builder = g_variant_builder_new (G_VARIANT_TYPE_ARRAY);
    ctx = app->local_startup_context;

    if (ctx->startup_flags)
      g_variant_builder_add (ctx_builder, "{iv}",
                             CTX_STARTUP_FLAGS,
                             g_variant_new_byte (ctx->startup_flags));

    if (ctx->bookmarks_filename)
      g_variant_builder_add (ctx_builder, "{iv}",
                             CTX_BOOKMARKS_FILENAME,
                             g_variant_new_string (ctx->bookmarks_filename));

    if (ctx->session_filename)
      g_variant_builder_add (ctx_builder, "{iv}",
                             CTX_SESSION_FILENAME,
                             g_variant_new_string (ctx->session_filename));

    if (ctx->bookmark_url)
      g_variant_builder_add (ctx_builder, "{iv}",
                             CTX_BOOKMARK_URL,
                             g_variant_new_string (ctx->bookmark_url));

    /*
     * If there are no URIs specified, pass an empty string, so that
     * the primary instance opens a new window.
     */
    if (ctx->arguments)
      arguments = (const gchar * const *)ctx->arguments;
    else
      arguments = empty_arguments;

    g_variant_builder_add (ctx_builder, "{iv}",
                           CTX_ARGUMENTS,
                           g_variant_new_strv (arguments, -1));

    g_variant_builder_add (ctx_builder, "{iv}",
                           CTX_USER_TIME,
                           g_variant_new_uint32 (ctx->user_time));

    g_variant_builder_add (builder, "{sv}",
                           "ephy-shell-startup-context",
                           g_variant_builder_end (ctx_builder));

    g_variant_builder_unref (ctx_builder);
  }
}

static void
ephy_shell_before_emit (GApplication *application,
                        GVariant     *platform_data)
{
  GVariantIter iter, ctx_iter;
  const char *key;
  CtxEnum ctx_key;
  GVariant *value, *ctx_value;
  EphyShellStartupContext *ctx = NULL;

  EphyShell *shell = EPHY_SHELL (application);

  g_variant_iter_init (&iter, platform_data);
  while (g_variant_iter_loop (&iter, "{&sv}", &key, &value)) {
    if (strcmp (key, "ephy-shell-startup-context") == 0) {
      ctx = g_slice_new0 (EphyShellStartupContext);

      /*
       * Iterate over the startup context variant and fill the members
       * that were wired. Everything else is just NULL.
       */
      g_variant_iter_init (&ctx_iter, value);
      while (g_variant_iter_loop (&ctx_iter, "{iv}", &ctx_key, &ctx_value)) {
        switch (ctx_key) {
          case CTX_STARTUP_FLAGS:
            ctx->startup_flags = g_variant_get_byte (ctx_value);
            break;
          case CTX_BOOKMARKS_FILENAME:
            ctx->bookmarks_filename = g_variant_dup_string (ctx_value, NULL);
            break;
          case CTX_SESSION_FILENAME:
            ctx->session_filename = g_variant_dup_string (ctx_value, NULL);
            break;
          case CTX_BOOKMARK_URL:
            ctx->bookmark_url = g_variant_dup_string (ctx_value, NULL);
            break;
          case CTX_ARGUMENTS:
            ctx->arguments = g_variant_dup_strv (ctx_value, NULL);
            break;
          case CTX_USER_TIME:
            ctx->user_time = g_variant_get_uint32 (ctx_value);
            break;
          default:
            g_assert_not_reached ();
            break;
        }
      }
    }
  }

  /* We have already processed and discarded any previous remote startup contexts. */
  g_assert (shell->remote_startup_context == NULL);
  shell->remote_startup_context = ctx;

  G_APPLICATION_CLASS (ephy_shell_parent_class)->before_emit (application,
                                                              platform_data);
}

static GObject *
ephy_shell_get_lockdown (EphyShell *shell)
{
  g_return_val_if_fail (EPHY_IS_SHELL (shell), NULL);

  if (shell->lockdown == NULL)
    shell->lockdown = g_object_new (EPHY_TYPE_LOCKDOWN, NULL);

  return G_OBJECT (shell->session);
}

static void
ephy_shell_constructed (GObject *object)
{
  if (ephy_embed_shell_get_mode (EPHY_EMBED_SHELL (object)) != EPHY_EMBED_SHELL_MODE_BROWSER) {
    GApplicationFlags flags;

    flags = g_application_get_flags (G_APPLICATION (object));
    flags |= G_APPLICATION_NON_UNIQUE;
    g_application_set_flags (G_APPLICATION (object), flags);
  }

  /* FIXME: not sure if this is the best place to put this stuff. */
  ephy_shell_get_lockdown (EPHY_SHELL (object));

  if (G_OBJECT_CLASS (ephy_shell_parent_class)->constructed)
    G_OBJECT_CLASS (ephy_shell_parent_class)->constructed (object);
}

static void
ephy_shell_class_init (EphyShellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = ephy_shell_dispose;
  object_class->finalize = ephy_shell_finalize;
  object_class->constructed = ephy_shell_constructed;

  application_class->startup = ephy_shell_startup;
  application_class->activate = ephy_shell_activate;
  application_class->before_emit = ephy_shell_before_emit;
  application_class->add_platform_data = ephy_shell_add_platform_data;
}

static void
ephy_shell_init (EphyShell *shell)
{
  EphyShell **ptr = &ephy_shell;

  /* globally accessible singleton */
  g_assert (ephy_shell == NULL);
  ephy_shell = shell;
  g_object_add_weak_pointer (G_OBJECT (ephy_shell),
                             (gpointer *)ptr);
}

static void
remove_open_uris_idle_cb (gpointer data)
{
  g_source_remove (GPOINTER_TO_UINT (data));
}

static void
ephy_shell_dispose (GObject *object)
{
  EphyShell *shell = EPHY_SHELL (object);

  LOG ("EphyShell disposing");

  g_clear_object (&shell->session);
  g_clear_object (&shell->lockdown);
  g_clear_pointer (&shell->history_dialog, gtk_widget_destroy);
  g_clear_object (&shell->prefs_dialog);
  g_clear_object (&shell->network_monitor);
#ifdef ENABLE_SYNC
  g_clear_object (&shell->sync_service);
#endif
  g_clear_object (&shell->bookmarks_manager);

  g_slist_free_full (shell->open_uris_idle_ids, remove_open_uris_idle_cb);
  shell->open_uris_idle_ids = NULL;

  G_OBJECT_CLASS (ephy_shell_parent_class)->dispose (object);
}

static void
ephy_shell_finalize (GObject *object)
{
  EphyShell *shell = EPHY_SHELL (object);

  g_clear_pointer (&shell->local_startup_context, ephy_shell_startup_context_free);
  g_clear_pointer (&shell->remote_startup_context, ephy_shell_startup_context_free);

  G_OBJECT_CLASS (ephy_shell_parent_class)->finalize (object);

  LOG ("Ephy shell finalised");
}

/**
 * ephy_shell_get_default:
 *
 * Retrieve the default #EphyShell object
 *
 * Return value: (transfer none): the default #EphyShell
 **/
EphyShell *
ephy_shell_get_default (void)
{
  return ephy_shell;
}

/**
 * ephy_shell_new_tab_full:
 * @shell: a #EphyShell
 * @window: the target #EphyWindow or %NULL
 * @previous_embed: the referrer embed, or %NULL
 * @user_time: a timestamp, or 0
 *
 * Create a new tab and the parent window when necessary.
 * Use this function to open urls in new window/tabs.
 *
 * Return value: (transfer none): the created #EphyEmbed
 **/
EphyEmbed *
ephy_shell_new_tab_full (EphyShell      *shell,
                         const char     *title,
                         WebKitWebView  *related_view,
                         EphyWindow     *window,
                         EphyEmbed      *previous_embed,
                         EphyNewTabFlags flags,
                         guint32         user_time)
{
  EphyEmbedShell *embed_shell;
  GtkWidget *web_view;
  EphyEmbed *embed = NULL;
  gboolean jump_to = FALSE;
  int position = -1;

  g_return_val_if_fail (EPHY_IS_SHELL (shell), NULL);
  g_return_val_if_fail (EPHY_IS_WINDOW (window), NULL);
  g_return_val_if_fail (EPHY_IS_EMBED (previous_embed) || !previous_embed, NULL);

  embed_shell = EPHY_EMBED_SHELL (shell);

  if (flags & EPHY_NEW_TAB_JUMP) jump_to = TRUE;

  LOG ("Opening new tab window %p parent-embed %p jump-to:%s",
       window, previous_embed, jump_to ? "t" : "f");

  if (flags & EPHY_NEW_TAB_APPEND_AFTER) {
    if (previous_embed) {
      GtkWidget *nb = ephy_window_get_notebook (window);
      /* FIXME this assumes the tab is the  direct notebook child */
      position = gtk_notebook_page_num (GTK_NOTEBOOK (nb),
                                        GTK_WIDGET (previous_embed)) + 1;
    } else
      g_warning ("Requested to append new tab after parent, but 'previous_embed' was NULL");
  }

  if (flags & EPHY_NEW_TAB_FIRST)
    position = 0;

  if (related_view)
    web_view = ephy_web_view_new_with_related_view (related_view);
  else
    web_view = ephy_web_view_new ();

  embed = EPHY_EMBED (g_object_new (EPHY_TYPE_EMBED,
                                    "web-view", web_view,
                                    "title", title,
                                    NULL));
  gtk_widget_show (GTK_WIDGET (embed));
  ephy_embed_container_add_child (EPHY_EMBED_CONTAINER (window), embed, position, jump_to);

  if ((flags & EPHY_NEW_TAB_DONT_SHOW_WINDOW) == 0 &&
      ephy_embed_shell_get_mode (embed_shell) != EPHY_EMBED_SHELL_MODE_TEST) {
    gtk_widget_show (GTK_WIDGET (window));
  }

  return embed;
}

/**
 * ephy_shell_new_tab:
 * @shell: a #EphyShell
 * @parent_window: the target #EphyWindow or %NULL
 * @previous_embed: the referrer embed, or %NULL
 *
 * Create a new tab and the parent window when necessary.
 * Use this function to open urls in new window/tabs.
 *
 * Return value: (transfer none): the created #EphyEmbed
 **/
EphyEmbed *
ephy_shell_new_tab (EphyShell      *shell,
                    EphyWindow     *parent_window,
                    EphyEmbed      *previous_embed,
                    EphyNewTabFlags flags)
{
  return ephy_shell_new_tab_full (shell, NULL, NULL, parent_window,
                                  previous_embed, flags,
                                  0);
}

/**
 * ephy_shell_get_session:
 * @shell: the #EphyShell
 *
 * Returns current session.
 *
 * Return value: (transfer none): the current session.
 **/
EphySession *
ephy_shell_get_session (EphyShell *shell)
{
  EphyEmbedShellMode mode;

  g_return_val_if_fail (EPHY_IS_SHELL (shell), NULL);

  mode = ephy_embed_shell_get_mode (EPHY_EMBED_SHELL (shell));
  if (mode ==  EPHY_EMBED_SHELL_MODE_APPLICATION || mode == EPHY_EMBED_SHELL_MODE_INCOGNITO)
    return NULL;

  if (shell->session == NULL)
    shell->session = g_object_new (EPHY_TYPE_SESSION, NULL);

  return shell->session;
}

#ifdef ENABLE_SYNC
/**
 * ephy_shell_get_sync_service:
 * @shell: the #EphyShell
 *
 * Returns the sync service.
 *
 * Return value: (transfer none): the global #EphySyncService
 **/
EphySyncService *
ephy_shell_get_sync_service (EphyShell *shell)
{
  g_return_val_if_fail (EPHY_IS_SHELL (shell), NULL);

  return shell->sync_service;
}
#endif

/**
 * ephy_shell_get_bookmarks_manager:
 * @shell: the #EphyShell
 *
 * Returns bookmarks manager.
 *
 * Return value: (transfer none): An #EphyBookmarksManager.
 */
EphyBookmarksManager *
ephy_shell_get_bookmarks_manager (EphyShell *shell)
{
  g_return_val_if_fail (EPHY_IS_SHELL (shell), NULL);

  if (shell->bookmarks_manager == NULL)
    shell->bookmarks_manager = ephy_bookmarks_manager_new ();

  return shell->bookmarks_manager;
}

/**
 * ephy_shell_get_net_monitor:
 *
 * Return value: (transfer none):
 **/
GNetworkMonitor *
ephy_shell_get_net_monitor (EphyShell *shell)
{
  if (shell->network_monitor == NULL)
    shell->network_monitor = g_network_monitor_get_default ();

  return shell->network_monitor;
}

/**
 * ephy_shell_get_history_dialog:
 *
 * Return value: (transfer none):
 **/
GtkWidget *
ephy_shell_get_history_dialog (EphyShell *shell)
{
  EphyEmbedShell *embed_shell;
  EphyHistoryService *service;

  embed_shell = ephy_embed_shell_get_default ();

  if (shell->history_dialog == NULL) {
    service = EPHY_HISTORY_SERVICE
                (ephy_embed_shell_get_global_history_service (embed_shell));
    shell->history_dialog = ephy_history_dialog_new (service);
    g_signal_connect (shell->history_dialog,
                      "destroy",
                      G_CALLBACK (gtk_widget_destroyed),
                      &shell->history_dialog);
  }

  return shell->history_dialog;
}

/**
 * ephy_shell_get_prefs_dialog:
 *
 * Return value: (transfer none):
 **/
GObject *
ephy_shell_get_prefs_dialog (EphyShell *shell)
{
  if (shell->prefs_dialog == NULL) {
    shell->prefs_dialog = g_object_new (EPHY_TYPE_PREFS_DIALOG,
                                        "use-header-bar", TRUE,
                                        NULL);
    g_signal_connect (shell->prefs_dialog,
                      "destroy",
                      G_CALLBACK (gtk_widget_destroyed),
                      &shell->prefs_dialog);
  }

  return shell->prefs_dialog;
}

void
_ephy_shell_create_instance (EphyEmbedShellMode mode)
{
  g_assert (ephy_shell == NULL);

  ephy_shell = EPHY_SHELL (g_object_new (EPHY_TYPE_SHELL,
                                         "application-id", "org.gnome.Epiphany",
                                         "mode", mode,
                                         NULL));
  /* FIXME weak ref */
  g_assert (ephy_shell != NULL);
}

/**
 * ephy_shell_set_startup_context:
 * @shell: A #EphyShell
 * @ctx: (transfer full): a #EphyShellStartupContext
 *
 * Sets the local startup context to be used during activation of a new instance.
 * See ephy_shell_set_startup_new().
 **/
void
ephy_shell_set_startup_context (EphyShell               *shell,
                                EphyShellStartupContext *ctx)
{
  g_return_if_fail (EPHY_IS_SHELL (shell));

  g_assert (shell->local_startup_context == NULL);

  shell->local_startup_context = ctx;
}

guint
ephy_shell_get_n_windows (EphyShell *shell)
{
  GList *list;

  g_return_val_if_fail (EPHY_IS_SHELL (shell), 0);

  list = gtk_application_get_windows (GTK_APPLICATION (shell));
  return g_list_length (list);
}

gboolean
ephy_shell_close_all_windows (EphyShell *shell)
{
  GList *windows;
  gboolean retval = TRUE;
  EphySession *session = ephy_shell_get_session (shell);

  g_return_val_if_fail (EPHY_IS_SHELL (shell), FALSE);

  if (session)
    ephy_session_close (session);

  windows = gtk_application_get_windows (GTK_APPLICATION (shell));
  while (windows) {
    EphyWindow *window = EPHY_WINDOW (windows->data);

    windows = windows->next;

    if (ephy_window_close (window))
      gtk_widget_destroy (GTK_WIDGET (window));
    else
      retval = FALSE;
  }

  return retval;
}

void
ephy_shell_try_quit (EphyShell *shell)
{
  if (ephy_shell_close_all_windows (shell))
    g_application_quit (G_APPLICATION (shell));
}

typedef struct {
  EphyShell *shell;
  EphySession *session;
  EphyWindow *window;
  char **uris;
  EphyNewTabFlags flags;
  guint32 user_time;
  EphyEmbed *previous_embed;
  guint current_uri;
  gboolean reuse_empty_tab;
  guint source_id;
} OpenURIsData;

static OpenURIsData *
open_uris_data_new (EphyShell       *shell,
                    const char     **uris,
                    EphyStartupFlags startup_flags,
                    guint32          user_time)
{
  OpenURIsData *data;
  gboolean new_windows_in_tabs;
  gboolean fullscreen_lockdown;
  gboolean have_uris;
  EphySession *session = ephy_shell_get_session (shell);

  data = g_slice_new0 (OpenURIsData);
  data->shell = shell;
  data->session = session ? g_object_ref (session) : NULL;
  data->uris = g_strdupv ((char **)uris);
  data->user_time = user_time;

  new_windows_in_tabs = g_settings_get_boolean (EPHY_SETTINGS_MAIN,
                                                EPHY_PREFS_NEW_WINDOWS_IN_TABS);
  fullscreen_lockdown = g_settings_get_boolean (EPHY_SETTINGS_LOCKDOWN,
                                                EPHY_PREFS_LOCKDOWN_FULLSCREEN);

  have_uris = uris && !(g_strv_length ((char **)uris) == 1 && g_str_equal (uris[0], ""));

  if (startup_flags & EPHY_STARTUP_NEW_WINDOW && !fullscreen_lockdown) {
    data->window = ephy_window_new ();
  } else if (startup_flags & EPHY_STARTUP_NEW_TAB || (new_windows_in_tabs && have_uris)) {
    data->flags |= EPHY_NEW_TAB_JUMP;
    data->window = EPHY_WINDOW (gtk_application_get_active_window (GTK_APPLICATION (shell)));
    data->reuse_empty_tab = TRUE;
  } else if (!have_uris) {
    data->window = ephy_window_new ();
  }

  g_application_hold (G_APPLICATION (shell));

  return data;
}

static void
open_uris_data_free (OpenURIsData *data)
{
  g_application_release (G_APPLICATION (data->shell));
  g_clear_object (&data->session);
  g_strfreev (data->uris);

  g_slice_free (OpenURIsData, data);
}

static gboolean
ephy_shell_open_uris_idle (OpenURIsData *data)
{
  EphyEmbed *embed = NULL;
  EphyHeaderBar *header_bar;
  EphyTitleWidget *title_widget;
  EphyEmbedShellMode mode;
  EphyNewTabFlags page_flags = 0;
  gboolean reusing_empty_tab = FALSE;
  const char *url;

  mode = ephy_embed_shell_get_mode (EPHY_EMBED_SHELL (data->shell));

  if (!data->window)
    data->window = ephy_window_new ();
  else if (data->previous_embed)
    page_flags |= EPHY_NEW_TAB_APPEND_AFTER;
  else if (data->reuse_empty_tab) {
    embed = ephy_embed_container_get_active_child (EPHY_EMBED_CONTAINER (data->window));
    /* Only load a new page in this embed if it was showing or loading the homepage */
    if (ephy_web_view_get_visit_type (ephy_embed_get_web_view (embed)) == EPHY_PAGE_VISIT_HOMEPAGE)
      reusing_empty_tab = TRUE;
  }

  if (!reusing_empty_tab) {
    embed = ephy_shell_new_tab_full (data->shell,
                                     NULL, NULL,
                                     data->window,
                                     data->previous_embed,
                                     data->flags | page_flags,
                                     data->user_time);
  }

  url = data->uris ? data->uris[data->current_uri] : NULL;
  if (url && url[0] != '\0') {
    ephy_web_view_load_url (ephy_embed_get_web_view (embed), url);

    /* When reusing an empty tab, the focus is in the location entry */
    if (reusing_empty_tab || data->flags & EPHY_NEW_TAB_JUMP)
      gtk_widget_grab_focus (GTK_WIDGET (embed));

    if (data->flags & EPHY_NEW_TAB_JUMP && mode != EPHY_EMBED_SHELL_MODE_TEST)
      gtk_window_present_with_time (GTK_WINDOW (data->window), data->user_time);
  } else {
    ephy_web_view_load_new_tab_page (ephy_embed_get_web_view (embed));
    if (data->flags & EPHY_NEW_TAB_JUMP)
      ephy_window_activate_location (data->window);
  }

  /* Set address from the very beginning. Looks odd in app mode if it appears later on. */
  header_bar = EPHY_HEADER_BAR (ephy_window_get_header_bar (data->window));
  title_widget = ephy_header_bar_get_title_widget (header_bar);
  ephy_title_widget_set_address (title_widget, url);

  data->current_uri++;
  data->previous_embed = embed;

  return data->uris && data->uris[data->current_uri] != NULL;
}

static void
ephy_shell_open_uris_idle_done (OpenURIsData *data)
{
  data->shell->open_uris_idle_ids = g_slist_remove (data->shell->open_uris_idle_ids,
                                                    GUINT_TO_POINTER (data->source_id));
  open_uris_data_free (data);
}

void
ephy_shell_open_uris (EphyShell       *shell,
                      const char     **uris,
                      EphyStartupFlags startup_flags,
                      guint32          user_time)
{
  OpenURIsData *data;
  guint id;

  g_return_if_fail (EPHY_IS_SHELL (shell));

  data = open_uris_data_new (shell, uris, startup_flags, user_time);
  id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                        (GSourceFunc)ephy_shell_open_uris_idle,
                        data,
                        (GDestroyNotify)ephy_shell_open_uris_idle_done);
  data->source_id = id;

  shell->open_uris_idle_ids = g_slist_prepend (shell->open_uris_idle_ids, GUINT_TO_POINTER (id));
}
