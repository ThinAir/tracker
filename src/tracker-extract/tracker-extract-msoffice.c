/*
 * Copyright (C) 2006, Edward Duffy (eduffy@gmail.com)
 * Copyright (C) 2006, Laurent Aguerreche (laurent.aguerreche@free.fr)
 * Copyright (C) 2008, Nokia
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>

#include <glib.h>

#include <gsf/gsf.h>
#include <gsf/gsf-doc-meta-data.h>
#include <gsf/gsf-infile.h>
#include <gsf/gsf-infile-msole.h>
#include <gsf/gsf-input-stdio.h>
#include <gsf/gsf-msole-utils.h>
#include <gsf/gsf-utils.h>

#include <libtracker-common/tracker-utils.h>
#include <libtracker-common/tracker-ontologies.h>
#include <libtracker-common/tracker-os-dependant.h>

#include <libtracker-extract/tracker-extract.h>
#include <libtracker-extract/tracker-utils.h>

#include "tracker-main.h"

/* Powerpoint files comprise of structures. Each structure contains a
 * header. Within that header is a record type that specifies what
 * strcture it is. It is called record type.
 *
 * Here are are some record types and description of the structure
 * (called atom) they contain.
 */

/* An atom record that specifies Unicode characters with no high byte
 * of a UTF-16 Unicode character. High byte is always 0.
 */
#define TEXTBYTESATOM_RECORD_TYPE      0x0FA0

/* An atom record that specifies Unicode characters. */
#define TEXTCHARSATOM_RECORD_TYPE      0x0FA8

/* A container record that specifies information about the powerpoint
 * document.
 */
#define DOCUMENTCONTAINER_RECORD_TYPE  0x03E8

/* Variant type of record. Within Powerpoint text extraction we are
 * interested of SlideListWithTextContainer type that contains the
 * textual content of the slide(s).
 *
 */
#define SLIDELISTWITHTEXT_RECORD_TYPE  0x0FF0

/**
 * @brief Header for all powerpoint structures
 *
 * A structure at the beginning of each container record and each atom record in
 * the file. The values in the record header and the context of the record are
 * used to identify and interpret the record data that follows.
 */
typedef struct {
	/**
	 * @brief An unsigned integer that specifies the version of the record
	 * data that follows the record header. A value of 0xF specifies that the
	 * record is a container record.
	 */
	guint recVer;

	/**
	 * @brief An unsigned integer that specifies the record instance data.
	 * Interpretation of the value is dependent on the particular record
	 * type.
	 */
	guint recInstance;

	/**
	 * @brief A RecordType enumeration that specifies the type of the record
	 * data that follows the record header.
	 */
	gint recType;

	/**
	 * @brief An unsigned integer that specifies the length, in bytes, of the
	 * record data that follows the record header.
	 */
	guint recLen;
} PowerPointRecordHeader;

typedef enum {
	TAG_TYPE_INVALID,
	TAG_TYPE_TITLE,
	TAG_TYPE_SUBJECT,
	TAG_TYPE_AUTHOR,
	TAG_TYPE_MODIFIED,
	TAG_TYPE_COMMENTS,
	TAG_TYPE_CREATED,
	TAG_TYPE_GENERATOR,
	TAG_TYPE_NUM_OF_PAGES,
	TAG_TYPE_NUM_OF_CHARACTERS,
	TAG_TYPE_NUM_OF_WORDS,
	TAG_TYPE_NUM_OF_LINES,
	TAG_TYPE_APPLICATION,
	TAG_TYPE_NUM_OF_PARAGRAPHS,
	TAG_TYPE_SLIDE_TEXT,
	TAG_TYPE_WORD_TEXT,
	TAG_TYPE_XLS_SHARED_TEXT,
	TAG_TYPE_DOCUMENT_CORE_DATA,
	TAG_TYPE_DOCUMENT_TEXT_DATA
} TagType;

typedef enum {
	FILE_TYPE_INVALID,
	FILE_TYPE_PPTX,
	FILE_TYPE_DOCX,
	FILE_TYPE_XLSX
} FileType;

typedef struct {
	TrackerSparqlBuilder *metadata;
	FileType file_type;
	TagType tag_type;
	gboolean style_element_present;
	gboolean preserve_attribute_present;
	const gchar *uri;
	GString *content;
} MsOfficeXMLParserInfo;

typedef struct {
	TrackerSparqlBuilder *metadata;
	const gchar *uri;
} MetadataInfo;

static void extract_msoffice     (const gchar          *uri,
                                  TrackerSparqlBuilder *preupdate,
                                  TrackerSparqlBuilder *metadata);
static void extract_msoffice_xml (const gchar          *uri,
                                  TrackerSparqlBuilder *preupdate,
                                  TrackerSparqlBuilder *metadata);
static void extract_ppt          (const gchar          *uri,
                                  TrackerSparqlBuilder *preupdate,
                                  TrackerSparqlBuilder *metadata);

static TrackerExtractData data[] = {
	{ "application/msword",            extract_msoffice },
	/* Powerpoint files */
	{ "application/vnd.ms-powerpoint", extract_ppt },
	{ "application/vnd.ms-*",          extract_msoffice },
	/* MSoffice2007*/
	{ "application/vnd.openxmlformats-officedocument.presentationml.presentation", extract_msoffice_xml },
	{ "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",         extract_msoffice_xml },
	{ "application/vnd.openxmlformats-officedocument.wordprocessingml.document",   extract_msoffice_xml },
	{ NULL, NULL }
};

