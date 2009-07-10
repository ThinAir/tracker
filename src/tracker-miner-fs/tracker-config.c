/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia (urho.konttori@nokia.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-type-utils.h>

#include "tracker-config.h"

#define TRACKER_CONFIG_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TRACKER_TYPE_CONFIG, TrackerConfigPrivate))

/* GKeyFile defines */
#define GROUP_GENERAL				 "General"
#define KEY_VERBOSITY				 "Verbosity"
#define KEY_INITIAL_SLEEP			 "InitialSleep"

#define GROUP_MONITORS				 "Monitors"
#define KEY_ENABLE_WATCHES		         "EnableWatches"
#define KEY_WATCH_DIRECTORY_ROOTS		 "WatchDirectoryRoots"
#define KEY_CRAWL_DIRECTORY_ROOTS		 "CrawlDirectory"
#define KEY_NO_WATCH_DIRECTORY_ROOTS		 "NoWatchDirectory"

#define GROUP_INDEXING				 "Indexing"
#define KEY_THROTTLE				 "Throttle"
#define KEY_ENABLE_THUMBNAILS			 "EnableThumbnails"
#define KEY_DISABLED_MODULES			 "DisabledModules"
#define KEY_DISABLE_INDEXING_ON_BATTERY		 "BatteryIndex"
#define KEY_DISABLE_INDEXING_ON_BATTERY_INIT	 "BatteryIndexInitial"
#define KEY_LOW_DISK_SPACE_LIMIT		 "LowDiskSpaceLimit"
#define KEY_INDEX_MOUNTED_DIRECTORIES		 "IndexMountedDirectories"
#define KEY_INDEX_REMOVABLE_DEVICES		 "IndexRemovableMedia"

/* Default values */
#define DEFAULT_VERBOSITY			 0
#define DEFAULT_INITIAL_SLEEP			 45	  /* 0->1000 */
#define DEFAULT_ENABLE_WATCHES			 TRUE
#define DEFAULT_THROTTLE			 0	  /* 0->20 */
#define DEFAULT_ENABLE_THUMBNAILS		 TRUE
#define DEFAULT_DISABLE_INDEXING_ON_BATTERY	 TRUE
#define DEFAULT_DISABLE_INDEXING_ON_BATTERY_INIT FALSE
#define DEFAULT_INDEX_MOUNTED_DIRECTORIES	 TRUE
#define DEFAULT_INDEX_REMOVABLE_DEVICES		 TRUE
#define DEFAULT_LOW_DISK_SPACE_LIMIT		 1	  /* 0->100 / -1 */

typedef struct _TrackerConfigPrivate TrackerConfigPrivate;

struct _TrackerConfigPrivate {
	GFile	     *file;
	GFileMonitor *monitor;

	GKeyFile     *key_file;

	/* General */
	gint	      verbosity;
	gint	      initial_sleep;

	/* Watches */
	GSList	     *watch_directory_roots;
	GSList	     *crawl_directory_roots;
	GSList	     *no_watch_directory_roots;
	gboolean      enable_watches;

	/* Indexing */
	gint	      throttle;
	gboolean      enable_thumbnails;
	GSList	     *disabled_modules;

	gboolean      disable_indexing_on_battery;
	gboolean      disable_indexing_on_battery_init;
	gint	      low_disk_space_limit;
	gboolean      index_mounted_directories;
	gboolean      index_removable_devices;
};

static void     config_finalize             (GObject       *object);
static void     config_get_property         (GObject       *object,
					     guint          param_id,
					     GValue        *value,
					     GParamSpec    *pspec);
static void     config_set_property         (GObject       *object,
					     guint          param_id,
					     const GValue  *value,
					     GParamSpec    *pspec);
static void     config_load                 (TrackerConfig *config);
static gboolean config_save                 (TrackerConfig *config);
static void     config_create_with_defaults (GKeyFile      *key_file,
					     gboolean       overwrite);

enum {
	PROP_0,

	/* General */
	PROP_VERBOSITY,
	PROP_INITIAL_SLEEP,

	/* Watches */
	PROP_WATCH_DIRECTORY_ROOTS,
	PROP_CRAWL_DIRECTORY_ROOTS,
	PROP_NO_WATCH_DIRECTORY_ROOTS,
	PROP_ENABLE_WATCHES,

	/* Indexing */
	PROP_THROTTLE,
	PROP_ENABLE_THUMBNAILS,
	PROP_DISABLED_MODULES,
	PROP_DISABLE_INDEXING_ON_BATTERY,
	PROP_DISABLE_INDEXING_ON_BATTERY_INIT,
	PROP_LOW_DISK_SPACE_LIMIT,
	PROP_INDEX_MOUNTED_DIRECTORIES,
	PROP_INDEX_REMOVABLE_DEVICES,
};

G_DEFINE_TYPE (TrackerConfig, tracker_config, G_TYPE_OBJECT);

static void
tracker_config_class_init (TrackerConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize	   = config_finalize;
	object_class->get_property = config_get_property;
	object_class->set_property = config_set_property;

	/* General */
	g_object_class_install_property (object_class,
					 PROP_VERBOSITY,
					 g_param_spec_int ("verbosity",
							   "Log verbosity",
							   "How much logging we have "
							   "(0=errors, 1=minimal, 2=detailed, 3=debug)",
							   0,
							   3,
							   DEFAULT_VERBOSITY,
							   G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_INITIAL_SLEEP,
					 g_param_spec_int ("initial-sleep",
							   "Initial sleep",
							   "Initial sleep time in seconds "
							   "(0->1000)",
							   0,
							   1000,
							   DEFAULT_INITIAL_SLEEP,
							   G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	/* Watches */
	g_object_class_install_property (object_class,
					 PROP_WATCH_DIRECTORY_ROOTS,
					 g_param_spec_pointer ("watch-directory-roots",
							       "Watched directory roots",
							       "This is a GSList of directory roots "
							       "to index and watch",
							       G_PARAM_READABLE));
	g_object_class_install_property (object_class,
					 PROP_CRAWL_DIRECTORY_ROOTS,
					 g_param_spec_pointer ("crawl-directory-roots",
							       "Crawl directory roots",
							       "This is a GSList of directory roots "
							       "to index but NOT watch",
							       G_PARAM_READABLE));
	g_object_class_install_property (object_class,
					 PROP_NO_WATCH_DIRECTORY_ROOTS,
					 g_param_spec_pointer ("no-watch-directory-roots",
							       "Not watched directory roots",
							       "This is a GSList of directory roots "
							       "to NOT index and NOT watch",
							       G_PARAM_READABLE));
	g_object_class_install_property (object_class,
					 PROP_ENABLE_WATCHES,
					 g_param_spec_boolean ("enable-watches",
							       "Enable watches",
							       "You can disable all watches "
							       "by setting this FALSE",
							       DEFAULT_ENABLE_WATCHES,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	/* Indexing */
	g_object_class_install_property (object_class,
					 PROP_THROTTLE,
					 g_param_spec_int ("throttle",
							   "Throttle",
							   "Throttle indexing, higher value "
							   "is slower (0->20)",
							   0,
							   20,
							   DEFAULT_THROTTLE,
							   G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_ENABLE_THUMBNAILS,
					 g_param_spec_boolean ("enable-thumbnails",
							       "Enable thumbnails",
							       "Create thumbnails from image based files",
							       DEFAULT_ENABLE_THUMBNAILS,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_DISABLED_MODULES,
					 g_param_spec_pointer ("disabled-modules",
							       "Disabled modules",
							       "Modules to disable, like 'files', etc.",
							       G_PARAM_READABLE));
;
	g_object_class_install_property (object_class,
					 PROP_DISABLE_INDEXING_ON_BATTERY,
					 g_param_spec_boolean ("disable-indexing-on-battery",
							       "Disable indexing on battery",
							       "Don't index when using AC battery",
							       DEFAULT_DISABLE_INDEXING_ON_BATTERY,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_DISABLE_INDEXING_ON_BATTERY_INIT,
					 g_param_spec_boolean ("disable-indexing-on-battery-init",
							       "Disable indexing on battery",
							       "Don't index when using AC "
							       "battery initially",
							       DEFAULT_DISABLE_INDEXING_ON_BATTERY_INIT,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_LOW_DISK_SPACE_LIMIT,
					 g_param_spec_int ("low-disk-space-limit",
							   "Low disk space limit",
							   "Pause the indexer when the "
							   "disk space is below this percentage "
							   "(-1=off, 0->100)",
							   -1,
							   100,
							   DEFAULT_LOW_DISK_SPACE_LIMIT,
							   G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_INDEX_MOUNTED_DIRECTORIES,
					 g_param_spec_boolean ("index-mounted-directories",
							       "Index mounted directories",
							       "Don't traverse mounted directories "
							       "which are not on the same file system",
							       DEFAULT_INDEX_MOUNTED_DIRECTORIES,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	g_object_class_install_property (object_class,
					 PROP_INDEX_REMOVABLE_DEVICES,
					 g_param_spec_boolean ("index-removable-devices",
							       "index removable devices",
							       "Don't traverse mounted directories "
							       "which are for removable devices",
							       DEFAULT_INDEX_REMOVABLE_DEVICES,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	g_type_class_add_private (object_class, sizeof (TrackerConfigPrivate));
}

static void
tracker_config_init (TrackerConfig *object)
{
	TrackerConfigPrivate *priv;

	priv = TRACKER_CONFIG_GET_PRIVATE (object);

	priv->key_file = g_key_file_new ();
}

static void
config_finalize (GObject *object)
{
	TrackerConfigPrivate *priv;

	priv = TRACKER_CONFIG_GET_PRIVATE (object);

	g_slist_foreach (priv->watch_directory_roots, (GFunc) g_free, NULL);
	g_slist_free (priv->watch_directory_roots);

	g_slist_foreach (priv->crawl_directory_roots, (GFunc) g_free, NULL);
	g_slist_free (priv->crawl_directory_roots);

	g_slist_foreach (priv->no_watch_directory_roots, (GFunc) g_free, NULL);
	g_slist_free (priv->no_watch_directory_roots);

	g_slist_foreach (priv->disabled_modules, (GFunc) g_free, NULL);
	g_slist_free (priv->disabled_modules);

	if (priv->key_file) {
		g_key_file_free (priv->key_file);
	}

	if (priv->monitor) {
		g_object_unref (priv->monitor);
	}

	if (priv->file) {
		g_object_unref (priv->file);
	}

	(G_OBJECT_CLASS (tracker_config_parent_class)->finalize) (object);
}

static void
config_get_property (GObject	*object,
		     guint	 param_id,
		     GValue	*value,
		     GParamSpec *pspec)
{
	TrackerConfigPrivate *priv;

	priv = TRACKER_CONFIG_GET_PRIVATE (object);

	switch (param_id) {
		/* General */
	case PROP_VERBOSITY:
		g_value_set_int (value, priv->verbosity);
		break;
	case PROP_INITIAL_SLEEP:
		g_value_set_int (value, priv->initial_sleep);
		break;

		/* Watches */
	case PROP_WATCH_DIRECTORY_ROOTS:
		g_value_set_pointer (value, priv->watch_directory_roots);
		break;
	case PROP_CRAWL_DIRECTORY_ROOTS:
		g_value_set_pointer (value, priv->crawl_directory_roots);
		break;
	case PROP_NO_WATCH_DIRECTORY_ROOTS:
		g_value_set_pointer (value, priv->no_watch_directory_roots);
		break;
	case PROP_ENABLE_WATCHES:
		g_value_set_boolean (value, priv->enable_watches);
		break;

		/* Indexing */
	case PROP_THROTTLE:
		g_value_set_int (value, priv->throttle);
		break;
	case PROP_ENABLE_THUMBNAILS:
		g_value_set_boolean (value, priv->enable_thumbnails);
		break;
	case PROP_DISABLED_MODULES:
		g_value_set_pointer (value, priv->disabled_modules);
		break;
	case PROP_DISABLE_INDEXING_ON_BATTERY:
		g_value_set_boolean (value, priv->disable_indexing_on_battery);
		break;
	case PROP_DISABLE_INDEXING_ON_BATTERY_INIT:
		g_value_set_boolean (value, priv->disable_indexing_on_battery_init);
		break;
	case PROP_LOW_DISK_SPACE_LIMIT:
		g_value_set_int (value, priv->low_disk_space_limit);
		break;
	case PROP_INDEX_MOUNTED_DIRECTORIES:
		g_value_set_boolean (value, priv->index_mounted_directories);
		break;
	case PROP_INDEX_REMOVABLE_DEVICES:
		g_value_set_boolean (value, priv->index_removable_devices);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static void
config_set_property (GObject	  *object,
		     guint	   param_id,
		     const GValue *value,
		     GParamSpec	  *pspec)
{
	switch (param_id) {
		/* General */
	case PROP_VERBOSITY:
		tracker_config_set_verbosity (TRACKER_CONFIG (object),
					      g_value_get_int (value));
		break;
	case PROP_INITIAL_SLEEP:
		tracker_config_set_initial_sleep (TRACKER_CONFIG (object),
						  g_value_get_int (value));
		break;

		/* Watches */
	case PROP_WATCH_DIRECTORY_ROOTS:    /* Not writable */
	case PROP_CRAWL_DIRECTORY_ROOTS:    /* Not writable */
	case PROP_NO_WATCH_DIRECTORY_ROOTS: /* Not writable */
		break;
	case PROP_ENABLE_WATCHES:
		tracker_config_set_enable_watches (TRACKER_CONFIG (object),
						   g_value_get_boolean (value));
		break;

		/* Indexing */
	case PROP_THROTTLE:
		tracker_config_set_throttle (TRACKER_CONFIG (object),
					     g_value_get_int (value));
		break;
	case PROP_ENABLE_THUMBNAILS:
		tracker_config_set_enable_thumbnails (TRACKER_CONFIG (object),
						      g_value_get_boolean (value));
		break;
	case PROP_DISABLED_MODULES:
		/* Not writable */
		break;
	case PROP_DISABLE_INDEXING_ON_BATTERY:
		tracker_config_set_disable_indexing_on_battery (TRACKER_CONFIG (object),
								g_value_get_boolean (value));
		break;
	case PROP_DISABLE_INDEXING_ON_BATTERY_INIT:
		tracker_config_set_disable_indexing_on_battery_init (TRACKER_CONFIG (object),
								     g_value_get_boolean (value));
		break;
	case PROP_LOW_DISK_SPACE_LIMIT:
		tracker_config_set_low_disk_space_limit (TRACKER_CONFIG (object),
							 g_value_get_int (value));
		break;
	case PROP_INDEX_MOUNTED_DIRECTORIES:
		tracker_config_set_index_mounted_directories (TRACKER_CONFIG (object),
							      g_value_get_boolean (value));
		break;
	case PROP_INDEX_REMOVABLE_DEVICES:
		tracker_config_set_index_removable_devices (TRACKER_CONFIG (object),
								g_value_get_boolean (value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static gchar *
config_dir_ensure_exists_and_return (void)
{
	gchar *directory;

	directory = g_build_filename (g_get_user_config_dir (),
				      "tracker",
				      NULL);

	if (!g_file_test (directory, G_FILE_TEST_EXISTS)) {
		g_print ("Creating config directory:'%s'\n", directory);

		if (g_mkdir_with_parents (directory, 0700) == -1) {
			g_warning ("Could not create configuration directory");
			g_free (directory);
			return NULL;
		}
	}

	return directory;
}

static void
config_create_with_defaults (GKeyFile *key_file, 
			     gboolean  overwrite)
{
	const gchar  *watch_directory_roots[2] = { NULL, NULL };
	const gchar  *empty_string_list[] = { NULL };

	/* Get default values */
	watch_directory_roots[0] = g_get_home_dir ();

	g_message ("Loading defaults into GKeyFile...");

	/* General */
	if (overwrite || !g_key_file_has_key (key_file, GROUP_GENERAL, KEY_VERBOSITY, NULL)) {
		g_key_file_set_integer (key_file, GROUP_GENERAL, KEY_VERBOSITY, 
					DEFAULT_VERBOSITY);
		g_key_file_set_comment (key_file, GROUP_GENERAL, KEY_VERBOSITY,
					" Log Verbosity (0=errors, 1=minimal, 2=detailed, 3=debug)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_GENERAL, KEY_INITIAL_SLEEP, NULL)) {
		g_key_file_set_integer (key_file, GROUP_GENERAL, KEY_INITIAL_SLEEP, 
					DEFAULT_INITIAL_SLEEP);
		g_key_file_set_comment (key_file, GROUP_GENERAL, KEY_INITIAL_SLEEP,
					" Initial sleep time in seconds (0->1000)",
					NULL);
	}

	/* Watches */
	if (overwrite || !g_key_file_has_key (key_file, GROUP_MONITORS, KEY_WATCH_DIRECTORY_ROOTS, NULL)) {
		g_key_file_set_string_list (key_file, GROUP_MONITORS, KEY_WATCH_DIRECTORY_ROOTS,
					    watch_directory_roots, 
					    g_strv_length ((gchar**) watch_directory_roots));
		g_key_file_set_comment (key_file, GROUP_MONITORS, KEY_WATCH_DIRECTORY_ROOTS,
					" List of directory roots to index and watch (separator=;)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_MONITORS, KEY_CRAWL_DIRECTORY_ROOTS, NULL)) {
		g_key_file_set_string_list (key_file, GROUP_MONITORS, KEY_CRAWL_DIRECTORY_ROOTS,
					    empty_string_list, 0);
		g_key_file_set_comment (key_file, GROUP_MONITORS, KEY_CRAWL_DIRECTORY_ROOTS,
					" List of directory roots to index but NOT watch (separator=;)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_MONITORS, KEY_NO_WATCH_DIRECTORY_ROOTS, NULL)) {
		g_key_file_set_string_list (key_file, GROUP_MONITORS, KEY_NO_WATCH_DIRECTORY_ROOTS,
					    empty_string_list, 0);
		g_key_file_set_comment (key_file, GROUP_MONITORS, KEY_NO_WATCH_DIRECTORY_ROOTS,
					" List of directory roots NOT to index and NOT to watch (separator=;)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_MONITORS, KEY_ENABLE_WATCHES, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_MONITORS, KEY_ENABLE_WATCHES, 
					DEFAULT_ENABLE_WATCHES);
		g_key_file_set_comment (key_file, GROUP_MONITORS, KEY_ENABLE_WATCHES,
					" Set to false to completely disable any watching",
					NULL);
	}

	/* Indexing */
	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_THROTTLE, NULL)) {
		g_key_file_set_integer (key_file, GROUP_INDEXING, KEY_THROTTLE, 
					DEFAULT_THROTTLE);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_THROTTLE,
					" Sets the indexing speed (0->20, where 20=slowest speed)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_ENABLE_THUMBNAILS, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_INDEXING, KEY_ENABLE_THUMBNAILS, 
					DEFAULT_ENABLE_THUMBNAILS);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_ENABLE_THUMBNAILS,
					" Set to false to completely disable thumbnail generation",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_DISABLED_MODULES, NULL)) {
		g_key_file_set_string_list (key_file, GROUP_INDEXING, KEY_DISABLED_MODULES,
					    empty_string_list, 0);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_DISABLED_MODULES,
					" List of disabled modules (separator=;)\n"
					" The modules that are indexed are kept in $prefix/lib/tracker/indexer-modules",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY,
					DEFAULT_DISABLE_INDEXING_ON_BATTERY);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY,
					" Set to true to disable indexing when running on battery",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY_INIT, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY_INIT,
					DEFAULT_DISABLE_INDEXING_ON_BATTERY_INIT);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY_INIT,
					" Set to true to disable initial indexing when running on battery",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_LOW_DISK_SPACE_LIMIT, NULL)) {
		g_key_file_set_integer (key_file, GROUP_INDEXING, KEY_LOW_DISK_SPACE_LIMIT, 
					DEFAULT_LOW_DISK_SPACE_LIMIT);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_LOW_DISK_SPACE_LIMIT,
					" Pause indexer when disk space is <= this value\n"
					" (0->100, value is in % of $HOME file system, -1=disable pausing)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_INDEX_MOUNTED_DIRECTORIES, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_INDEXING, KEY_INDEX_MOUNTED_DIRECTORIES,
					DEFAULT_INDEX_MOUNTED_DIRECTORIES);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_INDEX_MOUNTED_DIRECTORIES,
					" Set to true to enable traversing mounted directories on other file systems\n"
					" (this excludes removable devices)",
					NULL);
	}

	if (overwrite || !g_key_file_has_key (key_file, GROUP_INDEXING, KEY_INDEX_REMOVABLE_DEVICES, NULL)) {
		g_key_file_set_boolean (key_file, GROUP_INDEXING, KEY_INDEX_REMOVABLE_DEVICES, 
					DEFAULT_INDEX_REMOVABLE_DEVICES);
		g_key_file_set_comment (key_file, GROUP_INDEXING, KEY_INDEX_REMOVABLE_DEVICES,
					" Set to true to enable traversing mounted directories for removable devices",
					NULL);
	}
}

static gboolean
config_save_with_defaults (const gchar *filename,
			   GKeyFile    *key_file)
{
	GError *error = NULL;
	gchar  *content = NULL;

	/* Save to file */
	content = g_key_file_to_data (key_file, NULL, &error);

	if (error) {
		g_warning ("Couldn't produce default configuration, %s", error->message);
		g_clear_error (&error);
		return FALSE;
	}

	if (!g_file_set_contents (filename, content, -1, &error)) {
		g_warning ("Couldn't write default configuration, %s", error->message);
		g_clear_error (&error);
		g_free (content);
		return FALSE;
	}

	g_print ("Writing default configuration to file:'%s'\n", filename);
	g_free (content);

	return TRUE;
}

static GSList *
config_string_list_to_gslist (const gchar **value,
			      gboolean	    is_directory_list)
{
	GSList *list = NULL;
	gint	i;

	if (!value) {
		return NULL;
	}

	for (i = 0; value[i]; i++) {
		const gchar *str;
		gchar	    *validated;

		str = value[i];
		if (!str || str[0] == '\0') {
			continue;
		}

		if (!is_directory_list) {
			list = g_slist_prepend (list, g_strdup (str));
			continue;
		}

		/* For directories we validate any special characters,
		 * for example '~' and '../../'
		 */
		validated = tracker_path_evaluate_name (str);
		if (validated) {
			list = g_slist_prepend (list, validated);
		}
	}

	return g_slist_reverse (list);
}

static void
config_load_int (TrackerConfig *config,
		 const gchar   *property,
		 GKeyFile      *key_file,
		 const gchar   *group,
		 const gchar   *key)
{
	GError *error = NULL;
	gint	value;

	value = g_key_file_get_integer (key_file, group, key, &error);
	if (!error) {
		g_object_set (G_OBJECT (config), property, value, NULL);
	} else {
		g_message ("Couldn't load config option '%s' (int) in group '%s', %s",
			   property, group, error->message);
		g_error_free (error);
	}
}

static void
config_load_boolean (TrackerConfig *config,
		     const gchar   *property,
		     GKeyFile	   *key_file,
		     const gchar   *group,
		     const gchar   *key)
{
	GError	 *error = NULL;
	gboolean  value;

	value = g_key_file_get_boolean (key_file, group, key, &error);
	if (!error) {
		g_object_set (G_OBJECT (config), property, value, NULL);
	} else {
		g_message ("Couldn't load config option '%s' (bool) in group '%s', %s",
			   property, group, error->message);
		g_error_free (error);
	}
}

#if 0

static void
config_load_string (TrackerConfig *config,
		    const gchar	  *property,
		    GKeyFile	  *key_file,
		    const gchar	  *group,
		    const gchar	  *key)
{
	GError *error = NULL;
	gchar  *value;

	value = g_key_file_get_string (key_file, group, key, &error);
	if (!error) {
		g_object_set (G_OBJECT (config), property, value, NULL);
	} else {
		g_message ("Couldn't load config option '%s' (string) in group '%s', %s",
			   property, group, error->message);
		g_error_free (error);
	}

	g_free (value);
}

#endif

static void
config_load_string_list (TrackerConfig *config,
			 const gchar   *property,
			 GKeyFile      *key_file,
			 const gchar   *group,
			 const gchar   *key)
{
	TrackerConfigPrivate  *priv;
	GSList		      *l;
	gchar		     **value;
	gboolean               is_directory_list = TRUE;

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	if (strcmp (property, "disabled-modules") == 0) {
		is_directory_list = FALSE;
	}

	value = g_key_file_get_string_list (key_file, group, key, NULL, NULL);
	l = config_string_list_to_gslist ((const gchar **) value, is_directory_list);

	if (strcmp (property, "watch-directory-roots") == 0) {
		priv->watch_directory_roots = tracker_path_list_filter_duplicates (l, "."); 
	}
	else if (strcmp (property, "crawl-directory-roots") == 0) {
		priv->crawl_directory_roots = tracker_path_list_filter_duplicates (l, ".");
	}
	else if (strcmp (property, "no-watch-directory-roots") == 0) {
		priv->no_watch_directory_roots = tracker_path_list_filter_duplicates (l, ".");
	}
	else if (strcmp (property, "disabled-modules") == 0) {
		priv->disabled_modules = l;
	}
	else {
		g_warning ("Property '%s' not recognized to set string list from key '%s'",
			   property, key);
		return;
	}

	if (is_directory_list) {
		g_slist_foreach (l, (GFunc) g_free, NULL);
		g_slist_free (l);
	}

	g_strfreev (value);
}

static void
config_save_int (TrackerConfig *config,
		 const gchar   *property,
		 GKeyFile      *key_file,
		 const gchar   *group,
		 const gchar   *key)
{
	gint value;

	g_object_get (G_OBJECT (config), property, &value, NULL);
	g_key_file_set_integer (key_file, group, key, value);
}

static void
config_save_boolean (TrackerConfig *config,
		     const gchar   *property,
		     GKeyFile	   *key_file,
		     const gchar   *group,
		     const gchar   *key)
{
	gboolean value;

	g_object_get (G_OBJECT (config), property, &value, NULL);
	g_key_file_set_boolean (key_file, group, key, value);
}

#if 0

static void
config_save_string (TrackerConfig *config,
		    const gchar	  *property,
		    GKeyFile	  *key_file,
		    const gchar	  *group,
		    const gchar	  *key)
{
	gchar *value;

	g_object_get (G_OBJECT (config), property, &value, NULL);
	g_key_file_set_string (key_file, group, key, value);
	g_free (value);
}

#endif 

static void
config_save_string_list (TrackerConfig *config,
			 const gchar   *property,
			 GKeyFile      *key_file,
			 const gchar   *group,
			 const gchar   *key)
{
	TrackerConfigPrivate  *priv;
	GSList		      *list;
	gchar		     **value;

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	if (strcmp (property, "watch-directory-roots") == 0) {
		list = priv->watch_directory_roots;
	}
	else if (strcmp (property, "crawl-directory-roots") == 0) {
		list = priv->crawl_directory_roots;
	}
	else if (strcmp (property, "no-watch-directory-roots") == 0) {
		list = priv->no_watch_directory_roots;
	}
	else if (strcmp (property, "disabled-modules") == 0) {
		list = priv->disabled_modules;
	}
	else {
		g_warning ("Property '%s' not recognized to set string list from key '%s'",
			   property, key);
		return;
	}

	value = tracker_gslist_to_string_list (list);
	g_key_file_set_string_list (key_file, 
				    group, 
				    key, 
				    (const gchar * const *) value, 
				    (gsize) g_slist_length (list));
	g_strfreev (value);
}

static void
config_changed_cb (GFileMonitor     *monitor,
		   GFile	    *file,
		   GFile	    *other_file,
		   GFileMonitorEvent event_type,
		   gpointer	     user_data)
{
	TrackerConfig *config;
	gchar	      *filename;

	config = TRACKER_CONFIG (user_data);

	/* Do we recreate if the file is deleted? */

	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CHANGED:
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
		filename = g_file_get_path (file);
		g_message ("Config file changed:'%s', reloading settings...",
			   filename);
		g_free (filename);

		config_load (config);
		break;

	case G_FILE_MONITOR_EVENT_DELETED:
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
	case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
	default:
		break;
	}
}

static void
config_load (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;
	GError		     *error = NULL;
	gchar                *basename;
	gchar		     *filename;
	gchar		     *directory;
	gboolean	      value;

	/* Check we have a config file and if not, create it based on
	 * the default settings.
	 */
	directory = config_dir_ensure_exists_and_return ();
	if (!directory) {
		return;
	}

	basename = g_strdup_printf ("%s.cfg", g_get_application_name ());
	filename = g_build_filename (directory, basename, NULL);
	g_free (basename);
	g_free (directory);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	/* Add file monitoring for changes */
	if (!priv->file) {
		priv->file = g_file_new_for_path (filename);
	}

	if (!priv->monitor) {
		g_message ("Setting up monitor for changes to config file:'%s'",
			   filename);

		priv->monitor = g_file_monitor_file (priv->file,
						     G_FILE_MONITOR_NONE,
						     NULL,
						     NULL);

		g_signal_connect (priv->monitor, "changed",
				  G_CALLBACK (config_changed_cb),
				  config);
	}

	/* Load options */
	g_key_file_load_from_file (priv->key_file, 
				   filename, 
				   G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
				   &error);

	config_create_with_defaults (priv->key_file, FALSE);

	if (error) {
		config_save_with_defaults (filename, priv->key_file);
		g_clear_error (&error);
	}

	g_free (filename);

	/* General */
	config_load_int (config, "verbosity", priv->key_file, GROUP_GENERAL, KEY_VERBOSITY);
	config_load_int (config, "initial-sleep", priv->key_file, GROUP_GENERAL, KEY_INITIAL_SLEEP);

	/* Watches */
	config_load_string_list (config, "watch-directory-roots", priv->key_file, GROUP_MONITORS, KEY_WATCH_DIRECTORY_ROOTS);
	config_load_string_list (config, "crawl-directory-roots", priv->key_file, GROUP_MONITORS, KEY_CRAWL_DIRECTORY_ROOTS);
	config_load_string_list (config, "no-watch-directory-roots", priv->key_file, GROUP_MONITORS, KEY_NO_WATCH_DIRECTORY_ROOTS);
	config_load_boolean (config, "enable-watches", priv->key_file, GROUP_MONITORS, KEY_ENABLE_WATCHES);

	/* Indexing */
	config_load_int (config, "throttle", priv->key_file, GROUP_INDEXING, KEY_THROTTLE);
	config_load_boolean (config, "enable-thumbnails", priv->key_file, GROUP_INDEXING, KEY_ENABLE_THUMBNAILS);
	config_load_string_list (config, "disabled-modules", priv->key_file, GROUP_INDEXING, KEY_DISABLED_MODULES);
	config_load_boolean (config, "disable-indexing-on-battery", priv->key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY);
	config_load_boolean (config, "disable-indexing-on-battery-init", priv->key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY_INIT);
	config_load_int (config, "low-disk-space-limit", priv->key_file, GROUP_INDEXING, KEY_LOW_DISK_SPACE_LIMIT);
	config_load_boolean (config, "index-mounted-directories", priv->key_file, GROUP_INDEXING, KEY_INDEX_MOUNTED_DIRECTORIES);
	config_load_boolean (config, "index-removable-devices", priv->key_file, GROUP_INDEXING, KEY_INDEX_REMOVABLE_DEVICES);

	/*
	 * Legacy options no longer supported:
	 */
	value = g_key_file_get_boolean (priv->key_file, "Emails", "IndexEvolutionEmails", &error);
	if (!error) {
		const gchar * const modules[2] = { "evolution", NULL };

		g_message ("Legacy config option 'IndexEvolutionEmails' found");
		g_message ("  This option has been replaced by 'DisabledModules'");

		if (!value) {
			tracker_config_add_disabled_modules (config, modules);
			g_message ("  Option 'DisabledModules' added '%s'", modules[0]);
		} else {
			tracker_config_remove_disabled_modules (config, modules[0]);
			g_message ("  Option 'DisabledModules' removed '%s'", modules[0]);
		}
	} else {
		g_clear_error (&error);
	}

	value = g_key_file_get_boolean (priv->key_file, "Emails", "IndexThunderbirdEmails", &error);
	if (!error) {
		g_message ("Legacy config option 'IndexThunderbirdEmails' found");
		g_message ("  This option is no longer supported and has no effect");
	} else {
		g_clear_error (&error);
	}

	value = g_key_file_get_boolean (priv->key_file, "Indexing", "SkipMountPoints", &error);
	if (!error) {
		g_message ("Legacy config option 'SkipMountPoints' found");
		tracker_config_set_index_mounted_directories (config, !value);
		g_message ("  Option 'IndexMountedDirectories' set to %s", !value ? "true" : "false");
	} else {
		g_clear_error (&error);
	}
}

static gboolean
config_save (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;
	GError		     *error = NULL;
	gchar		     *filename;
	gchar		     *data;
	gsize                 size;

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	if (!priv->key_file) {
		g_critical ("Could not save config, GKeyFile was NULL, has the config been loaded?");

		return FALSE;
	}

	g_message ("Setting details to GKeyFile object...");

	/* Set properties to GKeyFile */
	config_save_int (config, "verbosity", priv->key_file, GROUP_GENERAL, KEY_VERBOSITY);
	config_save_int (config, "initial-sleep", priv->key_file, GROUP_GENERAL, KEY_INITIAL_SLEEP);

	/* Watches */
	config_save_string_list (config, "watch-directory-roots", priv->key_file, GROUP_MONITORS, KEY_WATCH_DIRECTORY_ROOTS);
	config_save_string_list (config, "crawl-directory-roots", priv->key_file, GROUP_MONITORS, KEY_CRAWL_DIRECTORY_ROOTS);
	config_save_string_list (config, "no-watch-directory-roots", priv->key_file, GROUP_MONITORS, KEY_NO_WATCH_DIRECTORY_ROOTS);
	config_save_boolean (config, "enable-watches", priv->key_file, GROUP_MONITORS, KEY_ENABLE_WATCHES);

	/* Indexing */
	config_save_int (config, "throttle", priv->key_file, GROUP_INDEXING, KEY_THROTTLE);
	config_save_boolean (config, "enable-thumbnails", priv->key_file, GROUP_INDEXING, KEY_ENABLE_THUMBNAILS);
	config_save_string_list (config, "disabled-modules", priv->key_file, GROUP_INDEXING, KEY_DISABLED_MODULES);
	config_save_boolean (config, "disable-indexing-on-battery", priv->key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY);
	config_save_boolean (config, "disable-indexing-on-battery-init", priv->key_file, GROUP_INDEXING, KEY_DISABLE_INDEXING_ON_BATTERY_INIT);
	config_save_int (config, "low-disk-space-limit", priv->key_file, GROUP_INDEXING, KEY_LOW_DISK_SPACE_LIMIT);
	config_save_boolean (config, "index-mounted-directories", priv->key_file, GROUP_INDEXING, KEY_INDEX_MOUNTED_DIRECTORIES);
	config_save_boolean (config, "index-removable-devices", priv->key_file, GROUP_INDEXING, KEY_INDEX_REMOVABLE_DEVICES);

	g_message ("Saving config to disk...");

	/* Do the actual saving to disk now */
	data = g_key_file_to_data (priv->key_file, &size, &error);
	if (error) {
		g_warning ("Could not get config data to write to file, %s",
			   error->message);
		g_error_free (error);

		return FALSE;
	}

	filename = g_file_get_path (priv->file);

	g_file_set_contents (filename, data, size, &error);
	g_free (data);

	if (error) {
		g_warning ("Could not write %" G_GSIZE_FORMAT " bytes to file '%s', %s",
			   size,
			   filename,
			   error->message);
		g_free (filename);
		g_error_free (error);

		return FALSE;
	}

	g_message ("Wrote config to '%s' (%" G_GSIZE_FORMAT " bytes)",
		   filename, 
		   size);

	g_free (filename);

	return TRUE;
}

static gboolean
config_int_validate (TrackerConfig *config,
		     const gchar   *property,
		     gint	    value)
{
#ifdef G_DISABLE_CHECKS
	GParamSpec *spec;
	GValue	    value = { 0 };
	gboolean    valid;

	spec = g_object_class_find_property (G_OBJECT_CLASS (config), property);
	g_return_val_if_fail (spec != NULL, FALSE);

	g_value_init (&value, spec->value_type);
	g_value_set_int (&value, verbosity);
	valid = g_param_value_validate (spec, &value);
	g_value_unset (&value);

	g_return_val_if_fail (valid != TRUE, FALSE);
#endif

	return TRUE;
}

/**
 * tracker_config_new:
 *
 * Creates a new GObject for handling Tracker's config file.
 *
 * Return value: A new TrackerConfig object. Must be unreferenced when
 * finished with.
 */
TrackerConfig *
tracker_config_new (void)
{
	TrackerConfig *config;

	config = g_object_new (TRACKER_TYPE_CONFIG, NULL);
	config_load (config);

	return config;
}

/**
 * tracker_config_save:
 * @config: a #TrackerConfig
 *
 * Writes the configuration stored in TrackerConfig to disk.
 *
 * Return value: %TRUE on success, %FALSE otherwise.
 */
gboolean
tracker_config_save (TrackerConfig *config)
{
	g_return_val_if_fail (TRACKER_IS_CONFIG (config), FALSE);

	return config_save (config);
}

/**
 * tracker_config_get_verbosity:
 * @config: a #TrackerConfig
 *
 * Gets the verbosity of the logging in the indexer and the daemon.
 *
 * If the verbosity is 0, there is no logging except for warnings and
 * errors.
 * If the verbosity is 1, information is displayed.
 * If the verbosity is 2, general messages are displayed.
 * If the verbosity is 3, debug messages are displayed.
 *
 * Note, you receive logging for anything less priority than the
 * verbosity level as well as the level you set. So if the verbosity
 * is 3 you receive debug, messages, info and warnings.
 *
 * Return value: An integer value from 0 to 3.
 */
gint
tracker_config_get_verbosity (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_VERBOSITY);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->verbosity;
}

gint
tracker_config_get_initial_sleep (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_INITIAL_SLEEP);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->initial_sleep;
}

GSList *
tracker_config_get_watch_directory_roots (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->watch_directory_roots;
}

GSList *
tracker_config_get_crawl_directory_roots (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->crawl_directory_roots;
}

GSList *
tracker_config_get_no_watch_directory_roots (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->no_watch_directory_roots;
}

gboolean
tracker_config_get_enable_watches (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_ENABLE_WATCHES);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->enable_watches;
}

gint
tracker_config_get_throttle (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_THROTTLE);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->throttle;
}

gboolean
tracker_config_get_enable_thumbnails (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_ENABLE_THUMBNAILS);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->enable_thumbnails;
}

GSList *
tracker_config_get_disabled_modules (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->disabled_modules;
}

gboolean
tracker_config_get_disable_indexing_on_battery (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_DISABLE_INDEXING_ON_BATTERY);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->disable_indexing_on_battery;
}

gboolean
tracker_config_get_disable_indexing_on_battery_init (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_DISABLE_INDEXING_ON_BATTERY_INIT);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->disable_indexing_on_battery_init;
}

gint
tracker_config_get_low_disk_space_limit (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_LOW_DISK_SPACE_LIMIT);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->low_disk_space_limit;
}

gboolean
tracker_config_get_index_mounted_directories (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_INDEX_MOUNTED_DIRECTORIES);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->index_mounted_directories;
}

gboolean
tracker_config_get_index_removable_devices (TrackerConfig *config)
{
	TrackerConfigPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_CONFIG (config), DEFAULT_INDEX_REMOVABLE_DEVICES);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	return priv->index_removable_devices;
}

void
tracker_config_set_verbosity (TrackerConfig *config,
			      gint	     value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	if (!config_int_validate (config, "verbosity", value)) {
		return;
	}

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->verbosity = value;
	g_object_notify (G_OBJECT (config), "verbosity");
}

void
tracker_config_set_initial_sleep (TrackerConfig *config,
				  gint		 value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	if (!config_int_validate (config, "initial-sleep", value)) {
		return;
	}

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->initial_sleep = value;
	g_object_notify (G_OBJECT (config), "initial-sleep");
}

void
tracker_config_set_enable_watches (TrackerConfig *config,
				   gboolean	  value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->enable_watches = value;
	g_object_notify (G_OBJECT (config), "enable-watches");
}

void
tracker_config_set_throttle (TrackerConfig *config,
			     gint	    value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	if (!config_int_validate (config, "throttle", value)) {
		return;
	}

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->throttle = value;
	g_object_notify (G_OBJECT (config), "throttle");
}

void
tracker_config_set_enable_thumbnails (TrackerConfig *config,
				      gboolean	     value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->enable_thumbnails = value;
	g_object_notify (G_OBJECT (config), "enable-thumbnails");
}

void
tracker_config_set_disable_indexing_on_battery (TrackerConfig *config,
						gboolean       value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->disable_indexing_on_battery = value;
	g_object_notify (G_OBJECT (config), "disable-indexing-on-battery");
}

void
tracker_config_set_disable_indexing_on_battery_init (TrackerConfig *config,
						     gboolean	    value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->disable_indexing_on_battery_init = value;
	g_object_notify (G_OBJECT (config), "disable-indexing-on-battery-init");
}

void
tracker_config_set_low_disk_space_limit (TrackerConfig *config,
					 gint		value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	if (!config_int_validate (config, "low-disk-space-limit", value)) {
		return;
	}

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->low_disk_space_limit = value;
	g_object_notify (G_OBJECT (config), "low-disk-space-limit");
}

void
tracker_config_set_index_mounted_directories (TrackerConfig *config,
					      gboolean	     value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->index_mounted_directories = value;
	g_object_notify (G_OBJECT (config), "index-mounted-directories");
}

void
tracker_config_set_index_removable_devices (TrackerConfig *config,
					  gboolean	 value)
{
	TrackerConfigPrivate *priv;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	priv->index_removable_devices = value;
	g_object_notify (G_OBJECT (config), "index-removable-devices");
}

void
tracker_config_add_watch_directory_roots (TrackerConfig *config,
					  gchar * const *roots)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;
	gchar		     *validated_root;
	gchar * const	     *p;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (roots != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	for (p = roots; *p; p++) {
		validated_root = tracker_path_evaluate_name (*p);
		if (!validated_root) {
			g_print ("Root '%s' is not valid to add to watch directory list\n",
				 validated_root);
			continue;
		}

		priv->watch_directory_roots = g_slist_append (priv->watch_directory_roots,
							      validated_root);
	}

	l = priv->watch_directory_roots;
	priv->watch_directory_roots =
		tracker_path_list_filter_duplicates (priv->watch_directory_roots, ".");

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "watch-directory-roots");
}

void
tracker_config_add_crawl_directory_roots (TrackerConfig *config,
					  gchar * const *roots)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;
	gchar		     *validated_root;
	gchar * const	     *p;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (roots != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	for (p = roots; *p; p++) {
		validated_root = tracker_path_evaluate_name (*p);
		if (!validated_root) {
			g_print ("Root '%s' is not valid to add to crawl directory list\n",
				 validated_root);
			continue;
		}

		priv->crawl_directory_roots = g_slist_append (priv->crawl_directory_roots,
							      validated_root);
	}

	l = priv->crawl_directory_roots;
	priv->crawl_directory_roots =
		tracker_path_list_filter_duplicates (priv->crawl_directory_roots, ".");

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "crawl-directory-roots");
}

void
tracker_config_add_no_watch_directory_roots (TrackerConfig *config,
					     gchar * const *roots)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;
	gchar		     *validated_root;
	gchar * const	     *p;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (roots != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	for (p = roots; *p; p++) {
		validated_root = tracker_path_evaluate_name (*p);
		if (!validated_root) {
			g_print ("Root '%s' is not valid to add to no_watch directory list\n",
				 validated_root);
			continue;
		}

		priv->no_watch_directory_roots = g_slist_append (priv->no_watch_directory_roots,
								 validated_root);
	}

	l = priv->no_watch_directory_roots;
	priv->no_watch_directory_roots =
		tracker_path_list_filter_duplicates (priv->no_watch_directory_roots, ".");

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "no-watch-directory-roots");
}

void
tracker_config_add_disabled_modules (TrackerConfig *config,
				     const gchar * const *modules)
{
	TrackerConfigPrivate *priv;
	GSList		     *new_modules;
	const gchar * const  *p;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (modules != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	new_modules = NULL;

	for (p = modules; *p; p++) {
		if (g_slist_find_custom (priv->disabled_modules,
					 *p,
					 (GCompareFunc) strcmp)) {
			continue;
		}

		new_modules = g_slist_append (new_modules, g_strdup (*p));
	}

	priv->disabled_modules = g_slist_concat (priv->disabled_modules,
						 new_modules);

	g_object_notify (G_OBJECT (config), "disabled-modules");
}

void
tracker_config_remove_watch_directory_roots (TrackerConfig *config,
					     const gchar   *root)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (root != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = g_slist_find_custom (priv->watch_directory_roots,
				 root,
				 (GCompareFunc) strcmp);

	if (l) {
		g_free (l->data);
		priv->watch_directory_roots = g_slist_delete_link (priv->watch_directory_roots, l);
		g_object_notify (G_OBJECT (config), "watch-directory-roots");
	}
}

void
tracker_config_remove_crawl_directory_roots (TrackerConfig *config,
					     const gchar   *root)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (root != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = g_slist_find_custom (priv->crawl_directory_roots,
				 root,
				 (GCompareFunc) strcmp);

	if (l) {
		g_free (l->data);
		priv->crawl_directory_roots = g_slist_delete_link (priv->crawl_directory_roots, l);
		g_object_notify (G_OBJECT (config), "crawl-directory-roots");
	}
}

void
tracker_config_remove_no_watch_directory_roots (TrackerConfig *config,
						const gchar   *root)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (root != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = g_slist_find_custom (priv->no_watch_directory_roots,
				 root,
				 (GCompareFunc) strcmp);

	if (l) {
		g_free (l->data);
		priv->no_watch_directory_roots = g_slist_delete_link (priv->no_watch_directory_roots, l);
		g_object_notify (G_OBJECT (config), "no-watch-directory-roots");
	}
}

void
tracker_config_remove_disabled_modules (TrackerConfig *config,
					const gchar   *module)
{
	TrackerConfigPrivate *priv;
	GSList		     *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));
	g_return_if_fail (module != NULL);

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = g_slist_find_custom (priv->disabled_modules,
				 module,
				 (GCompareFunc) strcmp);

	if (l) {
		g_free (l->data);
		priv->disabled_modules = g_slist_delete_link (priv->disabled_modules, l);
		g_object_notify (G_OBJECT (config), "disabled-modules");
	}
}

