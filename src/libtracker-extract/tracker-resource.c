/*
 * Copyright (C) 2016-2017, Sam Thursfield <sam@afuera.me.uk>
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

#include <glib.h>

#include <string.h>

#include "config.h"
#include <tracker-uri.h>
#include <tracker-resource.h>

/* For tracker_sparql_escape_string */
//#include "tracker-generated-no-checks.h"

typedef struct {
	char *identifier;
	GHashTable *properties;
	GHashTable *overwrite;
} TrackerResourcePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (TrackerResource, tracker_resource, G_TYPE_OBJECT);
#define GET_PRIVATE(object)  (tracker_resource_get_instance_private (object))

/**
 * SECTION: tracker-resource
 * @short_description: Represents a single Tracker resource
 * @title: TrackerResource
 * @stability: Stable
 * @include: tracker-resource.h
 *
 * <para>
 * #TrackerResource keeps track of a set of properties for a given resource.
 * The resulting data can be serialized in several ways.
 * </para>
 */

static char *
generate_blank_node_identifier (void)
{
	static gint64 counter = 0;

	return g_strdup_printf("_:%" G_GINT64_FORMAT, counter++);
}

/**
 * TrackerResource:
 *
 * The <structname>TrackerResource</structname> object represents information
 * about a given resource.
 */

enum {
	PROP_0,

	PROP_IDENTIFIER,
};

static void constructed  (GObject *object);
static void finalize     (GObject *object);
static void get_property (GObject    *object,
                          guint       param_id,
                          GValue     *value,
                          GParamSpec *pspec);
static void set_property (GObject      *object,
                          guint         param_id,
                          const GValue *value,
                          GParamSpec   *pspec);