static void
metadata_add_gvalue (TrackerSparqlBuilder *metadata,
                     const gchar          *uri,
                     const gchar          *key,
                     GValue const         *val,
                     const gchar          *type,
                     const gchar          *predicate,
                     gboolean              is_date)
{
	gchar *s;

	g_return_if_fail (metadata != NULL);
	g_return_if_fail (key != NULL);

	if (!val) {
		return;
	}

	s = g_strdup_value_contents (val);

	if (!s) {
		return;
	}

	if (!tracker_is_empty_string (s)) {
		gchar *str_val;

		/* Some fun: strings are always written "str" with double quotes
		 * around, but not numbers!
		 */
		if (s[0] == '"') {
			size_t len;

			len = strlen (s);

			if (s[len - 1] == '"') {
				if (is_date) {
					if (len > 2) {
						gchar *str = g_strndup (s + 1, len - 2);
						str_val = tracker_extract_guess_date (str);
						g_free (str);
					} else {
						str_val = NULL;
					}
				} else {
					str_val = len > 2 ? g_strndup (s + 1, len - 2) : NULL;
				}
			} else {
				/* We have a string that begins with a double
				 * quote but which finishes by something
				 * different... We copy the string from the
				 * beginning.
				 */
				if (is_date) {
					str_val = tracker_extract_guess_date (s);
				} else {
					str_val = g_strdup (s);
				}
			}
		} else {
			/* Here, we probably have a number */
			if (is_date) {
				str_val = tracker_extract_guess_date (s);
			} else {
				str_val = g_strdup (s);
			}
		}

		if (str_val) {
			if (type && predicate) {
				tracker_sparql_builder_predicate (metadata, key);

				tracker_sparql_builder_object_blank_open (metadata);
				tracker_sparql_builder_predicate (metadata, "a");
				tracker_sparql_builder_object (metadata, type);

				tracker_sparql_builder_predicate (metadata, predicate);
				tracker_sparql_builder_object_unvalidated (metadata, str_val);
				tracker_sparql_builder_object_blank_close (metadata);
			} else {
				tracker_sparql_builder_predicate (metadata, key);
				tracker_sparql_builder_object_unvalidated (metadata, str_val);
			}

			g_free (str_val);
		}
	}

	g_free (s);
}

static void
summary_metadata_cb (gpointer key,
                     gpointer value,
                     gpointer user_data)
{
	MetadataInfo *info = user_data;
	GValue const *val;

	val = gsf_doc_prop_get_val (value);

	if (g_strcmp0 (key, "dc:title") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nie:title", val, NULL, NULL, FALSE);
	} else if (g_strcmp0 (key, "dc:subject") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nie:subject", val, NULL, NULL, FALSE);
	} else if (g_strcmp0 (key, "dc:creator") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nco:creator", val, "nco:Contact", "nco:fullname", FALSE);
	} else if (g_strcmp0 (key, "dc:keywords") == 0) {
		gchar *keywords = g_strdup_value_contents (val);
		gchar *lasts, *keyw;
		size_t len;

		keyw = keywords;
		keywords = strchr (keywords, '"');

		if (keywords) {
			keywords++;
		} else {
			keywords = keyw;
		}

		len = strlen (keywords);
		if (keywords[len - 1] == '"') {
			keywords[len - 1] = '\0';
		}

		for (keyw = strtok_r (keywords, ",; ", &lasts); keyw;
		     keyw = strtok_r (NULL, ",; ", &lasts)) {
			tracker_sparql_builder_predicate (info->metadata, "nie:keyword");
			tracker_sparql_builder_object_unvalidated (info->metadata, keyw);
		}

		g_free (keyw);
	} else if (g_strcmp0 (key, "dc:description") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nie:comment", val, NULL, NULL, FALSE);
	} else if (g_strcmp0 (key, "gsf:page-count") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nfo:pageCount", val, NULL, NULL, FALSE);
	} else if (g_strcmp0 (key, "gsf:word-count") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nfo:wordCount", val, NULL, NULL, FALSE);
	} else if (g_strcmp0 (key, "meta:creation-date") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nie:contentCreated", val, NULL, NULL, TRUE);
	} else if (g_strcmp0 (key, "meta:generator") == 0) {
		metadata_add_gvalue (info->metadata, info->uri, "nie:generator", val, NULL, NULL, FALSE);
	}
}

static void
document_metadata_cb (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
	if (g_strcmp0 (key, "CreativeCommons_LicenseURL") == 0) {
		MetadataInfo *info = user_data;

		metadata_add_gvalue (info->metadata,
		                     info->uri,
		                     "nie:license",
		                     gsf_doc_prop_get_val (value),
		                     NULL,
		                     NULL,
		                     FALSE);
	}
}

/**
 * @brief Read 16 bit unsigned integer
 * @param buffer data to read integer from
 * @return 16 bit unsigned integer
 */
static gint
read_16bit (const guint8* buffer)
{
	return buffer[0] + (buffer[1] << 8);
}

/**
 * @brief Read 32 bit unsigned integer
 * @param buffer data to read integer from
 * @return 32 bit unsigned integer
 */
static gint
read_32bit (const guint8 *buffer)
{
	return buffer[0] + (buffer[1] << 8) + (buffer[2] << 16) + (buffer[3] << 24);
}

/**
 * @brief Read header data from given stream
 * @param stream Stream to read header data
 * @param header Pointer to header where to store results
 */
static gboolean
ppt_read_header (GsfInput               *stream,
                 PowerPointRecordHeader *header)
{
	guint8 buffer[8] = {0};

	g_return_val_if_fail (stream, FALSE);
	g_return_val_if_fail (header, FALSE);
	g_return_val_if_fail (!gsf_input_eof (stream), FALSE);


	/* Header is always 8 bytes, read it */
	g_return_val_if_fail (gsf_input_read (stream, 8, buffer), FALSE);

	/* Then parse individual details
	 *
	 * Record header is 8 bytes long. Data is split as follows:
	 * recVer (4 bits)
	 * recInstance (12 bits)
	 * recType (2 bytes)
	 * recLen (4 bytes)
	 *
	 * See RecordHeader for more detailed explanation of each field.
	 *
	 * Here we parse each of those fields.
	 */

	header->recType = read_16bit (&buffer[2]);
	header->recLen = read_32bit (&buffer[4]);
	header->recVer = (read_16bit (buffer) & 0xF000) >> 12;
	header->recInstance = read_16bit (buffer) & 0x0FFF;

	return TRUE;
}

