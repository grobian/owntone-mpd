/*
 * Copyright (C) 2016 Christian Meffert <christian.meffert@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <stdint.h>
#include <inttypes.h>
#include <netinet/in.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/listener.h>


#include "artwork.h"
#include "commands.h"
#include "conffile.h"
#include "db.h"
#include "library.h"
#include "listener.h"
#include "logger.h"
#include "misc.h"
#include "player.h"
#include "remote_pairing.h"


enum mpd_type {
  MPD_TYPE_INT,
  MPD_TYPE_STRING,
  MPD_TYPE_SPECIAL,
};


#define MPD_ALL_IDLE_LISTENER_EVENTS (LISTENER_PLAYER | LISTENER_QUEUE | LISTENER_VOLUME | LISTENER_SPEAKER | LISTENER_OPTIONS | LISTENER_DATABASE | LISTENER_UPDATE | LISTENER_STORED_PLAYLIST | LISTENER_RATING)
#define MPD_RATING_FACTOR 10.0
#define MPD_BINARY_SIZE 8192  /* MPD MAX_BINARY_SIZE */
#define MPD_BINARY_SIZE_MIN 64  /* min size from MPD ClientCommands.cxx */

static pthread_t tid_mpd;

static struct event_base *evbase_mpd;

static struct commands_base *cmdbase;

static struct evhttp *evhttpd;

static struct evconnlistener *mpd_listener;
static int mpd_sockfd;

static bool mpd_plugin_httpd;

// Virtual path to the default playlist directory
static char *default_pl_dir;
static bool allow_modifying_stored_playlists;

#define COMMAND_ARGV_MAX 37

/* MPD error codes (taken from ack.h) */
enum ack
{
  ACK_ERROR_NOT_LIST = 1,
  ACK_ERROR_ARG = 2,
  ACK_ERROR_PASSWORD = 3,
  ACK_ERROR_PERMISSION = 4,
  ACK_ERROR_UNKNOWN = 5,

  ACK_ERROR_NO_EXIST = 50,
  ACK_ERROR_PLAYLIST_MAX = 51,
  ACK_ERROR_SYSTEM = 52,
  ACK_ERROR_PLAYLIST_LOAD = 53,
  ACK_ERROR_UPDATE_ALREADY = 54,
  ACK_ERROR_PLAYER_SYNC = 55,
  ACK_ERROR_EXIST = 56,
};

enum command_list_type
{
  COMMAND_LIST = 1,
  COMMAND_LIST_OK = 2,
  COMMAND_LIST_END = 3,
  COMMAND_LIST_NONE = 4
};

enum position_type
{
  POSITION_ABSOLUTE = 1,
  POSITION_RELATIVE_BEFORE,
  POSITION_RELATIVE_AFTER
};

/**
 * This lists for ffmpeg suffixes and mime types are taken from the ffmpeg decoder plugin from mpd
 * (FfmpegDecoderPlugin.cxx, git revision 9fb351a139a56fc7b1ece549894f8fc31fa887cd).
 *
 * The server does not support different decoders and always uses ffmpeg or libav for decoding.
 * Some clients rely on a response for the decoder commands (e.g. ncmpccp) therefor return something
 * valid for this command.
 */
static const char * const ffmpeg_suffixes[] = { "16sv", "3g2", "3gp", "4xm", "8svx", "aa3", "aac", "ac3", "afc", "aif",
    "aifc", "aiff", "al", "alaw", "amr", "anim", "apc", "ape", "asf", "atrac", "au", "aud", "avi", "avm2", "avs", "bap",
    "bfi", "c93", "cak", "cin", "cmv", "cpk", "daud", "dct", "divx", "dts", "dv", "dvd", "dxa", "eac3", "film", "flac",
    "flc", "fli", "fll", "flx", "flv", "g726", "gsm", "gxf", "iss", "m1v", "m2v", "m2t", "m2ts", "m4a", "m4b", "m4v",
    "mad", "mj2", "mjpeg", "mjpg", "mka", "mkv", "mlp", "mm", "mmf", "mov", "mp+", "mp1", "mp2", "mp3", "mp4", "mpc",
    "mpeg", "mpg", "mpga", "mpp", "mpu", "mve", "mvi", "mxf", "nc", "nsv", "nut", "nuv", "oga", "ogm", "ogv", "ogx",
    "oma", "ogg", "omg", "psp", "pva", "qcp", "qt", "r3d", "ra", "ram", "rl2", "rm", "rmvb", "roq", "rpl", "rvc", "shn",
    "smk", "snd", "sol", "son", "spx", "str", "swf", "tgi", "tgq", "tgv", "thp", "ts", "tsp", "tta", "xa", "xvid", "uv",
    "uv2", "vb", "vid", "vob", "voc", "vp6", "vmd", "wav", "webm", "wma", "wmv", "wsaud", "wsvga", "wv", "wve",
    NULL
};
static const char * const ffmpeg_mime_types[] = { "application/flv", "application/m4a", "application/mp4",
    "application/octet-stream", "application/ogg", "application/x-ms-wmz", "application/x-ms-wmd", "application/x-ogg",
    "application/x-shockwave-flash", "application/x-shorten", "audio/8svx", "audio/16sv", "audio/aac", "audio/ac3",
    "audio/aiff", "audio/amr", "audio/basic", "audio/flac", "audio/m4a", "audio/mp4", "audio/mpeg", "audio/musepack",
    "audio/ogg", "audio/qcelp", "audio/vorbis", "audio/vorbis+ogg", "audio/x-8svx", "audio/x-16sv", "audio/x-aac",
    "audio/x-ac3", "audio/x-aiff", "audio/x-alaw", "audio/x-au", "audio/x-dca", "audio/x-eac3", "audio/x-flac",
    "audio/x-gsm", "audio/x-mace", "audio/x-matroska", "audio/x-monkeys-audio", "audio/x-mpeg", "audio/x-ms-wma",
    "audio/x-ms-wax", "audio/x-musepack", "audio/x-ogg", "audio/x-vorbis", "audio/x-vorbis+ogg", "audio/x-pn-realaudio",
    "audio/x-pn-multirate-realaudio", "audio/x-speex", "audio/x-tta", "audio/x-voc", "audio/x-wav", "audio/x-wma",
    "audio/x-wv", "video/anim", "video/quicktime", "video/msvideo", "video/ogg", "video/theora", "video/webm",
    "video/x-dv", "video/x-flv", "video/x-matroska", "video/x-mjpeg", "video/x-mpeg", "video/x-ms-asf",
    "video/x-msvideo", "video/x-ms-wmv", "video/x-ms-wvx", "video/x-ms-wm", "video/x-ms-wmx", "video/x-nut",
    "video/x-pva", "video/x-theora", "video/x-vid", "video/x-wmv", "video/x-xvid",

    /* special value for the "ffmpeg" input plugin: all streams by
     the "ffmpeg" input plugin shall be decoded by this
     plugin */
    "audio/x-mpd-ffmpeg",

    NULL
};

struct mpd_tagtype
{
  char *tag;
  char *field;
  char *sort_field;
  char *group_field;
  enum mpd_type type;
  ssize_t mfi_offset;

  /*
   * This allows omitting the "group" fields in the created group by clause to improve
   * performance in the "list" command. For example listing albums and artists already
   * groups by their persistent id, an additional group clause by artist/album will
   * decrease performance of the select query and will in general not change the result
   * (e. g. album persistent id is generated by artist and album and listing albums
   * grouped by artist is therefor not necessary).
   */
  bool group_in_listcommand;
};

/* https://mpd.readthedocs.io/en/latest/protocol.html#tags */
static struct mpd_tagtype tagtypes[] =
  {
    /* tag               | db field             | db sort field                        | db group field  | type             | media_file offset                | group_in_listcommand */

    // We treat the artist tag as album artist, this allows grouping over the artist-persistent-id index and increases performance
    // { "Artist",           "f.artist",             "f.artist",             "f.artist",             MPD_TYPE_STRING,   dbmfi_offsetof(artist), },
    { "Artist",           "f.album_artist",       "f.album_artist_sort, f.album_artist", "f.songartistid", MPD_TYPE_STRING,   dbmfi_offsetof(album_artist),      false, },
    { "ArtistSort",       "f.album_artist_sort",  "f.album_artist_sort, f.album_artist", "f.songartistid", MPD_TYPE_STRING,   dbmfi_offsetof(album_artist_sort), false, },
    { "Album",            "f.album",              "f.album_sort, f.album",               "f.songalbumid",  MPD_TYPE_STRING,   dbmfi_offsetof(album),             false, },
    { "AlbumSort",        "f.album_sort",         "f.album_sort, f.album",               "f.songalbumid",  MPD_TYPE_STRING,   dbmfi_offsetof(album),             false, },
    { "AlbumArtist",      "f.album_artist",       "f.album_artist_sort, f.album_artist", "f.songartistid", MPD_TYPE_STRING,   dbmfi_offsetof(album_artist),      false, },
    { "AlbumArtistSort",  "f.album_artist_sort",  "f.album_artist_sort, f.album_artist", "f.songartistid", MPD_TYPE_STRING,   dbmfi_offsetof(album_artist_sort), false, },
    { "Title",            "f.title",              "f.title",                             "f.title_sort",   MPD_TYPE_STRING,   dbmfi_offsetof(title),             true, },
    { "TitleSort",        "f.title_sort",         "f.title",                             "f.title_sort",   MPD_TYPE_STRING,   dbmfi_offsetof(title),             true, },
    { "Track",            "f.track",              "f.track",                             "f.track",        MPD_TYPE_INT,      dbmfi_offsetof(track),             true, },
    { "Name",             "f.title",              "f.title_sort",                        "f.title",        MPD_TYPE_STRING,   dbmfi_offsetof(genre),             true, },
    { "Genre",            "f.genre",              "f.genre",                             "f.genre",        MPD_TYPE_STRING,   dbmfi_offsetof(genre),             true, },
    /* mood */
    { "Date",             "f.year",               "f.year",                              "f.year",         MPD_TYPE_INT,      dbmfi_offsetof(year),              true, },
    { "OriginalDate",     "f.date_released",      "f.date_released",                     "f.date_released", MPD_TYPE_INT,      dbmfi_offsetof(date_released),     true, },
    { "Composer",         "f.composer",           "f.composer_sort",                     "f.composer",     MPD_TYPE_STRING, dbmfi_offsetof(composer),          true, },
    { "ComposerSort",     "f.composer_sort",       "f.composer_sort",                     "f.composer_sort", MPD_TYPE_STRING, dbmfi_offsetof(composer_sort),     true, },
    /* performer */
    { "Conductor",        "f.conductor",           "f.conductor",                         "f.conductor",     MPD_TYPE_STRING, dbmfi_offsetof(conductor),         true, },
    /* work */
    /* ensemble */
    /* movement */
    /* movementnumber */
    /* location */
    { "Grouping",         "f.grouping",            "f.grouping",                          "f.grouping",      MPD_TYPE_STRING, dbmfi_offsetof(grouping),          true, },
    { "Comment",          "f.comment",             "f.comment",                           "f.comment",       MPD_TYPE_STRING, dbmfi_offsetof(comment),           true, },
    { "Disc",             "f.disc",                "f.disc",                              "f.disc",          MPD_TYPE_INT,      dbmfi_offsetof(disc),              true, },
    /* label */
    /* musicbrainz_* */
    /* below are pseudo tags not defined in the docs but used in
     * examples */
    { "file",             NULL,                   NULL,                                  NULL,             MPD_TYPE_SPECIAL,  -1,                                true, },
    { "base",             NULL,                   NULL,                                  NULL,             MPD_TYPE_SPECIAL,  -1,                                true, },
    { "any",              NULL,                   NULL,                                  NULL,             MPD_TYPE_SPECIAL,  -1,                                true, },
    { "modified-since",   NULL,                   NULL,                                  NULL,             MPD_TYPE_SPECIAL,  -1,                                true, },

  };

static struct mpd_tagtype *
find_tagtype(const char *tag)
{
  int i;

  if (!tag)
    return 0;

  for (i = 0; i < ARRAY_SIZE(tagtypes); i++)
    {
      if (strcasecmp(tag, tagtypes[i].tag) == 0)
	return &tagtypes[i];
    }

  return NULL;
}

/*
 * MPD client connection data
 */
struct mpd_client_ctx
{
  // True if the connection is already authenticated or does not need authentication
  bool authenticated;

  // The events the client needs to be notified of
  short events;

  // True if the client is waiting for idle events
  bool is_idle;

  // The events the client is waiting for (set by the idle command)
  short idle_events;

  // The current binary limit size
  unsigned int binarylimit;

  // The output buffer for the client (used to send data to the client)
  struct evbuffer *evbuffer;

  struct mpd_client_ctx *next;
};

// List of all connected mpd clients
struct mpd_client_ctx *mpd_clients;

static void
free_mpd_client_ctx(void *ctx)
{
  struct mpd_client_ctx *client_ctx = ctx;
  struct mpd_client_ctx *client;
  struct mpd_client_ctx *prev;

  if (!client_ctx)
    return;

  client = mpd_clients;
  prev = NULL;

  while (client)
    {
      if (client == client_ctx)
	{
	  DPRINTF(E_DBG, L_MPD, "Removing mpd client\n");

	  if (prev)
	    prev->next = client->next;
	  else
	    mpd_clients = client->next;

	  break;
	}

      prev = client;
      client = client->next;
    }

  free(client_ctx);
}

struct output
{
  unsigned short shortid;
  uint64_t id;
  char *name;

  unsigned selected;
};

struct output_get_param
{
  unsigned short curid;
  unsigned short shortid;
  struct output *output;
};

struct output_outputs_param
{
  unsigned short nextid;
  struct evbuffer *buf;
};

static void
free_output(struct output *output)
{
  if (output)
    {
      free(output->name);
      free(output);
    }
}

/*
 * Creates a new string for the given path that starts with a '/'.
 * If 'path' already starts with a '/' the returned string is a duplicate
 * of 'path'.
 *
 * The returned string needs to be freed by the caller.
 */
static char *
prepend_slash(const char *path)
{
  char *result;

  if (path[0] == '/')
    result = strdup(path);
  else
    result = safe_asprintf("/%s", path);

  return result;
}


/* Thread: mpd */
static void *
mpd(void *arg)
{
  int ret;

  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MPD, "Error: DB init failed\n");

      pthread_exit(NULL);
    }

  event_base_dispatch(evbase_mpd);

  db_perthread_deinit();

  pthread_exit(NULL);
}

static void
mpd_time(char *buffer, size_t bufferlen, time_t t)
{
  struct tm tm;
  const struct tm *tm2 = gmtime_r(&t, &tm);
  if (tm2 == NULL)
    return;

  strftime(buffer, bufferlen, "%FT%TZ", tm2);
}

/*
 * Parses a rage argument of the form START:END (the END item is not included in the range)
 * into its start and end position.
 *
 * @param range the range argument
 * @param start_pos set by this method to the start position
 * @param end_pos set by this method to the end postion
 * @return 0 on success, -1 on failure
 */
static int
mpd_pars_range_arg(char *range, int *start_pos, int *end_pos)
{
  int ret;

  if (strchr(range, ':'))
    {
      ret = sscanf(range, "%d:%d", start_pos, end_pos);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error parsing range argument '%s' (return code = %d)\n", range, ret);
	  return -1;
	}
    }
  else
    {
      ret = safe_atoi32(range, start_pos);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error parsing integer argument '%s' (return code = %d)\n", range, ret);
	  return -1;
	}

      *end_pos = (*start_pos) + 1;
    }

  return 0;
}

/*
 * Returns the next unquoted string argument from the input string
 */
static char*
mpd_pars_unquoted(char **input)
{
  char *arg;

  arg = *input;

  while (**input != 0)
    {
      if (**input == ' ')
	{
	  **input = '\0';
	  (*input)++;
	  return arg;
	}

      (*input)++;
    }

  return arg;
}

/*
 * Returns the next quoted string argument from the input string
 * with the quotes removed
 */
static char*
mpd_pars_quoted(char **input)
{
  char *arg;
  char *src;
  char *dst;
  char ch;

  // skip double quote character
  (*input)++;

  src = dst = arg = *input;
  while ((ch = *src) != '"')
    {
      // A backslash character escapes the following character and should be removed
      if (ch == '\\')
	{
	  ch = *(++src);
	}
      *dst++ = ch;

      if (ch == 0)
	{
	  // Error handling for missing double quote at end of parameter
	  DPRINTF(E_LOG, L_MPD, "Error missing closing double quote in argument\n");
	  *input = src;
	  return NULL;
	}

      ++src;
    }

  *dst = '\0';
  *input = ++src;

  return arg;
}

/**
 * Helper for writing binary responses.
 * https://mpd.readthedocs.io/en/latest/protocol.html#binary
 * This helper writes the size line, and binary blocks respecting the
 * binarylimit.
 */
static bool
mpd_write_binary_response(struct mpd_client_ctx *ctx,
			  struct evbuffer *output,
			  struct evbuffer *data,
			  size_t offset)
{
  unsigned char *p;
  size_t len = evbuffer_get_length(data);

  if (len == 0 || len < offset)
    return false;

  /* write header for total size */
  evbuffer_add_printf(output, "size: %zu\n", len);

  len = MIN(len - offset, ctx->binarylimit);
  evbuffer_drain(data, offset);
  p = evbuffer_pullup(data, len);
  evbuffer_add_printf(output, "binary: %zu\n", len);
  evbuffer_add(output, p, len);
  evbuffer_add(output, "\n", 1);
  evbuffer_drain(data, len);

  return true;
}

/*
 * Parses the argument string into an array of strings.
 * Arguments are seperated by a whitespace character and may be wrapped in double quotes.
 *
 * @param args the arguments
 * @param argc the number of arguments in the argument string
 * @param argv the array containing the found arguments
 */
static int
mpd_parse_args(char *args, int *argc, char **argv, int argvsz)
{
  char *input;

  input = args;
  *argc = 0;

  while (*input != 0 && *argc < argvsz)
    {
      // Ignore whitespace characters
      if (*input == ' ')
	{
	  input++;
	  continue;
	}

      // Check if the parameter is wrapped in double quotes
      if (*input == '"')
	{
	  argv[*argc] = mpd_pars_quoted(&input);
	  if (argv[*argc] == NULL)
	    {
	      return -1;
	    }
	  *argc = *argc + 1;
	}
      else
	{
	  argv[*argc] = mpd_pars_unquoted(&input);
	  *argc = *argc + 1;
	}
    }

  return 0;
}

/*
 * Adds the informations (path, id, tags, etc.) for the given song to the given buffer
 * with additional information for the position of this song in the playqueue.
 *
 * Example output:
 *   file: foo/bar/song.mp3
 *   Last-Modified: 2013-07-14T06:57:59Z
 *   Time: 172
 *   Artist: foo
 *   AlbumArtist: foo
 *   ArtistSort: foo
 *   AlbumArtistSort: foo
 *   Title: song
 *   Album: bar
 *   Track: 1/11
 *   Date: 2012-09-11
 *   Genre: Alternative
 *   Disc: 1/1
 *   MUSICBRAINZ_ALBUMARTISTID: c5c2ea1c-4bde-4f4d-bd0b-47b200bf99d6
 *   MUSICBRAINZ_ARTISTID: c5c2ea1c-4bde-4f4d-bd0b-47b200bf99d6
 *   MUSICBRAINZ_ALBUMID: 812f4b87-8ad9-41bd-be79-38151f17a2b4
 *   MUSICBRAINZ_TRACKID: fde95c39-ee51-48f6-a7f9-b5631c2ed156
 *   Pos: 0
 *   Id: 1
 *
 * @param evbuf the response event buffer
 * @param queue_item queue item information
 * @return the number of bytes added if successful, or -1 if an error occurred.
 */
