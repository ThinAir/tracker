/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>

#include "tracker-monitor.h"
#include "tracker-marshal.h"

#define TRACKER_MONITOR_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TRACKER_TYPE_MONITOR, TrackerMonitorPrivate))

/* The life time of an item in the cache */
#define CACHE_LIFETIME_SECONDS 1

/* When we receive IO monitor events, we pause sending information to
 * the indexer for a few seconds before continuing. We have to receive
 * NO events for at least a few seconds before unpausing.
 */
#define PAUSE_ON_IO_SECONDS    5

/* If this is defined, we pause the indexer when we get events. If it
 * is not, we don't do any pausing.
 */
#undef  PAUSE_ON_IO

struct TrackerMonitorPrivate {
	GHashTable    *monitors;

	gboolean       enabled;

	GType          monitor_backend;

	guint          monitor_limit;
	gboolean       monitor_limit_warned;
	guint          monitors_ignored;

	/* For FAM, the _CHANGES_DONE event is not signalled, so we
	 * have to just use the _CHANGED event instead.
	 */
	gboolean       use_changed_event;

#ifdef PAUSE_ON_IO
	/* Timeout id for pausing when we get IO */
	guint          unpause_timeout_id;
#endif /* PAUSE_ON_IO */

	GHashTable    *pre_update;
	GHashTable    *pre_delete;
	guint          event_pairs_timeout_id;
};

typedef struct {
	GFile    *file;
	gchar    *file_uri;
	GFile    *other_file;
	gchar    *other_file_uri;
	gboolean  is_directory;
	GTimeVal  start_time;
	guint32   event_type;
} EventData;

enum {
	ITEM_CREATED,
	ITEM_UPDATED,
	ITEM_DELETED,
	ITEM_MOVED,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_ENABLED
};

static void           tracker_monitor_finalize     (GObject        *object);
static void           tracker_monitor_set_property (GObject        *object,
                                                    guint           prop_id,
                                                    const GValue   *value,
                                                    GParamSpec     *pspec);
static void           tracker_monitor_get_property (GObject        *object,
                                                    guint           prop_id,
                                                    GValue         *value,
                                                    GParamSpec     *pspec);
static guint          get_inotify_limit            (void);
static GFileMonitor * directory_monitor_new        (TrackerMonitor *monitor,
						    GFile          *file);
static void           directory_monitor_cancel     (GFileMonitor     *dir_monitor);


static void           event_data_free              (gpointer        data);
static void           emit_signal_for_event        (TrackerMonitor *monitor,
                                                    EventData      *event_data);

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE(TrackerMonitor, tracker_monitor, G_TYPE_OBJECT)