void	       
tracker_config_set_watch_directory_roots (TrackerConfig *config,
					  GSList        *roots)
{
	TrackerConfigPrivate *priv;
	GSList               *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = priv->watch_directory_roots;

	if (!roots) {
		priv->watch_directory_roots = NULL;
	} else {
		priv->watch_directory_roots = tracker_gslist_copy_with_string_data (roots);
	}

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "watch-directory-roots");
}

void	       
tracker_config_set_crawl_directory_roots (TrackerConfig *config,
					  GSList        *roots)
{
	TrackerConfigPrivate *priv;
	GSList               *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = priv->crawl_directory_roots;

	if (!roots) {
		priv->crawl_directory_roots = NULL;
	} else {
		priv->crawl_directory_roots = tracker_gslist_copy_with_string_data (roots);
	}

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "crawl-directory-roots");
}

void	       
tracker_config_set_no_watch_directory_roots (TrackerConfig *config,
					     GSList        *roots)
{
	TrackerConfigPrivate *priv;
	GSList               *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);
	
	l = priv->no_watch_directory_roots;

	if (!roots) {
		priv->no_watch_directory_roots = NULL;
	} else {
		priv->no_watch_directory_roots = tracker_gslist_copy_with_string_data (roots);
	}

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "no-watch-directory-roots");
}

void	       
tracker_config_set_disabled_modules (TrackerConfig *config,
				     GSList        *modules)
{
	TrackerConfigPrivate *priv;
	GSList               *l;

	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_CONFIG_GET_PRIVATE (config);

	l = priv->disabled_modules;

	if (!modules) {
		priv->disabled_modules = NULL;
	} else {
		priv->disabled_modules = tracker_gslist_copy_with_string_data (modules);
	}

	g_slist_foreach (l, (GFunc) g_free, NULL);
	g_slist_free (l);

	g_object_notify (G_OBJECT (config), "disabled-modules");
}
