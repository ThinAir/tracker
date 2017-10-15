/*
 * Copyright (C) 2008-2010, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2010, Codeminded BVBA <abustany@gnome.org>
 * Copyright (C) 2016, Sam Thursfield <sam@afuera.me.uk>
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

#include <string.h>

#include <glib.h>

#include "tracker-uri.h"

/* The TrackerUri GType is useful when encapsulating a URI inside a GValue.
 * When we generate SPARQL we need to treat URIs differently to normal strings
 * (one goes in "", one goes in <>) so we can't use regular G_TYPE_STRING for
 * them.
 */
GType
tracker_uri_get_type (void)
{
	static volatile gsize g_define_type_id__volatile = 0;
	if (g_once_init_enter (&g_define_type_id__volatile)) {
		GTypeInfo info = { 0, NULL, NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, };
		GType g_define_type_id = g_type_register_static (G_TYPE_STRING,
		                                                 g_intern_static_string ("TrackerUri"),
		                                                 &info,
		                                                 0);
		g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
	}
	return g_define_type_id__volatile;
}