static void
tracker_monitor_class_init (TrackerMonitorClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_monitor_finalize;
	object_class->set_property = tracker_monitor_set_property;
	object_class->get_property = tracker_monitor_get_property;

	signals[ITEM_CREATED] =
		g_signal_new ("item-created",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__OBJECT_BOOLEAN,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_UPDATED] =
		g_signal_new ("item-updated",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__OBJECT_BOOLEAN,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_DELETED] =
		g_signal_new ("item-deleted",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__OBJECT_BOOLEAN,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN);
	signals[ITEM_MOVED] =
		g_signal_new ("item-moved",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__OBJECT_OBJECT_BOOLEAN_BOOLEAN,
		              G_TYPE_NONE,
		              4,
		              G_TYPE_OBJECT,
		              G_TYPE_OBJECT,
		              G_TYPE_BOOLEAN,
		              G_TYPE_BOOLEAN);

	g_object_class_install_property (object_class,
	                                 PROP_ENABLED,
	                                 g_param_spec_boolean ("enabled",
	                                                       "Enabled",
	                                                       "Enabled",
	                                                       TRUE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	g_type_class_add_private (object_class, sizeof (TrackerMonitorPrivate));
}

static void
tracker_monitor_init (TrackerMonitor *object)
{
	TrackerMonitorPrivate *priv;
	GFile                 *file;
	GFileMonitor          *monitor;
	const gchar           *name;

	object->private = TRACKER_MONITOR_GET_PRIVATE (object);

	priv = object->private;

	/* By default we enable monitoring */
	priv->enabled = TRUE;

	/* Create monitors table for this module */
	priv->monitors =
		g_hash_table_new_full (g_file_hash,
		                       (GEqualFunc) g_file_equal,
		                       (GDestroyNotify) g_object_unref,
		                       (GDestroyNotify) directory_monitor_cancel);

	priv->pre_update =
		g_hash_table_new_full (g_file_hash,
				       (GEqualFunc) g_file_equal,
				       (GDestroyNotify) g_object_unref,
				       event_data_free);
	priv->pre_delete =
		g_hash_table_new_full (g_file_hash,
				       (GEqualFunc) g_file_equal,
				       (GDestroyNotify) g_object_unref,
				       event_data_free);

	/* For the first monitor we get the type and find out if we
	 * are using inotify, FAM, polling, etc.
	 */
	file = g_file_new_for_path (g_get_home_dir ());
	monitor = g_file_monitor_directory (file,
	                                    G_FILE_MONITOR_WATCH_MOUNTS,
	                                    NULL,
	                                    NULL);

	priv->monitor_backend = G_OBJECT_TYPE (monitor);

	/* We use the name because the type itself is actually
	 * private and not available publically. Note this is
	 * subject to change, but unlikely of course.
	 */
	name = g_type_name (priv->monitor_backend);
	if (name) {
		/* Set limits based on backend... */
		if (strcmp (name, "GInotifyDirectoryMonitor") == 0) {
			/* Using inotify */
			g_message ("Monitor backend is Inotify");

			/* Setting limit based on kernel
			 * settings in /proc...
			 */
			priv->monitor_limit = get_inotify_limit ();

			/* We don't use 100% of the monitors, we allow other
			 * applications to have at least 500 or so to use
			 * between them selves. This only
			 * applies to inotify because it is a
			 * user shared resource.
			 */
			priv->monitor_limit -= 500;

			/* Make sure we don't end up with a
			 * negative maximum.
			 */
			priv->monitor_limit = MAX (priv->monitor_limit, 0);
		}
		else if (strcmp (name, "GFamDirectoryMonitor") == 0) {
			/* Using Fam */
			g_message ("Monitor backend is Fam");

			/* Setting limit to an arbitary limit
			 * based on testing
			 */
			priv->monitor_limit = 400;
			priv->use_changed_event = TRUE;
		}
		else if (strcmp (name, "GFenDirectoryMonitor") == 0) {
			/* Using Fen, what is this? */
			g_message ("Monitor backend is Fen");

			/* Guessing limit... */
			priv->monitor_limit = 8192;
		}
		else if (strcmp (name, "GWin32DirectoryMonitor") == 0) {
			/* Using Windows */
			g_message ("Monitor backend is Windows");

			/* Guessing limit... */
			priv->monitor_limit = 8192;
		}
		else {
			/* Unknown */
			g_warning ("Monitor backend:'%s' is unknown, we have no limits "
			           "in place because we don't know what we are dealing with!",
			           name);

			/* Guessing limit... */
			priv->monitor_limit = 100;
		}
	}

	g_file_monitor_cancel (monitor);
	g_object_unref (monitor);
	g_object_unref (file);

	g_message ("Monitor limit is %d", priv->monitor_limit);
}

static void
tracker_monitor_finalize (GObject *object)
{
	TrackerMonitorPrivate *priv;

	priv = TRACKER_MONITOR_GET_PRIVATE (object);

#ifdef PAUSE_ON_IO
	if (priv->unpause_timeout_id) {
		g_source_remove (priv->unpause_timeout_id);
	}
#endif /* PAUSE_ON_IO */

	if (priv->event_pairs_timeout_id) {
		g_source_remove (priv->event_pairs_timeout_id);
	}

	g_hash_table_unref (priv->pre_update);
	g_hash_table_unref (priv->pre_delete);
	g_hash_table_unref (priv->monitors);

	G_OBJECT_CLASS (tracker_monitor_parent_class)->finalize (object);
}

static void
tracker_monitor_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
	switch (prop_id) {
	case PROP_ENABLED:
		tracker_monitor_set_enabled (TRACKER_MONITOR (object),
		                             g_value_get_boolean (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_monitor_get_property (GObject      *object,
                              guint         prop_id,
                              GValue       *value,
                              GParamSpec   *pspec)
{
	TrackerMonitorPrivate *priv;

	priv = TRACKER_MONITOR_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_ENABLED:
		g_value_set_boolean (value, priv->enabled);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static guint
get_inotify_limit (void)
{
	GError      *error = NULL;
	const gchar *filename;
	gchar       *contents = NULL;
	guint        limit;

	filename = "/proc/sys/fs/inotify/max_user_watches";

	if (!g_file_get_contents (filename,
	                          &contents,
	                          NULL,
	                          &error)) {
		g_warning ("Couldn't get INotify monitor limit from:'%s', %s",
		           filename,
		           error ? error->message : "no error given");
		g_clear_error (&error);

		/* Setting limit to an arbitary limit */
		limit = 8192;
	} else {
		limit = atoi (contents);
		g_free (contents);
	}

	return limit;
}

#ifdef PAUSE_ON_IO

static gboolean
unpause_cb (gpointer data)
{
	TrackerMonitor *monitor;

	monitor = data;

	g_message ("Resuming indexing now we have stopped "
	           "receiving monitor events for %d seconds",
	           PAUSE_ON_IO_SECONDS);

	monitor->private->unpause_timeout_id = 0;
	tracker_status_set_is_paused_for_io (FALSE);

	return FALSE;
}

#endif /* PAUSE_ON_IO */

static gboolean
check_is_directory (TrackerMonitor *monitor,
		    GFile          *file)
{
	GFileType file_type;

	file_type = g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL);

	if (file_type == G_FILE_TYPE_DIRECTORY)
		return TRUE;

	if (file_type == G_FILE_TYPE_UNKNOWN) {
		/* Whatever it was, it's gone. Check the monitors
		 * hashtable to know whether it was a directory
		 * we knew about
		 */
		if (g_hash_table_lookup (monitor->private->monitors, file) != NULL)
			return TRUE;
	}

	return FALSE;
}

static EventData *
event_data_new (GFile    *file,
                GFile    *other_file,
                gboolean  is_directory,
                guint32   event_type)
{
	EventData *event;
	GTimeVal now;

	event = g_slice_new0 (EventData);
	g_get_current_time (&now);

	event->file = g_object_ref (file);
	event->file_uri = g_file_get_uri (file);
	if (other_file) {
		event->other_file = g_object_ref (other_file);
		event->other_file_uri = g_file_get_uri (other_file);
	} else {
		event->other_file = NULL;
		event->other_file_uri = NULL;
	}
	event->is_directory = is_directory;
	event->start_time = now;
	event->event_type = event_type;

	return event;
}

static void
event_data_free (gpointer data)
{
	EventData *event;

	event = data;

	g_object_unref (event->file);
	g_free (event->file_uri);
	if (event->other_file) {
		g_object_unref (event->other_file);
		g_free (event->other_file_uri);
	}
	g_slice_free (EventData, data);
}

static gboolean
tracker_monitor_move (TrackerMonitor *monitor,
                      GFile          *old_file,
                      GFile          *new_file)
{
	GHashTableIter iter;
	GHashTable *new_monitors;
	gchar *old_prefix;
	gpointer iter_file, iter_file_monitor;
	guint items_moved = 0;

	/* So this is tricky. What we have to do is:
	 *
	 * 1) Add all monitors for the new_file directory hierarchy
	 * 2) Then remove the monitors for old_file
	 *
	 * This order is necessary because inotify can reuse watch
	 * descriptors, and libinotify will remove handles
	 * asynchronously on IN_IGNORE, so the opposite sequence
	 * may possibly remove valid, just added, monitors.
	 */
	new_monitors = g_hash_table_new_full (g_file_hash,
	                                      (GEqualFunc) g_file_equal,
	                                      (GDestroyNotify) g_object_unref,
	                                      NULL);
	old_prefix = g_file_get_path (old_file);

	/* Find out which subdirectories should have a file monitor added */
	g_hash_table_iter_init (&iter, monitor->private->monitors);
	while (g_hash_table_iter_next (&iter, &iter_file, &iter_file_monitor)) {
		GFile *f;
		gchar *old_path, *new_path;
		gchar *new_prefix;
		gchar *p;

		if (!g_file_has_prefix (iter_file, old_file) &&
		    !g_file_equal (iter_file, old_file)) {
			continue;
		}

		old_path = g_file_get_path (iter_file);
		p = strstr (old_path, old_prefix);

		if (!p || strcmp (p, old_prefix) == 0) {
			g_free (old_path);
			continue;
		}

		/* Move to end of prefix */
		p += strlen (old_prefix) + 1;

		/* Check this is not the end of the string */
		if (*p == '\0') {
			g_free (old_path);
			continue;
		}

		new_prefix = g_file_get_path (new_file);
		new_path = g_build_path (G_DIR_SEPARATOR_S, new_prefix, p, NULL);
		g_free (new_prefix);

		f = g_file_new_for_path (new_path);
		g_free (new_path);

		if (!g_hash_table_lookup (new_monitors, f)) {
			g_hash_table_insert (new_monitors, f, GINT_TO_POINTER (1));
		} else {
			g_object_unref (f);
		}

		g_free (old_path);
		items_moved++;
	}

	/* Add a new monitor for the top level directory */
	tracker_monitor_add (monitor, new_file);

	/* Add a new monitor for all subdirectories */
	g_hash_table_iter_init (&iter, new_monitors);
	while (g_hash_table_iter_next (&iter, &iter_file, NULL)) {
		tracker_monitor_add (monitor, iter_file);
		g_hash_table_iter_remove (&iter);
	}

	/* Remove the monitor for the old top level directory hierarchy */
	tracker_monitor_remove_recursively (monitor, old_file);

	g_hash_table_unref (new_monitors);
	g_free (old_prefix);

	return items_moved > 0;
}

static const gchar *
monitor_event_to_string (GFileMonitorEvent event_type)
{
	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CHANGED:
		return "G_FILE_MONITOR_EVENT_CHANGED";
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		return "G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT";
	case G_FILE_MONITOR_EVENT_DELETED:
		return "G_FILE_MONITOR_EVENT_DELETED";
	case G_FILE_MONITOR_EVENT_CREATED:
		return "G_FILE_MONITOR_EVENT_CREATED";
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		return "G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED";
	case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
		return "G_FILE_MONITOR_EVENT_PRE_UNMOUNT";
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
		return "G_FILE_MONITOR_EVENT_UNMOUNTED";
	case G_FILE_MONITOR_EVENT_MOVED:
		return "G_FILE_MONITOR_EVENT_MOVED";
	}

	return "unknown";
}

static void
emit_signal_for_event (TrackerMonitor *monitor,
		       EventData      *event_data)
{
	switch (event_data->event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
		g_debug ("Emitting ITEM_CREATED for (%s) '%s'",
		         event_data->is_directory ? "DIRECTORY" : "FILE",
		         event_data->file_uri);
		g_signal_emit (monitor,
			       signals[ITEM_CREATED], 0,
			       event_data->file,
		               event_data->is_directory);
		/* Note that if the created item is a Directory, we must NOT
		 * add automatically a new monitor. */
		break;

	case G_FILE_MONITOR_EVENT_CHANGED:
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		g_debug ("Emitting ITEM_UPDATED for (%s) '%s'",
		         event_data->is_directory ? "DIRECTORY" : "FILE",
		         event_data->file_uri);
		g_signal_emit (monitor,
			       signals[ITEM_UPDATED], 0,
			       event_data->file,
			       event_data->is_directory);
		break;

	case G_FILE_MONITOR_EVENT_DELETED:
		g_debug ("Emitting ITEM_DELETED for (%s) '%s'",
		         event_data->is_directory ? "DIRECTORY" : "FILE",
		         event_data->file_uri);
		g_signal_emit (monitor,
		               signals[ITEM_DELETED], 0,
		               event_data->file,
		               event_data->is_directory);
		/* Remove monitors recursively */
		if (event_data->is_directory) {
			tracker_monitor_remove_recursively (monitor,
							    event_data->file);
		}
		break;

	case G_FILE_MONITOR_EVENT_MOVED:
		g_debug ("Emitting ITEM_MOVED for (%s) '%s'->'%s'",
		         event_data->is_directory ? "DIRECTORY" : "FILE",
		         event_data->file_uri,
		         event_data->other_file_uri);
		g_signal_emit (monitor,
		               signals[ITEM_MOVED], 0,
		               event_data->file,
		               event_data->other_file,
		               event_data->is_directory,
		               TRUE);
		/* Move monitors to the new place */
		if (event_data->is_directory) {
			tracker_monitor_move (monitor,
			                      event_data->file,
			                      event_data->other_file);
		}
		break;
	default:
		break;
	}
}

static void
event_pairs_process_in_ht (TrackerMonitor *monitor,
                           GHashTable     *ht,
                           GTimeVal       *now)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init (&iter, ht);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		EventData *event_data = value;
		glong seconds;

		seconds = now->tv_sec - event_data->start_time.tv_sec;

		if (seconds < 2) {
			continue;
		}

		g_debug ("Event '%s' for URI '%s' has timed out (%ld seconds have elapsed)",
		         monitor_event_to_string (event_data->event_type),
		         event_data->file_uri,
		         seconds);

		emit_signal_for_event (monitor, event_data);
		g_hash_table_iter_remove (&iter);
	}
}

static gboolean
event_pairs_timeout_cb (gpointer user_data)
{
	TrackerMonitor *monitor;
	GTimeVal now;

	monitor = user_data;
	g_get_current_time (&now);

	/* Process PRE-UPDATE hash table */
	event_pairs_process_in_ht (monitor, monitor->private->pre_update, &now);

	/* Process PRE-DELETE hash table */
	event_pairs_process_in_ht (monitor, monitor->private->pre_delete, &now);

	if (g_hash_table_size (monitor->private->pre_update) > 0 ||
	    g_hash_table_size (monitor->private->pre_delete) > 0) {
		return TRUE;
	}

	g_debug ("No more events to pair");
	monitor->private->event_pairs_timeout_id = 0;
	return FALSE;
}

static void
monitor_event_cb (GFileMonitor	    *file_monitor,
		  GFile		    *file,
		  GFile		    *other_file,
		  GFileMonitorEvent  event_type,
		  gpointer	     user_data)
{
	TrackerMonitor *monitor;
	gchar *file_uri;
	gchar *other_file_uri;
	gboolean is_directory;

	monitor = user_data;

	if (G_UNLIKELY (!monitor->private->enabled)) {
		g_debug ("Silently dropping monitor event, monitor disabled for now");
		return;
	}

	/* Get URIs as paths may not be in UTF-8 */
	file_uri = g_file_get_uri (file);
	other_file_uri = other_file ? g_file_get_uri (other_file) : NULL;

	g_debug ("Received monitor event:%d->'%s' for file:'%s' and other file:'%s'",
	         event_type,
	         monitor_event_to_string (event_type),
	         file_uri,
	         other_file_uri ? other_file_uri : "");

	is_directory = check_is_directory (monitor, file);

#ifdef PAUSE_ON_IO
	if (monitor->private->unpause_timeout_id != 0) {
		g_source_remove (monitor->private->unpause_timeout_id);
	} else {
		g_message ("Pausing indexing because we are "
			   "receiving monitor events");

		tracker_status_set_is_paused_for_io (TRUE);
	}

	monitor->private->unpause_timeout_id =
		g_timeout_add_seconds (PAUSE_ON_IO_SECONDS,
				       unpause_cb,
				       monitor);
#endif /* PAUSE_ON_IO */

	if (!is_directory) {
		/* FILE Events */

		switch (event_type) {
		case G_FILE_MONITOR_EVENT_CREATED: {
			/*  - When a G_FILE_MONITOR_EVENT_CREATED(A) is received,
			 *    -- Add it to the cache, replacing any previous element
			 *       (shouldn't be any)
			 */
			g_hash_table_replace (monitor->private->pre_update,
			                      g_object_ref (file),
			                      event_data_new (file, NULL, FALSE, event_type));
			break;
		}

		case G_FILE_MONITOR_EVENT_CHANGED: {
			/* If use_changed_event, treat as an ATTRIBUTE_CHANGED. Otherwise,
			 * assume there will be a CHANGES_DONE_HINT afterwards... */
			if (!monitor->private->use_changed_event) {
				EventData *previous_update_event_data;

				/* Get previous event data, if any */
				previous_update_event_data = g_hash_table_lookup (monitor->private->pre_update, file);

				/* If there is a previous ATTRIBUTE_CHANGED still not notified,
				 * remove it, as we know there will be a CHANGES_DONE_HINT afterwards
				 */
				if (previous_update_event_data &&
				    previous_update_event_data->event_type == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) {
					g_hash_table_remove (monitor->private->pre_update, file);
				}

				break;
			}
		} /* Else, Fall through and treat as an ATTRIBUTE_CHANGED */

		case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED: {
			if (!g_hash_table_lookup (monitor->private->pre_update, file)) {
				g_hash_table_insert (monitor->private->pre_update,
				                     g_object_ref (file),
				                     event_data_new (file, NULL, FALSE, event_type));
			}

			break;
		}

		case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: {
			EventData *previous_update_event_data;

			/* Get previous event data, if any */
			previous_update_event_data = g_hash_table_lookup (monitor->private->pre_update, file);

			if (previous_update_event_data) {
				emit_signal_for_event (monitor, previous_update_event_data);
				g_hash_table_remove (monitor->private->pre_update, file);
			} else {
				EventData *new_event;

				new_event = event_data_new (file, NULL, FALSE, event_type);
				emit_signal_for_event (monitor, new_event);
				event_data_free (new_event);
			}

			break;
		}

		case G_FILE_MONITOR_EVENT_DELETED: {
			EventData *new_event;

			new_event = event_data_new (file, NULL, FALSE, event_type);
			emit_signal_for_event (monitor, new_event);
			event_data_free (new_event);

			break;
		}
		case G_FILE_MONITOR_EVENT_MOVED: {
			EventData *new_event;

			new_event = event_data_new (file, other_file, FALSE, event_type);
			emit_signal_for_event (monitor, new_event);
			event_data_free (new_event);

			break;
		}
		case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
		case G_FILE_MONITOR_EVENT_UNMOUNTED:
			/* Do nothing */
			break;
		}
	} else {
		/* DIRECTORY Events */

		switch (event_type) {
		case G_FILE_MONITOR_EVENT_CREATED:
		case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		case G_FILE_MONITOR_EVENT_CHANGED: {
			EventData *previous_delete_event_data;

			/* If any previous event on this item, notify it
			 *  before creating it */
			previous_delete_event_data = g_hash_table_lookup (monitor->private->pre_delete, file);
			if (previous_delete_event_data) {
				emit_signal_for_event (monitor, previous_delete_event_data);
				g_hash_table_remove (monitor->private->pre_delete, file);
			}

			if (!g_hash_table_lookup (monitor->private->pre_update, file)) {
				g_hash_table_insert (monitor->private->pre_update,
				                     g_object_ref (file),
				                     event_data_new (file, NULL, TRUE, event_type));
			}

			break;
		}

		case G_FILE_MONITOR_EVENT_DELETED: {
			EventData *previous_update_event_data;
			EventData *previous_delete_event_data;

			/* If any previous update event on this item, notify it */
			previous_update_event_data = g_hash_table_lookup (monitor->private->pre_update, file);
			if (previous_update_event_data) {
				emit_signal_for_event (monitor, previous_update_event_data);
				g_hash_table_remove (monitor->private->pre_update, file);
			}

			/* Check if there is a previous delete event */
			previous_delete_event_data = g_hash_table_lookup (monitor->private->pre_delete, file);
			if (previous_delete_event_data &&
			    previous_delete_event_data->event_type == G_FILE_MONITOR_EVENT_MOVED) {


				g_debug ("Processing MOVE(A->B) + DELETE(A) as MOVE(A->B) for directory '%s->%s'",
				         previous_delete_event_data->file_uri,
				         previous_delete_event_data->other_file_uri);

				emit_signal_for_event (monitor, previous_delete_event_data);
				g_hash_table_remove (monitor->private->pre_delete, file);
			}  else {
				/* If no previous, add to HT */
				g_hash_table_replace (monitor->private->pre_delete,
				                      g_object_ref (file),
				                      event_data_new (file, NULL, TRUE, event_type));
			}

			break;
		}

		case G_FILE_MONITOR_EVENT_MOVED: {
			EventData *previous_update_event_data;
			EventData *previous_delete_event_data;

			/* If any previous update event on this item, notify it */
			previous_update_event_data = g_hash_table_lookup (monitor->private->pre_update, file);
			if (previous_update_event_data) {
				emit_signal_for_event (monitor, previous_update_event_data);
				g_hash_table_remove (monitor->private->pre_update, file);
			}

			/* Check if there is a previous delete event */
			previous_delete_event_data = g_hash_table_lookup (monitor->private->pre_delete, file);
			if (previous_delete_event_data &&
			    previous_delete_event_data->event_type == G_FILE_MONITOR_EVENT_DELETED) {
				EventData *new_event;

				new_event = event_data_new (file, other_file, TRUE, G_FILE_MONITOR_EVENT_MOVED);
				g_debug ("Processing DELETE(A) + MOVE(A->B) as MOVE(A->B) for directory '%s->%s'",
				         new_event->file_uri,
				         new_event->other_file_uri);

				emit_signal_for_event (monitor, new_event);
				event_data_free (new_event);

				/* And remove the previous DELETE */
				g_hash_table_remove (monitor->private->pre_delete, file);
			} else {
				/* If no previous, add to HT */
				g_hash_table_replace (monitor->private->pre_delete,
				                      g_object_ref (file),
				                      event_data_new (file, other_file, TRUE, event_type));
			}

			break;
		}

		case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
		case G_FILE_MONITOR_EVENT_UNMOUNTED:
			/* Do nothing */
			break;
		}
	}

	if (g_hash_table_size (monitor->private->pre_update) > 0 ||
	    g_hash_table_size (monitor->private->pre_delete) > 0) {
		if (monitor->private->event_pairs_timeout_id == 0) {
			g_debug ("Waiting for event pairs");
			monitor->private->event_pairs_timeout_id =
				g_timeout_add_seconds (CACHE_LIFETIME_SECONDS,
						       event_pairs_timeout_cb,
						       monitor);
		}
	} else {
		if (monitor->private->event_pairs_timeout_id != 0) {
			g_source_remove (monitor->private->event_pairs_timeout_id);
		}

		monitor->private->event_pairs_timeout_id = 0;
	}

	g_free (file_uri);
	g_free (other_file_uri);
}

static GFileMonitor *
directory_monitor_new (TrackerMonitor *monitor,
		       GFile          *file)
{
	GFileMonitor *file_monitor;
	GError *error = NULL;

	file_monitor = g_file_monitor_directory (file,
						 G_FILE_MONITOR_SEND_MOVED | G_FILE_MONITOR_WATCH_MOUNTS,
						 NULL,
						 &error);

	if (error) {
		gchar *path;

		path = g_file_get_path (file);
		g_warning ("Could not add monitor for path:'%s', %s",
			   path, error->message);

		g_error_free (error);
		g_free (path);

		return NULL;
	}

	g_signal_connect (file_monitor, "changed",
			  G_CALLBACK (monitor_event_cb),
			  monitor);

	return file_monitor;
}

static void
directory_monitor_cancel (GFileMonitor *monitor)
{
	if (monitor) {
		g_file_monitor_cancel (G_FILE_MONITOR (monitor));
		g_object_unref (monitor);
	}
}

TrackerMonitor *
tracker_monitor_new (void)
{
	return g_object_new (TRACKER_TYPE_MONITOR, NULL);
}

gboolean
tracker_monitor_get_enabled (TrackerMonitor *monitor)
{
	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);

	return monitor->private->enabled;
}

void
tracker_monitor_set_enabled (TrackerMonitor *monitor,
                             gboolean        enabled)
{
	GList *keys, *k;

	g_return_if_fail (TRACKER_IS_MONITOR (monitor));

	/* Don't replace all monitors if we are already
	 * enabled/disabled.
	 */
	if (monitor->private->enabled == enabled) {
		return;
	}

	monitor->private->enabled = enabled;
	g_object_notify (G_OBJECT (monitor), "enabled");

	keys = g_hash_table_get_keys (monitor->private->monitors);

	/* Update state on all monitored dirs */
	for (k = keys; k != NULL; k = k->next) {
		GFile *file;

		file = k->data;

		if (enabled) {
			GFileMonitor *dir_monitor;

			dir_monitor = directory_monitor_new (monitor, file);
			g_hash_table_replace (monitor->private->monitors,
			                      g_object_ref (file), dir_monitor);
		} else {
			/* Remove monitor */
			g_hash_table_replace (monitor->private->monitors,
			                      g_object_ref (file), NULL);
		}
	}

	g_list_free (keys);
}

gboolean
tracker_monitor_add (TrackerMonitor *monitor,
                     GFile          *file)
{
	GFileMonitor *dir_monitor = NULL;
	gchar *path;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	if (g_hash_table_lookup (monitor->private->monitors, file)) {
		return TRUE;
	}

	/* Cap the number of monitors */
	if (g_hash_table_size (monitor->private->monitors) >= monitor->private->monitor_limit) {
		monitor->private->monitors_ignored++;

		if (!monitor->private->monitor_limit_warned) {
			g_warning ("The maximum number of monitors to set (%d) "
			           "has been reached, not adding any new ones",
			           monitor->private->monitor_limit);
			monitor->private->monitor_limit_warned = TRUE;
		}

		return FALSE;
	}

	path = g_file_get_path (file);

	if (monitor->private->enabled) {
		/* We don't check if a file exists or not since we might want
		 * to monitor locations which don't exist yet.
		 *
		 * Also, we assume ALL paths passed are directories.
		 */
		dir_monitor = directory_monitor_new (monitor, file);

		if (!dir_monitor) {
			g_warning ("Could not add monitor for path:'%s'",
			           path);
			g_free (path);
			return FALSE;
		}
	}

	/* NOTE: it is ok to add a NULL file_monitor, when our
	 * enabled/disabled state changes, we iterate all keys and
	 * add or remove monitors.
	 */
	g_hash_table_insert (monitor->private->monitors,
	                     g_object_ref (file),
	                     dir_monitor);

	g_debug ("Added monitor for path:'%s', total monitors:%d",
	         path,
	         g_hash_table_size (monitor->private->monitors));

	g_free (path);

	return TRUE;
}

gboolean
tracker_monitor_remove (TrackerMonitor *monitor,
                        GFile          *file)
{
	gboolean removed;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	removed = g_hash_table_remove (monitor->private->monitors, file);

	if (removed) {
		gchar *path;

		path = g_file_get_path (file);
		g_debug ("Removed monitor for path:'%s', total monitors:%d",
		         path,
		         g_hash_table_size (monitor->private->monitors));

		g_free (path);
	}

	return removed;
}

gboolean
tracker_monitor_remove_recursively (TrackerMonitor *monitor,
                                    GFile          *file)
{
	GHashTableIter iter;
	gpointer iter_file, iter_file_monitor;
	guint items_removed = 0;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	g_hash_table_iter_init (&iter, monitor->private->monitors);
	while (g_hash_table_iter_next (&iter, &iter_file, &iter_file_monitor)) {
		gchar *path;

		if (!g_file_has_prefix (iter_file, file) &&
		    !g_file_equal (iter_file, file)) {
			continue;
		}

		path = g_file_get_path (iter_file);

		g_hash_table_iter_remove (&iter);

		g_debug ("Removed monitor for path:'%s', total monitors:%d",
		         path,
		         g_hash_table_size (monitor->private->monitors));

		g_free (path);

		/* We reset this because now it is possible we have limit - 1 */
		monitor->private->monitor_limit_warned = FALSE;
		items_removed++;
	}

	return items_removed > 0;
}

gboolean
tracker_monitor_is_watched (TrackerMonitor *monitor,
                            GFile          *file)
{
	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	return g_hash_table_lookup (monitor->private->monitors, file) != NULL;
}

gboolean
tracker_monitor_is_watched_by_string (TrackerMonitor *monitor,
                                      const gchar    *path)
{
	GFile      *file;
	gboolean    watched;

	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), FALSE);
	g_return_val_if_fail (path != NULL, FALSE);

	file = g_file_new_for_path (path);
	watched = g_hash_table_lookup (monitor->private->monitors, file) != NULL;
	g_object_unref (file);

	return watched;
}

guint
tracker_monitor_get_count (TrackerMonitor *monitor)
{
	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), 0);

	return g_hash_table_size (monitor->private->monitors);
}

guint
tracker_monitor_get_ignored (TrackerMonitor *monitor)
{
	g_return_val_if_fail (TRACKER_IS_MONITOR (monitor), 0);

	return monitor->private->monitors_ignored;
}
