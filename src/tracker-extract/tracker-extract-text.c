/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>

#include <libtracker-common/tracker-file-utils.h>

#include <libtracker-extract/tracker-extract.h>

#include "tracker-main.h"
#include "tracker-read.h"

//dummy timed wait


void
pop_data_timed (double seconds)
{
    GMutex data_mutex;
    GCond data_cond;
    
    g_mutex_init(&data_mutex);
    g_cond_init(&data_cond);
    gint64 end_time;
    gpointer data = NULL;
    
    g_mutex_lock (&data_mutex);
    
    end_time = g_get_monotonic_time () + seconds * G_TIME_SPAN_SECOND;
    if (!g_cond_wait_until (&data_cond, &data_mutex, end_time))
    {
        // timeout has passed.
        g_mutex_unlock (&data_mutex);
        goto done;
    }
    
done:
    g_cond_clear(&data_cond);
    g_mutex_unlock (&data_mutex);
    g_mutex_clear(&data_mutex);
}



static gchar *
get_file_content (GFile *file,
                  gsize  n_bytes,TrackerExtractInfo *info)
{
	gchar *text, *uri, *path;
	int fd;

	/* If no content requested, return */
	if (n_bytes == 0) {
		return NULL;
	}

	uri = g_file_get_uri (file);

	/* Get filename from URI */
	path = g_file_get_path (file);

	fd = tracker_file_open_fd (path);

	if (fd == -1) {
		g_message ("Could not open file '%s': %s",
		           uri,
		           g_strerror (errno));
		g_free (uri);
		g_free (path);
		return NULL;
	}

	g_debug ("  Starting to read '%s' up to %" G_GSIZE_FORMAT " bytes...",
	         uri, n_bytes);

	/* Read up to n_bytes from stream. Output is always, always valid UTF-8,
	 * this function closes the FD.
	 */
    
    parsed_data_availble cb = tracker_extract_info_get_callback(info);
    if (cb) {
        FILE *file = NULL;
        gsize bytes_remaining=n_bytes;
        do{
            text = tracker_read_text_chunk_from_fd_(fd,n_bytes,&file,&bytes_remaining);
            if (text) {
                cb(text,tracker_extract_info_get_callback_context(info),path);
            }
            if (text==NULL) {
                break;
            }
            g_free(text);
            text=NULL;
            pop_data_timed(0.05);
        }while (bytes_remaining>0);
    }
    else{
        text = tracker_read_text_from_fd (fd, n_bytes);

    }
	g_free (uri);
	g_free (path);

	return text;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo *info)
{
	TrackerResource *metadata;
	TrackerConfig *config;
	gchar *content;

	config = tracker_main_get_config ();

	content = get_file_content (tracker_extract_info_get_file (info),
	                            tracker_config_get_max_bytes (config),info);

	metadata = tracker_resource_new (NULL);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PlainTextDocument");
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:FileDataObject");

	if (content) {
		tracker_resource_set_string (metadata, "nie:plainTextContent", content);
		g_free (content);
	}

	tracker_extract_info_set_resource (info, metadata);
	g_object_unref (metadata);

	return TRUE;
}