/**
 * @brief Read powerpoint text from given stream.
 *
 * Powerpoint contains texts in either TextBytesAtom or TextCharsAtom. Below
 * are excerpt from [MS-PPT].pdf file describing the ppt file struture:
 *
 * TextCharsAtom contains an array of UTF-16 Unicode [RFC2781] characters that
 * specifies the characters of the corresponding text. The length, in bytes, of
 * the array is specified by rh.recLen. The array MUST NOT contain the NUL
 * character 0x0000.
 *
 * TextBytesAtom contains an array of bytes that specifies the characters of the
 * corresponding text. Each item represents the low byte of a UTF-16 Unicode
 * [RFC2781] character whose high byte is 0x00. The length, in bytes, of the
 * array is specified by rh.recLen. The array MUST NOT contain a 0x00 byte.
 *
 * @param stream Stream to read text bytes/chars atom
 * @return read text or NULL if no text was read. Has to be freed by the caller
 */
static gchar *
ppt_read_text (GsfInput *stream)
{
	gint i = 0;
	PowerPointRecordHeader header;
	guint8 *data = NULL;

	g_return_val_if_fail (stream, NULL);

	/* First read the header that describes the structures type
	 * (TextBytesAtom or TextCharsAtom) and it's length.
	 */
	g_return_val_if_fail (ppt_read_header (stream, &header), NULL);

	/* We only want header with type either TEXTBYTESATOM_RECORD_TYPE
	 * (TextBytesAtom) or TEXTCHARSATOM_RECORD_TYPE (TextCharsAtom).
	 *
	 * We don't care about anything else
	 */
	if (header.recType != TEXTBYTESATOM_RECORD_TYPE &&
	    header.recType != TEXTCHARSATOM_RECORD_TYPE) {
		return NULL;
	}

	/* Then we'll allocate data for the actual texts */
	if (header.recType == TEXTBYTESATOM_RECORD_TYPE) {
		/* TextBytesAtom doesn't include high bytes propably in order to
		 * save space on the ppt files. We'll have to allocate double the
		 * size for it to get the high bytes
		 */
		data = g_try_new0 (guint8,header.recLen * 2);
	} else {
		data = g_try_new0 (guint8,header.recLen);
	}

	g_return_val_if_fail (data, NULL);

	/* Then read the textual data from the stream */
	if (!gsf_input_read (stream, header.recLen, data)) {
		g_free (data);
		return NULL;
	}

	/* Again if we are reading TextBytesAtom we'll need to add those utf16
	 * high bytes ourselves. They are zero as specified in [MS-PPT].pdf
	 * and this function's comments
	 */
	if (header.recType == TEXTBYTESATOM_RECORD_TYPE) {
		for (i = 0; i < header.recLen; i++) {
			/* We'll add an empty 0 byte between each byte in the
			 * array
			 */
			data[(header.recLen - i - 1) * 2] = data[header.recLen - i - 1];

			if ((header.recLen - i - 1) % 2) {
				data[header.recLen - i - 1] = 0;
			}
		}

		/* Then double the recLen now that we have the high bytes added
		 * between read bytes
		 */
		header.recLen *= 2;
	}

	/* Return read text */
	return data;
}

/**
 * @brief Find a specific header from given stream
 * @param stream Stream to parse headers from
 * @param type1 first type of header to look for
 * @param type2 convenience parameter if we are looking for either of two
 * header types
 * @param rewind if a proper header is found should this function seek
 * to the start of the header (TRUE)
 * @return TRUE if either of specified headers was found
 */
static gboolean
ppt_seek_header (GsfInput *stream,
                 gint      type1,
                 gint      type2,
                 gboolean  rewind)
{
	PowerPointRecordHeader header;

	g_return_val_if_fail (stream,FALSE);

	/* Read until we reach eof */
	while (!gsf_input_eof (stream)) {
		/* Read first header */
		g_return_val_if_fail (ppt_read_header (stream, &header), FALSE);

		/* Check if it's the correct type */
		if (header.recType == type1 || header.recType == type2) {
			/* Sometimes it's needed to rewind to the start of the
			 * header
			 */
			if (rewind) {
				gsf_input_seek (stream, -8, G_SEEK_CUR);
			}

			return TRUE;
		}

		/* If it's not the correct type, seek to the beginning of the
		 * next header
		 */
		g_return_val_if_fail (!gsf_input_seek (stream,
		                                       header.recLen,
		                                       G_SEEK_CUR),
		                      FALSE);
	}

	return FALSE;
}

/**
 * @brief Normalize and append given text to all_texts variable
 * @param text text to append
 * @param all_texts GString to append text after normalizing it
 * @param words number of words already in all_texts
 * @param max_words maximum number of words allowed in all_texts
 * @return number of words appended to all_text
 */
static gint
ppt_append_text (gchar   *text,
                 GString *all_texts,
                 gint     words,
                 gint     max_words)
{
	gchar *normalized_text;
	guint count = 0;

	g_return_val_if_fail (text, -1);
	g_return_val_if_fail (all_texts, -1);

	normalized_text = tracker_extract_text_normalize (text,
	                                                  max_words - words,
	                                                  &count);

	if (normalized_text) {
		/* If the last added text didn't end in a space, we'll
		 * append a space between this text and previous text
		 * so the last word of previous text and first word of
		 * this text don't become one big word.
		 */
		if (all_texts->len > 0 &&
		    all_texts->str[all_texts->len-1] != ' ') {
			g_string_append_c(all_texts,' ');
		}

		g_string_append (all_texts,normalized_text);
		g_free (normalized_text);
	}

	g_free (text);

	return count;
}