static void
tracker_resource_class_init (TrackerResourceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed  = constructed;
	object_class->finalize     = finalize;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	/**
	 * TrackerResource:identifier
	 *
	 * The URI identifier for this class, or %NULL for a
	 * blank node.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_IDENTIFIER,
	                                 g_param_spec_string ("identifier",
	                                                      "Identifier",
	                                                      "Identifier",
	                                                      NULL,
	                                                      G_PARAM_READWRITE));
}

/* Destroy-notify function for the values stored in the hash table. */
static void
free_value (GValue *value)
{
	g_value_unset (value);
	g_slice_free (GValue, value);
}

static void
tracker_resource_init (TrackerResource *resource)
{
	TrackerResourcePrivate *priv = GET_PRIVATE(resource);

	/* Values of properties */
	priv->properties = g_hash_table_new_full (
	        g_str_hash,
	        g_str_equal,
	        g_free,
	        (GDestroyNotify) free_value);

	/* TRUE for any property where we should delete any existing values. */
	priv->overwrite = g_hash_table_new_full (
	        g_str_hash,
	        g_str_equal,
	        g_free,
	        NULL);
}

static void
constructed (GObject *object)
{
	TrackerResourcePrivate *priv;

	priv = GET_PRIVATE (TRACKER_RESOURCE (object));

	if (! priv->identifier) {
		priv->identifier = generate_blank_node_identifier ();
	}

	G_OBJECT_CLASS (tracker_resource_parent_class)->constructed (object);
}

static void
finalize (GObject *object)
{
	TrackerResourcePrivate *priv;

	priv = GET_PRIVATE (TRACKER_RESOURCE (object));

	if (priv->identifier) {
		g_free (priv->identifier);
	}

	g_hash_table_unref (priv->overwrite);
	g_hash_table_unref (priv->properties);

	(G_OBJECT_CLASS (tracker_resource_parent_class)->finalize)(object);
}

static void
get_property (GObject    *object,
              guint       param_id,
              GValue     *value,
              GParamSpec *pspec)
{
	TrackerResourcePrivate *priv;

	priv = GET_PRIVATE (TRACKER_RESOURCE (object));

	switch (param_id) {
	case PROP_IDENTIFIER:
		g_value_set_string (value, priv->identifier);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
set_property (GObject      *object,
              guint         param_id,
              const GValue *value,
              GParamSpec   *pspec)
{
	switch (param_id) {
	case PROP_IDENTIFIER:
		tracker_resource_set_identifier (TRACKER_RESOURCE (object), g_value_get_string (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

/**
 * tracker_resource_new:
 * @identifier: A string containing a URI
 *
 * Creates a TrackerResource instance.
 *
 * Returns: a newly created #TrackerResource. Free with g_object_unref() when done
 *
 * Since: 1.10
 */
TrackerResource *
tracker_resource_new (const char *identifier)
{
	TrackerResource *resource;

	resource = g_object_new (TRACKER_TYPE_RESOURCE,
	                         "identifier", identifier,
	                         NULL);

	return resource;
}

/* Difference between 'set' and 'add': when generating a SPARQL update, the
 * setter will generate a corresponding DELETE, the adder will not. The setter
 * will also overwrite existing values in the Resource object, while the adder
 * will make a list.
 */

/**
 * tracker_resource_set_gvalue:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to set
 * @value: an initialised #GValue
 *
 * State that the only value for the given property is 'value'. Any existing
 * values for 'property' will be removed.
 *
 * When serialising to SPARQL, any properties that were set with this function
 * will get a corresponding DELETE statement to remove any existing values in
 * the database.
 *
 * You can pass any kind of GValue for @value, but serialization functions will
 * normally only be able to serialize URIs/relationships and fundamental value
 * types (string, int, etc.).
 *
 * Since: 1.10
 */
void
tracker_resource_set_gvalue (TrackerResource *self,
                             const char      *property_uri,
                             const GValue    *value)
{
	TrackerResourcePrivate *priv;
	GValue *our_value;

	g_return_if_fail (TRACKER_IS_RESOURCE (self));
	g_return_if_fail (property_uri != NULL);
	g_return_if_fail (G_IS_VALUE (value));

	priv = GET_PRIVATE (self);

	our_value = g_slice_new0 (GValue);
	g_value_init (our_value, G_VALUE_TYPE (value));
	g_value_copy (value, our_value);

	g_hash_table_insert (priv->properties, g_strdup (property_uri), our_value);

	g_hash_table_insert (priv->overwrite, g_strdup (property_uri), GINT_TO_POINTER (TRUE));
};

static gboolean
validate_boolean (gboolean    value,
                  const char *func_name) {
	return TRUE;
}

static gboolean
validate_double (double      value,
                 const char *func_name) {
	return TRUE;
}

static gboolean
validate_int (int         value,
              const char *func_name) {
	return TRUE;
}

static gboolean
validate_int64 (gint64      value,
                const char *func_name) {
	return TRUE;
}

static gboolean
validate_pointer (const void *pointer,
                  const char *func_name)
{
	if (pointer == NULL) {
		g_warning ("%s: NULL is not a valid value.", func_name);
		return FALSE;
	}

	return TRUE;
}

#define SET_PROPERTY_FOR_GTYPE(name, ctype, gtype, set_function, validate_function) \
	void name (TrackerResource *self,                                           \
	           const char *property_uri,                                        \
	           ctype value)                                                     \
	{                                                                           \
		TrackerResourcePrivate *priv;                                       \
		GValue *our_value;                                                  \
                                                                                    \
		g_return_if_fail (TRACKER_IS_RESOURCE (self));                      \
		g_return_if_fail (property_uri != NULL);                            \
                                                                                    \
		priv = GET_PRIVATE (self);                                          \
                                                                                    \
		if (!validate_function (value, __func__)) {                         \
			return;                                                     \
		}                                                                   \
                                                                                    \
		our_value = g_slice_new0 (GValue);                                  \
		g_value_init (our_value, gtype);                                    \
		set_function (our_value, value);                                    \
                                                                                    \
		g_hash_table_insert (priv->properties,                              \
		                     g_strdup (property_uri),                       \
		                     our_value);                                    \
                                                                                    \
		g_hash_table_insert (priv->overwrite,                               \
		                     g_strdup (property_uri),                       \
		                     GINT_TO_POINTER (TRUE));                       \
	};

/**
 * tracker_resource_set_boolean:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued boolean object.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_boolean, gboolean, G_TYPE_BOOLEAN, g_value_set_boolean, validate_boolean);

/**
 * tracker_resource_set_double:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued double object.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_double, double, G_TYPE_DOUBLE, g_value_set_double, validate_double);

/**
 * tracker_resource_set_int:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued integer object.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_int, int, G_TYPE_INT, g_value_set_int, validate_int);

/**
 * tracker_resource_set_int64:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued integer object.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_int64, gint64, G_TYPE_INT64, g_value_set_int64, validate_int64);

/**
 * tracker_resource_set_relation:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @resource: the property object
 *
 * Sets a single-valued resource object as a #TrackerResource. This
 * function produces similar RDF to tracker_resource_set_uri(),
 * although in this function the URI will depend on the identifier
 * set on @resource.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_relation, TrackerResource *, TRACKER_TYPE_RESOURCE, g_value_set_object, validate_pointer);

/**
 * tracker_resource_set_string:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued string object.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_string, const char *, G_TYPE_STRING, g_value_set_string, validate_pointer);

/**
 * tracker_resource_set_uri:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Sets a single-valued resource object as a string URI. This function
 * produces similar RDF to tracker_resource_set_relation(), although
 * it requires that the URI is previously known.
 *
 * Since: 1.10
 */
SET_PROPERTY_FOR_GTYPE (tracker_resource_set_uri, const char *, TRACKER_TYPE_URI, g_value_set_string, validate_pointer);

/**
 * tracker_resource_add_gvalue:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to set
 * @value: an initialised #GValue
 *
 * Add 'value' to the list of values for given property.
 *
 * You can pass any kind of GValue for @value, but serialization functions will
 * normally only be able to serialize URIs/relationships and fundamental value
 * types (string, int, etc.).
 *
 * Since: 1.10
 */
void
tracker_resource_add_gvalue (TrackerResource *self,
                             const char      *property_uri,
                             const GValue    *value)
{
	TrackerResourcePrivate *priv;
	GValue *existing_value, *array_holder, *our_value;
	GPtrArray *array;

	g_return_if_fail (TRACKER_IS_RESOURCE (self));
	g_return_if_fail (property_uri != NULL);
	g_return_if_fail (G_IS_VALUE (value));

	priv = GET_PRIVATE (self);

	existing_value = g_hash_table_lookup (priv->properties, property_uri);

	if (existing_value && G_VALUE_HOLDS (existing_value, G_TYPE_PTR_ARRAY)) {
		array = g_value_get_boxed (existing_value);
		array_holder = existing_value;
	} else {
		array = g_ptr_array_new_with_free_func ((GDestroyNotify)free_value);
		array_holder = g_slice_new0 (GValue);
		g_value_init (array_holder, G_TYPE_PTR_ARRAY);
		g_value_take_boxed (array_holder, array);

		if (existing_value) {
			/* The existing_value is owned by the hash table and will be freed
			 * when we overwrite it with array_holder, so we need to allocate a
			 * new value and give it to the ptrarray.
			 */
			our_value = g_slice_new0 (GValue);
			g_value_init (our_value, G_VALUE_TYPE (existing_value));
			g_value_copy (existing_value, our_value);
			g_ptr_array_add (array, our_value);
		}
	}

	our_value = g_slice_new0 (GValue);
	g_value_init (our_value, G_VALUE_TYPE (value));
	g_value_copy (value, our_value);

	g_ptr_array_add (array, our_value);

	if (array_holder != existing_value) {
		g_hash_table_insert (priv->properties, g_strdup (property_uri), array_holder);
	}
};

#define ADD_PROPERTY_FOR_GTYPE(name, ctype, gtype, set_function, validate_function)     \
	void name (TrackerResource *self,                                               \
	           const char *property_uri,                                            \
	           ctype value)                                                         \
	{                                                                               \
		TrackerResourcePrivate *priv;                                           \
		GValue *existing_value, *array_holder, *our_value;                      \
		GPtrArray *array;                                                       \
                                                                                        \
		g_return_if_fail (TRACKER_IS_RESOURCE (self));                          \
		g_return_if_fail (property_uri != NULL);                                \
                                                                                        \
		priv = GET_PRIVATE (self);                                              \
                                                                                        \
		if (!validate_function (value, __func__)) {                             \
			return;                                                         \
		}                                                                       \
                                                                                        \
		existing_value = g_hash_table_lookup (priv->properties,                 \
		                                      property_uri);                    \
                                                                                        \
		if (existing_value && G_VALUE_HOLDS (existing_value,                    \
		                                     G_TYPE_PTR_ARRAY)) {               \
			array = g_value_get_boxed (existing_value);                     \
			array_holder = existing_value;                                  \
		} else {                                                                \
			array = g_ptr_array_new_with_free_func (                        \
			        (GDestroyNotify)free_value);                            \
			array_holder = g_slice_new0 (GValue);                           \
			g_value_init (array_holder, G_TYPE_PTR_ARRAY);                  \
			g_value_take_boxed (array_holder, array);                       \
                                                                                        \
			if (existing_value) {                                           \
				/* existing_value is owned by hash table */             \
				our_value = g_slice_new0 (GValue);                      \
				g_value_init (our_value, G_VALUE_TYPE(existing_value)); \
				g_value_copy (existing_value, our_value);               \
				g_ptr_array_add (array, our_value);                     \
			}                                                               \
		}                                                                       \
                                                                                        \
		our_value = g_slice_new0 (GValue);                                      \
		g_value_init (our_value, gtype);                                        \
		set_function (our_value, value);                                        \
                                                                                        \
		g_ptr_array_add (array, our_value);                                     \
                                                                                        \
		if (array_holder != existing_value) {                                   \
			g_hash_table_insert (priv->properties,                          \
			                     g_strdup (property_uri), array_holder);    \
		}                                                                       \
	};

/**
 * tracker_resource_add_boolean:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds a boolean object to a multi-valued property.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_boolean, gboolean, G_TYPE_BOOLEAN, g_value_set_boolean, validate_boolean);

/**
 * tracker_resource_add_double:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds a double object to a multi-valued property.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_double, double, G_TYPE_DOUBLE, g_value_set_double, validate_double);

/**
 * tracker_resource_add_int:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds an integer object to a multi-valued property.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_int, int, G_TYPE_INT, g_value_set_int, validate_int);

/**
 * tracker_resource_add_int64:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds an integer object to a multi-valued property.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_int64, gint64, G_TYPE_INT64, g_value_set_int64, validate_int64);

/**
 * tracker_resource_add_relation:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @resource: the property object
 *
 * Adds a resource object to a multi-valued property. This
 * function produces similar RDF to tracker_resource_add_uri(),
 * although in this function the URI will depend on the identifier
 * set on @resource.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_relation, TrackerResource *, TRACKER_TYPE_RESOURCE, g_value_set_object, validate_pointer);

/**
 * tracker_resource_add_string:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds a string object to a multi-valued property.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_string, const char *, G_TYPE_STRING, g_value_set_string, validate_pointer);

/**
 * tracker_resource_add_uri:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to modify
 * @value: the property object
 *
 * Adds a resource object to a multi-valued property. This function
 * produces similar RDF to tracker_resource_add_relation(), although
 * it requires that the URI is previously known.
 *
 * Since: 1.10
 */
ADD_PROPERTY_FOR_GTYPE (tracker_resource_add_uri, const char *, TRACKER_TYPE_URI, g_value_set_string, validate_pointer);


/**
 * tracker_resource_get_values:
 * @self: the #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the list of all known values of the given property.
 *
 * Returns: (transfer full) (element-type GValue): a #GList of #GValue
 * instances, which must be freed by the caller.
 *
 * Since: 1.10
 */
GList *tracker_resource_get_values (TrackerResource *self,
                                    const char      *property_uri)
{
	TrackerResourcePrivate *priv;
	GValue *value;

	g_return_val_if_fail (TRACKER_IS_RESOURCE (self), NULL);
	g_return_val_if_fail (property_uri, NULL);

	priv = GET_PRIVATE (self);

	value = g_hash_table_lookup (priv->properties, property_uri);

	if (value == NULL) {
		return NULL;
	}

	if (G_VALUE_HOLDS (value, G_TYPE_PTR_ARRAY)) {
		GList *result = NULL;
		GPtrArray *array;
		int i;

		array = g_value_get_boxed (value);

		for (i = 0; i < array->len; i++) {
			value = g_ptr_array_index (array, i);
			result = g_list_prepend (result, value);
		};

		return g_list_reverse (result);
	} else {
		return g_list_append (NULL, value);
	}
}

#define GET_PROPERTY_FOR_GTYPE(name, ctype, gtype, get_function, no_value)    \
	ctype name (TrackerResource *self,                                    \
	            const char *property_uri)                                 \
	{                                                                     \
		TrackerResourcePrivate *priv;                                 \
		GValue *value;                                                \
                                                                              \
		g_return_val_if_fail (TRACKER_IS_RESOURCE (self), no_value);  \
		g_return_val_if_fail (property_uri, no_value);                \
                                                                              \
		priv = GET_PRIVATE (self);                                    \
                                                                              \
		value = g_hash_table_lookup (priv->properties, property_uri); \
                                                                              \
		if (value == NULL) {                                          \
			return no_value;                                      \
		};                                                            \
                                                                              \
		if (G_VALUE_HOLDS (value, G_TYPE_PTR_ARRAY)) {                \
			GPtrArray *array;                                     \
			array = g_value_get_boxed (value);                    \
			if (array->len == 0) {                                \
				return no_value;                              \
			} else {                                              \
				value = g_ptr_array_index (array, 0);         \
			}                                                     \
		}                                                             \
                                                                              \
		return get_function (value);                                  \
	};

/**
 * tracker_resource_get_first_boolean:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first boolean object previously assigned to a property.
 *
 * Returns: the first boolean object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_boolean, gboolean, G_TYPE_BOOLEAN, g_value_get_boolean, FALSE);

/**
 * tracker_resource_get_first_double:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first double object previously assigned to a property.
 *
 * Returns: the first double object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_double, double, G_TYPE_DOUBLE, g_value_get_double, 0.0);

/**
 * tracker_resource_get_first_int:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first integer object previously assigned to a property.
 *
 * Returns: the first integer object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_int, int, G_TYPE_INT, g_value_get_int, 0);

/**
 * tracker_resource_get_first_int64:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first integer object previously assigned to a property.
 *
 * Returns: the first integer object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_int64, gint64, G_TYPE_INT64, g_value_get_int64, 0);

/**
 * tracker_resource_get_first_relation:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first resource object previously assigned to a property.
 *
 * Returns: (transfer none): the first resource object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_relation, TrackerResource *, TRACKER_TYPE_RESOURCE, g_value_get_object, NULL);

/**
 * tracker_resource_get_first_string:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first string object previously assigned to a property.
 *
 * Returns: the first string object
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_string, const char *, G_TYPE_STRING, g_value_get_string, NULL);

/**
 * tracker_resource_get_first_uri:
 * @self: A #TrackerResource
 * @property_uri: a string identifying the property to look up
 *
 * Returns the first resource object previously assigned to a property.
 *
 * Returns: the first resource object as an URI.
 *
 * Since: 1.10
 */
GET_PROPERTY_FOR_GTYPE (tracker_resource_get_first_uri, const char *, TRACKER_TYPE_URI, g_value_get_string, NULL);

/**
 * tracker_resource_get_identifier:
 * @self: A #TrackerResource
 *
 * Returns the identifier of a resource.
 *
 * If the identifier was set to NULL, the identifier returned will be a unique
 * SPARQL blank node identifier, such as "_:123".
 *
 * Returns: a string owned by the resource
 *
 * Since: 1.10
 */
const char *
tracker_resource_get_identifier (TrackerResource *self)
{
	TrackerResourcePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_RESOURCE (self), NULL);

	priv = GET_PRIVATE (self);

	return priv->identifier;
}

/**
 * tracker_resource_set_identifier:
 * @self: a #TrackerResource
 * @identifier: (allow-none): a string identifying the resource
 *
 * Changes the identifier of a #TrackerResource. The identifier should be a
 * URI or compact URI, but this is not necessarily enforced. Invalid
 * identifiers may cause errors when serializing the resource or trying to
 * insert the results in a database.
 *
 * If the identifier is set to NULL, a SPARQL blank node identifier such as
 * "_:123" is assigned to the resource.
 *
 * Since: 1.10
 */
void
tracker_resource_set_identifier (TrackerResource *self,
                                 const char      *identifier)
{
	TrackerResourcePrivate *priv;

	g_return_if_fail (TRACKER_IS_RESOURCE (self));

	priv = GET_PRIVATE (self);

	g_free (priv->identifier);

	if (identifier == NULL) {
		/* We take NULL to mean "this is a blank node", and generate a
		 * unique blank node identifier right away. This is easier than
		 * leaving it NULL and trying to generate a blank node ID at
		 * serialization time, and it means that the serialization
		 * output is stable when called multiple times.
		 */
		priv->identifier = generate_blank_node_identifier ();
	} else {
		priv->identifier = g_strdup (identifier);
	}
}

gint
tracker_resource_identifier_compare_func (TrackerResource *resource,
                                          const char      *identifier)
{
	TrackerResourcePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_RESOURCE (resource), 0);
	g_return_val_if_fail (identifier != NULL, 0);

	priv = GET_PRIVATE (resource);

	return strcmp (priv->identifier, identifier);
}

///**
// * tracker_resource_compare:
// * @self: A #TrackerResource
// *
// * Compare the identifiers of two TrackerResource instances. The resources
// * are considered identical if they have the same identifier.
// *
// * Note that there can be false negatives with this simplistic approach: two
// * resources may have different identifiers that actually refer to the same
// * thing.
// *
// * Returns: 0 if the identifiers are the same, -1 or +1 otherwise
// *
// * Since: 1.10
// */
//gint
//tracker_resource_compare (TrackerResource *a,
//                          TrackerResource *b)
//{
//    TrackerResourcePrivate *a_priv, *b_priv;
//
//    g_return_val_if_fail (TRACKER_IS_RESOURCE (a), 0);
//    g_return_val_if_fail (TRACKER_IS_RESOURCE (b), 0);
//
//    a_priv = GET_PRIVATE (a);
//    b_priv = GET_PRIVATE (b);
//
//    return strcmp (a_priv->identifier, b_priv->identifier);
//};


///* Helper function for serialization code. This allows you to selectively
// * populate 'interned_namespaces' from 'all_namespaces' based on when a
// * particular prefix is actually used. This is quite inefficient compared
// * to just dumping all known namespaces, but it makes the serializated
// * output more readable.
// */
//static void
//maybe_intern_prefix_of_compact_uri (TrackerNamespaceManager *all_namespaces,
//                                    TrackerNamespaceManager *interned_namespaces,
//                                    const char              *uri)
//{
//    /* The TrackerResource API doesn't distinguish between compact URIs and full
//     * URIs. This is fine as long as users don't add prefixes that can be
//     * confused with URIs. Both URIs and CURIEs can have anything following
//     * the ':', so without hardcoding knowledge of well-known protocols here,
//     * we can't really tell if the user has done something dumb like defining a
//     * "urn" prefix.
//     */
//    char *prefix = g_uri_parse_scheme (uri);
//
//    if (prefix == NULL) {
//        g_warning ("Invalid URI or compact URI: %s", uri);
//        return;
//    }
//
//    if (tracker_namespace_manager_has_prefix (all_namespaces, prefix)) {
//        if (!tracker_namespace_manager_has_prefix (interned_namespaces, prefix)) {
//            const char *namespace = tracker_namespace_manager_lookup_prefix (all_namespaces, prefix);
//            tracker_namespace_manager_add_prefix (interned_namespaces, prefix, namespace);
//        }
//    }
//
//    g_free (prefix);
//}
//
//