static int
mpd_add_db_queue_item(struct evbuffer *evbuf, struct db_queue_item *queue_item)
{
  char modified[32];
  int ret;

  mpd_time(modified, sizeof(modified), queue_item->time_modified);

  ret = evbuffer_add_printf(evbuf,
      "file: %s\n"
      "Last-Modified: %s\n"
      "Time: %d\n"
      "Artist: %s\n"
      "AlbumArtist: %s\n"
      "ArtistSort: %s\n"
      "AlbumArtistSort: %s\n"
      "Album: %s\n"
      "Title: %s\n"
      "Track: %d\n"
      "Date: %d\n"
      "Genre: %s\n"
      "Disc: %d\n"
      "Pos: %d\n"
      "Id: %d\n",
      (queue_item->virtual_path + 1),
      modified,
      (queue_item->song_length / 1000),
      queue_item->artist,
      queue_item->album_artist,
      queue_item->artist_sort,
      queue_item->album_artist_sort,
      queue_item->album,
      queue_item->title,
      queue_item->track,
      queue_item->year,
      queue_item->genre,
      queue_item->disc,
      queue_item->pos,
      queue_item->id);

  return ret;
}

/*
 * Adds the informations (path, id, tags, etc.) for the given song to the given buffer.
 *
 * Example output:
 *   file: foo/bar/song.mp3
 *   Last-Modified: 2013-07-14T06:57:59Z
 *   Time: 172
 *   Artist: foo
 *   AlbumArtist: foo
 *   ArtistSort: foo
 *   AlbumArtistSort: foo
 *   Title: song
 *   Album: bar
 *   Track: 1/11
 *   Date: 2012-09-11
 *   Genre: Alternative
 *   Disc: 1/1
 *   MUSICBRAINZ_ALBUMARTISTID: c5c2ea1c-4bde-4f4d-bd0b-47b200bf99d6
 *   MUSICBRAINZ_ARTISTID: c5c2ea1c-4bde-4f4d-bd0b-47b200bf99d6
 *   MUSICBRAINZ_ALBUMID: 812f4b87-8ad9-41bd-be79-38151f17a2b4
 *   MUSICBRAINZ_TRACKID: fde95c39-ee51-48f6-a7f9-b5631c2ed156
 *
 * @param evbuf the response event buffer
 * @param mfi media information
 * @return the number of bytes added if successful, or -1 if an error occurred.
 */
static int
mpd_add_db_media_file_info(struct evbuffer *evbuf, struct db_media_file_info *dbmfi)
{
  char modified[32];
  uint32_t time_modified;
  uint32_t songlength;
  int ret;

  if (safe_atou32(dbmfi->time_modified, &time_modified) != 0)
    {
      DPRINTF(E_LOG, L_MPD, "Error converting time modified to uint32_t: %s\n", dbmfi->time_modified);
      return -1;
    }

  mpd_time(modified, sizeof(modified), time_modified);

  if (safe_atou32(dbmfi->song_length, &songlength) != 0)
    {
      DPRINTF(E_LOG, L_MPD, "Error converting song length to uint32_t: %s\n", dbmfi->song_length);
      return -1;
    }

  ret = evbuffer_add_printf(evbuf,
      "file: %s\n"
      "Last-Modified: %s\n"
      "Time: %d\n"
      "duration: %.3f\n"
      "Artist: %s\n"
      "AlbumArtist: %s\n"
      "ArtistSort: %s\n"
      "AlbumArtistSort: %s\n"
      "Album: %s\n"
      "Title: %s\n"
      "Track: %s\n"
      "Date: %s\n"
      "Genre: %s\n"
      "Disc: %s\n",
      (dbmfi->virtual_path + 1),
      modified,
      (songlength / 1000),
      ((float) songlength / 1000),
      dbmfi->artist,
      dbmfi->album_artist,
      dbmfi->artist_sort,
      dbmfi->album_artist_sort,
      dbmfi->album,
      dbmfi->title,
      dbmfi->track,
      dbmfi->year,
      dbmfi->genre,
      dbmfi->disc);

  return ret;
}

static void
append_string(char **a, const char *b, const char *separator)
{
  char *temp;

  if (*a)
    temp = db_mprintf("%s%s%s", *a, (separator ? separator : ""), b);
  else
    temp = db_mprintf("%s", b);

  free(*a);
  *a = temp;
}

/*
 * Computes the absolute position of a relative position.  This is a
 * feature introduced since MPD 0.23 where + or - for position can be
 * used to indicate relative to the currently selected (playing/paused)
 * song.
 * This feature does the necessary lookups to resolve the current song
 * and calculate the absolute position.  When ptype is POSITION_ABSOLUTE
 * this function acts as a noop and simply returns position.
 */
static int
mpd_get_relative_queue_pos(enum position_type ptype, int position)
{
  struct player_status status;
  struct db_queue_item *queue_item;
  uint32_t curpos;

  /* shortcut absolute case */
  if (ptype == POSITION_ABSOLUTE)
    return position;

  player_get_status(&status);

  curpos = 0;
  if (status.status != PLAY_STOPPED)
    {
      queue_item = db_queue_fetch_byitemid(status.item_id);
      if (queue_item != NULL)
  	{
  	  if (queue_item->id > 0)
    	    curpos = queue_item->pos;

  	  free_queue_item(queue_item, 0);
  	}
    }

  /* +0 inserts right after the current song */
  if (ptype == POSITION_RELATIVE_AFTER)
    position = curpos + position + 1;
  else if (ptype == POSITION_RELATIVE_BEFORE)
    position = curpos - position;

  DPRINTF(E_DBG, L_MPD,
      	  "current song: %d->%d, relative new position: %d\n",
      	  status.item_id, curpos, position);

  return position;
}

struct mpd_cmd_params {
    int params_allow;
    int params_set;
    struct query_params qp;
    struct mpd_tagtype **groups;
    int groupssize;
    int groupslen;
    bool addgroupfilter;
    bool exactmatch;
    int pos;
};

enum mpd_param_cmd {
    CMD_UNSET    = 0 << 0,
    CMD_WINDOW   = 1 << 0,
    CMD_GROUP    = 1 << 1,
    CMD_POSITION = 1 << 2,
    CMD_SORT     = 1 << 3,
    CMD_FILTER   = 1 << 4
};

/**
 * {START:END}
 *
 * parse START and END as integer numbers and store in query_params as
 * limit and offset
 */
static int
mpd_parse_cmd_window(char *arg, struct mpd_cmd_params *param)
{
  struct query_params *qp = &param->qp;
  int start_pos;
  int end_pos;
  int ret;

  ret = mpd_pars_range_arg(arg, &start_pos, &end_pos);
  if (ret == 0 && qp != NULL)
    {
      qp->idx_type = I_SUB;
      qp->limit = end_pos - start_pos;
      qp->offset = start_pos;

      param->params_set |= CMD_WINDOW;
    }
  else
    {
      DPRINTF(E_LOG, L_MPD,
	      "Window argument doesn't convert "
	      "to integer or range: '%s'\n", arg);
      return 1;
    }

  return 0;
}

/**
 * {GROUPTYPE}
 *
 * parse GROUPTYPE as tagtype (album, artist, etc) and store in groups
 * and increment groupslen.  It is the callers responsibility to ensure
 * groups is allocated and has sufficient space, else results are
 * silently dropped.  If addgroupfilter is requested, the group argument
 * will be appended to (with comma-space separation) for e.g. ORDER BY use.
 */
static int
mpd_parse_cmd_group(char *arg, struct mpd_cmd_params *param)
{
  struct query_params *qp = &param->qp;
  struct mpd_tagtype *tagtype = find_tagtype(arg);

  if (tagtype != NULL && tagtype->type != MPD_TYPE_SPECIAL)
    {
      if (param->addgroupfilter)
    	append_string(&qp->group, tagtype->group_field, ", ");

      /* caller should ensure sufficient memory was allocated */
      if (param->groupslen < param->groupssize)
      	{
      	  param->groups[param->groupslen] = tagtype;
      	  param->groupslen++;
      	}

      param->params_set |= CMD_GROUP;
    }

  return 0;
}

/**
 * {POSITION}
 *
 * parse POSITION as an integer number and store the result in pos from
 * mpd_cmd_params.  If POSITION starts with '+' or '-', the number
 * following the sign is considered relative to the current song.  As
 * such, its value is resolved and stored in pos instead.
 */
static int
mpd_parse_cmd_position(char *arg, struct mpd_cmd_params *param)
{
  enum position_type ptype = POSITION_ABSOLUTE;
  int to_pos;
  int ret;

  if (*arg == '-')
    {
      ptype = POSITION_RELATIVE_BEFORE;
      arg++;
    }
  else if (*arg == '+')
    {
      ptype = POSITION_RELATIVE_AFTER;
      arg++;
    }

  ret = safe_atoi32(arg, &to_pos);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MPD,
	      "Argument doesn't convert to integer: '%s'\n", arg);
      return 1;
    }
  else
    {
      param->pos = mpd_get_relative_queue_pos(ptype, to_pos);
      param->params_set |= CMD_POSITION;
    }

  return 0;
}

/**
 * {(TAG [OP] VALUE)}
 *
 * parse filter expression on THING being VALUE in relation to EXPR.
 * The possible expressions can be found at:
 * https://mpd.readthedocs.io/en/latest/protocol.html#filter-syntax
 * The result is stored in filter member from query_params, and appended
 * to create one compound SQL WHERE-condition.
 *
 * NOTE: this command differs from the others in that it isn't prefixed
 * by some tag to indicate what the type is and that there are
 * single-argument filters (as opposed to key/value), thus the filter
 * command is run for as long as no other known tag is found.
 *
 * The parsed input here comes from e.g. the find command:
 *   find "((album == \"Flash Gordon\"))"    (post v0.21)
 *   find album "Flash Gordon"               (<= v0.21)
 * and we deal with
 *   argv[1]: ((album == "Flash Gordon"))    (post v0.21)
 *   argv[1]: album  argv[2]: Flash Gordon   (<= v0.21)
 * here.
 * While the double ( ) is what's in the official docs, and seen with
 * some clients, others use just a single pair and single quotes (like
 * Maximum MPD).
 *
 * If narg is used, it is truncated to the empty string (*narg == '\0')
 */
static int
mpd_parse_cmd_filter(char *arg, char *narg, struct mpd_cmd_params *param)
{
  char *condition = NULL;
  bool exact_match = param->exactmatch;
  size_t len = 0;

  /* determine if we're using v0.21 syntax */
  if (arg[0] == '(' && (len = strlen(arg)) > 2 && arg[len - 1] == ')')
    {
      bool first = true;
      bool negate = false;
      char *p;
      char *q;
      char *val = NULL;
      char *argend = &arg[len - 1];
      struct mpd_tagtype *tagtype = NULL;
      enum parsestate {
      	  STATE_INIT,
      	  STATE_EXPR,
      	  STATE_FINI,
      	  STATE_OP,
      	  STATE_VAL
      } state = STATE_INIT;
      enum operator {  /* CI: case-insensitive, CS, case-sensitive */
      	  OP_NONE,
      	  OP_EQUALS,   /* order below matters for promotion to CI/CS */
      	  OP_EQUALS_CI,
      	  OP_EQUALS_CS,
      	  OP_NEQUALS,
      	  OP_NEQUALS_CI,
      	  OP_NEQUALS_CS,
      	  OP_CONTAINS,
      	  OP_CONTAINS_CI,
      	  OP_CONTAINS_CS,
      	  OP_NCONTAINS,
      	  OP_NCONTAINS_CI,
      	  OP_NCONTAINS_CS,
      	  OP_STARTSWITH,
      	  OP_STARTSWITH_CI,
      	  OP_STARTSWITH_CS,
      	  OP_NSTARTSWITH,
      	  OP_NSTARTSWITH_CI,
      	  OP_NSTARTSWITH_CS,
      	  OP_REGEX,
      	  OP_NREGEX,
      	  OP_GREQ
      } op = OP_NONE;

      /* ((TAG [OP] VALUE)) */
      /* the double parenthesis are used in just two cases:
       * - negation    (!(artist == "VAL"))
       * - conjunction ((artist == "FOO") AND (album == "BAR"))
       * this means we need to proper-parse the values, since we need to
       * know the closing parenthesis is real, and not inside the value
       * to possible parse another expression (via AND) */
      for (p = &arg[1]; p < argend; p++)
      	{
      	  DPRINTF(E_DBG, L_MPD, "state: %u, tagtype=%s, op=%u, val=%s\n",
      	      	  state, tagtype ? tagtype->tag : "?", op, val ? val : "?");
      	  switch (state)
      	    {
      	    case STATE_INIT:
      	      tagtype = NULL;
      	      op = OP_NONE;
      	      switch (*p)
      	    	{
      	    	case '!':
      	    	  negate = true;
      	    	  break;
      	    	case '(':
      	    	  state = STATE_EXPR;
      	    	  break;
      	    	default:
      	    	  if (first)
      	    	    {
      	    	      /* deal with clients that do a single expression
      	    	       * without the double pair of parenthesis, faking
      	    	       * the start and end parenthesis */
      	    	      p--;
      	    	      argend++;
      	    	      state = STATE_EXPR;
      	    	      break;
      	    	    }
      	    	  /* silently eat away garbage we don't grok */
      	    	  negate = false;
      	    	  break;
      	    	}
      	      break;
      	    case STATE_EXPR:
      	      /* TAG<space> -- hunt for the space, lookup tag */
      	      for (q = p; *q != ' ' && q < argend; q++)
      	      	;
      	      if (q == argend)
      	      	{
      	      	  state = STATE_INIT;
      	      	}
      	      else
      	      	{
      	      	  *q = '\0';
      	      	  tagtype = find_tagtype(p);
      	      	  if (tagtype == NULL)
	  	    {
	  	      DPRINTF(E_WARN, L_MPD,
	  	  	      "Tag '%s' is not supported, condition ignored\n",
	  	  	      p);
	  	      state = STATE_INIT;
	  	    }
	  	  else
	  	    {
	  	      if (strcmp(tagtype->tag, "base") == 0 ||
	  	      	  strcmp(tagtype->tag, "modified-since") == 0 ||
	  	      	  /* added-since: not supported (yet) */ false)
	  	      	{
	  	      	  /* these expressions somehow lack an operator,
	  	      	   * the meaning is special per tag */
	  	      	  op = OP_NONE;
	  	      	  state = STATE_VAL;
	  	      	}
	  	      else
	  	      	{
	  	      	  state = STATE_OP;
	  	      	}
	  	    }
      	      	  p = q;
      	      	}
      	      break;
      	    case STATE_OP:
      	      /* OP<space> -- hunt for the space */
      	      for (q = p; *q != ' ' && q < argend; q++)
      	      	;
      	      if (q == argend)
      	      	{
      	      	  state = STATE_INIT;
      	      	}
      	      else
      	      	{
      	      	  *q = '\0';
      	      	  if (strcmp(p, ">=") == 0)
      	      	    op = OP_GREQ;
      	      	  else if (strcmp(p, "==") == 0)
      	      	    op = OP_EQUALS;
      	      	  else if (strcmp(p, "!=") == 0)
      	      	    op = OP_NEQUALS;
      	      	  else if (strcmp(p, "eq_cs") == 0)
      	      	    op = OP_EQUALS_CS;
      	      	  else if (strcmp(p, "!eq_cs") == 0)
      	      	    op = OP_NEQUALS_CS;
      	      	  else if (strcmp(p, "eq_ci") == 0)
      	      	    op = OP_EQUALS_CI;
      	      	  else if (strcmp(p, "!eq_ci") == 0)
      	      	    op = OP_NEQUALS_CI;
      	      	  else if (strcmp(p, "=~") == 0)
      	      	    op = OP_REGEX;
      	      	  else if (strcmp(p, "!~") == 0)
      	      	    op = OP_NREGEX;
      	      	  else if (strcmp(p, "contains") == 0)
      	      	    op = OP_CONTAINS;
      	      	  else if (strcmp(p, "!contains") == 0)
      	      	    op = OP_NCONTAINS;
      	      	  else if (strcmp(p, "contains_cs") == 0)
      	      	    op = OP_CONTAINS_CS;
      	      	  else if (strcmp(p, "!contains_cs") == 0)
      	      	    op = OP_NCONTAINS_CS;
      	      	  else if (strcmp(p, "contains_ci") == 0)
      	      	    op = OP_CONTAINS_CI;
      	      	  else if (strcmp(p, "!contains_ci") == 0)
      	      	    op = OP_NCONTAINS_CI;
      	      	  else if (strcmp(p, "startswith") == 0)
      	      	    op = OP_STARTSWITH;
      	      	  else if (strcmp(p, "!startswith") == 0)
      	      	    op = OP_NSTARTSWITH;
      	      	  else if (strcmp(p, "startswith_cs") == 0)
      	      	    op = OP_STARTSWITH_CS;
      	      	  else if (strcmp(p, "!startswith_cs") == 0)
      	      	    op = OP_NSTARTSWITH_CS;
      	      	  else if (strcmp(p, "startswith_ci") == 0)
      	      	    op = OP_STARTSWITH_CI;
      	      	  else if (strcmp(p, "!startswith_ci") == 0)
      	      	    op = OP_NSTARTSWITH_CI;
      	      	  else
      	      	    {
	  	      DPRINTF(E_WARN, L_MPD,
	  	  	      "Operator '%s' is not supported, "
	  	  	      "condition ignored\n",
	  	  	      p);
      	      	      state = STATE_INIT;
      	      	      break;
      	      	    }

      	      	  /* exactmatch is actually "find" commands, which are
      	      	   * case-sensitive, the rest ignore case, promote the
      	      	   * non-explicit ones (v0.24)
      	      	   * further, historically search used strstr behaviour,
      	      	   * find strcmp, so promote equals to contains when
      	      	   * used with search */
      	      	  switch (op)
      	      	    {
      	      	    case OP_EQUALS:
      	      	    case OP_NEQUALS:
      	      	      /* don't promote equals when used on numbers */
      	      	      if (tagtype->type == MPD_TYPE_INT)
      	      	      	break;
      	      	      if (!exact_match)
      	      	      	op += 6;
      	      	    case OP_CONTAINS:
      	      	    case OP_NCONTAINS:
      	      	    case OP_STARTSWITH:
      	      	    case OP_NSTARTSWITH:
      	      	      op += exact_match ? 2 : 1;
      	      	      break;
      	      	    default:
      	      	      /* nothing to do */
      	      	      break;
      	      	    }

		  /* simplify handling in FINI */
      	      	  if (negate)
      	      	    {
      	      	      switch (op)
      	      	      	{
      	      	      	case OP_EQUALS:
      	      	      	case OP_EQUALS_CI:
      	      	      	case OP_EQUALS_CS:
      	      	      	case OP_CONTAINS_CI:
      	      	      	case OP_CONTAINS_CS:
      	      	      	case OP_STARTSWITH_CI:
      	      	      	case OP_STARTSWITH_CS:
      	      	      	  op += 3;  /* become NOT */
      	      	      	  break;
      	      	      	case OP_NEQUALS:
      	      	      	case OP_NEQUALS_CI:
      	      	      	case OP_NEQUALS_CS:
      	      	      	case OP_NCONTAINS:
      	      	      	case OP_NCONTAINS_CI:
      	      	      	case OP_NCONTAINS_CS:
      	      	      	case OP_NSTARTSWITH:
      	      	      	case OP_NSTARTSWITH_CI:
      	      	      	case OP_NSTARTSWITH_CS:
      	      	      	  op -= 3;  /* remove NOT */
      	      	      	  break;
      	      	      	default:
      	      	      	  /* nothing to do */
      	      	      	  break;
      	      	      	}
      	      	    }

      	      	  p = q;
      	      	  state = STATE_VAL;
      	      	}
      	      break;
      	    case STATE_VAL:
      	      switch (*p)
      	      	{
      	      	case '0':
      	      	case '1':
      	      	case '2':
      	      	case '3':
      	      	case '4':
      	      	case '5':
      	      	case '6':
      	      	case '7':
      	      	case '8':
      	      	case '9':
      	      	  /* VAL) -- hunt for the closing parenthesis */
      	      	  for (q = p; *q != ')' && q < argend; q++)
      	      	    ;
      	      	  if (q == argend)
      	      	    {
      	      	      state = STATE_INIT;
      	      	    }
      	      	  else
      	      	    {
      	  	      *q = '\0';
      	      	      val = p;
      	      	      state = STATE_FINI;
      	      	    }
      	      	  break;
      	      	case '"':
      	      	case '\'':
      	      	    {
      	      	      char *quote = p;
      	      	      for (q = ++p; q < argend; q++)
      	      	      	{
      	      	      	  if (*q == *quote)
      	      	      	    break;
      	      	      	  if (*q == '\\')
      	      	      	    *p++ = *++q;
      	      	      	  else
      	      	      	    *p++ = *q;
      	      	      	}
      	      	      if (q == argend)
      	      	      	{
      	      	      	  state = STATE_INIT;
      	      	      	}
      	      	      else
      	      	      	{
      	      	      	  *p = '\0';
      	      	      	  p = q;
      	      	      	  val = quote + 1;
      	      	      	  state = STATE_FINI;
      	      	      	}
      	      	    }
      	      	  break;
      	      	default:
	  	  DPRINTF(E_WARN, L_MPD,
	  	  	  "illegal value for expression: '%s'\n",
	  	  	  p);
      	      	  state = STATE_INIT;
      	      	  break;
      	      	}
      	      break;
      	    case STATE_FINI:
      	      	{
      	      	  char *sqlopstr;

      	      	  /* push out expression, take negate into account
      	      	   * recursing here for reuse would be nice, but there
      	      	   * are a bunch of subtle differences which make this
      	      	   * not as straightforward as it ought to be */

      	      	  switch (op)
      	      	    {
      	      	    case OP_GREQ:
      	      	      if (negate)
      	      	      	sqlopstr = "(%s < %u)";
      	      	      else
      	      	      	sqlopstr = "(%s >= %u)";
      	      	      break;
      	      	    case OP_EQUALS:
      	      	      sqlopstr = "(%s = %u)";
      	      	      break;
      	      	    case OP_NEQUALS:
      	      	      sqlopstr = "(%s != %u)";
      	      	      break;
      	      	    case OP_EQUALS_CI:
      	      	      sqlopstr = "(%s LIKE '%q')";
      	      	      break;
      	      	    case OP_NEQUALS_CI:
      	      	      sqlopstr = "(%s NOT LIKE '%q')";
      	      	      break;
      	      	    case OP_EQUALS_CS:
      	      	      sqlopstr = "(%s = '%q')";
      	      	      break;
      	      	    case OP_NEQUALS_CS:
      	      	      sqlopstr = "(%s != '%q')";
      	      	      break;
      	      	    case OP_CONTAINS_CI:
      	      	      sqlopstr = "(%s LIKE '%%%q%%')";
      	      	      break;
      	      	    case OP_NCONTAINS_CI:
      	      	      sqlopstr = "(%s NOT LIKE '%%%q%%')";
      	      	      break;
      	      	    case OP_CONTAINS_CS:
      	      	      sqlopstr = "(%s GLOB '*%q*')";
      	      	      break;
      	      	    case OP_NCONTAINS_CS:
      	      	      sqlopstr = "(%s NOT GLOB '*%q*')";
      	      	      break;
      	      	    case OP_STARTSWITH_CI:
      	      	      sqlopstr = "(%s LIKE '%q%%')";
    		      break;
      	      	    case OP_NSTARTSWITH_CI:
      	      	      sqlopstr = "(%s NOT LIKE '%q%%')";
    		      break;
      	      	    case OP_STARTSWITH_CS:
      	      	      sqlopstr = "(%s GLOB '%q*')";
    		      break;
      	      	    case OP_NSTARTSWITH_CS:
      	      	      sqlopstr = "(%s NOT GLOB '%q*')";
    		      break;
      	  	    case OP_REGEX:
      	      	      sqlopstr = "(%s REGEX '%q')";
      	      	      break;
      	  	    case OP_NREGEX:
      	      	      sqlopstr = "(NOT %s REGEX '%q')";
      	      	      break;
      	      	    default:
      	      	      sqlopstr = NULL;  /* invalid, cause crash */
      	      	      break;
      	      	    }

      		  if (tagtype->type == MPD_TYPE_STRING)
		    {
		      condition = db_mprintf(sqlopstr,
		      			     tagtype->field,
		      			     val);
		    }
      		  else if (tagtype->type == MPD_TYPE_INT)
		    {
	  	      uint32_t num;
	  	      int ret = safe_atou32(val, &num);
	  	      if (ret < 0)
	    		DPRINTF(E_WARN, L_MPD,
	    	    		"%s parameter '%s' is not an integer and "
	    	    		"will be ignored\n", tagtype->tag, val);
	  	      else
		      	condition = db_mprintf(sqlopstr,
		      			       tagtype->field,
		      			       num);
		    }
      		  else if (tagtype->type == MPD_TYPE_SPECIAL)
		    {
	  	      if (strcmp(tagtype->tag, "any") == 0)
	    		{
	    		  char *tmp;
	      		  /* this really is a hack, the documentation
	      		   * says it should check *all* tag types, not
	      		   * just these three */
	      		  condition = db_mprintf("(");
	      		  tmp = db_mprintf(sqlopstr, "f.artist", val);
	      		  append_string(&condition, tmp, NULL);
	      		  free(tmp);
	      		  tmp = db_mprintf(sqlopstr, "f.album", val);
	      		  append_string(&condition, tmp, " OR ");
	      		  free(tmp);
	      		  tmp = db_mprintf(sqlopstr, "f.title", val);
	      		  append_string(&condition, tmp, " OR ");
	      		  free(tmp);
	      		  append_string(&condition, ")", NULL);
	    		}
	  	      else if (strcmp(tagtype->tag, "file") == 0 ||
	  	      	       strcmp(tagtype->tag, "base") == 0)
	    		{
		      	  condition = db_mprintf(sqlopstr,
		      			     	 tagtype->field,
		      			     	 val);
	    		}
	  	      else if (strcmp(tagtype->tag, "modified-since") == 0)
	    		{
	      		  char *datefmt;

	      		  /* according to the mpd protocol specification
	      		   * the value can be a unix timestamp or ISO8601 */
	      		  if (strchr(val, '-') == NULL)
	      		    datefmt = "unixepoch";
	      		  else
	      		    datefmt = "utc";

	      		  condition =
			    db_mprintf("(f.time_modified > strftime('%%s', "
			   	       "datetime('%q', '%s')))", val, datefmt);
	    		}
	  	      else
	    		{
	      		  DPRINTF(E_WARN, L_MPD,
	      	      		  "Unknown special parameter '%s' "
	      	      		  "will be ignored\n",
	      	      		  tagtype->tag);
	    		}
		    }

  		  if (condition != NULL)
    		    {
      		      struct query_params *qp = &param->qp;

      		      append_string(&qp->filter, condition, " AND ");

      		      free(condition);
      		      condition = NULL;

      		      param->params_set |= CMD_FILTER;
    		    }

    		  if (*p == ')')
    		    p++;
    		  while (*p == ' ')
    		    p++;
    		  if (strcasecmp(p, "AND") == 0)
    		    p += 3;

      	      	  negate = false;
      	      	  state = STATE_INIT;
      	      	  break;
      	    	}
      	    }
      	  first = false;
      	}

      return 0;
    }
  else if (narg != NULL)
    {
      struct mpd_tagtype *tagtype = find_tagtype(arg);

      /* arg: TYPE, narg: VALUE */

      if (!tagtype)
	{
	  DPRINTF(E_WARN, L_MPD,
	  	  "Parameter '%s' is not supported and will be ignored\n",
	  	  arg);
	  return 1;
	}

      if (tagtype->type == MPD_TYPE_STRING)
	{
	  if (exact_match)
	    condition = db_mprintf("(%s = '%q')", tagtype->field, narg);
	  else
	    condition = db_mprintf("(%s LIKE '%%%q%%')", tagtype->field, narg);
	}
      else if (tagtype->type == MPD_TYPE_INT)
	{
	  uint32_t num;
	  int ret = safe_atou32(narg, &num);
	  if (ret < 0)
	    DPRINTF(E_WARN, L_MPD,
	    	    "%s parameter '%s' is not an integer and "
	    	    "will be ignored\n", tagtype->tag, narg);
	  else
	    condition = db_mprintf("(%s = %u)", tagtype->field, num);
	}
      else if (tagtype->type == MPD_TYPE_SPECIAL)
	{
	  if (strcasecmp(tagtype->tag, "any") == 0)
	    {
	      condition = db_mprintf("(f.artist LIKE '%%%q%%' OR "
	      			     " f.album  LIKE '%%%q%%' OR "
	      			     " f.title  LIKE '%%%q%%')",
	      			     narg, narg, narg);
	    }
	  else if (strcasecmp(tagtype->tag, "file") == 0)
	    {
	      if (exact_match)
		condition = db_mprintf("(f.virtual_path = '/%q')", narg);
	      else
		condition = db_mprintf("(f.virtual_path LIKE '%%%q%%')", narg);
	    }
	  else if (strcasecmp(tagtype->tag, "base") == 0)
	    {
	      condition = db_mprintf("(f.virtual_path LIKE '/%q%%')", narg);
	    }
	  else if (strcasecmp(tagtype->tag, "modified-since") == 0)
	    {
	      char *datefmt;

	      /* according to the mpd protocol specification the value
	       * can be a unix timestamp or ISO 8601 */
	      if (strchr(narg, '-') == NULL)
	      	datefmt = "unixepoch";
	      else
	      	datefmt = "utc";

	      condition =
		db_mprintf("(f.time_modified > strftime('%%s', "
			   "datetime('%q', '%s')))", narg, datefmt);
	    }
	  else
	    {
	      DPRINTF(E_WARN, L_MPD,
	      	      "Unknown special parameter '%s' will be ignored\n",
	      	      tagtype->tag);
	      return 1;
	    }
	}
      *narg = '\0';  /* flag value being used */
    }
  else
    {
      /* Special case: a single token is allowed if listing albums for
       * an artist */
      condition = db_mprintf("(f.album_artist = '%q')", narg);
    }

  if (condition != NULL)
    {
      struct query_params *qp = &param->qp;

      append_string(&qp->filter, condition, " AND ");

      free(condition);

      param->params_set |= CMD_FILTER;
    }

  return 0;
}