static void
ppt_read (GsfInfile            *infile,
          TrackerSparqlBuilder *metadata,
          gint                  max_words)
{
	/* Try to find Powerpoint Document stream */
	GsfInput *stream;
	gsf_off_t last_document_container = -1;

	stream = gsf_infile_child_by_name (infile, "PowerPoint Document");

	g_return_if_fail (stream);

	/* Powerpoint documents have a "editing history" stored within them.
	 * There is a structure that defines what changes were made each time
	 * but it is just easier to get the current/latest version just by
	 * finding the last occurrence of DocumentContainer structure
	 */
	last_document_container = -1;

	/* Read until we reach eof. */
	while (!gsf_input_eof (stream)) {
		PowerPointRecordHeader header;

		/*
		 * We only read headers of data structures
		 */
		if (!ppt_read_header (stream, &header)) {
			break;
		}

		/* And we only care about headers with type 1000,
		 * DocumentContainer
		 */

		if (header.recType == DOCUMENTCONTAINER_RECORD_TYPE) {
			last_document_container = gsf_input_tell (stream);
		}

		/* and then seek to the start of the next data
		 * structure so it is fast and we don't have to read
		 * through the whole file
		 */
		if (gsf_input_seek (stream, header.recLen, G_SEEK_CUR)) {
			break;
		}
	}

	/* If a DocumentContainer was found and we are able to seek to it.
	 *
	 * Then we'll have to find the second header with type
	 * SLIDELISTWITHTEXT_RECORD_TYPE since DocumentContainer
	 * contains MasterListWithTextContainer and
	 * SlideListWithTextContainer structures with both having the
	 * same header type. We however only want
	 * SlideListWithTextContainer which contains the textual
	 * content of the power point file.
	 */
	if (last_document_container >= 0 &&
	    !gsf_input_seek (stream, last_document_container, G_SEEK_SET) &&
	    ppt_seek_header (stream,
	                     SLIDELISTWITHTEXT_RECORD_TYPE,
	                     SLIDELISTWITHTEXT_RECORD_TYPE,
	                     FALSE) &&
	    ppt_seek_header (stream,
	                     SLIDELISTWITHTEXT_RECORD_TYPE,
	                     SLIDELISTWITHTEXT_RECORD_TYPE,
	                     FALSE)) {
		GString *all_texts = g_string_new ("");
		gint word_count = 0;

		/*
		 * Read while we have either TextBytesAtom or
		 * TextCharsAtom and we have read less than max_words
		 * amount of words
		 */
		while (ppt_seek_header (stream,
		                        TEXTBYTESATOM_RECORD_TYPE,
		                        TEXTCHARSATOM_RECORD_TYPE,
		                        TRUE) &&
		       word_count < max_words) {
			gchar *text = ppt_read_text (stream);

			if (text) {
				gint count;

				count = ppt_append_text (text, all_texts, word_count, max_words);
				if (count < 0) {
					break;
				}

				word_count += count;
			}
		}

		/* If we have any text read */
		if (all_texts->len > 0) {
			/* Send it to tracker */
			tracker_sparql_builder_predicate (metadata, "nie:plainTextContent");
			tracker_sparql_builder_object_unvalidated (metadata, all_texts->str);
		}

		g_string_free (all_texts, TRUE);
	}

	g_object_unref (stream);
}

/**
 * @brief get maximum number of words to index
 * @return maximum number of words to index
 */
static gint
fts_max_words (void)
{
	TrackerFTSConfig *fts_config = tracker_main_get_fts_config ();
	return tracker_fts_config_get_max_words_to_index (fts_config);
}

/**
 * @brief Open specified uri for reading and initialize gsf
 * @param uri URI of the file to open
 * @return GsfInFile of the opened file or NULL if failed to open file
 */
static GsfInfile *
open_uri (const gchar *uri)
{
	GsfInput *input;
	GsfInfile *infile;
	gchar *filename;

	filename = g_filename_from_uri (uri, NULL, NULL);
	input = gsf_input_stdio_new (filename, NULL);
	g_free (filename);

	if (!input) {
		return NULL;
	}

	infile = gsf_infile_msole_new (input, NULL);
	g_object_unref (G_OBJECT (input));

	return infile;
}

/* This function was programmed by using ideas and algorithms from
 * b2xtranslator project (http://b2xtranslator.sourceforge.net/)
 */
static gchar *
extract_msword_content (GsfInfile *infile,
                        gint       n_words,
                        gboolean  *is_encrypted)
{
	GsfInput *document_stream, *table_stream;
	gint16 i = 0;
	guint8 tmp_buffer[4] = { 0 };
	gint fcClx, lcbClx;
	guint8 *piece_table = NULL;
	guint8 *clx = NULL;
	gint lcb_piece_table;
	gint piece_count;
	gint32 fc;
	GString *content = NULL;
	gchar *normalized = NULL;

	document_stream = gsf_infile_child_by_name (infile, "WordDocument");
	if (document_stream == NULL) {
		return NULL;
	}

	/* abort if FIB can't be found from beginning of WordDocument stream */
	gsf_input_seek (document_stream, 0, G_SEEK_SET);
	gsf_input_read (document_stream, 2, tmp_buffer);
	if (read_16bit (tmp_buffer) != 0xa5ec) {
		g_object_unref (document_stream);
		return NULL;
	}

	/* abort if document is encrypted */
	gsf_input_seek (document_stream, 11, G_SEEK_SET);
	gsf_input_read (document_stream, 1, tmp_buffer);
	if ((tmp_buffer[0] & 0x1) == 0x1) {
		g_object_unref (document_stream);
		*is_encrypted = TRUE;
		return NULL;
	} else
		*is_encrypted = FALSE;

	/* document can have 0Table or 1Table or both. If flag 0x0200 is
	 * set to true in word 0x000A of the FIB then 1Table is used
	 */
	gsf_input_seek (document_stream, 0x000A, G_SEEK_SET);
	gsf_input_read (document_stream, 2, tmp_buffer);
	i = read_16bit (tmp_buffer);

	if ((i & 0x0200) == 0x0200) {
		table_stream = gsf_infile_child_by_name (infile, "1Table");
	} else {
		table_stream = gsf_infile_child_by_name (infile, "0Table");
	}

	if (table_stream == NULL) {
		g_object_unref (G_OBJECT (document_stream));
		return NULL;
	}

	/* find out location and length of piece table from FIB */
	gsf_input_seek (document_stream, 418, G_SEEK_SET);
	gsf_input_read (document_stream, 4, tmp_buffer);
	fcClx = read_32bit (tmp_buffer);
	gsf_input_read (document_stream, 4, tmp_buffer);
	lcbClx = read_32bit (tmp_buffer);

	/* copy the structure holding the piece table into the clx array. */
	clx = g_malloc (lcbClx);
	gsf_input_seek (table_stream, fcClx, G_SEEK_SET);
	gsf_input_read (table_stream, lcbClx, clx);

	/* find out piece table from clx and set piece_table -pointer to it */
	i = 0;
	lcb_piece_table = 0;

	while (TRUE) {
		if (clx[i] == 2) {
			lcb_piece_table = read_32bit (clx + (i + 1));
			piece_table = clx + i + 5;
			piece_count = (lcb_piece_table - 4) / 12;
			break;
		} else if (clx[i] == 1) {
			i = i + 2 + clx[i + 1];
		} else {
			break;
		}
	}

	/* iterate over pieces and save text to the content -variable */
	for (i = 0; i < piece_count; i++) {
		gchar *converted_text;
		guint8 *text_buffer;
		guint8 *piece_descriptor;
		gint piece_start;
		gint piece_end;
		gint piece_size;
		gboolean is_ansi;

		/* logical position of the text piece in the document_stream */
		piece_start = read_32bit (piece_table + (i * 4));
		piece_end = read_32bit (piece_table + ((i + 1) * 4));

		/* descriptor of single piece from piece table */
		piece_descriptor = piece_table + ((piece_count + 1) * 4) + (i * 8);

		/* file character position */
		fc = read_32bit (piece_descriptor + 2);

		/* second bit is set to 1 if text is saved in ANSI encoding */
		is_ansi = (fc & 0x40000000) == 0x40000000;

		/* modify file character position according to text encoding */
		if (!is_ansi) {
			fc = (fc & 0xBFFFFFFF);
		} else {
			fc = (fc & 0xBFFFFFFF) >> 1;
		}

		/* unicode uses twice as many bytes as CP1252 */
		piece_size  = piece_end - piece_start;
		if (!is_ansi) {
			piece_size *= 2;
		}

		if (piece_size < 1) {
			continue;
		}

		/* read single text piece from document_stream */
		text_buffer = g_malloc (piece_size);
		gsf_input_seek (document_stream, fc, G_SEEK_SET);
		gsf_input_read (document_stream, piece_size, text_buffer);

		/* pieces can have different encoding */
		converted_text = g_convert (text_buffer,
		                            piece_size,
		                            "UTF-8",
		                            is_ansi ? "CP1252" : "UTF-16",
		                            NULL,
		                            NULL,
		                            NULL);

		if (converted_text) {
			if (!content) {
				content = g_string_new (converted_text);
			} else {
				g_string_append (content, converted_text);
			}

			g_free (converted_text);
		}

		g_free (text_buffer);
	}

	g_object_unref (document_stream);
	g_object_unref (table_stream);
	g_free (clx);

	if (content) {
		normalized = tracker_extract_text_normalize (content->str, n_words, NULL);
		g_string_free (content, TRUE);
	}

	return normalized;
}

/**
 * @brief Extract summary OLE stream from specified uri
 * @param metadata where to store summary
 * @param infile file to read summary from
 * @param uri uri of the file
 */
static gboolean
extract_summary (TrackerSparqlBuilder *metadata,
                 GsfInfile            *infile,
                 const gchar          *uri)
{
	GsfInput *stream;
	gchar *content;
	gboolean is_encrypted = FALSE;

	tracker_sparql_builder_predicate (metadata, "a");
	tracker_sparql_builder_object (metadata, "nfo:PaginatedTextDocument");

	stream = gsf_infile_child_by_name (infile, "\05SummaryInformation");

	if (stream) {
		GsfDocMetaData *md;
		MetadataInfo info;
		GError *error = NULL;

		md = gsf_doc_meta_data_new ();
		error = gsf_msole_metadata_read (stream, md);

		if (error) {
			g_warning ("Could not extract summary information, %s",
			           error->message ? error->message : "no error given");

			g_error_free (error);
			g_object_unref (md);
			g_object_unref (stream);
			gsf_shutdown ();

			return FALSE;
		}

		info.metadata = metadata;
		info.uri = uri;

		gsf_doc_meta_data_foreach (md, summary_metadata_cb, &info);

		g_object_unref (md);
		g_object_unref (stream);
	}

	stream = gsf_infile_child_by_name (infile, "\05DocumentSummaryInformation");

	if (stream) {
		GsfDocMetaData *md;
		MetadataInfo info;
		GError *error = NULL;

		md = gsf_doc_meta_data_new ();

		error = gsf_msole_metadata_read (stream, md);
		if (error) {
			g_warning ("Could not extract document summary information, %s",
			           error->message ? error->message : "no error given");

			g_error_free (error);
			g_object_unref (md);
			g_object_unref (stream);
			gsf_shutdown ();

			return FALSE;
		}

		info.metadata = metadata;
		info.uri = uri;

		gsf_doc_meta_data_foreach (md, document_metadata_cb, &info);

		g_object_unref (md);
		g_object_unref (stream);
	}

	content = extract_msword_content (infile, fts_max_words (), &is_encrypted);

	if (content) {
		tracker_sparql_builder_predicate (metadata, "nie:plainTextContent");
		tracker_sparql_builder_object_unvalidated (metadata, content);
		g_free (content);
	}

	if (is_encrypted) {
		tracker_sparql_builder_predicate (metadata, "nfo:isContentEncrypted");
		tracker_sparql_builder_object_boolean (metadata, TRUE);
	}

	return TRUE;
}

/**
 * @brief Extract data from generic office files
 *
 * At the moment only extracts document summary from summary OLE stream.
 * @param uri URI of the file to extract data
 * @param metadata where to store extracted data to
 */
static void
extract_msoffice (const gchar          *uri,
                  TrackerSparqlBuilder *preupdate,
                  TrackerSparqlBuilder *metadata)
{
	GsfInfile *infile;

	gsf_init ();

	infile = open_uri (uri);
	if (infile) {
		extract_summary (metadata, infile, uri);
		g_object_unref (infile);
	}

	gsf_shutdown ();
}

/**
 * @brief Extract data from powerpoin files
 *
 * At the moment can extract textual content and summary.
 * @param uri URI of the file to extract data
 * @param metadata where to store extracted data to
 */
static void
extract_ppt (const gchar          *uri,
             TrackerSparqlBuilder *preupdate,
             TrackerSparqlBuilder *metadata)
{
	GsfInfile *infile;

	gsf_init ();

	infile = open_uri (uri);
	if (infile) {
		extract_summary (metadata, infile, uri);
		ppt_read (infile, metadata, fts_max_words());
		g_object_unref (infile);
	}

	gsf_shutdown ();
}