/**
 * Parse command arguments as instructed via param.  Populates param
 * with the found arguments.  The caller is expected to setup
 * param->params_allow to indicate what it expects to be parsed.  Any
 * parameter not matching are ignored.
 * NOTE: param is an in/out structure, config is read, parsed results
 * are stored in it.
 *
 * Examples of the commands that are processed are:
 * - playlistfind {FILTER} [sort {TYPE}] [window {START:END}]
 * - searchadd {FILTER} [sort {TYPE}] [window {START:END}] [position POS]
 * - searchcount {FILTER} [group {GROUPTYPE}]
 * In each of these, a call is made using argv positioned at FILTER to
 * this function, which then tries to handle any FILTER commands as long
 * as it doesn't find a tag like sort, window, position or group.
 * For instance, the searchadd call will have to be setup that
 * param->params_allow contains (CMD_FILTER | CMD_SORT | CMD_WINDOW |
 * CMD_POSITION).  Since sort, window and position are optional, after
 * the call param->params_set can be queried to check what commands were
 * found in the input in order to handle the arguments.
 */
static int
mpd_parse_cmd_params(int argc, char **argv, struct mpd_cmd_params *param)
{
  bool dofilters;
  enum mpd_param_cmd cmd;
  int ret = 0;
  int i;

  if (param == NULL)
    return 1;

  /* only do filter processing if requested */
  dofilters = param->params_allow & CMD_FILTER;

  /* loop over arguments, detecting parameters and process them
   * accordingly -- arguments prior known parameters are assumed to be
   * filter arguments */
  for (i = 0; i < argc; i += 2)
    {
      cmd = dofilters ? CMD_FILTER : CMD_UNSET;
      if (strcasecmp(argv[i], "window") == 0)
      	cmd = CMD_WINDOW;
      else if (strcasecmp(argv[i], "group") == 0)
      	cmd = CMD_GROUP;
      else if (strcasecmp(argv[i], "position") == 0)
      	cmd = CMD_POSITION;
      else if (strcasecmp(argv[i], "sort") == 0)
      	cmd = CMD_SORT;

      /* filters stop after the first command is seen */
      if (cmd != CMD_FILTER)
      	dofilters = false;

      /* ignore this command if not requested */
      if ((param->params_allow & cmd) == CMD_UNSET)
      	continue;

      /* currently all but filter commands need a single argument */
      if (cmd != CMD_FILTER && i + 1 >= argc)
      	{
	  DPRINTF(E_WARN, L_MPD,
	  	  "Missing mandatory argument to Parameter '%s'\n",
	  	  argv[i]);
	  /* be lenient, historically thus functionality ignored
	   * problems, possibly on purpose for forwards compatibility */
      	  ret = 1;
      	  break;
      	}

      switch (cmd)
      	{
      	case CMD_WINDOW:
      	  ret |= mpd_parse_cmd_window(argv[i + 1], param);
      	  break;
      	case CMD_GROUP:
      	  /* need to allocate space if we haven't, group command can be
      	   * repeated, so take worst case and assume all remaining
      	   * commands are repetitions */
      	  if (param->groups == NULL)
      	    {
      	      param->groupssize = (argc - i) / 2;
      	      CHECK_NULL(L_MPD,
      	      		 param->groups = calloc(param->groupssize,
      	  					sizeof(param->groups[0])));
      	    }
      	  ret |= mpd_parse_cmd_group(argv[i + 1], param);
      	  break;
      	case CMD_POSITION:
      	  ret |= mpd_parse_cmd_position(argv[i + 1], param);
      	  break;
      	case CMD_SORT:
      	  /* currently unhandled, ignore */
      	  break;
      	case CMD_FILTER:
      	    {
      	      char *nextarg = NULL;
      	      if (i + 1 < argc)
      	      	nextarg = argv[i + 1];
      	      ret |= mpd_parse_cmd_filter(argv[i], nextarg, param);
      	      if (nextarg != NULL && *nextarg != '\0')
      	      	i--;
      	      break;
      	    }
      	case CMD_UNSET:
      	  break;
      	}
    }

  return ret;
}

/*
 * Command handler function for 'currentsong'
 */