static void
start_element_handler_text_data (GMarkupParseContext  *context,
				 const gchar          *element_name,
				 const gchar         **attribute_names,
				 const gchar         **attribute_values,
				 gpointer              user_data,
				 GError              **error)
{
	MsOfficeXMLParserInfo *info = user_data;
	const gchar **a;
	const gchar **v;

	switch (info->file_type) {
	case FILE_TYPE_DOCX:
		if (g_ascii_strcasecmp (element_name, "w:pStyle") == 0) {
			for (a = attribute_names, v = attribute_values; *a; ++a, ++v) {
				if (g_ascii_strcasecmp (*a, "w:val") != 0) {
					continue;
				}

				if (g_ascii_strncasecmp (*v, "Heading", 7) == 0) {
					info->style_element_present = TRUE;
				} else if (g_ascii_strncasecmp (*v, "TOC", 3) == 0) {
					info->style_element_present = TRUE;
				} else if (g_ascii_strncasecmp (*v, "Section", 7) == 0) {
					info->style_element_present = TRUE;
				} else if (g_ascii_strncasecmp (*v, "Title", 5) == 0) {
					info->style_element_present = TRUE;
				} else if (g_ascii_strncasecmp (*v, "Subtitle", 8) == 0) {
					info->style_element_present = TRUE;
				}
			}
		} else if (g_ascii_strcasecmp (element_name, "w:rStyle") == 0) {
			for (a = attribute_names, v = attribute_values; *a; ++a, ++v) {
				if (g_ascii_strcasecmp (*a, "w:val") != 0) {
					continue;
				}

				if (g_ascii_strncasecmp (*v, "SubtleEmphasis", 14) == 0) {
					info->style_element_present = TRUE;
				} else if (g_ascii_strncasecmp (*v, "SubtleReference", 15) == 0) {
					info->style_element_present = TRUE;
				}
			}
		} else if (g_ascii_strcasecmp (element_name, "w:sz") == 0) {
			for (a = attribute_names, v = attribute_values; *a; ++a, ++v) {
				if (g_ascii_strcasecmp (*a, "w:val") != 0) {
					continue;
				}

				if (atoi (*v) >= 38) {
					info->style_element_present = TRUE;
				}
			}
		} else if (g_ascii_strcasecmp (element_name, "w:smartTag") == 0) {
			info->style_element_present = TRUE;
		} else if (g_ascii_strcasecmp (element_name, "w:sdtContent") == 0) {
			info->style_element_present = TRUE;
		} else if (g_ascii_strcasecmp (element_name, "w:hyperlink") == 0) {
			info->style_element_present = TRUE;
		} else if (g_ascii_strcasecmp (element_name, "w:t") == 0) {
		        for (a = attribute_names, v = attribute_values; *a; ++a, ++v) {
				if (g_ascii_strcasecmp (*a, "xml:space") != 0) {
					continue;
				}

				if (g_ascii_strncasecmp (*v, "preserve", 8) == 0) {
					info->preserve_attribute_present = TRUE;
				}
			}

		        info->tag_type = TAG_TYPE_WORD_TEXT;
		}
		break;

	case FILE_TYPE_XLSX:
		if (g_ascii_strcasecmp (element_name, "sheet") == 0) {
			for (a = attribute_names, v = attribute_values; *a; ++a, ++v) {
				if (g_ascii_strcasecmp (*a, "name") == 0) {
					info->tag_type = TAG_TYPE_XLS_SHARED_TEXT;
				}
			}

		} else if (g_ascii_strcasecmp (element_name, "t") == 0) {
			info->tag_type = TAG_TYPE_XLS_SHARED_TEXT;
		}
		break;

	case FILE_TYPE_PPTX:
		info->tag_type = TAG_TYPE_SLIDE_TEXT;
		break;

	case FILE_TYPE_INVALID:
		g_message ("Microsoft document type:%d invalid", info->file_type);
		break;
	}
}

static void
end_element_handler_document_data (GMarkupParseContext  *context,
                                   const gchar          *element_name,
                                   gpointer              user_data,
                                   GError              **error)
{
	MsOfficeXMLParserInfo *info = user_data;

	if (g_ascii_strcasecmp (element_name, "w:p") == 0) {
		info->style_element_present = FALSE;
		info->preserve_attribute_present = FALSE;
	}

	((MsOfficeXMLParserInfo*) user_data)->tag_type = TAG_TYPE_INVALID;
}

static void
start_element_handler_core_data	(GMarkupParseContext  *context,
				const gchar           *element_name,
				const gchar          **attribute_names,
				const gchar          **attribute_values,
				gpointer               user_data,
				GError               **error)
{
	MsOfficeXMLParserInfo *info = user_data;

	if (g_ascii_strcasecmp (element_name, "dc:title") == 0) {
		info->tag_type = TAG_TYPE_TITLE;
	} else if (g_ascii_strcasecmp (element_name, "dc:subject") == 0) {
		info->tag_type = TAG_TYPE_SUBJECT;
	} else if (g_ascii_strcasecmp (element_name, "dc:creator") == 0) {
		info->tag_type = TAG_TYPE_AUTHOR;
	} else if (g_ascii_strcasecmp (element_name, "dc:description") == 0) {
		info->tag_type = TAG_TYPE_COMMENTS;
	} else if (g_ascii_strcasecmp (element_name, "dcterms:created") == 0) {
		info->tag_type = TAG_TYPE_CREATED;
	} else if (g_ascii_strcasecmp (element_name, "meta:generator") == 0) {
		info->tag_type = TAG_TYPE_GENERATOR;
	} else if (g_ascii_strcasecmp (element_name, "dcterms:modified") == 0) {
		info->tag_type = TAG_TYPE_MODIFIED;
	} else if (g_ascii_strcasecmp (element_name, "cp:lastModifiedBy") == 0) {
		/* Do nothing ? */
	} else if (g_ascii_strcasecmp (element_name, "Pages") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_PAGES;
	} else if (g_ascii_strcasecmp (element_name, "Slides") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_PAGES;
	} else if (g_ascii_strcasecmp (element_name, "Paragraphs") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_PARAGRAPHS;
	} else if (g_ascii_strcasecmp (element_name, "Characters") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_CHARACTERS;
	} else if (g_ascii_strcasecmp (element_name, "Words") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_WORDS;
	} else if (g_ascii_strcasecmp (element_name, "Lines") == 0) {
		info->tag_type = TAG_TYPE_NUM_OF_LINES;
	} else if (g_ascii_strcasecmp (element_name, "Application") == 0) {
		info->tag_type = TAG_TYPE_APPLICATION;
	} else {
		info->tag_type = TAG_TYPE_INVALID;
	}
}

static void
text_handler_document_data (GMarkupParseContext  *context,
                            const gchar          *text,
                            gsize                 text_len,
                            gpointer              user_data,
                            GError              **error)
{
	MsOfficeXMLParserInfo *info = user_data;
	static gboolean found = FALSE;
	static gboolean added = FALSE;

	switch (info->tag_type) {
	case TAG_TYPE_WORD_TEXT:
		if (info->style_element_present) {
			if (atoi (text) == 0) {
				g_string_append_printf (info->content, "%s ", text);
			}
		}

		if (info->preserve_attribute_present) {
			gchar *keywords = g_strdup (text);

			if (found && (strlen (keywords) > 3)) {
				g_string_append_printf (info->content, "%s ", text);
				found = FALSE;
			} else {
				gchar *lasts;
				gchar *keyw;

				for (keyw = strtok_r (keywords, ",; ", &lasts);
				     keyw;
				     keyw = strtok_r (NULL, ",; ", &lasts)) {
					if ((g_ascii_strncasecmp (keyw, "Table", 6) == 0) ||
					    (g_ascii_strncasecmp (keyw, "Figure", 6) == 0) ||
					    (g_ascii_strncasecmp (keyw, "Section", 7) == 0) ||
					    (g_ascii_strncasecmp (keyw, "Index", 5) == 0)) {
						found = TRUE;
					}
				}
			}

			g_free (keywords);
		}
		break;

	case TAG_TYPE_SLIDE_TEXT:
		if (strlen (text) > 3) {
			g_string_append_printf (info->content, "%s ", text);
		}
		break;

	case TAG_TYPE_XLS_SHARED_TEXT:
		if ((atoi (text) == 0) && (strlen (text) > 4))  {
			g_string_append_printf (info->content, "%s ", text);
		}
		break;

	case TAG_TYPE_TITLE:
		tracker_sparql_builder_predicate (info->metadata, "nie:title");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_SUBJECT:
		tracker_sparql_builder_predicate (info->metadata, "nie:subject");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_AUTHOR:
		tracker_sparql_builder_predicate (info->metadata, "nco:publisher");

		tracker_sparql_builder_object_blank_open (info->metadata);
		tracker_sparql_builder_predicate (info->metadata, "a");
		tracker_sparql_builder_object (info->metadata, "nco:Contact");

		tracker_sparql_builder_predicate (info->metadata, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		tracker_sparql_builder_object_blank_close (info->metadata);
		break;

	case TAG_TYPE_COMMENTS:
		tracker_sparql_builder_predicate (info->metadata, "nie:comment");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_CREATED:
		tracker_sparql_builder_predicate (info->metadata, "nie:contentCreated");
		tracker_sparql_builder_object_unvalidated (info->metadata, tracker_extract_guess_date (text));
		break;

	case TAG_TYPE_GENERATOR:
		if (!added) {
			tracker_sparql_builder_predicate (info->metadata, "nie:generator");
			tracker_sparql_builder_object_unvalidated (info->metadata, text);
			added = TRUE;
		}
		break;

	case TAG_TYPE_APPLICATION:
		/* FIXME: Same code as TAG_TYPE_GENERATOR should be
		 * used, but nie:generator has max cardinality of 1
		 * and this would cause errors.
		 */
		break;

	case TAG_TYPE_MODIFIED:
		tracker_sparql_builder_predicate (info->metadata, "nie:contentLastModified");
		tracker_sparql_builder_object_unvalidated (info->metadata, tracker_extract_guess_date (text));
		break;

	case TAG_TYPE_NUM_OF_PAGES:
		tracker_sparql_builder_predicate (info->metadata, "nfo:pageCount");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_NUM_OF_CHARACTERS:
		tracker_sparql_builder_predicate (info->metadata, "nfo:characterCount");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_NUM_OF_WORDS:
		tracker_sparql_builder_predicate (info->metadata, "nfo:wordCount");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_NUM_OF_LINES:
		tracker_sparql_builder_predicate (info->metadata, "nfo:lineCount");
		tracker_sparql_builder_object_unvalidated (info->metadata, text);
		break;

	case TAG_TYPE_NUM_OF_PARAGRAPHS:
		/* TODO: There is no ontology for this. */
		break;

	case TAG_TYPE_DOCUMENT_CORE_DATA:
	case TAG_TYPE_DOCUMENT_TEXT_DATA:
		/* Nothing as we are using it in defining type of data */
		break;

	case TAG_TYPE_INVALID:
		/* Here we cant use log otheriwse it will print for other non useful files */
		break;
	}
}

static gboolean
xml_read (MsOfficeXMLParserInfo *parser_info,
          const gchar           *xml_filename,
          TagType                type)
{
	GMarkupParseContext *context;
	MsOfficeXMLParserInfo info;
	gchar *xml;
	gchar *filename;
	const gchar *argv[5];
	gboolean success;

	filename = g_filename_from_uri (parser_info->uri, NULL, NULL);

	argv[0] = "unzip";
	argv[1] = "-p";
	argv[2] = filename;
	argv[3] = xml_filename;
	argv[4] = NULL;

	g_debug ("Reading XML data '%s'", argv[3]);

	xml = NULL;

	success = tracker_spawn ((gchar**) argv, 10, &xml, NULL);
	g_free (filename);

	if (!success) {
		g_free (xml);
		return FALSE;
	}

	/* FIXME: Can we use the original info here? */
	info.metadata = parser_info->metadata;
	info.file_type = parser_info->file_type;
	info.tag_type = TAG_TYPE_INVALID;
	info.style_element_present = FALSE;
	info.preserve_attribute_present = FALSE;
	info.uri = parser_info->uri;
	info.content = parser_info->content;

	switch (type) {
	case TAG_TYPE_DOCUMENT_CORE_DATA: {
		GMarkupParser parser = {
			start_element_handler_core_data,
			end_element_handler_document_data,
			text_handler_document_data,
			NULL,
			NULL
		};

		context = g_markup_parse_context_new (&parser,
		                                      0,
		                                      &info,
		                                      NULL);
		break;
	}

	case TAG_TYPE_DOCUMENT_TEXT_DATA: {
		GMarkupParser parser = {
			start_element_handler_text_data,
			end_element_handler_document_data,
			text_handler_document_data,
			NULL,
			NULL
		};

		context = g_markup_parse_context_new (&parser,
		                                      0,
		                                      &info,
		                                      NULL);
		break;
	}

	default:
		context = NULL;
		break;
	}

	if (context) {
		g_markup_parse_context_parse (context, xml, -1, NULL);
		g_markup_parse_context_free (context);
	}

	g_free (xml);

	return TRUE;
}

static void
start_element_handler_content_types (GMarkupParseContext  *context,
                                     const gchar          *element_name,
                                     const gchar         **attribute_names,
                                     const gchar         **attribute_values,
                                     gpointer              user_data,
                                     GError              **error)
{
	MsOfficeXMLParserInfo *info;
	const gchar *part_name;
	const gchar *content_type;
	gint i;

	info = user_data;

	if (g_ascii_strcasecmp (element_name, "Override") != 0) {
		info->tag_type = TAG_TYPE_INVALID;
		return;
	}

	part_name = NULL;
	content_type = NULL;

	for (i = 0; attribute_names[i]; i++) {
		if (g_ascii_strcasecmp (attribute_names[i], "PartName") == 0) {
			part_name = attribute_values[i];
		} else if (g_ascii_strcasecmp (attribute_names[i], "ContentType") == 0) {
			content_type = attribute_values[i];
		}
	}

	if ((g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-package.core-properties+xml") == 0) ||
	    (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.extended-properties+xml") == 0)) {
		xml_read (info, part_name + 1, TAG_TYPE_DOCUMENT_CORE_DATA);
		return;
	}

	switch (info->file_type) {
	case FILE_TYPE_DOCX:
		if (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml") == 0) {
			xml_read (info, part_name + 1, TAG_TYPE_DOCUMENT_TEXT_DATA);
		}
		break;

	case FILE_TYPE_PPTX:
		if ((g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.presentationml.slide+xml") == 0) ||
		    (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.drawingml.diagramData+xml") == 0)) {
			xml_read (info, part_name + 1, TAG_TYPE_DOCUMENT_TEXT_DATA);
		}
		break;

	case FILE_TYPE_XLSX:
		if ((g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml") == 0) ||
		    (g_ascii_strcasecmp (content_type, "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml") == 0)) {
			xml_read (info, part_name + 1, TAG_TYPE_DOCUMENT_TEXT_DATA);
		}
		break;

	case FILE_TYPE_INVALID:
		g_message ("Invalid file type:'%d'", info->file_type);
		break;
	}
}

static void
extract_msoffice_xml (const gchar          *uri,
                      TrackerSparqlBuilder *preupdate,
                      TrackerSparqlBuilder *metadata)
{
	MsOfficeXMLParserInfo info;
	FileType file_type;
	GFile *file;
	GFileInfo *file_info;
	GMarkupParseContext *context = NULL;
	GMarkupParser parser = {
		start_element_handler_content_types,
		end_element_handler_document_data,
		NULL,
		NULL,
		NULL
	};
	gchar *filename;
	gchar *xml;
	const gchar *mime_used;
	const gchar *argv[5];
	gboolean success;

	file = g_file_new_for_uri (uri);

	if (!file) {
		g_warning ("Could not create GFile for URI:'%s'",
		           uri);
		return;
	}

	file_info = g_file_query_info (file,
	                               G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
	                               G_FILE_QUERY_INFO_NONE,
	                               NULL,
	                               NULL);
	g_object_unref (file);

	if (!file_info) {
		g_warning ("Could not get GFileInfo for URI:'%s'",
		           uri);
		return;
	}

	mime_used = g_file_info_get_content_type (file_info);

	if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.wordprocessingml.document") == 0) {
		file_type = FILE_TYPE_DOCX;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.presentationml.presentation") == 0) {
		file_type = FILE_TYPE_PPTX;
	} else if (g_ascii_strcasecmp (mime_used, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") == 0) {
		file_type = FILE_TYPE_XLSX;
	} else {
		g_message ("Mime type was not recognised:'%s'", mime_used);
		file_type = FILE_TYPE_INVALID;
	}

	g_object_unref (file_info);

	filename = g_filename_from_uri (uri, NULL, NULL);

	argv[0] = "unzip";
	argv[1] = "-p";
	argv[2] = filename;
	argv[3] = "\\[Content_Types\\].xml";
	argv[4] = NULL;

        g_debug ("Extracting MsOffice XML format...");

	tracker_sparql_builder_predicate (metadata, "a");
	tracker_sparql_builder_object (metadata, "nfo:PaginatedTextDocument");

	xml = NULL;

	success = tracker_spawn ((gchar**) argv, 10, &xml, NULL);
	g_free (filename);

	if (!success) {
		g_free (xml);
		return;
	}

	info.metadata = metadata;
	info.file_type = file_type;
	info.tag_type = TAG_TYPE_INVALID;
	info.style_element_present = FALSE;
	info.preserve_attribute_present = FALSE;
	info.uri = uri;
	info.content = g_string_new ("");

	context = g_markup_parse_context_new (&parser, 0, &info, NULL);
	g_markup_parse_context_parse (context, xml, -1, NULL);
	g_free (xml);

	if (info.content) {
		gchar *content;

		content = g_string_free (info.content, FALSE);
		info.content = NULL;

		if (content) {
			tracker_sparql_builder_predicate (metadata, "nie:plainTextContent");
			tracker_sparql_builder_object_unvalidated (metadata, content);
			g_free (content);
		}
	}

	g_markup_parse_context_free (context);
}

TrackerExtractData *
tracker_extract_get_data (void)
{
	return data;
}