static int
mpd_command_currentsong(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{

  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  player_get_status(&status);

  if (status.status == PLAY_STOPPED)
    queue_item = db_queue_fetch_bypos(0, status.shuffle);
  else
    queue_item = db_queue_fetch_byitemid(status.item_id);

  if (!queue_item)
    {
      return 0;
    }

  ret = mpd_add_db_queue_item(evbuf, queue_item);

  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Error adding media info for file with id: %d", status.id);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

static int
mpd_notify_idle_client(struct mpd_client_ctx *client_ctx, short events);

/*
 * Example input:
 * idle "database" "mixer" "options" "output" "player" "playlist" "sticker" "update"
 */
static int
mpd_command_idle(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int i;

  ctx->idle_events = 0;
  ctx->is_idle = true;

  if (argc > 1)
    {
      for (i = 1; i < argc; i++)
	{
	  if (0 == strcmp(argv[i], "database"))
	    ctx->idle_events |= LISTENER_DATABASE;
	  else if (0 == strcmp(argv[i], "update"))
	    ctx->idle_events |= LISTENER_UPDATE;
	  else if (0 == strcmp(argv[i], "player"))
	    ctx->idle_events |= LISTENER_PLAYER;
	  else if (0 == strcmp(argv[i], "playlist"))
	    ctx->idle_events |= LISTENER_QUEUE;
	  else if (0 == strcmp(argv[i], "mixer"))
	    ctx->idle_events |= LISTENER_VOLUME;
	  else if (0 == strcmp(argv[i], "output"))
	    ctx->idle_events |= LISTENER_SPEAKER;
	  else if (0 == strcmp(argv[i], "options"))
	    ctx->idle_events |= LISTENER_OPTIONS;
	  else if (0 == strcmp(argv[i], "stored_playlist"))
	    ctx->idle_events |= LISTENER_STORED_PLAYLIST;
	  else if (0 == strcmp(argv[i], "sticker"))
            ctx->idle_events |= LISTENER_RATING;
	  else
	    DPRINTF(E_DBG, L_MPD, "Idle command for '%s' not supported\n", argv[i]);
	}
    }
  else
    ctx->idle_events = MPD_ALL_IDLE_LISTENER_EVENTS;

  // If events the client listens to occurred since the last idle call (or since the client connected,
  // if it is the first idle call), notify immediately.
  if (ctx->events & ctx->idle_events)
    mpd_notify_idle_client(ctx, ctx->events);

  return 0;
}

static int
mpd_command_noidle(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  /*
   * The protocol specifies: "The idle command can be canceled by
   * sending the command noidle (no other commands are allowed). MPD
   * will then leave idle mode and print results immediately; might be
   * empty at this time."
   */
  if (ctx->events)
    mpd_notify_idle_client(ctx, ctx->events);
  else
    evbuffer_add(ctx->evbuffer, "OK\n", 3);

  ctx->is_idle = false;
  return 0;
}

/*
 * Command handler function for 'status'
 *
 * Example output:
 *  volume: -1
 *  repeat: 0
 *  random: 0
 *  single: 0
 *  consume: 0
 *  playlist: 2
 *  playlistlength: 34
 *  mixrampdb: 0.000000
 *  state: stop
 *  song: 0
 *  songid: 1
 *  time: 28:306
 *  elapsed: 28.178
 *  bitrate: 278
 *  audio: 44100:f:2
 *  nextsong: 1
 *  nextsongid: 2
 */
static int
mpd_command_status(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct player_status status;
  uint32_t queue_length = 0;
  int queue_version = 0;
  char *state;
  uint32_t itemid = 0;
  struct db_queue_item *queue_item;

  player_get_status(&status);

  switch (status.status)
    {
      case PLAY_PAUSED:
	state = "pause";
	break;

      case PLAY_PLAYING:
	state = "play";
	break;

      default:
	state = "stop";
	break;
    }

  db_admin_getint(&queue_version, DB_ADMIN_QUEUE_VERSION);
  db_queue_get_count(&queue_length);

  evbuffer_add_printf(evbuf,
      "volume: %d\n"
      "repeat: %d\n"
      "random: %d\n"
      "single: %d\n"
      "consume: %d\n"
      "playlist: %d\n"
      "playlistlength: %d\n"
      "mixrampdb: 0.000000\n"
      "state: %s\n",
      status.volume,
      (status.repeat == REPEAT_OFF ? 0 : 1),
      status.shuffle,
      (status.repeat == REPEAT_SONG ? 1 : 0),
      status.consume,
      queue_version,
      queue_length,
      state);

  if (status.status != PLAY_STOPPED)
    queue_item = db_queue_fetch_byitemid(status.item_id);
  else
    queue_item = db_queue_fetch_bypos(0, status.shuffle);

  if (queue_item)
   {
      evbuffer_add_printf(evbuf,
	  "song: %d\n"
	  "songid: %d\n",
	  queue_item->pos,
	  queue_item->id);

      itemid = queue_item->id;
      free_queue_item(queue_item, 0);
   }

  if (status.status != PLAY_STOPPED)
   {
      evbuffer_add_printf(evbuf,
	  "time: %d:%d\n"
	  "elapsed: %#.3f\n"
	  "bitrate: 128\n"
	  "audio: 44100:16:2\n",
	  (status.pos_ms / 1000), (status.len_ms / 1000),
	  (status.pos_ms / 1000.0));
   }

  if (library_is_scanning())
    {
      evbuffer_add(evbuf, "updating_db: 1\n", 15);
    }

  if (itemid > 0)
    {
      queue_item = db_queue_fetch_next(itemid, status.shuffle);
      if (queue_item)
	{
	  evbuffer_add_printf(evbuf,
	      "nextsong: %d\n"
	      "nextsongid: %d\n",
	      queue_item->pos,
	      queue_item->id);

	  free_queue_item(queue_item, 0);
	}
    }

  return 0;
}

/*
 * Command handler function for 'stats'
 */
static int
mpd_command_stats(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params qp;
  struct filecount_info fci;
  double uptime;
  int64_t db_start = 0;
  int64_t db_update = 0;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_COUNT_ITEMS;

  ret = db_filecount_get(&fci, &qp);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  db_admin_getint64(&db_start, DB_ADMIN_START_TIME);
  uptime = difftime(time(NULL), (time_t) db_start);
  db_admin_getint64(&db_update, DB_ADMIN_DB_UPDATE);

  //TODO [mpd] Implement missing stats attributes (playtime)
  evbuffer_add_printf(evbuf,
      "artists: %d\n"
      "albums: %d\n"
      "songs: %d\n"
      "uptime: %.f\n" //in seceonds
      "db_playtime: %" PRIi64 "\n"
      "db_update: %" PRIi64 "\n"
      "playtime: %d\n",
      fci.artist_count,
      fci.album_count,
      fci.count,
      uptime,
      (fci.length / 1000),
      db_update,
      7);

  return 0;
}

/*
 * Command handler function for 'consume'
 * Sets the consume mode, expects argument argv[1] to be an integer with
 *   0 = disable consume
 *   1 = enable consume
 */
static int
mpd_command_consume(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int enable;
  int ret;

  ret = safe_atoi32(argv[1], &enable);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  player_consume_set(enable);
  return 0;
}

/*
 * Command handler function for 'random'
 * Sets the shuffle mode, expects argument argv[1] to be an integer with
 *   0 = disable shuffle
 *   1 = enable shuffle
 */
static int
mpd_command_random(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int enable;
  int ret;

  ret = safe_atoi32(argv[1], &enable);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  player_shuffle_set(enable);
  return 0;
}

/*
 * Command handler function for 'repeat'
 * Sets the repeat mode, expects argument argv[1] to be an integer with
 *   0 = repeat off
 *   1 = repeat all
 */
static int
mpd_command_repeat(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int enable;
  int ret;

  ret = safe_atoi32(argv[1], &enable);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  if (enable == 0)
    player_repeat_set(REPEAT_OFF);
  else
    player_repeat_set(REPEAT_ALL);

  return 0;
}

/*
 * Command handler function for 'setvol'
 * Sets the volume, expects argument argv[1] to be an integer 0-100
 */
static int
mpd_command_setvol(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int volume;
  int ret;

  ret = safe_atoi32(argv[1], &volume);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  player_volume_set(volume);

  return 0;
}

/*
 * Command handler function for 'single'
 * Sets the repeat mode, expects argument argv[1] to be an integer or
 * "oneshot" for 0.21 protocol.
 * The server only allows single-mode in combination with repeat, therefore
 * the command single translates (depending on the current repeat mode) into:
 * a) if repeat off:
 *   0 = repeat off
 *   1 = repeat song
 * b) if repeat all:
 *   0 = repeat all
 *   1 = repeat song
 * c) if repeat song:
 *   0 = repeat all
 *   1 = repeat song
 * Thus "oneshot" is accepted, but ignored under all circumstances.
 */
static int
mpd_command_single(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int enable;
  struct player_status status;
  int ret;

  ret = safe_atoi32(argv[1], &enable);
  if (ret < 0)
    {
      /* 0.21 protocol: accept "oneshot" mode */
      if (strcmp(argv[1], "oneshot") == 0)
      	return 0;
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  player_get_status(&status);

  if (enable == 0 && status.repeat != REPEAT_OFF)
    player_repeat_set(REPEAT_ALL);
  else if (enable == 0)
    player_repeat_set(REPEAT_OFF);
  else
    player_repeat_set(REPEAT_SONG);

  return 0;
}

/*
 * Command handler function for 'replay_gain_status'
 * The server does not support replay gain, therefor this function returns always
 * "replay_gain_mode: off".
 */
static int
mpd_command_replay_gain_status(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  evbuffer_add(evbuf, "replay_gain_mode: off\n", 22);
  return 0;
}

/*
 * Command handler function for 'volume'
 * Changes the volume by the given amount, expects argument argv[1] to be an integer
 *
 * According to the mpd protocoll specification this function is deprecated.
 */
static int
mpd_command_volume(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct player_status status;
  int volume;
  int ret;

  ret = safe_atoi32(argv[1], &volume);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  player_get_status(&status);

  volume += status.volume;

  player_volume_set(volume);

  return 0;
}

/*
 * Command handler function for 'next'
 * Skips to the next song in the playqueue
 */
static int
mpd_command_next(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int ret;

  ret = player_playback_next();

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to skip to next song");
      return ACK_ERROR_UNKNOWN;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Player returned an error for start after nextitem");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'pause'
 * Toggles pause/play, if the optional argument argv[1] is present, it must be an integer with
 *   0 = play
 *   1 = pause
 */
static int
mpd_command_pause(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int pause = -1;
  struct player_status status;
  int ret;

  if (argc > 1)
    {
      ret = safe_atoi32(argv[1], &pause);
      if (ret < 0 || pause > 1 || pause < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert "
	  			  "to integer or has unsupported value: '%s'",
	  			  argv[1]);
	  return ACK_ERROR_ARG;
	}
    }

  /* ignore pause when in stopped state or when explicit request matches
   * current state, like MPD */
  player_get_status(&status);
  if (status.status == PLAY_PAUSED && pause <= 0)
    ret = player_playback_start();
  else if (status.status == PLAY_PLAYING && (pause < 0 || pause == 1))
    ret = player_playback_pause();
  else
    ret = 0;

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to pause/resume playback");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'play'
 * Starts playback, the optional argument argv[1] represents the position in the playqueue
 * where to start playback.
 */
static int
mpd_command_play(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int songpos;
  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  songpos = -1;
  if (argc > 1)
    {
      ret = safe_atoi32(argv[1], &songpos);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
	  return ACK_ERROR_ARG;
	}
    }

  player_get_status(&status);

  if (status.status == PLAY_PLAYING && songpos < 0)
    {
      DPRINTF(E_DBG, L_MPD, "Ignoring play command with parameter '%s', player is already playing.\n", argv[1]);
      return 0;
    }

  if (status.status == PLAY_PLAYING)
    {
      // Stop playback, if player is already playing and a valid song position is given (it will be restarted for the given song position)
      player_playback_stop();
    }

  if (songpos > 0)
    {
      queue_item = db_queue_fetch_bypos(songpos, 0);
      if (!queue_item)
	{
	  *errmsg = safe_asprintf("Failed to start playback");
	  return ACK_ERROR_UNKNOWN;
	}

      ret = player_playback_start_byitem(queue_item);
      free_queue_item(queue_item, 0);
    }
  else
    ret = player_playback_start();

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to start playback");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'playid'
 * Starts playback, the optional argument argv[1] represents the songid of the song
 * where to start playback.
 */
static int
mpd_command_playid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  uint32_t id;
  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  player_get_status(&status);

  id = 0;
  if (argc > 1)
    {
      //TODO [mpd] mpd allows passing "-1" as argument and simply ignores it, the server fails to convert "-1" to an unsigned int
      ret = safe_atou32(argv[1], &id);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
	  return ACK_ERROR_ARG;
	}
    }

  if (status.status == PLAY_PLAYING)
    {
      // Stop playback, if player is already playing and a valid item id is given (it will be restarted for the given song)
      player_playback_stop();
    }

  if (id > 0)
    {
      queue_item = db_queue_fetch_byitemid(id);
      if (!queue_item)
	{
	  *errmsg = safe_asprintf("Failed to start playback");
	  return ACK_ERROR_UNKNOWN;
	}

      ret = player_playback_start_byitem(queue_item);
      free_queue_item(queue_item, 0);
    }
  else
    ret = player_playback_start();

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to start playback");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'previous'
 * Skips to the previous song in the playqueue
 */
static int
mpd_command_previous(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int ret;

  ret = player_playback_prev();

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to skip to previous song");
      return ACK_ERROR_UNKNOWN;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Player returned an error for start after previtem");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'seekid'
 * Seeks to song at the given position in argv[1] to the position in seconds given in argument argv[2]
 * (fractions allowed).
 */
static int
mpd_command_seek(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  uint32_t songpos;
  float seek_target_sec;
  int seek_target_msec;
  int ret;

  ret = safe_atou32(argv[1], &songpos);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  //TODO Allow seeking in songs not currently playing

  seek_target_sec = strtof(argv[2], NULL);
  seek_target_msec = seek_target_sec * 1000;

  ret = player_playback_seek(seek_target_msec, PLAYER_SEEK_POSITION);

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to seek current song to time %d msec", seek_target_msec);
      return ACK_ERROR_UNKNOWN;
    }

  ret = player_playback_start();
    if (ret < 0)
      {
	*errmsg = safe_asprintf("Player returned an error for start after seekcur");
	return ACK_ERROR_UNKNOWN;
      }

  return 0;
}

/*
 * Command handler function for 'seekid'
 * Seeks to song with id given in argv[1] to the position in seconds given in argument argv[2]
 * (fractions allowed).
 */
static int
mpd_command_seekid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct player_status status;
  uint32_t id;
  float seek_target_sec;
  int seek_target_msec;
  int ret;

  ret = safe_atou32(argv[1], &id);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  //TODO Allow seeking in songs not currently playing
  player_get_status(&status);
  if (status.item_id != id)
    {
      *errmsg = safe_asprintf("Given song is not the current playing one, seeking is not supported");
      return ACK_ERROR_UNKNOWN;
    }

  seek_target_sec = strtof(argv[2], NULL);
  seek_target_msec = seek_target_sec * 1000;

  ret = player_playback_seek(seek_target_msec, PLAYER_SEEK_POSITION);

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to seek current song to time %d msec", seek_target_msec);
      return ACK_ERROR_UNKNOWN;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Player returned an error for start after seekcur");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'seekcur'
 * Seeks the current song to the position in seconds given in argument argv[1] (fractions allowed).
 */
static int
mpd_command_seekcur(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  float seek_target_sec;
  int seek_target_msec;
  int ret;

  seek_target_sec = strtof(argv[1], NULL);
  seek_target_msec = seek_target_sec * 1000;

  // TODO If prefixed by '+' or '-', then the time is relative to the current playing position.
  ret = player_playback_seek(seek_target_msec, PLAYER_SEEK_POSITION);

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to seek current song to time %d msec", seek_target_msec);
      return ACK_ERROR_UNKNOWN;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Player returned an error for start after seekcur");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'stop'
 * Stop playback.
 */
static int
mpd_command_stop(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int ret;

  ret = player_playback_stop();

  if (ret != 0)
    {
      *errmsg = safe_asprintf("Failed to stop playback");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Add media file item with given virtual path to the queue
 *
 * @param path The virtual path
 * @param exact_match If TRUE add only item with exact match, otherwise add all items virtual path start with the given path
 * @return The queue item id of the last inserted item or -1 on failure
 */
static int
mpd_queue_add(char *path, bool exact_match, int position)
{
  struct query_params qp;
  struct player_status status;
  int new_item_id;
  int ret;

  new_item_id = 0;
  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_ITEMS;
  qp.idx_type = I_NONE;
  qp.sort = S_ARTIST;

  if (exact_match)
    qp.filter = db_mprintf("f.disabled = 0 AND f.virtual_path LIKE '/%q'", path);
  else
    qp.filter = db_mprintf("f.disabled = 0 AND f.virtual_path LIKE '/%q%%'", path);

  if (!qp.filter)
    {
      DPRINTF(E_DBG, L_PLAYER, "Out of memory\n");
      return -1;
    }

  player_get_status(&status);

  ret = db_queue_add_by_query(&qp, status.shuffle, status.item_id, position, NULL, &new_item_id);

  free(qp.filter);

  if (ret == 0)
    return new_item_id;

  return ret;
}

/*
 * Command handler function for 'add'
 * Adds the all songs under the given path to the end of the playqueue (directories add recursively).
 * Expects argument argv[1] to be a path to a single file or directory.
 */
static int
mpd_command_add(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct player_status status;
  int ret;
  int pos = -1;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing arguments to command add");
      return ACK_ERROR_ARG;
    }

  /* 0.23.3: POSITION argument */
  if (argc >= 3)
    {
      struct mpd_cmd_params param;

      memset(&param, 0, sizeof(param));

      if (mpd_parse_cmd_position(argv[2], &param) != 0)
      	{
      	  *errmsg = safe_asprintf("Could not parse POSITION '%s'", argv[2]);
      	  return ACK_ERROR_ARG;
      	}

      pos = param.pos;
    }

  ret = mpd_queue_add(argv[1], false, pos);

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to add song '%s' to playlist", argv[1]);
      return ACK_ERROR_UNKNOWN;
    }

  if (ret == 0)
    {
      player_get_status(&status);

      // Given path is not in the library, check if it is possible to add as a non-library queue item
      ret = library_queue_item_add(argv[1], pos, status.shuffle, status.item_id, NULL, NULL);
      if (ret != LIBRARY_OK)
	{
	  *errmsg = safe_asprintf("Failed to add song '%s' to playlist (unkown path)", argv[1]);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  return 0;
}

/*
 * Command handler function for 'addid'
 * Adds the song under the given path to the end or to the given position of the playqueue.
 * Expects argument argv[1] to be a path to a single file. argv[2] is optional, if present
 * it must be an integer representing the position in the playqueue.  If
 * the parameter starts with + or -, it is relative to the current song,
 * with +0 being right after the current song, and -0 before the current
 * song.
 */
static int
mpd_command_addid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct player_status status;
  int to_pos = -1;
  int ret;

  if (argc > 2)
    {
      struct mpd_cmd_params param;

      memset(&param, 0, sizeof(param));

      if (mpd_parse_cmd_position(argv[2], &param) != 0)
      	{
      	  *errmsg = safe_asprintf("Could not parse POSITION '%s'", argv[2]);
      	  return ACK_ERROR_ARG;
      	}

      to_pos = param.pos;
    }

  ret = mpd_queue_add(argv[1], true, to_pos);

  if (ret == 0)
    {
      player_get_status(&status);

      // Given path is not in the library, directly add it as a new queue item
      ret = library_queue_item_add(argv[1], to_pos, status.shuffle, status.item_id, NULL, NULL);
      if (ret != LIBRARY_OK)
	{
	  *errmsg = safe_asprintf("Failed to add song '%s' to playlist (unknown path)", argv[1]);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to add song '%s' to playlist", argv[1]);
      return ACK_ERROR_UNKNOWN;
    }

  evbuffer_add_printf(evbuf,
      "Id: %d\n",
      ret); // mpd_queue_add returns the item_id of the last inserted queue item

  return 0;
}

/*
 * Command handler function for 'clear'
 * Stops playback and removes all songs from the playqueue
 */
static int
mpd_command_clear(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int ret;

  ret = player_playback_stop();
  if (ret != 0)
    {
      DPRINTF(E_DBG, L_MPD, "Failed to stop playback\n");
    }

  db_queue_clear(0);

  return 0;
}

/*
 * Command handler function for 'delete'
 * Removes songs from the playqueue. Expects argument argv[1] (optional) to be an integer or
 * an integer range {START:END} representing the position of the songs in the playlist, that
 * should be removed.
 */
static int
mpd_command_delete(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int start_pos;
  int end_pos;
  int count;
  int ret;

  // If argv[1] is ommited clear the whole queue
  if (argc < 2)
    {
      db_queue_clear(0);
      return 0;
    }

  // If argument argv[1] is present remove only the specified songs
  ret = mpd_pars_range_arg(argv[1], &start_pos, &end_pos);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer or range: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  count = end_pos - start_pos;

  ret = db_queue_delete_bypos(start_pos, count);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to remove %d songs starting at position %d", count, start_pos);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'deleteid'
 * Removes the song with given id from the playqueue. Expects argument argv[1] to be an integer (song id).
 */
static int
mpd_command_deleteid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  uint32_t songid;
  int ret;

  ret = safe_atou32(argv[1], &songid);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  ret = db_queue_delete_byitemid(songid);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to remove song with id '%s'", argv[1]);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

//Moves the song at FROM or range of songs at START:END to TO in the playlist.
static int
mpd_command_move(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int start_pos;
  int end_pos;
  int count;
  int ret;
  struct mpd_cmd_params param;

  memset(&param, 0, sizeof(param));

  ret = mpd_pars_range_arg(argv[1], &start_pos, &end_pos);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer or range: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  count = end_pos - start_pos;

  if (mpd_parse_cmd_position(argv[2], &param) != 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[2]);
      return ACK_ERROR_ARG;
    }

  if (start_pos <= param.pos && end_pos >= param.pos)
    {
      *errmsg = safe_asprintf("Range overlaps with destination: %d-%d -> %d",
      			      start_pos, end_pos, param.pos);
      return ACK_ERROR_ARG;
    }

  while (count-- >= 0)
    {
      DPRINTF(E_WARN, L_MPD, "moving %d -> %d\n", start_pos, param.pos);
      ret = db_queue_move_bypos(start_pos, param.pos);
      if (ret < 0)
      	{
      	  *errmsg = safe_asprintf("Failed to move song at position "
      	  			  "%d to %d", start_pos, param.pos);
      	  return ACK_ERROR_UNKNOWN;
      	}
    }

  return 0;
}

static int
mpd_command_moveid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  uint32_t songid;
  int ret;
  struct mpd_cmd_params param;

  memset(&param, 0, sizeof(param));

  ret = safe_atou32(argv[1], &songid);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  if (mpd_parse_cmd_position(argv[2], &param) != 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[2]);
      return ACK_ERROR_ARG;
    }

  ret = db_queue_move_byitemid(songid, param.pos, 0);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to move song with id '%s' to index '%s'", argv[1], argv[2]);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'playlistid'
 * Displays a list of all songs in the queue, or if the optional argument is given, displays information
 * only for the song with ID.
 *
 * The order of the songs is always the not shuffled order.
 */
static int
mpd_command_playlistid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params query_params;
  struct db_queue_item queue_item;
  uint32_t songid;
  int ret;

  songid = 0;

  if (argc > 1)
    {
      ret = safe_atou32(argv[1], &songid);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
	  return ACK_ERROR_ARG;
	}
    }

  memset(&query_params, 0, sizeof(struct query_params));

  if (songid > 0)
    query_params.filter = db_mprintf("id = %d", songid);

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    {
      free(query_params.filter);
      *errmsg = safe_asprintf("Failed to start queue enum for command playlistid: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      ret = mpd_add_db_queue_item(evbuf, &queue_item);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Error adding media info for file with id: %d", queue_item.file_id);

	  db_queue_enum_end(&query_params);
	  free(query_params.filter);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  db_queue_enum_end(&query_params);
  free(query_params.filter);

  return 0;
}


/*
 * Command handler function for 'playlistinfo'
 * Displays a list of all songs in the queue, or if the optional argument is given, displays information
 * only for the song SONGPOS or the range of songs START:END given in argv[1].
 *
 * The order of the songs is always the not shuffled order.
 */
static int
mpd_command_playlistinfo(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params query_params;
  struct db_queue_item queue_item;
  int start_pos;
  int end_pos;
  int ret;

  start_pos = 0;
  end_pos = 0;
  memset(&query_params, 0, sizeof(struct query_params));

  if (argc > 1)
    {
      ret = mpd_pars_range_arg(argv[1], &start_pos, &end_pos);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert to integer or range: '%s'", argv[1]);
	  return ACK_ERROR_ARG;
	}

      if (start_pos < 0)
	DPRINTF(E_DBG, L_MPD, "Command 'playlistinfo' called with pos < 0 (arg = '%s'), ignore arguments and return whole queue\n", argv[1]);
      else
	query_params.filter = db_mprintf("pos >= %d AND pos < %d", start_pos, end_pos);
    }

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    {
      free(query_params.filter);
      *errmsg = safe_asprintf("Failed to start queue enum for command playlistinfo: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      ret = mpd_add_db_queue_item(evbuf, &queue_item);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Error adding media info for file with id: %d", queue_item.file_id);

	  db_queue_enum_end(&query_params);
	  free(query_params.filter);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  db_queue_enum_end(&query_params);
  free(query_params.filter);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-playlistfind */
static int
mpd_command_playlistfind(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *query_params;
  struct db_queue_item queue_item;
  int ret;

  if (argc < 3 || ((argc - 1) % 2) != 0)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'playlistfind'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  params.exactmatch = true;
  query_params = &params.qp;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  ret = db_queue_enum_start(query_params);
  if (ret < 0)
    {
      free(query_params->filter);
      *errmsg = safe_asprintf("Failed to start queue enum for command playlistinfo: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  while ((ret = db_queue_enum_fetch(query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      ret = mpd_add_db_queue_item(evbuf, &queue_item);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Error adding media info for file with id: %d", queue_item.file_id);

	  db_queue_enum_end(query_params);
	  free(query_params->filter);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  db_queue_enum_end(query_params);
  free(query_params->filter);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-playlistsearch */
static int
mpd_command_playlistsearch(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *query_params;
  struct db_queue_item queue_item;
  int ret;

  if (argc < 3 || ((argc - 1) % 2) != 0)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'playlistfind'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  query_params = &params.qp;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  ret = db_queue_enum_start(query_params);
  if (ret < 0)
    {
      free(query_params->filter);
      *errmsg = safe_asprintf("Failed to start queue enum for command playlistinfo: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  while ((ret = db_queue_enum_fetch(query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      ret = mpd_add_db_queue_item(evbuf, &queue_item);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Error adding media info for file with id: %d", queue_item.file_id);

	  db_queue_enum_end(query_params);
	  free(query_params->filter);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  db_queue_enum_end(query_params);
  free(query_params->filter);

  return 0;
}

static int
plchanges_build_queryparams(struct query_params *query_params, int argc, char **argv, char **errmsg)
{
  uint32_t version;
  int start_pos;
  int end_pos;
  int ret;

  memset(query_params, 0, sizeof(struct query_params));

  ret = safe_atou32(argv[1], &version);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  start_pos = 0;
  end_pos = 0;
  if (argc > 2)
    {
      ret = mpd_pars_range_arg(argv[2], &start_pos, &end_pos);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Argument doesn't convert to integer or range: '%s'", argv[2]);
	  return ACK_ERROR_ARG;
	}

      if (start_pos < 0)
	DPRINTF(E_DBG, L_MPD, "Command 'playlistinfo' called with pos < 0 (arg = '%s'), ignore arguments and return whole queue\n", argv[1]);
    }

  if (start_pos < 0 || end_pos <= 0)
    query_params->filter = db_mprintf("(queue_version > %d)", version);
  else
    query_params->filter = db_mprintf("(queue_version > %d AND pos >= %d AND pos < %d)", version, start_pos, end_pos);

  return 0;
}

/*
 * Command handler function for 'plchanges'
 * Lists all changed songs in the queue since the given playlist version in argv[1].
 */
static int
mpd_command_plchanges(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params query_params;
  struct db_queue_item queue_item;
  int ret;

  ret = plchanges_build_queryparams(&query_params, argc, argv, errmsg);
  if (ret != 0)
    return ret;

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      ret = mpd_add_db_queue_item(evbuf, &queue_item);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error adding media info for file with id: %d", queue_item.file_id);
	  goto error;
	}
    }

  db_queue_enum_end(&query_params);
  free_query_params(&query_params, 1);

  return 0;

 error:
  db_queue_enum_end(&query_params);
  free_query_params(&query_params, 1);
  *errmsg = safe_asprintf("Failed to start queue enum for command plchanges");
  return ACK_ERROR_UNKNOWN;
}

/*
 * Command handler function for 'plchangesposid'
 * Lists all changed songs in the queue since the given playlist version in argv[1] without metadata.
 */
static int
mpd_command_plchangesposid(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params query_params;
  struct db_queue_item queue_item;
  int ret;

  ret = plchanges_build_queryparams(&query_params, argc, argv, errmsg);
  if (ret != 0)
    return ret;

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      evbuffer_add_printf(evbuf,
      	  "cpos: %d\n"
      	  "Id: %d\n",
      	  queue_item.pos,
	  queue_item.id);
    }

  db_queue_enum_end(&query_params);
  free_query_params(&query_params, 1);

  return 0;

 error:
  db_queue_enum_end(&query_params);
  free_query_params(&query_params, 1);
  *errmsg = safe_asprintf("Failed to start queue enum for command plchangesposid");
  return ACK_ERROR_UNKNOWN;
}

/*
 * Command handler function for 'listplaylist'
 * Lists all songs in the playlist given by virtual-path in argv[1].
 */
static int
mpd_command_listplaylist(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *path;
  struct playlist_info *pli;
  struct db_media_file_info dbmfi;
  struct mpd_cmd_params param;
  int ret;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument for listplaylist");
      return ACK_ERROR_ARG;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      path = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      path = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  pli = db_pl_fetch_byvirtualpath(path);
  free(path);
  if (!pli)
    {
      *errmsg = safe_asprintf("Playlist not found for path '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  memset(&param, 0, sizeof(param));

  param.qp.type = Q_PLITEMS;
  param.qp.idx_type = I_NONE;
  param.qp.id = pli->id;

  if (argc >= 3)
    mpd_parse_cmd_window(argv[2], &param);

  ret = db_query_start(&param.qp);
  if (ret < 0)
    {
      db_query_end(&param.qp);

      free_pli(pli, 0);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while ((ret = db_query_fetch_file(&dbmfi, &param.qp)) == 0)
    {
      evbuffer_add_printf(evbuf,
	  "file: %s\n",
	  (dbmfi.virtual_path + 1));
    }

  db_query_end(&param.qp);

  free_pli(pli, 0);

  return 0;
}

/*
 * Command handler function for 'listplaylistinfo'
 * Lists all songs in the playlist given by virtual-path in argv[1] with metadata.
 */
static int
mpd_command_listplaylistinfo(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *path;
  struct playlist_info *pli;
  struct mpd_cmd_params param;
  struct db_media_file_info dbmfi;
  int ret;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument for listplaylistinfo");
      return ACK_ERROR_ARG;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      path = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      path = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  pli = db_pl_fetch_byvirtualpath(path);
  free(path);
  if (!pli)
    {
      *errmsg = safe_asprintf("Playlist not found for path '%s'", argv[1]);
      return ACK_ERROR_NO_EXIST;
    }

  memset(&param, 0, sizeof(param));

  param.qp.type = Q_PLITEMS;
  param.qp.idx_type = I_NONE;
  param.qp.id = pli->id;

  if (argc >= 3)
    mpd_parse_cmd_window(argv[2], &param);

  ret = db_query_start(&param.qp);
  if (ret < 0)
    {
      db_query_end(&param.qp);

      free_pli(pli, 0);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while ((ret = db_query_fetch_file(&dbmfi, &param.qp)) == 0)
    {
      ret = mpd_add_db_media_file_info(evbuf, &dbmfi);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error adding song to the evbuffer, song id: %s\n", dbmfi.id);
	}
    }

  db_query_end(&param.qp);

  free_pli(pli, 0);

  return 0;
}

/*
 * Command handler function for 'listplaylists'
 * Lists all playlists with their last modified date.
 */
static int
mpd_command_listplaylists(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct query_params qp;
  struct db_playlist_info dbpli;
  char modified[32];
  uint32_t time_modified;
  int ret;

  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_PL;
  qp.sort = S_PLAYLIST;
  qp.idx_type = I_NONE;
  qp.filter = db_mprintf("(f.type = %d OR f.type = %d)", PL_PLAIN, PL_SMART);

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      db_query_end(&qp);
      free(qp.filter);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while (((ret = db_query_fetch_pl(&dbpli, &qp)) == 0) && (dbpli.id))
    {
      if (safe_atou32(dbpli.db_timestamp, &time_modified) != 0)
	{
	  *errmsg = safe_asprintf("Error converting time modified to uint32_t: %s\n", dbpli.db_timestamp);
	  db_query_end(&qp);
	  free(qp.filter);
	  return ACK_ERROR_UNKNOWN;
	}

      mpd_time(modified, sizeof(modified), time_modified);

      evbuffer_add_printf(evbuf,
	  "playlist: %s\n"
	  "Last-Modified: %s\n"
	  "added: -1\n",  /* MPD v0.24 */
	  (dbpli.virtual_path + 1),
	  modified);
    }

  db_query_end(&qp);
  free(qp.filter);

  return 0;
}

/*
 * Command handler function for 'load'
 * Adds the playlist given by virtual-path in argv[1] to the queue.
 */
static int
mpd_command_load(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *path;
  struct playlist_info *pli;
  struct player_status status;
  struct query_params qp = { .type = Q_PLITEMS };
  int ret;
  int pos = -1;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing arguments to command load");
      return ACK_ERROR_ARG;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      path = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      path = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  pli = db_pl_fetch_byvirtualpath(path);
  free(path);
  if (!pli)
    {
      *errmsg = safe_asprintf("Playlist not found for path '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  //TODO If a second parameter is given only add the specified range of songs to the playqueue
  
  /* 0.23.1: POSITION specifies where to insert in the queue */
  if (argc >= 4)
    {
      struct mpd_cmd_params param;

      memset(&param, 0, sizeof(param));

      if (mpd_parse_cmd_position(argv[3], &param) != 0)
      	{
      	  *errmsg = safe_asprintf("Could not parse POSITION '%s'", argv[3]);
      	  return ACK_ERROR_ARG;
      	}

      pos = param.pos;
    }

  qp.id = pli->id;
  free_pli(pli, 0);

  player_get_status(&status);

  ret = db_queue_add_by_query(&qp, status.shuffle, status.item_id, pos, NULL, NULL);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to add song '%s' to playlist", argv[1]);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

static int
mpd_command_playlistadd(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *vp_playlist;
  char *vp_item;
  int ret;

  if (argc < 3)
    {
      *errmsg = safe_asprintf("Missing arguments to command playlistadd");
      return ACK_ERROR_ARG;
    }

  if (!allow_modifying_stored_playlists)
    {
      *errmsg = safe_asprintf("Modifying stored playlists is not enabled");
      return ACK_ERROR_PERMISSION;
    }

  /* 0.23.1: POSITION specifies where to insert, not supported by
   * library currently */
  if (argc >= 4)
    {
      *errmsg = safe_asprintf("Positional updates to playlists not supported");
      return ACK_ERROR_SYSTEM;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      vp_playlist = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      vp_playlist = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  vp_item = prepend_slash(argv[2]);

  ret = library_playlist_item_add(vp_playlist, vp_item);
  free(vp_playlist);
  free(vp_item);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Error adding item to file '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  return 0;
}

static int
mpd_command_rm(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *virtual_path;
  int ret;

  if (!allow_modifying_stored_playlists)
    {
      *errmsg = safe_asprintf("Modifying stored playlists is not enabled");
      return ACK_ERROR_PERMISSION;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      virtual_path = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      virtual_path = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  ret = library_playlist_remove(virtual_path);
  free(virtual_path);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Error removing playlist '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  return 0;
}

static int
mpd_command_save(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *virtual_path;
  struct playlist_info *pli;
  int ret;
  enum {
      SAVEMODE_CREATE,
      SAVEMODE_APPEND,
      SAVEMODE_REPLACE
  } save_mode = SAVEMODE_CREATE;  /* default */

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing arguments to command save");
      return ACK_ERROR_ARG;
    }

  if (!allow_modifying_stored_playlists)
    {
      *errmsg = safe_asprintf("Modifying stored playlists is not enabled");
      return ACK_ERROR_PERMISSION;
    }

  if (argc >= 3)
    {
      if (strcasecmp(argv[2], "create") == 0)
      	save_mode = SAVEMODE_CREATE;
      else if (strcasecmp(argv[2], "append") == 0)
      	save_mode = SAVEMODE_APPEND;
      else if (strcasecmp(argv[2], "replace") == 0)
      	save_mode = SAVEMODE_REPLACE;
    }

  if (!default_pl_dir || strstr(argv[1], ":/"))
    {
      // Argument is a virtual path, make sure it starts with a '/'
      virtual_path = prepend_slash(argv[1]);
    }
  else
    {
      // Argument is a playlist name, prepend default playlist directory
      virtual_path = safe_asprintf("%s/%s", default_pl_dir, argv[1]);
    }

  /* lookup the playlist to see if it exists */
  pli = db_pl_fetch_byvirtualpath(virtual_path);

  if (pli)
    free_pli(pli, 0);

  if (pli && save_mode == SAVEMODE_CREATE)
    {
      *errmsg = safe_asprintf("Playlist already exists by that name: %s",
      			      virtual_path);
      free(virtual_path);
      return ACK_ERROR_ARG;
    }
  else if (!pli && save_mode != SAVEMODE_CREATE)
    {
      *errmsg = safe_asprintf("No such playlist by that name: %s",
      			      virtual_path);
      free(virtual_path);
      return ACK_ERROR_ARG;
    }

  if (save_mode == SAVEMODE_REPLACE)
    {
      library_playlist_remove(virtual_path);
    }

  if (save_mode == SAVEMODE_APPEND)
    {
      struct query_params query_params;
      struct db_queue_item queue_item;

      /* walk through queue, append one by one */
      memset(&query_params, 0, sizeof(query_params));

      ret = db_queue_enum_start(&query_params);
      if (ret < 0)
    	{
      	  *errmsg = safe_asprintf("Failed to start queue enum "
      	  			  "for command save append");
  	  free(virtual_path);
      	  return ACK_ERROR_ARG;
    	}

      while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 &&
      	     queue_item.id > 0)
      	{
      	  ret = library_playlist_item_add(virtual_path,
      	  				  queue_item.virtual_path);
  	  if (ret < 0)
  	    break;
      	}

      db_queue_enum_end(&query_params);
    }
  else /* SAVEMODE_CREATE/REPLACE */
    {
      ret = library_queue_save(virtual_path);
    }

  free(virtual_path);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Error saving queue to file '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-albumart */
static int
mpd_command_albumart(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct evbuffer *evbuffer;
  const char *type = NULL;
  size_t size;
  int itemid;
  int format;
  uint32_t off;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'albumart'");
      return ACK_ERROR_ARG;
    }

  itemid = db_file_id_byvirtualpath_match(argv[1]);
  if (!itemid)
    {
      DPRINTF(E_WARN, L_MPD, "No item found for path '%s'\n", argv[1]);
      *errmsg = safe_asprintf("Item not found");
      return ACK_ERROR_ARG;
    }

  if (safe_atou32(argv[2], &off) != 0)
    {
      DPRINTF(E_WARN, L_MPD, "Argument not a number: '%s'\n", argv[2]);
      *errmsg = safe_asprintf("Illegal offset argument");
      return ACK_ERROR_ARG;
    }

  evbuffer = evbuffer_new();
  if (!evbuffer)
    {
      DPRINTF(E_LOG, L_MPD,
      	      "Could not allocate an evbuffer for artwork request\n");
      *errmsg = safe_asprintf("Item not found");
      return ACK_ERROR_ARG;
    }

  format = artwork_get_item(evbuffer, itemid,
  			    ART_DEFAULT_WIDTH, ART_DEFAULT_HEIGHT, 0);
  if (format < 0)
    {
      *errmsg = safe_asprintf("Item was not found");
      evbuffer_free(evbuffer);
      return ACK_ERROR_ARG;
    }

  switch (format)
    {
      case ART_FMT_PNG:
      	type = "image/png";
      	break;

      default:
      	type = "image/jpeg";
      	break;
    }

  size = evbuffer_get_length(evbuffer);
  if (size == 0)
    {
      *errmsg = safe_asprintf("Item contains no data");
      evbuffer_free(evbuffer);
      return ACK_ERROR_ARG;
    }

  evbuffer_add_printf(evbuf, "type: %s\n", type);

  mpd_write_binary_response(ctx, evbuf, evbuffer, (size_t)off);
  evbuffer_free(evbuffer);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-count */
static int
mpd_command_count(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct filecount_info fci;
  int ret;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'count'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  params.exactmatch = true;
  qp = &params.qp;
  qp->type = Q_COUNT_ITEMS;

  params.params_allow = CMD_FILTER | CMD_GROUP;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  ret = db_filecount_get(&fci, qp);
  if (ret < 0)
    {
      free(qp->filter);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  evbuffer_add_printf(evbuf,
      "songs: %d\n"
      "playtime: %" PRIu64 "\n",
      fci.count,
      (fci.length / 1000));

  db_query_end(qp);
  free(qp->filter);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-find */
static int
mpd_command_find(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct db_media_file_info dbmfi;
  int ret;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'find'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  params.exactmatch = true;
  qp = &params.qp;

  qp->type = Q_ITEMS;
  qp->sort = S_NAME;
  qp->idx_type = I_NONE;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  ret = db_query_start(qp);
  if (ret < 0)
    {
      db_query_end(qp);
      free(qp->filter);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while ((ret = db_query_fetch_file(&dbmfi, qp)) == 0)
    {
      ret = mpd_add_db_media_file_info(evbuf, &dbmfi);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error adding song to the evbuffer, song id: %s\n", dbmfi.id);
	}
    }

  db_query_end(qp);
  free(qp->filter);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-findadd */
static int
mpd_command_findadd(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct player_status status;
  int ret;
  int pos = -1;

  if (argc < 3 || ((argc - 1) % 2) != 0)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'findadd'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  params.exactmatch = true;
  qp = &params.qp;

  qp->type = Q_ITEMS;
  qp->sort = S_ARTIST;
  qp->idx_type = I_NONE;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW | CMD_POSITION;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  player_get_status(&status);

  ret = db_queue_add_by_query(qp, status.shuffle, status.item_id, pos, NULL, NULL);
  free(qp->filter);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to add songs to playlist");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Some MPD clients crash if the tag value includes the newline character.
 * While they should normally not be included in most ID3 tags, they sometimes
 * are, so we just change them to space. See #1613 for more details.
 */

static void
sanitize_value(char **strval)
{
  char *ptr = *strval;

  while(*ptr != '\0')
    {
      if(*ptr == '\n')
  {
    *ptr = ' ';
  }
    ptr++;
  }
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-list */
static int
mpd_command_list(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_tagtype *tagtype;
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct db_media_file_info dbmfi;
  char **strval;
  int i;
  int ret;

  if (argc < 2 || ((argc % 2) != 0))
    {
      if (argc != 3 || (0 != strcasecmp(argv[1], "album")))
	{
	  *errmsg = safe_asprintf("Missing argument(s) for command 'list'");
	  return ACK_ERROR_ARG;
	}
    }

  tagtype = find_tagtype(argv[1]);

  if (!tagtype || tagtype->type == MPD_TYPE_SPECIAL) //FIXME allow "file" tagtype
    {
      DPRINTF(E_WARN, L_MPD, "Unsupported type argument for command 'list': %s\n", argv[1]);
      return 0;
    }

  memset(&params, 0, sizeof(params));
  qp = &params.qp;
  qp->type = Q_ITEMS;
  qp->idx_type = I_NONE;
  qp->order = tagtype->sort_field;
  qp->group = strdup(tagtype->group_field);
  params.addgroupfilter = tagtype->group_in_listcommand;

  params.params_allow = CMD_FILTER | CMD_GROUP;
  mpd_parse_cmd_params(argc - 2, argv + 2, &params);

  ret = db_query_start(qp);
  if (ret < 0)
    {
      db_query_end(qp);
      free(qp->filter);
      free(qp->group);
      free(params.groups);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while ((ret = db_query_fetch_file(&dbmfi, qp)) == 0)
    {
      strval = (char **) ((char *)&dbmfi + tagtype->mfi_offset);

      if (!(*strval) || (**strval == '\0'))
	continue;

      sanitize_value(strval);
      evbuffer_add_printf(evbuf,
			  "%s: %s\n",
			  tagtype->tag,
			  *strval);

      if (params.groups && params.groupslen > 0)
	{
	  for (i = 0; i < params.groupslen; i++)
	    {
	      if (!params.groups[i])
		continue;

	      strval = (char **)((char *)&dbmfi + params.groups[i]->mfi_offset);

	      if (!(*strval) || (**strval == '\0'))
		continue;

	      evbuffer_add_printf(evbuf,
	      			  "%s: %s\n",
				  params.groups[i]->tag,
	      			  *strval);
	    }
	}
    }

  db_query_end(qp);
  free(qp->filter);
  free(qp->group);
  free(params.groups);

  return 0;
}

static int
mpd_add_directory(struct evbuffer *evbuf, int directory_id, int listall, int listinfo, char **errmsg)
{
  struct directory_info subdir;
  struct query_params qp;
  struct directory_enum dir_enum;
  struct db_playlist_info dbpli;
  char modified[32];
  uint32_t time_modified;
  struct db_media_file_info dbmfi;
  int ret;

  // Load playlists for dir-id
  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_PL;
  qp.sort = S_PLAYLIST;
  qp.idx_type = I_NONE;
  qp.filter = db_mprintf("(f.directory_id = %d AND (f.type = %d OR f.type = %d))", directory_id, PL_PLAIN, PL_SMART);
  ret = db_query_start(&qp);
  if (ret < 0)
    {
      db_query_end(&qp);
      free(qp.filter);
      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }
  while (((ret = db_query_fetch_pl(&dbpli, &qp)) == 0) && (dbpli.id))
    {
      if (safe_atou32(dbpli.db_timestamp, &time_modified) != 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error converting time modified to uint32_t: %s\n", dbpli.db_timestamp);
	}

      if (listinfo)
	{
	  mpd_time(modified, sizeof(modified), time_modified);
	  evbuffer_add_printf(evbuf,
	    "playlist: %s\n"
	    "Last-Modified: %s\n",
	    (dbpli.virtual_path + 1),
	    modified);
	}
      else
	{
	  evbuffer_add_printf(evbuf,
	    "playlist: %s\n",
	    (dbpli.virtual_path + 1));
	}
    }
  db_query_end(&qp);
  free(qp.filter);

  // Load sub directories for dir-id
  memset(&dir_enum, 0, sizeof(struct directory_enum));
  dir_enum.parent_id = directory_id;
  ret = db_directory_enum_start(&dir_enum);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MPD, "Failed to start directory enum for parent_id %d\n", directory_id);
      db_directory_enum_end(&dir_enum);
      return -1;
    }
  while ((ret = db_directory_enum_fetch(&dir_enum, &subdir)) == 0 && subdir.id > 0)
    {
      if (listinfo)
	{
	  evbuffer_add_printf(evbuf,
	    "directory: %s\n"
	    "Last-Modified: %s\n",
	    (subdir.virtual_path + 1),
	    "2015-12-01 00:00");
	}
      else
	{
	  evbuffer_add_printf(evbuf,
	    "directory: %s\n",
	    (subdir.virtual_path + 1));
	}

      if (listall)
	{
	  mpd_add_directory(evbuf, subdir.id, listall, listinfo, errmsg);
	}
    }
  db_directory_enum_end(&dir_enum);

  // Load files for dir-id
  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_ITEMS;
  qp.sort = S_ARTIST;
  qp.idx_type = I_NONE;
  qp.filter = db_mprintf("(f.directory_id = %d)", directory_id);
  ret = db_query_start(&qp);
  if (ret < 0)
    {
      db_query_end(&qp);
      free(qp.filter);
      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }
  while ((ret = db_query_fetch_file(&dbmfi, &qp)) == 0)
    {
      if (listinfo)
	{
	  ret = mpd_add_db_media_file_info(evbuf, &dbmfi);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_MPD, "Error adding song to the evbuffer, song id: %s\n", dbmfi.id);
	    }
	}
      else
	{
	  evbuffer_add_printf(evbuf,
	    "file: %s\n",
	    (dbmfi.virtual_path + 1));
	}
    }
  db_query_end(&qp);
  free(qp.filter);

  return 0;
}

static int
mpd_command_listall(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int dir_id;
  char parent[PATH_MAX];
  int ret;

  if (argc < 2 || strlen(argv[1]) == 0
      || (strncmp(argv[1], "/", 1) == 0 && strlen(argv[1]) == 1))
    {
      ret = snprintf(parent, sizeof(parent), "/");
    }
  else if (strncmp(argv[1], "/", 1) == 0)
    {
      ret = snprintf(parent, sizeof(parent), "%s/", argv[1]);
    }
  else
    {
      ret = snprintf(parent, sizeof(parent), "/%s", argv[1]);
    }

  if ((ret < 0) || (ret >= sizeof(parent)))
    {
      *errmsg = safe_asprintf("Parent path exceeds PATH_MAX");
      return ACK_ERROR_UNKNOWN;
    }

  // Load dir-id from db for parent-path
  dir_id = db_directory_id_byvirtualpath(parent);
  if (dir_id == 0)
    {
      *errmsg = safe_asprintf("Directory info not found for virtual-path '%s'", parent);
      return ACK_ERROR_NO_EXIST;
    }

  ret = mpd_add_directory(evbuf, dir_id, 1, 0, errmsg);

  return ret;
}

static int
mpd_command_listallinfo(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int dir_id;
  char parent[PATH_MAX];
  int ret;

  if (argc < 2 || strlen(argv[1]) == 0
      || (strncmp(argv[1], "/", 1) == 0 && strlen(argv[1]) == 1))
    {
      ret = snprintf(parent, sizeof(parent), "/");
    }
  else if (strncmp(argv[1], "/", 1) == 0)
    {
      ret = snprintf(parent, sizeof(parent), "%s/", argv[1]);
    }
  else
    {
      ret = snprintf(parent, sizeof(parent), "/%s", argv[1]);
    }

  if ((ret < 0) || (ret >= sizeof(parent)))
    {
      *errmsg = safe_asprintf("Parent path exceeds PATH_MAX");
      return ACK_ERROR_UNKNOWN;
    }

  // Load dir-id from db for parent-path
  dir_id = db_directory_id_byvirtualpath(parent);
  if (dir_id == 0)
    {
      *errmsg = safe_asprintf("Directory info not found for virtual-path '%s'", parent);
      return ACK_ERROR_NO_EXIST;
    }

  ret = mpd_add_directory(evbuf, dir_id, 1, 1, errmsg);

  return ret;
}

/*
 * Command handler function for 'lsinfo'
 * Lists the contents of the directory given in argv[1].
 */
static int
mpd_command_lsinfo(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int dir_id;
  char parent[PATH_MAX];
  int print_playlists;
  int ret;

  if (argc < 2 || strlen(argv[1]) == 0
      || (strncmp(argv[1], "/", 1) == 0 && strlen(argv[1]) == 1))
    {
      ret = snprintf(parent, sizeof(parent), "/");
    }
  else if (strncmp(argv[1], "/", 1) == 0)
    {
      ret = snprintf(parent, sizeof(parent), "%s/", argv[1]);
    }
  else
    {
      ret = snprintf(parent, sizeof(parent), "/%s", argv[1]);
    }

  if ((ret < 0) || (ret >= sizeof(parent)))
    {
      *errmsg = safe_asprintf("Parent path exceeds PATH_MAX");
      return ACK_ERROR_UNKNOWN;
    }

  print_playlists = 0;
  if ((strncmp(parent, "/", 1) == 0 && strlen(parent) == 1))
    {
      /*
       * Special handling necessary if the root directory '/' is given.
       * In this case additional to the directory contents the stored playlists will be returned.
       * This behavior is deprecated in the mpd protocol but clients like ncmpccp or ympd uses it.
       */
      print_playlists = 1;
    }


  // Load dir-id from db for parent-path
  dir_id = db_directory_id_byvirtualpath(parent);
  if (dir_id == 0)
    {
      *errmsg = safe_asprintf("Directory info not found for virtual-path '%s'", parent);
      return ACK_ERROR_NO_EXIST;
    }

  ret = mpd_add_directory(evbuf, dir_id, 0, 1, errmsg);

  // If the root directory was passed as argument add the stored playlists to the response
  if (ret == 0 && print_playlists)
    {
      return mpd_command_listplaylists(evbuf, argc, argv, errmsg, ctx);
    }

  return ret;
}

/*
 * Command handler function for 'listfiles'
 *
 * This command should list all files including files that are not part of the library. We do not support this
 * and only report files in the library.
 */
static int
mpd_command_listfiles(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  return mpd_command_lsinfo(evbuf, argc, argv, errmsg, ctx);
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-search
 * Command handler function for 'search'
 * Lists any song that matches the given list of arguments. Arguments are pairs of TYPE and WHAT, where
 * TYPE is the tag that contains WHAT (case insensitiv).
 *
 * TYPE can also be one of the special parameter:
 * - any: checks all tags
 * - file: checks the virtual_path
 * - base: restricts result to the given directory
 * - modified-since (not supported)
 * - window: limits result to the given range of "START:END"
 *
 * Example request: "search artist foo album bar"
 */
static int
mpd_command_search(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct db_media_file_info dbmfi;
  int ret;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'search'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  qp = &params.qp;

  qp->type = Q_ITEMS;
  qp->sort = S_NAME;
  qp->idx_type = I_NONE;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  ret = db_query_start(qp);
  if (ret < 0)
    {
      db_query_end(qp);
      free(qp->filter);

      *errmsg = safe_asprintf("Could not start query");
      return ACK_ERROR_UNKNOWN;
    }

  while ((ret = db_query_fetch_file(&dbmfi, qp)) == 0)
    {
      ret = mpd_add_db_media_file_info(evbuf, &dbmfi);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error adding song to the evbuffer, song id: %s\n", dbmfi.id);
	}
    }

  db_query_end(qp);
  free(qp->filter);

  return 0;
}

/* https://mpd.readthedocs.io/en/latest/protocol.html#command-searchadd */
static int
mpd_command_searchadd(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_cmd_params params;
  struct query_params *qp;
  struct player_status status;
  int ret;
  int pos = -1;

  if (argc < 2)
    {
      *errmsg = safe_asprintf("Missing argument(s) for command 'search'");
      return ACK_ERROR_ARG;
    }

  memset(&params, 0, sizeof(params));
  qp = &params.qp;

  qp->type = Q_ITEMS;
  qp->sort = S_ARTIST;
  qp->idx_type = I_NONE;

  params.params_allow = CMD_FILTER | CMD_SORT | CMD_WINDOW | CMD_POSITION;
  mpd_parse_cmd_params(argc - 1, argv + 1, &params);

  player_get_status(&status);

  ret = db_queue_add_by_query(qp, status.shuffle, status.item_id, pos, NULL, NULL);
  free(qp->filter);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Failed to add songs to playlist");
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

/*
 * Command handler function for 'update'
 * Initiates an init-rescan (scans for new files)
 */
static int
mpd_command_update(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  if (argc > 1 && strlen(argv[1]) > 0)
    {
      *errmsg = safe_asprintf("Update for specific uri not supported for command 'update'");
      return ACK_ERROR_ARG;
    }

  library_rescan(0);

  evbuffer_add(evbuf, "updating_db: 1\n", 15);

  return 0;
}

static int
mpd_sticker_get(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path)
{
  struct media_file_info *mfi = NULL;
  uint32_t rating;

  if (strcmp(argv[4], "rating") != 0)
    {
      *errmsg = safe_asprintf("no such sticker");
      return ACK_ERROR_NO_EXIST;
    }

  mfi = db_file_fetch_byvirtualpath(virtual_path);
  if (!mfi)
    {
      DPRINTF(E_LOG, L_MPD, "Virtual path not found: %s\n", virtual_path);
      *errmsg = safe_asprintf("unknown sticker domain");
      return ACK_ERROR_ARG;
    }

  if (mfi->rating > 0)
    {
      rating = mfi->rating / MPD_RATING_FACTOR;
      evbuffer_add_printf(evbuf, "sticker: rating=%d\n", rating);
    }

  free_mfi(mfi, 0);

  return 0;
}

static int
mpd_sticker_set(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path)
{
  uint32_t rating;
  int id;
  int ret;

  if (strcmp(argv[4], "rating") != 0)
    {
      *errmsg = safe_asprintf("no such sticker");
      return ACK_ERROR_NO_EXIST;
    }

  ret = safe_atou32(argv[5], &rating);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("rating '%s' doesn't convert to integer", argv[5]);
      return ACK_ERROR_ARG;
    }

  rating *= MPD_RATING_FACTOR;
  if (rating > DB_FILES_RATING_MAX)
    {
      *errmsg = safe_asprintf("rating '%s' is greater than maximum value allowed", argv[5]);
      return ACK_ERROR_ARG;
    }

  id = db_file_id_byvirtualpath(virtual_path);
  if (id <= 0)
    {
      *errmsg = safe_asprintf("Invalid path '%s'", virtual_path);
      return ACK_ERROR_ARG;
    }

  library_item_attrib_save(id, LIBRARY_ATTRIB_RATING, rating);

  return 0;
}

static int
mpd_sticker_delete(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path)
{
  int id;

  if (strcmp(argv[4], "rating") != 0)
    {
      *errmsg = safe_asprintf("no such sticker");
      return ACK_ERROR_NO_EXIST;
    }

  id = db_file_id_byvirtualpath(virtual_path);
  if (id <= 0)
    {
      *errmsg = safe_asprintf("Invalid path '%s'", virtual_path);
      return ACK_ERROR_ARG;
    }

  library_item_attrib_save(id, LIBRARY_ATTRIB_RATING, 0);

  return 0;
}

static int
mpd_sticker_list(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path)
{
  struct media_file_info *mfi = NULL;
  uint32_t rating;

  mfi = db_file_fetch_byvirtualpath(virtual_path);
  if (!mfi)
    {
      DPRINTF(E_LOG, L_MPD, "Virtual path not found: %s\n", virtual_path);
      *errmsg = safe_asprintf("unknown sticker domain");
      return ACK_ERROR_ARG;
    }

  if (mfi->rating > 0)
    {
      rating = mfi->rating / MPD_RATING_FACTOR;
      evbuffer_add_printf(evbuf, "sticker: rating=%d\n", rating);
    }

  free_mfi(mfi, 0);

  /* |:todo:| real sticker implementation */
  return 0;
}

static int
mpd_sticker_find(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path)
{
  struct query_params qp;
  struct db_media_file_info dbmfi;
  uint32_t rating = 0;
  uint32_t rating_arg = 0;
  const char *operator;
  int ret = 0;

  if (strcmp(argv[4], "rating") != 0)
    {
      *errmsg = safe_asprintf("no such sticker");
      return ACK_ERROR_NO_EXIST;
    }

  if (argc == 6)
    {
      *errmsg = safe_asprintf("not enough arguments for 'sticker find'");
      return ACK_ERROR_ARG;
    }

  if (argc > 6)
    {
      if (strcmp(argv[5], "=") != 0 && strcmp(argv[5], ">") != 0 && strcmp(argv[5], "<") != 0)
	{
	  *errmsg = safe_asprintf("invalid operator '%s' given to 'sticker find'", argv[5]);
	  return ACK_ERROR_ARG;
	}
      operator = argv[5];

      ret = safe_atou32(argv[6], &rating_arg);
      if (ret < 0)
	{
	  *errmsg = safe_asprintf("rating '%s' doesn't convert to integer", argv[6]);
	  return ACK_ERROR_ARG;
	}
      rating_arg *= MPD_RATING_FACTOR;
    }
  else
    {
      operator = ">";
      rating_arg = 0;
    }

  memset(&qp, 0, sizeof(struct query_params));

  qp.type = Q_ITEMS;
  qp.sort = S_VPATH;
  qp.idx_type = I_NONE;

  qp.filter = db_mprintf("(f.virtual_path LIKE '%s%%' AND f.rating > 0 AND f.rating %s %d)", virtual_path, operator, rating_arg);
  if (!qp.filter)
    {
      *errmsg = safe_asprintf("Out of memory");
      ret = ACK_ERROR_UNKNOWN;
      return ret;
    }

  ret = db_query_start(&qp);
  if (ret < 0)
    {
      db_query_end(&qp);
      free(qp.filter);

      *errmsg = safe_asprintf("Could not start query");
      ret = ACK_ERROR_UNKNOWN;
      return ret;
    }

  while ((ret = db_query_fetch_file(&dbmfi, &qp)) == 0)
    {
      ret = safe_atou32(dbmfi.rating, &rating);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error rating=%s doesn't convert to integer, song id: %s\n",
		  dbmfi.rating, dbmfi.id);
	  continue;
	}

      rating /= MPD_RATING_FACTOR;
      ret = evbuffer_add_printf(evbuf,
				"file: %s\n"
				"sticker: rating=%d\n",
				(dbmfi.virtual_path + 1),
				rating);
      if (ret < 0)
	DPRINTF(E_LOG, L_MPD, "Error adding song to the evbuffer, song id: %s\n", dbmfi.id);
    }

  db_query_end(&qp);
  free(qp.filter);
  return 0;
}

struct mpd_sticker_command {
  const char *cmd;
  int (*handler)(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, const char *virtual_path);
  int need_args;
};

static struct mpd_sticker_command mpd_sticker_handlers[] =
  {
    /* sticker command    | handler function        | minimum argument count */
    { "get",                mpd_sticker_get,          5 },
    { "set",                mpd_sticker_set,          6 },
    { "delete",             mpd_sticker_delete,       5 },
    { "list",               mpd_sticker_list,         4 },
    { "find",               mpd_sticker_find,         5 },
    { NULL, NULL, 0 },
  };

/*
 * Command handler function for 'sticker'
 *
 *   sticker get "noth here" rating
 *   ACK [2@0] {sticker} unknown sticker domain
 *
 *   sticker get song "Al Cohn & Shorty Rogers/East Coast - West Coast Scene/04 Shorty Rogers - Cool Sunshine.flac" rating
 *   ACK [50@0] {sticker} no such sticker
 *
 *   sticker get song "Al Cohn & Shorty Rogers/East Coast - West Coast Scene/03 Al Cohn - Serenade For Kathy.flac" rating
 *   sticker: rating=8
 *   OK
 *
 * From cantata:
 *   sticker set song "file:/srv/music/VA/The Electro Swing Revolution Vol 3 1 - Hop, Hop, Hop/13 Mr. Hotcut - You Are.mp3" rating "6"
 *   OK
 */
static int
mpd_command_sticker(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct mpd_sticker_command *cmd_param = NULL;  // Quell compiler warning about uninitialized use of cmd_param
  char *virtual_path = NULL;
  int i;
  int ret;

  if (strcmp(argv[2], "song") != 0)
    {
      *errmsg = safe_asprintf("unknown sticker domain");
      return ACK_ERROR_ARG;
    }

  for (i = 0; i < (sizeof(mpd_sticker_handlers) / sizeof(struct mpd_sticker_command)); ++i)
    {
      cmd_param = &mpd_sticker_handlers[i];
      if (cmd_param->cmd && strcmp(argv[1], cmd_param->cmd) == 0)
	break;
    }
  if (!cmd_param->cmd)
    {
      *errmsg = safe_asprintf("bad request");
      return ACK_ERROR_ARG;
    }
  if (argc < cmd_param->need_args)
    {
      *errmsg = safe_asprintf("not enough arguments");
      return ACK_ERROR_ARG;
    }

  virtual_path = prepend_slash(argv[3]);

  ret = cmd_param->handler(evbuf, argc, argv, errmsg, virtual_path);

  free(virtual_path);

  return ret;
}

/*
static int
mpd_command_rescan(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int ret;

  if (argc > 1)
    {
      DPRINTF(E_LOG, L_MPD, "Rescan for specific uri not supported for command 'rescan'\n");
      *errmsg = safe_asprintf("Rescan for specific uri not supported for command 'rescan'");
      return ACK_ERROR_ARG;
    }

  filescanner_trigger_fullrescan();

  evbuffer_add(evbuf, "updating_db: 1\n", 15);

  return 0;
}
*/

static int
mpd_command_password(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  char *required_password;
  char *supplied_password = "";
  int unrequired;

  if (argc > 1)
    {
      supplied_password = argv[1];
    }

  required_password = cfg_getstr(cfg_getsec(cfg, "library"), "password");
  unrequired = !required_password || required_password[0] == '\0';

  if (unrequired || strcmp(supplied_password, required_password) == 0)
    {
      DPRINTF(E_DBG, L_MPD,
	      "Authentication succeeded with supplied password: %s%s\n",
	      supplied_password,
	      unrequired ? " although no password is required" : "");
      return 0;
    }

  DPRINTF(E_LOG, L_MPD,
	  "Authentication failed with supplied password: %s"
	  " for required password: %s\n",
	  supplied_password, required_password);
  *errmsg = safe_asprintf("Wrong password. Authentication failed.");
  return ACK_ERROR_PASSWORD;
}

static int
mpd_command_binarylimit(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  unsigned int size;

  if (safe_atou32(argv[1], &size) < 0)
    {
      DPRINTF(E_DBG, L_MPD,
      	      "Argument %s to binarylimit is not a number\n",
      	      argv[1]);
      return ACK_ERROR_ARG;
    }

  if (size < MPD_BINARY_SIZE_MIN)
    {
      *errmsg = safe_asprintf("Value too small");
      return ACK_ERROR_ARG;
    }

  ctx->binarylimit = size;

  return 0;
}

/*
 * Callback function for the 'player_speaker_enumerate' function.
 * Expect a struct output_get_param as argument and allocates a struct output if
 * the shortid of output_get_param matches the given speaker/output spk.
 */
static void
output_get_cb(struct player_speaker_info *spk, void *arg)
{
  struct output_get_param *param = arg;

  if (!param->output
      && param->shortid == param->curid)
    {
      CHECK_NULL(L_MPD, param->output = calloc(1, sizeof(struct output)));

      param->output->id = spk->id;
      param->output->shortid = param->shortid;
      param->output->name = strdup(spk->name);
      param->output->selected = spk->selected;

      param->curid++;

      DPRINTF(E_DBG, L_MPD, "Output found: shortid %d, id %" PRIu64 ", name '%s', selected %d\n",
	param->output->shortid, param->output->id, param->output->name, param->output->selected);
    }
}

/*
 * Command handler function for 'disableoutput'
 * Expects argument argv[1] to be the id of the speaker to disable.
 */
static int
mpd_command_disableoutput(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct output_get_param param;
  uint32_t num;
  int ret;

  ret = safe_atou32(argv[1], &num);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  memset(&param, 0, sizeof(struct output_get_param));
  param.shortid = num;

  player_speaker_enumerate(output_get_cb, &param);

  if (param.output && param.output->selected)
    {
      ret = player_speaker_disable(param.output->id);
      free_output(param.output);

      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Speakers deactivation failed: %d", num);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  return 0;
}

/*
 * Command handler function for 'enableoutput'
 * Expects argument argv[1] to be the id of the speaker to enable.
 */
static int
mpd_command_enableoutput(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct output_get_param param;
  uint32_t num;
  int ret;

  ret = safe_atou32(argv[1], &num);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  memset(&param, 0, sizeof(struct output_get_param));
  param.shortid = num;

  player_speaker_enumerate(output_get_cb, &param);

  if (param.output && !param.output->selected)
    {
      ret = player_speaker_enable(param.output->id);
      free_output(param.output);

      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Speakers deactivation failed: %d", num);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  return 0;
}

/*
 * Command handler function for 'toggleoutput'
 * Expects argument argv[1] to be the id of the speaker to enable/disable.
 */
static int
mpd_command_toggleoutput(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct output_get_param param;
  uint32_t num;
  int ret;

  ret = safe_atou32(argv[1], &num);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  memset(&param, 0, sizeof(struct output_get_param));
  param.shortid = num;

  player_speaker_enumerate(output_get_cb, &param);

  if (param.output)
    {
      if (param.output->selected)
	ret = player_speaker_disable(param.output->id);
      else
	ret = player_speaker_enable(param.output->id);

      free_output(param.output);

      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Toggle speaker failed: %d", num);
	  return ACK_ERROR_UNKNOWN;
	}
    }

  return 0;
}

/*
 * Callback function for the 'outputs' command.
 * Gets called for each available speaker and prints the speaker information to the evbuffer given in *arg.
 *
 * Example output:
 *   outputid: 0
 *   outputname: Computer
 *   plugin: alsa
 *   outputenabled: 1
 * https://mpd.readthedocs.io/en/latest/protocol.html#command-outputs
 */
static void
speaker_enum_cb(struct player_speaker_info *spk, void *arg)
{
  struct output_outputs_param *param = arg;
  struct evbuffer *evbuf = param->buf;
  char plugin[sizeof(spk->output_type)];
  char *p;
  char *q;

  /* MPD outputs lowercase plugin (audio_output:type) so convert to
   * lowercase, convert spaces to underscores to make it a single word */
  for (p = spk->output_type, q = plugin; *p != '\0'; p++, q++)
    {
      *q = tolower(*p);
      if (*q == ' ')
      	*q = '_';
    }
  *q = '\0';

  evbuffer_add_printf(evbuf,
		      "outputid: %u\n"
		      "outputname: %s\n"
		      "plugin: %s\n"
		      "outputenabled: %d\n",
		      param->nextid,
		      spk->name,
		      plugin,
		      spk->selected);
  param->nextid++;
}

/*
 * Command handler function for 'output'
 * Returns a lists with the avaiable speakers.
 */
static int
mpd_command_outputs(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  struct output_outputs_param param;

  /* Reference:
   * https://mpd.readthedocs.io/en/latest/protocol.html#audio-output-devices
   * the ID returned by mpd may change between excutions, so what we do
   * is simply enumerate the speakers, and for get/set commands we count
   * ID times to the output referenced. */
  memset(&param, 0, sizeof(param));
  param.buf = evbuf;

  player_speaker_enumerate(speaker_enum_cb, &param);

  /* streaming output is not in the speaker list, so add it as pseudo
   * element when configured to do so */
  if (mpd_plugin_httpd)
    {
      evbuffer_add_printf(evbuf,
		      	  "outputid: %u\n"
		      	  "outputname: MP3 stream\n"
		      	  "plugin: httpd\n"
		      	  "outputenabled: 1\n",
		      	  param.nextid);
      param.nextid++;
    }

  return 0;
}

static int
outputvolume_set(uint32_t shortid, int volume, char **errmsg)
{
  struct output_get_param param;
  int ret;

  memset(&param, 0, sizeof(struct output_get_param));
  param.shortid = shortid;

  player_speaker_enumerate(output_get_cb, &param);

  if (param.output)
    {
      ret = player_volume_setabs_speaker(param.output->id, volume);
      free_output(param.output);

      if (ret < 0)
	{
	  *errmsg = safe_asprintf("Setting volume to %d for speaker with short-id %d failed", volume, shortid);
	  return ACK_ERROR_UNKNOWN;
	}
    }
  else
    {
      *errmsg = safe_asprintf("No speaker found for short id: %d", shortid);
      return ACK_ERROR_UNKNOWN;
    }

  return 0;
}

static int
mpd_command_outputvolume(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  uint32_t shortid;
  int volume;
  int ret;

  ret = safe_atou32(argv[1], &shortid);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[1]);
      return ACK_ERROR_ARG;
    }

  ret = safe_atoi32(argv[2], &volume);
  if (ret < 0)
    {
      *errmsg = safe_asprintf("Argument doesn't convert to integer: '%s'", argv[2]);
      return ACK_ERROR_ARG;
    }

  ret = outputvolume_set(shortid, volume, errmsg);

  return ret;
}

static void
channel_outputvolume(const char *message)
{
  uint32_t shortid;
  int volume;
  char *tmp;
  char *ptr;
  char *errmsg = NULL;
  int ret;

  tmp = strdup(message);
  ptr = strrchr(tmp, ':');
  if (!ptr)
    {
      free(tmp);
      DPRINTF(E_LOG, L_MPD, "Failed to parse output id and volume from message '%s' (expected format: \"output-id:volume\"\n", message);
      return;
    }

  *ptr = '\0';

  ret = safe_atou32(tmp, &shortid);
  if (ret < 0)
    {
      free(tmp);
      DPRINTF(E_LOG, L_MPD, "Failed to parse output id from message: '%s'\n", message);
      return;
    }

  ret = safe_atoi32((ptr + 1), &volume);
  if (ret < 0)
    {
      free(tmp);
      DPRINTF(E_LOG, L_MPD, "Failed to parse volume from message: '%s'\n", message);
      return;
    }

  outputvolume_set(shortid, volume, &errmsg);
  if (errmsg)
    DPRINTF(E_LOG, L_MPD, "Failed to set output volume from message: '%s' (error='%s')\n", message, errmsg);

  free(tmp);
}

static void
channel_pairing(const char *message)
{
  remote_pairing_kickoff((char **)&message);
}

static void
channel_verification(const char *message)
{
  player_raop_verification_kickoff((char **)&message);
}

struct mpd_channel
{
  /* The channel name */
  const char *channel;

  /*
   * The function to execute the sendmessage command for a specific channel
   *
   * @param message message received on this channel
   */
  void (*handler)(const char *message);
};

static struct mpd_channel mpd_channels[] =
  {
    /* channel               | handler function */
    { "outputvolume",          channel_outputvolume },
    { "pairing",               channel_pairing },
    { "verification",          channel_verification },
    { NULL, NULL },
  };

/*
 * Finds the channel handler for the given channel name
 *
 * @param name channel name from sendmessage command
 * @return the channel or NULL if it is an unknown/unsupported channel
 */
static struct mpd_channel *
mpd_find_channel(const char *name)
{
  int i;

  for (i = 0; mpd_channels[i].handler; i++)
    {
      if (0 == strcmp(name, mpd_channels[i].channel))
	{
	  return &mpd_channels[i];
	}
    }

  return NULL;
}

static int
mpd_command_channels(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int i;

  for (i = 0; mpd_channels[i].handler; i++)
    {
      evbuffer_add_printf(evbuf,
	  "channel: %s\n",
	  mpd_channels[i].channel);
    }

  return 0;
}

static int
mpd_command_sendmessage(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  const char *channelname;
  const char *message;
  struct mpd_channel *channel;

  if (argc < 3)
    {
      *errmsg = safe_asprintf("Missing argument for command 'sendmessage'");
      return ACK_ERROR_ARG;
    }

  channelname = argv[1];
  message = argv[2];

  channel = mpd_find_channel(channelname);
  if (!channel)
    {
      // Just ignore the message, only log an error message
      DPRINTF(E_LOG, L_MPD, "Unsupported channel '%s'\n", channelname);
      return 0;
    }

  channel->handler(message);
  return 0;
}

/*
 * Dummy function to handle commands that are not supported and should
 * not raise an error.
 */
static int
mpd_command_ignore(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  //do nothing
  DPRINTF(E_DBG, L_MPD, "Ignore command %s\n", argv[0]);
  return 0;
}

static int
mpd_command_commands(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx);

/*
 * Command handler function for 'tagtypes'
 * Returns a lists with supported tags in the form:
 *   tagtype: Artist
 */
static int
mpd_command_tagtypes(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int i;

  for (i = 0; i < ARRAY_SIZE(tagtypes); i++)
    {
      if (tagtypes[i].type != MPD_TYPE_SPECIAL)
	evbuffer_add_printf(evbuf, "tagtype: %s\n", tagtypes[i].tag);
    }

  return 0;
}

/*
 * Command handler function for 'urlhandlers'
 * Returns a lists with supported tags in the form:
 *   handler: protocol://
 */
static int
mpd_command_urlhandlers(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  evbuffer_add_printf(evbuf,
      "handler: http://\n"
      // handlers supported by MPD 0.19.12
      // "handler: https://\n"
      // "handler: mms://\n"
      // "handler: mmsh://\n"
      // "handler: mmst://\n"
      // "handler: mmsu://\n"
      // "handler: gopher://\n"
      // "handler: rtp://\n"
      // "handler: rtsp://\n"
      // "handler: rtmp://\n"
      // "handler: rtmpt://\n"
      // "handler: rtmps://\n"
      // "handler: smb://\n"
      // "handler: nfs://\n"
      // "handler: cdda://\n"
      // "handler: alsa://\n"
      );

  return 0;
}

/*
 * Command handler function for 'decoders'
 * MPD returns the decoder plugins with their supported suffix and mime types.
 *
 * The server only uses libav/ffmepg for decoding and does not support decoder plugins,
 * therefor the function reports only ffmpeg as available.
 */
static int
mpd_command_decoders(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int i;

  evbuffer_add_printf(evbuf, "plugin: ffmpeg\n");

  for (i = 0; ffmpeg_suffixes[i]; i++)
    {
      evbuffer_add_printf(evbuf, "suffix: %s\n", ffmpeg_suffixes[i]);
    }

  for (i = 0; ffmpeg_mime_types[i]; i++)
    {
      evbuffer_add_printf(evbuf, "mime_type: %s\n", ffmpeg_mime_types[i]);
    }

  return 0;
}

struct mpd_command
{
  /* The command name */
  const char *mpdcommand;

  /*
   * The function to execute the command
   *
   * @param evbuf the response event buffer
   * @param argc number of arguments in argv
   * @param argv argument array, first entry is the commandname
   * @param errmsg error message set by this function if an error occured
   * @return 0 if successful, one of ack values if an error occured
   */
  int (*handler)(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx);
  int min_argc;
};

static struct mpd_command mpd_handlers[] =
  {
    /* commandname                | handler function                      | minimum argument count*/

    // Commands for querying status
    { "clearerror",                 mpd_command_ignore,                     -1 },
    { "currentsong",                mpd_command_currentsong,                -1 },
    { "idle",                       mpd_command_idle,                       -1 },
    { "noidle",                     mpd_command_noidle,                     -1 },
    { "status",                     mpd_command_status,                     -1 },
    { "stats",                      mpd_command_stats,                      -1 },

    // Playback options
    { "consume",                    mpd_command_consume,                     2 },
    { "crossfade",                  mpd_command_ignore,                     -1 },
    { "mixrampdb",                  mpd_command_ignore,                     -1 },
    { "mixrampdelay",               mpd_command_ignore,                     -1 },
    { "random",                     mpd_command_random,                      2 },
    { "repeat",                     mpd_command_repeat,                      2 },
    { "setvol",                     mpd_command_setvol,                      2 },
    { "single",                     mpd_command_single,                      2 },
    { "replay_gain_mode",           mpd_command_ignore,                     -1 },
    { "replay_gain_status",         mpd_command_replay_gain_status,         -1 },
    { "volume",                     mpd_command_volume,                      2 },

    // Controlling playback
    { "next",                       mpd_command_next,                       -1 },
    { "pause",                      mpd_command_pause,                      -1 },
    { "play",                       mpd_command_play,                       -1 },
    { "playid",                     mpd_command_playid,                     -1 },
    { "previous",                   mpd_command_previous,                   -1 },
    { "seek",                       mpd_command_seek,                        3 },
    { "seekid",                     mpd_command_seekid,                      3 },
    { "seekcur",                    mpd_command_seekcur,                     2 },
    { "stop",                       mpd_command_stop,                       -1 },

    // The current playlist
    { "add",                        mpd_command_add,                        -1 },
    { "addid",                      mpd_command_addid,                       2 },
    { "clear",                      mpd_command_clear,                      -1 },
    { "delete",                     mpd_command_delete,                     -1 },
    { "deleteid",                   mpd_command_deleteid,                    2 },
    { "move",                       mpd_command_move,                        3 },
    { "moveid",                     mpd_command_moveid,                      3 },
    { "playlist",                   mpd_command_playlistinfo,               -1 }, // According to the mpd protocol the use of "playlist" is deprecated
    { "playlistfind",               mpd_command_playlistfind,               -1 },
    { "playlistid",                 mpd_command_playlistid,                 -1 },
    { "playlistinfo",               mpd_command_playlistinfo,               -1 },
    { "playlistsearch",             mpd_command_playlistsearch,             -1 },
    { "plchanges",                  mpd_command_plchanges,                   2 },
    { "plchangesposid",             mpd_command_plchangesposid,              2 },
//    { "prio",                       mpd_command_prio,                       -1 },
//    { "prioid",                     mpd_command_prioid,                     -1 },
//    { "rangeid",                    mpd_command_rangeid,                    -1 },
//    { "shuffle",                    mpd_command_shuffle,                    -1 },
//    { "swap",                       mpd_command_swap,                       -1 },
//    { "swapid",                     mpd_command_swapid,                     -1 },
//    { "addtagid",                   mpd_command_addtagid,                   -1 },
//    { "cleartagid",                 mpd_command_cleartagid,                 -1 },

    // Stored playlists
    { "listplaylist",               mpd_command_listplaylist,               -1 },
    { "listplaylistinfo",           mpd_command_listplaylistinfo,           -1 },
    { "listplaylists",              mpd_command_listplaylists,              -1 },
    { "load",                       mpd_command_load,                       -1 },
    { "playlistadd",                mpd_command_playlistadd,                -1 },
//    { "playlistclear",              mpd_command_playlistclear,              -1 },
//    { "playlistdelete",             mpd_command_playlistdelete,             -1 },
//    { "playlistmove",               mpd_command_playlistmove,               -1 },
//    { "rename",                     mpd_command_rename,                     -1 },
    { "rm",                         mpd_command_rm,                          2 },
    { "save",                       mpd_command_save,                       -1 },

    // The music database
    { "albumart",                   mpd_command_albumart,                    2 },
    { "count",                      mpd_command_count,                      -1 },
    { "find",                       mpd_command_find,                       -1 },
    { "findadd",                    mpd_command_findadd,                    -1 },
    { "list",                       mpd_command_list,                       -1 },
    { "listall",                    mpd_command_listall,                    -1 },
    { "listallinfo",                mpd_command_listallinfo,                -1 },
    { "listfiles",                  mpd_command_listfiles,                  -1 },
    { "lsinfo",                     mpd_command_lsinfo,                     -1 },
//    { "readcomments",               mpd_command_readcomments,               -1 },
    { "readpicture",                mpd_command_albumart,                    2 },
    { "search",                     mpd_command_search,                     -1 },
    { "searchadd",                  mpd_command_searchadd,                  -1 },
//    { "searchaddpl",                mpd_command_searchaddpl,                -1 },
    { "update",                     mpd_command_update,                     -1 },
//    { "rescan",                     mpd_command_rescan,                     -1 },

    // Mounts and neighbors
//    { "mount",                      mpd_command_mount,                      -1 },
//    { "unmount",                    mpd_command_unmount,                    -1 },
//    { "listmounts",                 mpd_command_listmounts,                 -1 },
//    { "listneighbors",              mpd_command_listneighbors,              -1 },

    // Stickers
    { "sticker",                    mpd_command_sticker,                     4 },

    // Connection settings
    { "close",                      mpd_command_ignore,                     -1 },
//    { "kill",                       mpd_command_kill,                       -1 },
    { "password",                   mpd_command_password,                   -1 },
    { "ping",                       mpd_command_ignore,                     -1 },
    { "binarylimit",                mpd_command_binarylimit,                 2 },
    /* missing: tagtypes */

    // Audio output devices
    { "disableoutput",              mpd_command_disableoutput,               2 },
    { "enableoutput",               mpd_command_enableoutput,                2 },
    { "toggleoutput",               mpd_command_toggleoutput,                2 },
    { "outputs",                    mpd_command_outputs,                    -1 },

    // Reflection
//    { "config",                     mpd_command_config,                     -1 },
    { "commands",                   mpd_command_commands,                   -1 },
    { "notcommands",                mpd_command_ignore,                     -1 },
    { "tagtypes",                   mpd_command_tagtypes,                   -1 },
    { "urlhandlers",                mpd_command_urlhandlers,                -1 },
    { "decoders",                   mpd_command_decoders,                   -1 },

    // Client to client
    { "subscribe",                  mpd_command_ignore,                     -1 },
    { "unsubscribe",                mpd_command_ignore,                     -1 },
    { "channels",                   mpd_command_channels,                   -1 },
    { "readmessages",               mpd_command_ignore,                     -1 },
    { "sendmessage",                mpd_command_sendmessage,                -1 },

    // Custom commands (not supported by mpd)
    { "outputvolume",               mpd_command_outputvolume,                3 },

    // NULL command to terminate loop
    { NULL, NULL, -1 }
  };

/*
 * Finds the command handler for the given command name
 *
 * @param name the name of the command
 * @return the command or NULL if it is an unknown/unsupported command
 */
static struct mpd_command*
mpd_find_command(const char *name)
{
  int i;

  for (i = 0; mpd_handlers[i].handler; i++)
    {
      if (0 == strcmp(name, mpd_handlers[i].mpdcommand))
	{
	  return &mpd_handlers[i];
	}
    }

  return NULL;
}

static int
mpd_command_commands(struct evbuffer *evbuf, int argc, char **argv, char **errmsg, struct mpd_client_ctx *ctx)
{
  int i;

  for (i = 0; mpd_handlers[i].handler; i++)
    {
      evbuffer_add_printf(evbuf,
	  "command: %s\n",
	  mpd_handlers[i].mpdcommand);
    }

  return 0;
}


/*
 * The read callback function is invoked if a complete command sequence was received from the client
 * (see mpd_input_filter function).
 *
 * @param bev the buffer event
 * @param ctx used for authentication
 */
static void
mpd_read_cb(struct bufferevent *bev, void *ctx)
{
  struct evbuffer *input;
  struct evbuffer *output;
  int ret;
  int ncmd;
  char *line;
  char *errmsg;
  struct mpd_command *command;
  enum command_list_type listtype;
  int idle_cmd;
  int close_cmd;
  char *argv[COMMAND_ARGV_MAX];
  int argc;
  struct mpd_client_ctx *client_ctx = (struct mpd_client_ctx *)ctx;

  /* Get the input evbuffer, contains the command sequence received from the client */
  input = bufferevent_get_input(bev);
  /* Get the output evbuffer, used to send the server response to the client */
  output = bufferevent_get_output(bev);

  DPRINTF(E_SPAM, L_MPD, "Received MPD command sequence\n");

  idle_cmd = 0;
  close_cmd = 0;

  listtype = COMMAND_LIST_NONE;
  ncmd = 0;
  ret = -1;

  while ((line = evbuffer_readln(input, NULL, EVBUFFER_EOL_ANY)))
    {
      DPRINTF(E_DBG, L_MPD, "MPD message: %s\n", line);

      // Split the read line into command name and arguments
      ret = mpd_parse_args(line, &argc, argv, COMMAND_ARGV_MAX);
      if (ret != 0 || argc <= 0)
	{
	  // Error handling for argument parsing error
	  DPRINTF(E_LOG, L_MPD, "Error parsing arguments for MPD message: %s\n", line);
	  errmsg = safe_asprintf("Error parsing arguments");
	  ret = ACK_ERROR_ARG;
	  evbuffer_add_printf(output, "ACK [%d@%d] {%s} %s\n", ret, ncmd, "unkown", errmsg);
	  free(errmsg);
	  free(line);
	  break;
	}

      /*
       * Check if it is a list command
       */
      if (0 == strcmp(argv[0], "command_list_ok_begin"))
	{
	  listtype = COMMAND_LIST_OK;
	  free(line);
	  continue;
	}
      else if (0 == strcmp(argv[0], "command_list_begin"))
	{
	  listtype = COMMAND_LIST;
	  free(line);
	  continue;
	}
      else if (0 == strcmp(argv[0], "command_list_end"))
	{
	  listtype = COMMAND_LIST_END;
	  free(line);
	  break;
	}
      else if (0 == strcmp(argv[0], "idle"))
	idle_cmd = 1;
      else if (0 == strcmp(argv[0], "noidle"))
	idle_cmd = 1;
      else if (0 == strcmp(argv[0], "close"))
	close_cmd = 1;

      /*
       * Find the command handler and execute the command function
       */
      command = mpd_find_command(argv[0]);

      if (command == NULL)
	{
	  errmsg = safe_asprintf("Unsupported command '%s'", argv[0]);
	  ret = ACK_ERROR_UNKNOWN;
	}
      else if (command->min_argc > argc)
	{
	  errmsg = safe_asprintf("Missing argument(s) for command '%s', expected %d, given %d", argv[0], command->min_argc, argc);
	  ret = ACK_ERROR_ARG;
	}
      else if (strcmp(command->mpdcommand, "password") == 0)
	{
	  ret = command->handler(output, argc, argv, &errmsg, client_ctx);
	  client_ctx->authenticated = ret == 0;
	}
      else if (!client_ctx->authenticated)
	{
	  errmsg = safe_asprintf("Not authenticated");
	  ret = ACK_ERROR_PERMISSION;
	}
      else
	ret = command->handler(output, argc, argv, &errmsg, client_ctx);

      /*
       * If an error occurred, add the ACK line to the response buffer and exit the loop
       */
      if (ret != 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error executing command '%s': %s\n", argv[0], errmsg);
	  evbuffer_add_printf(output, "ACK [%d@%d] {%s} %s\n", ret, ncmd, argv[0], errmsg);
	  free(errmsg);
	  free(line);
	  break;
	}

      /*
       * If the command sequence started with command_list_ok_begin, add a list_ok line to the
       * response buffer after each command output.
       */
      if (listtype == COMMAND_LIST_OK)
	{
	  evbuffer_add(output, "list_OK\n", 8);
	}
      /*
       * If everything was successful add OK line to signal clients end of command message.
       */
      else if (listtype == COMMAND_LIST_NONE && idle_cmd == 0 && close_cmd == 0)
	{
	  evbuffer_add(output, "OK\n", 3);
	}
      free(line);
      ncmd++;
    }

  DPRINTF(E_SPAM, L_MPD, "Finished MPD command sequence: %d\n", ret);

  /*
   * If everything was successful and we are processing a command list, add OK line to signal
   * clients end of message.
   * If an error occurred the necessary ACK line should already be added to the response buffer.
   */
  if (ret == 0 && close_cmd == 0 && listtype == COMMAND_LIST_END)
    {
      evbuffer_add(output, "OK\n", 3);
    }

  if (close_cmd)
    {
      /*
       * Freeing the bufferevent closes the connection, if it was
       * opened with BEV_OPT_CLOSE_ON_FREE.
       * Since bufferevent is reference-counted, it will happen as
       * soon as possible, not necessarily immediately.
       */
      bufferevent_free(bev);
    }
}

/*
 * Callback when an event occurs on the bufferevent
 */
static void
mpd_event_cb(struct bufferevent *bev, short events, void *ctx)
{
  if (events & BEV_EVENT_ERROR)
    {
      DPRINTF(E_LOG, L_MPD, "Error from bufferevent: %s\n",
	  evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
      bufferevent_free(bev);
    }
}

/*
 * The input filter buffer callback checks if the data received from the client is a complete command sequence.
 * A command sequence has end with '\n' and if it starts with "command_list_begin\n" or "command_list_ok_begin\n"
 * the last line has to be "command_list_end\n".
 *
 * @param src evbuffer to read data from (contains the data received from the client)
 * @param dst evbuffer to write data to (this is the evbuffer for the read callback)
 * @param lim the upper bound of bytes to add to destination
 * @param state write mode
 * @param ctx (not used)
 * @return BEV_OK if a complete command sequence was received otherwise BEV_NEED_MORE
 */
static enum bufferevent_filter_result
mpd_input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t lim, enum bufferevent_flush_mode state, void *ctx)
{
  struct evbuffer_ptr p;
  char *line;
  int ret;

  while ((line = evbuffer_readln(src, NULL, EVBUFFER_EOL_ANY)))
    {
      ret = evbuffer_add_printf(dst, "%s\n", line);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MPD, "Error adding line to buffer: '%s'\n", line);
	  free(line);
	  return BEV_ERROR;
	}
      free(line);
    }

  if (evbuffer_get_length(src) > 0)
    {
      DPRINTF(E_DBG, L_MPD, "Message incomplete, waiting for more data\n");
      return BEV_NEED_MORE;
    }

  p = evbuffer_search(dst, "command_list_begin", 18, NULL);
  if (p.pos < 0)
    {
      p = evbuffer_search(dst, "command_list_ok_begin", 21, NULL);
    }

  if (p.pos >= 0)
    {
      p = evbuffer_search(dst, "command_list_end", 16, NULL);
      if (p.pos < 0)
	{
	  DPRINTF(E_DBG, L_MPD, "Message incomplete (missing command_list_end), waiting for more data\n");
	  return BEV_NEED_MORE;
	}
    }

  return BEV_OK;
}

/*
 * The connection listener callback function is invoked when a new connection was received.
 *
 * @param listener the connection listener that received the connection
 * @param sock the new socket
 * @param address the address from which the connection was received
 * @param socklen the length of that address
 * @param ctx (not used)
 */
static void
mpd_accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t sock, struct sockaddr *address, int socklen,
    void *ctx)
{
  /*
   * For each new connection setup a new buffer event and wrap it around a filter event.
   * The filter event ensures, that the read callback on the buffer event is only invoked if a complete
   * command sequence from the client was received.
   */
  struct event_base *base = evconnlistener_get_base(listener);
  struct bufferevent *bev = bufferevent_socket_new(base, sock, BEV_OPT_CLOSE_ON_FREE);
  struct mpd_client_ctx *client_ctx = calloc(1, sizeof(struct mpd_client_ctx));

  if (!client_ctx)
    {
      DPRINTF(E_LOG, L_MPD, "Out of memory for command context\n");
      bufferevent_free(bev);
      return;
    }

  client_ctx->authenticated = !cfg_getstr(cfg_getsec(cfg, "library"), "password");
  if (!client_ctx->authenticated)
    {
      client_ctx->authenticated = net_peer_address_is_trusted((union net_sockaddr *)address);
    }

  client_ctx->binarylimit = MPD_BINARY_SIZE;

  client_ctx->next = mpd_clients;
  mpd_clients = client_ctx;

  bev = bufferevent_filter_new(bev, mpd_input_filter, NULL, BEV_OPT_CLOSE_ON_FREE, free_mpd_client_ctx, client_ctx);
  bufferevent_setcb(bev, mpd_read_cb, NULL, mpd_event_cb, client_ctx);
  bufferevent_enable(bev, EV_READ | EV_WRITE);

  /*
   * According to the mpd protocol send "OK MPD <version>\n" to the client, where version is the version
   * of the supported mpd protocol and not the server version.
   */
  evbuffer_add(bufferevent_get_output(bev), "OK MPD 0.24.0\n", 14);
  client_ctx->evbuffer = bufferevent_get_output(bev);

  DPRINTF(E_INFO, L_MPD, "New mpd client connection accepted\n");
}

/*
 * Error callback that gets called whenever an accept() call fails on the listener
 * @param listener the connection listener that received the connection
 * @param ctx (not used)
 */
static void
mpd_accept_error_cb(struct evconnlistener *listener, void *ctx)
{
  int err;

  err = EVUTIL_SOCKET_ERROR();
  DPRINTF(E_LOG, L_MPD, "Error occured %d (%s) on the listener.\n", err, evutil_socket_error_to_string(err));
}

static int
mpd_notify_idle_client(struct mpd_client_ctx *client_ctx, short events)
{
  if (!client_ctx->is_idle)
    {
      client_ctx->events |= events;
      return 1;
    }

  if (!(client_ctx->idle_events & events))
    {
      DPRINTF(E_DBG, L_MPD, "Client not listening for events: %d\n", events);
      return 1;
    }

  if (events & LISTENER_DATABASE)
    evbuffer_add(client_ctx->evbuffer, "changed: database\n", 18);
  if (events & LISTENER_UPDATE)
    evbuffer_add(client_ctx->evbuffer, "changed: update\n", 16);
  if (events & LISTENER_QUEUE)
    evbuffer_add(client_ctx->evbuffer, "changed: playlist\n", 18);
  if (events & LISTENER_PLAYER)
    evbuffer_add(client_ctx->evbuffer, "changed: player\n", 16);
  if (events & LISTENER_VOLUME)
    evbuffer_add(client_ctx->evbuffer, "changed: mixer\n", 15);
  if (events & LISTENER_SPEAKER)
    evbuffer_add(client_ctx->evbuffer, "changed: output\n", 16);
  if (events & LISTENER_OPTIONS)
    evbuffer_add(client_ctx->evbuffer, "changed: options\n", 17);
  if (events & LISTENER_STORED_PLAYLIST)
    evbuffer_add(client_ctx->evbuffer, "changed: stored_playlist\n", 25);
  if (events & LISTENER_RATING)
    evbuffer_add(client_ctx->evbuffer, "changed: sticker\n", 17);

  evbuffer_add(client_ctx->evbuffer, "OK\n", 3);

  client_ctx->is_idle = false;
  client_ctx->idle_events = 0;
  client_ctx->events = 0;

  return 0;
}

static enum command_state
mpd_notify_idle(void *arg, int *retval)
{
  short event_mask;
  struct mpd_client_ctx *client;
  int i;

  event_mask = *(short *)arg;
  DPRINTF(E_DBG, L_MPD, "Notify clients waiting for idle results: %d\n", event_mask);

  i = 0;
  client = mpd_clients;
  while (client)
    {
      DPRINTF(E_DBG, L_MPD, "Notify client #%d\n", i);

      mpd_notify_idle_client(client, event_mask);
      client = client->next;
      i++;
    }

  *retval = 0;
  return COMMAND_END;
}

static void
mpd_listener_cb(short event_mask)
{
  short *ptr;

  ptr = (short *)malloc(sizeof(short));
  *ptr = event_mask;
  DPRINTF(E_DBG, L_MPD, "Asynchronous listener callback called with event type %d.\n", event_mask);
  commands_exec_async(cmdbase, mpd_notify_idle, ptr);
}

/*
 * Callback function that handles http requests for artwork files
 *
 * Some MPD clients allow retrieval of local artwork by making http request for artwork
 * files.
 *
 * A request for the artwork of an item with virtual path "file:/path/to/example.mp3" looks
 * like:
 * GET http://<host>:<port>/path/to/cover.jpg
 *
 * Artwork is found by taking the uri and removing everything after the last '/'. The first
 * item in the library with a virtual path that matches *path/to* is used to read the artwork
 * file through the default artwork logic.
 */
static void
artwork_cb(struct evhttp_request *req, void *arg)
{
  struct evbuffer *evbuffer;
  struct evhttp_uri *decoded;
  const char *uri;
  const char *path;
  char *decoded_path;
  char *last_slash;
  int itemid;
  int format;

  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET)
    {
      DPRINTF(E_LOG, L_MPD, "Unsupported request type for artwork\n");
      evhttp_send_error(req, HTTP_BADMETHOD, "Method not allowed");
      return;
    }

  uri = evhttp_request_get_uri(req);
  DPRINTF(E_DBG, L_MPD, "Got artwork request with uri '%s'\n", uri);

  decoded = evhttp_uri_parse(uri);
  if (!decoded)
    {
      DPRINTF(E_LOG, L_MPD, "Bad artwork request with uri '%s'\n", uri);
      evhttp_send_error(req, HTTP_BADREQUEST, 0);
      return;
    }

  path = evhttp_uri_get_path(decoded);
  if (!path)
    {
      DPRINTF(E_LOG, L_MPD, "Invalid path from artwork request with uri '%s'\n", uri);
      evhttp_send_error(req, HTTP_BADREQUEST, 0);
      evhttp_uri_free(decoded);
      return;
    }

  decoded_path = evhttp_uridecode(path, 0, NULL);
  if (!decoded_path)
    {
      DPRINTF(E_LOG, L_MPD, "Error decoding path from artwork request with uri '%s'\n", uri);
      evhttp_send_error(req, HTTP_BADREQUEST, 0);
      evhttp_uri_free(decoded);
      return;
    }

  last_slash = strrchr(decoded_path, '/');
  if (last_slash)
    *last_slash = '\0';

  DPRINTF(E_DBG, L_MPD, "Artwork request for path: %s\n", decoded_path);

  itemid = db_file_id_byvirtualpath_match(decoded_path);
  if (!itemid)
    {
      DPRINTF(E_WARN, L_MPD, "No item found for path '%s' from request uri '%s'\n", decoded_path, uri);
      evhttp_send_error(req, HTTP_NOTFOUND, "Document was not found");
      evhttp_uri_free(decoded);
      free(decoded_path);
      return;
    }

  evbuffer = evbuffer_new();
  if (!evbuffer)
    {
      DPRINTF(E_LOG, L_MPD, "Could not allocate an evbuffer for artwork request\n");
      evhttp_send_error(req, HTTP_INTERNAL, "Document was not found");
      evhttp_uri_free(decoded);
      free(decoded_path);
      return;
    }

  format = artwork_get_item(evbuffer, itemid, ART_DEFAULT_WIDTH, ART_DEFAULT_HEIGHT, 0);
  if (format < 0)
    {
      evhttp_send_error(req, HTTP_NOTFOUND, "Document was not found");
    }
  else
    {
      switch (format)
	{
	  case ART_FMT_PNG:
	    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "image/png");
	    break;

	  default:
	    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "image/jpeg");
	    break;
	}

      evhttp_send_reply(req, HTTP_OK, "OK", evbuffer);
    }

  evbuffer_free(evbuffer);
  evhttp_uri_free(decoded);
  free(decoded_path);
}

/* Thread: main */
static int
mpd_httpd_init(void)
{
  unsigned short http_port;
  int ret;

  http_port = cfg_getint(cfg_getsec(cfg, "mpd"), "http_port");
  if (http_port == 0)
    return 0;

  evhttpd = evhttp_new(evbase_mpd);
  if (!evhttpd)
    return -1;

  ret = net_evhttp_bind(evhttpd, http_port, "mpd artwork");
  if (ret < 0)
    {
      evhttp_free(evhttpd);
      evhttpd = NULL;
      return -1;
    }

  evhttp_set_gencb(evhttpd, artwork_cb, NULL);

  return 0;
}

/* Thread: main */
static void
mpd_httpd_deinit(void)
{
  if (evhttpd)
    evhttp_free(evhttpd);

  evhttpd = NULL;
}

/* Thread: main */
int
mpd_init(void)
{
  unsigned short port;
  const char *pl_dir;
  int ret;

  port = cfg_getint(cfg_getsec(cfg, "mpd"), "port");
  if (port <= 0)
    {
      DPRINTF(E_INFO, L_MPD, "MPD not enabled\n");
      return 0;
    }

  CHECK_NULL(L_MPD, evbase_mpd = event_base_new());
  CHECK_NULL(L_MPD, cmdbase = commands_base_new(evbase_mpd, NULL));

  mpd_sockfd = net_bind(&port, SOCK_STREAM, "mpd");
  if (mpd_sockfd < 0)
    {
      DPRINTF(E_LOG, L_MPD, "Could not bind mpd server to port %hu\n", port);
      goto bind_fail;
    }

  mpd_listener = evconnlistener_new(evbase_mpd, mpd_accept_conn_cb, NULL, 0, -1, mpd_sockfd);
  if (!mpd_listener)
    {
      DPRINTF(E_LOG, L_MPD, "Could not create connection listener for mpd clients on port %d\n", port);
      goto connew_fail;
    }
  evconnlistener_set_error_cb(mpd_listener, mpd_accept_error_cb);

  ret = mpd_httpd_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MPD, "Could not initialize HTTP artwork server\n");
      goto httpd_fail;
    }

  allow_modifying_stored_playlists = cfg_getbool(cfg_getsec(cfg, "library"), "allow_modifying_stored_playlists");
  pl_dir = cfg_getstr(cfg_getsec(cfg, "library"), "default_playlist_directory");
  if (pl_dir)
    default_pl_dir = safe_asprintf("/file:%s", pl_dir);

  mpd_plugin_httpd = cfg_getbool(cfg_getsec(cfg, "mpd"), "enable_httpd_plugin");

  /* Handle deprecated config options */
  if (0 < cfg_opt_size(cfg_getopt(cfg_getsec(cfg, "mpd"), "allow_modifying_stored_playlists")))
    {
      DPRINTF(E_LOG, L_MPD, "Found deprecated option 'allow_modifying_stored_playlists' in section 'mpd', please update configuration file (move option to section 'library').\n");
      allow_modifying_stored_playlists = cfg_getbool(cfg_getsec(cfg, "mpd"), "allow_modifying_stored_playlists");
    }
  if (0 < cfg_opt_size(cfg_getopt(cfg_getsec(cfg, "mpd"), "default_playlist_directory")))
    {
      DPRINTF(E_LOG, L_MPD, "Found deprecated option 'default_playlist_directory' in section 'mpd', please update configuration file (move option to section 'library').\n");
      free(default_pl_dir);
      pl_dir = cfg_getstr(cfg_getsec(cfg, "mpd"), "default_playlist_directory");
      if (pl_dir)
        default_pl_dir = safe_asprintf("/file:%s", pl_dir);
    }

  DPRINTF(E_INFO, L_MPD, "mpd thread init\n");

  ret = pthread_create(&tid_mpd, NULL, mpd, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MPD, "Could not spawn MPD thread: %s\n", strerror(errno));

      goto thread_fail;
    }

  thread_setname(tid_mpd, "mpd");

  mpd_clients = NULL;
  listener_add(mpd_listener_cb, MPD_ALL_IDLE_LISTENER_EVENTS);

  return 0;

 thread_fail:
  mpd_httpd_deinit();
 httpd_fail:
  evconnlistener_free(mpd_listener);
 connew_fail:
  close(mpd_sockfd);
 bind_fail:
  commands_base_free(cmdbase);
  event_base_free(evbase_mpd);
  evbase_mpd = NULL;

  return -1;
}

/* Thread: main */
void
mpd_deinit(void)
{
  unsigned short port;
  int ret;

  port = cfg_getint(cfg_getsec(cfg, "mpd"), "port");
  if (port <= 0)
    {
      DPRINTF(E_INFO, L_MPD, "MPD not enabled\n");
      return;
    }

  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_mpd, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MPD, "Could not join MPD thread: %s\n", strerror(errno));
      return;
    }

  listener_remove(mpd_listener_cb);

  while (mpd_clients)
    {
      free_mpd_client_ctx(mpd_clients);
    }

  mpd_httpd_deinit();

  evconnlistener_free(mpd_listener);

  close(mpd_sockfd);

  // Free event base (should free events too)
  event_base_free(evbase_mpd);

  free(default_pl_dir);
}
