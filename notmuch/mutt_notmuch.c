/**
 * @file
 * Notmuch virtual mailbox type
 *
 * @authors
 * Copyright (C) 2011-2016 Karel Zak <kzak@redhat.com>
 * Copyright (C) 2016-2018 Richard Russon <rich@flatcap.org>
 * Copyright (C) 2016 Kevin Velghe <kevin@paretje.be>
 * Copyright (C) 2017 Bernard 'Guyzmo' Pratz <guyzmo+github+pub@m0g.net>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ## Notes
 *
 * - notmuch uses private Mailbox->data and private Email->data
 *
 * - all exported functions are usable within notmuch context only
 *
 * - all functions have to be covered by "mailbox->magic == MUTT_NOTMUCH" check
 *   (it's implemented in get_mboxdata() and init_mailbox() functions).
 *
 * - exception are nm_nonctx_* functions -- these functions use nm_default_uri
 *   (or parse URI from another resource)
 */

/**
 * @page nm_notmuch Notmuch virtual mailbox type
 *
 * Notmuch virtual mailbox type
 */

#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <notmuch.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "config/lib.h"
#include "email/lib.h"
#include "mutt.h"
#include "mutt_notmuch.h"
#include "context.h"
#include "curs_lib.h"
#include "curs_main.h"
#include "globals.h"
#include "mailbox.h"
#include "maildir/maildir.h"
#include "mutt_logging.h"
#include "mutt_thread.h"
#include "mx.h"
#include "progress.h"
#include "protos.h"

/* These Config Variables are only used in notmuch/mutt_notmuch.c */
int NmDbLimit;       ///< Config: (notmuch) Default limit for Notmuch queries
char *NmDefaultUri;  ///< Config: (notmuch) Path to the Notmuch database
char *NmExcludeTags; ///< Config: (notmuch) Exclude messages with these tags
int NmOpenTimeout;   ///< Config: (notmuch) Database timeout
char *NmQueryType; ///< Config: (notmuch) Default query type: 'threads' or 'messages'
int NmQueryWindowCurrentPosition; ///< Config: (notmuch) Position of current search window
char *NmQueryWindowTimebase; ///< Config: (notmuch) Units for the time duration
char *NmRecordTags; ///< Config: (notmuch) Tags to apply to the 'record' mailbox (sent mail)
char *NmUnreadTag; ///< Config: (notmuch) Tag to use for unread messages

#ifdef LIBNOTMUCH_CHECK_VERSION
#undef LIBNOTMUCH_CHECK_VERSION
#endif

/* @def The definition in <notmuch.h> is broken */
#define LIBNOTMUCH_CHECK_VERSION(major, minor, micro)                             \
  (LIBNOTMUCH_MAJOR_VERSION > (major) ||                                          \
   (LIBNOTMUCH_MAJOR_VERSION == (major) && LIBNOTMUCH_MINOR_VERSION > (minor)) || \
   (LIBNOTMUCH_MAJOR_VERSION == (major) &&                                        \
    LIBNOTMUCH_MINOR_VERSION == (minor) && LIBNOTMUCH_MICRO_VERSION >= (micro)))

/**
 * enum NmQueryType - Notmuch Query Types
 *
 * Read whole-thread or matching messages only?
 */
enum NmQueryType
{
  NM_QUERY_TYPE_MESGS = 1, /**< Default: Messages only */
  NM_QUERY_TYPE_THREADS    /**< Whole threads */
};

/**
 * struct NmEmailData - Notmuch data attached to an Email - @extends Email
 */
struct NmEmailData
{
  char *folder; /**< Location of the Email */
  char *oldpath;
  char *virtual_id;       /**< Unique Notmuch Id */
  enum MailboxType magic; /**< Type of Mailbox the Email is in */
};

/**
 * struct NmMboxData - Notmuch data attached to a Mailbox - @extends Mailbox
 *
 * This stores the global Notmuch data, such as the database connection.
 */
struct NmMboxData
{
  notmuch_database_t *db;

  struct Url db_url;   /**< Parsed view url of the Notmuch database */
  char *db_url_holder; /**< The storage string used by db_url, we keep it
                        *   to be able to free db_url */
  char *db_query;      /**< Previous query */
  int db_limit;        /**< Maximum number of results to return */
  enum NmQueryType query_type; /**< Messages or Threads */

  struct Progress progress; /**< A progress bar */
  int oldmsgcount;
  int ignmsgcount; /**< Ignored messages */

  bool noprogress : 1;     /**< Don't show the progress bar */
  bool longrun : 1;        /**< A long-lived action is in progress */
  bool trans : 1;          /**< Atomic transaction in progress */
  bool progress_ready : 1; /**< A progress bar has been initialised */
};

/**
 * free_emaildata - Free data attached to an Email
 * @param data Email data
 *
 * Each email has an attached NmEmailData, which contains things like the tags
 * (labels).
 */
static void free_emaildata(void **data)
{
  if (!data || !*data)
    return;

  struct NmEmailData *edata = *data;
  mutt_debug(2, "nm: freeing email %p\n", (void *) edata);
  FREE(&edata->folder);
  FREE(&edata->oldpath);
  FREE(&edata->virtual_id);
  FREE(data);
}

/**
 * new_emaildata - Create a new NmEmailData for an email
 * @retval ptr New NmEmailData struct
 */
static struct NmEmailData *new_emaildata(void)
{
  return mutt_mem_calloc(1, sizeof(struct NmEmailData));
}

/**
 * free_mboxdata - Free data attached to the Mailbox
 * @param data Notmuch data
 *
 * The NmMboxData struct stores global Notmuch data, such as the connection to
 * the database.  This function will close the database, free the resources and
 * the struct itself.
 */
static void free_mboxdata(void **data)
{
  if (!data || !*data)
    return;

  mutt_debug(1, "nm: freeing context data %p\n", data);

  struct NmMboxData *mdata = *data;

  if (mdata->db)
#ifdef NOTMUCH_API_3
    notmuch_database_destroy(mdata->db);
#else
    notmuch_database_close(mdata->db);
#endif
  mdata->db = NULL;

  url_free(&mdata->db_url);
  FREE(&mdata->db_url_holder);
  FREE(&mdata->db_query);
  FREE(data);
}

/**
 * string_to_query_type - Lookup a query type
 * @param str String to lookup
 * @retval num Query type, e.g. #NM_QUERY_TYPE_MESGS
 */
static enum NmQueryType string_to_query_type(const char *str)
{
  if (mutt_str_strcmp(str, "threads") == 0)
    return NM_QUERY_TYPE_THREADS;
  else if (mutt_str_strcmp(str, "messages") == 0)
    return NM_QUERY_TYPE_MESGS;

  mutt_error(_("failed to parse notmuch query type: %s"), NONULL(str));
  return NM_QUERY_TYPE_MESGS;
}

/**
 * new_mboxdata - Create a new NmMboxData object from a query
 * @param uri Notmuch query string
 * @retval ptr New NmMboxData struct
 *
 * A new NmMboxData struct is created, then the query is parsed and saved
 * within it.  This should be freed using free_mboxdata().
 */
static struct NmMboxData *new_mboxdata(const char *uri)
{
  if (!uri)
    return NULL;

  struct NmMboxData *mdata = mutt_mem_calloc(1, sizeof(struct NmMboxData));
  mutt_debug(1, "nm: initialize mailbox mdata %p\n", (void *) mdata);

  mdata->db_limit = NmDbLimit;
  mdata->query_type = string_to_query_type(NmQueryType);
  mdata->db_url_holder = mutt_str_strdup(uri);

  if (url_parse(&mdata->db_url, mdata->db_url_holder) < 0)
  {
    mutt_error(_("failed to parse notmuch uri: %s"), uri);
    FREE(&mdata);
    return NULL;
  }
  return mdata;
}

/**
 * nm_get_default_data - Create a Mailbox with default Notmuch settings
 * @retval ptr  Mailbox with default Notmuch settings
 * @retval NULL Error, it's impossible to create an NmMboxData
 */
struct NmMboxData *nm_get_default_data(void)
{
  // path to DB + query + URI "decoration"
  char uri[PATH_MAX + LONG_STRING + 32];

  // Try to use NmDefaultUri or Folder.
  // If neither are set, it is impossible to create a Notmuch URI.
  if (NmDefaultUri)
    snprintf(uri, sizeof(uri), "%s", NmDefaultUri);
  else if (Folder)
    snprintf(uri, sizeof(uri), "notmuch://%s", Folder);
  else
    return NULL;

  return new_mboxdata(uri);
}

/**
 * get_mboxdata - Get the Notmuch data
 * @param mailbox Mailbox
 * @retval ptr  Success
 * @retval NULL Failure, not a Notmuch mailbox
 */
static struct NmMboxData *get_mboxdata(struct Mailbox *mailbox)
{
  if (mailbox && (mailbox->magic == MUTT_NOTMUCH))
    return mailbox->data;

  return NULL;
}

/**
 * init_mailbox - Add Notmuch data to the Mailbox
 * @param mailbox Mailbox
 * @retval  0 Success
 * @retval -1 Error Bad format
 *
 * Create a new NmMboxData struct and add it Mailbox::data.
 * Notmuch-specific data will be stored in this struct.
 * This struct can be freed using free_emaildata().
 */
static int init_mailbox(struct Mailbox *mailbox)
{
  if (!mailbox || (mailbox->magic != MUTT_NOTMUCH))
    return -1;

  if (mailbox->data)
    return 0;

  mailbox->data = new_mboxdata(mailbox->path);
  if (!mailbox->data)
    return -1;

  mailbox->free_data = free_mboxdata;
  return 0;
}

/**
 * email_get_id - Get the unique Notmuch Id
 * @param e Email
 * @retval ptr  ID string
 * @retval NULL Error
 */
static char *email_get_id(struct Email *e)
{
  return (e && e->data) ? ((struct NmEmailData *) e->data)->virtual_id : NULL;
}

/**
 * email_get_fullpath - Get the full path of an email
 * @param e      Email
 * @param buf    Buffer for the path
 * @param buflen Length of the buffer
 * @retval ptr Path string
 */
static char *email_get_fullpath(struct Email *e, char *buf, size_t buflen)
{
  snprintf(buf, buflen, "%s/%s", nm_email_get_folder(e), e->path);
  return buf;
}

/**
 * query_type_to_string - Turn a query type into a string
 * @param query_type Query type
 * @retval ptr String
 *
 * @note This is a static string and must not be freed.
 */
static const char *query_type_to_string(enum NmQueryType query_type)
{
  if (query_type == NM_QUERY_TYPE_THREADS)
    return "threads";
  else
    return "messages";
}

/**
 * query_window_check_timebase - Checks if a given timebase string is valid
 * @param[in] timebase: string containing a time base
 * @retval true if the given time base is valid
 *
 * This function returns whether a given timebase string is valid or not,
 * which is used to validate the user settable configuration setting:
 *
 *     nm_query_window_timebase
 */
static bool query_window_check_timebase(const char *timebase)
{
  if ((strcmp(timebase, "hour") == 0) || (strcmp(timebase, "day") == 0) ||
      (strcmp(timebase, "week") == 0) || (strcmp(timebase, "month") == 0) ||
      (strcmp(timebase, "year") == 0))
  {
    return true;
  }
  return false;
}

/**
 * query_window_reset - Restore vfolder's search window to its original position
 *
 * After moving a vfolder search window backward and forward, calling this function
 * will reset the search position to its original value, setting to 0 the user settable
 * variable:
 *
 *     nm_query_window_current_position
 */
static void query_window_reset(void)
{
  mutt_debug(2, "entering\n");
  cs_str_native_set(Config, "nm_query_window_current_position", 0, NULL);
}

/**
 * windowed_query_from_query - transforms a vfolder search query into a windowed one
 * @param[in]  query vfolder search string
 * @param[out] buf   allocated string buffer to receive the modified search query
 * @param[in]  buflen allocated maximum size of the buf string buffer
 * @retval true  Transformed search query is available as a string in buf
 * @retval false Search query shall not be transformed
 *
 * This is where the magic of windowed queries happens. Taking a vfolder search
 * query string as parameter, it will use the following two user settings:
 *
 * - `nm_query_window_duration` and
 * - `nm_query_window_timebase`
 *
 * to amend given vfolder search window. Then using a third parameter:
 *
 * - `nm_query_window_current_position`
 *
 * it will generate a proper notmuch `date:` parameter. For example, given a
 * duration of `2`, a timebase set to `week` and a position defaulting to `0`,
 * it will prepend to the 'tag:inbox' notmuch search query the following string:
 *
 * - `query`: `tag:inbox`
 * - `buf`:   `date:2week..now and tag:inbox`
 *
 * If the position is set to `4`, with `duration=3` and `timebase=month`:
 *
 * - `query`: `tag:archived`
 * - `buf`:   `date:12month..9month and tag:archived`
 *
 * The window won't be applied:
 *
 * - If the duration of the search query is set to `0` this function will be disabled.
 * - If the timebase is invalid, it will show an error message and do nothing.
 *
 * If there's no search registered in `nm_query_window_current_search` or this is
 * a new search, it will reset the window and do the search.
 */
static bool windowed_query_from_query(const char *query, char *buf, size_t buflen)
{
  mutt_debug(2, "nm: %s\n", query);

  int beg = NmQueryWindowDuration * (NmQueryWindowCurrentPosition + 1);
  int end = NmQueryWindowDuration * NmQueryWindowCurrentPosition;

  /* if the duration is a non positive integer, disable the window */
  if (NmQueryWindowDuration <= 0)
  {
    query_window_reset();
    return false;
  }

  /* if the query has changed, reset the window position */
  if (!NmQueryWindowCurrentSearch || (strcmp(query, NmQueryWindowCurrentSearch) != 0))
    query_window_reset();

  if (!query_window_check_timebase(NmQueryWindowTimebase))
  {
    mutt_message(_("Invalid nm_query_window_timebase value (valid values are: "
                   "hour, day, week, month or year)"));
    mutt_debug(2, "Invalid nm_query_window_timebase value\n");
    return false;
  }

  if (end == 0)
  {
    // Open-ended date allows mail from the future.
    // This may occur is the sender's time settings are off.
    snprintf(buf, buflen, "date:%d%s.. and %s", beg, NmQueryWindowTimebase,
             NmQueryWindowCurrentSearch);
  }
  else
  {
    snprintf(buf, buflen, "date:%d%s..%d%s and %s", beg, NmQueryWindowTimebase,
             end, NmQueryWindowTimebase, NmQueryWindowCurrentSearch);
  }

  mutt_debug(2, "nm: %s -> %s\n", query, buf);

  return true;
}

/**
 * get_query_string - builds the notmuch vfolder search string
 * @param mdata Notmuch Mailbox data
 * @param window If true enable application of the window on the search string
 * @retval ptr  String containing a notmuch search query
 * @retval NULL If none can be generated
 *
 * This function parses the internal representation of a search, and returns
 * a search query string ready to be fed to the notmuch API, given the search
 * is valid.
 *
 * @note The window parameter here is here to decide contextually whether we
 * want to return a search query with window applied (for the actual search
 * result in mailbox) or not (for the count in the sidebar). It is not aimed at
 * enabling/disabling the feature.
 */
static char *get_query_string(struct NmMboxData *mdata, bool window)
{
  mutt_debug(2, "nm: %s\n", window ? "true" : "false");

  if (!mdata)
    return NULL;
  if (mdata->db_query)
    return mdata->db_query;

  mdata->query_type = string_to_query_type(NmQueryType); /* user's default */

  struct UrlQueryString *item = NULL;
  STAILQ_FOREACH(item, &mdata->db_url.query_strings, entries)
  {
    if (!item->value || !item->name)
      continue;

    if (strcmp(item->name, "limit") == 0)
    {
      if (mutt_str_atoi(item->value, &mdata->db_limit))
        mutt_error(_("failed to parse notmuch limit: %s"), item->value);
    }
    else if (strcmp(item->name, "type") == 0)
      mdata->query_type = string_to_query_type(item->value);

    else if (strcmp(item->name, "query") == 0)
      mdata->db_query = mutt_str_strdup(item->value);
  }

  if (!mdata->db_query)
    return NULL;

  if (window)
  {
    char buf[LONG_STRING];
    mutt_str_replace(&NmQueryWindowCurrentSearch, mdata->db_query);

    /* if a date part is defined, do not apply windows (to avoid the risk of
     * having a non-intersected date frame). A good improvement would be to
     * accept if they intersect
     */
    if (!strstr(mdata->db_query, "date:") &&
        windowed_query_from_query(mdata->db_query, buf, sizeof(buf)))
    {
      mdata->db_query = mutt_str_strdup(buf);
    }

    mutt_debug(2, "nm: query (windowed) '%s'\n", mdata->db_query);
  }
  else
    mutt_debug(2, "nm: query '%s'\n", mdata->db_query);

  return mdata->db_query;
}

/**
 * get_limit - Get the database limit
 * @param mdata Notmuch Mailbox data
 * @retval num Current limit
 */
static int get_limit(struct NmMboxData *mdata)
{
  return mdata ? mdata->db_limit : 0;
}

/**
 * get_db_filename - Get the filename of the Notmuch database
 * @param mdata Notmuch Mailbox data
 * @retval ptr Filename
 *
 * @note The return value is a pointer into the NmDefaultUri global variable.
 *       If that variable changes, the result will be invalid.
 *       It must not be freed.
 */
static const char *get_db_filename(struct NmMboxData *mdata)
{
  if (!mdata)
    return NULL;

  char *db_filename = mdata->db_url.path ? mdata->db_url.path : NmDefaultUri;
  if (!db_filename)
    db_filename = Folder;
  if (!db_filename)
    return NULL;
  if (strncmp(db_filename, "notmuch://", 10) == 0)
    db_filename += 10;

  mutt_debug(2, "nm: db filename '%s'\n", db_filename);
  return db_filename;
}

/**
 * do_database_open - Open a Notmuch database
 * @param filename Database filename
 * @param writable Read/write?
 * @param verbose  Show errors on failure?
 * @retval ptr Notmuch database
 */
static notmuch_database_t *do_database_open(const char *filename, bool writable, bool verbose)
{
  notmuch_database_t *db = NULL;
  int ct = 0;
  notmuch_status_t st = NOTMUCH_STATUS_SUCCESS;
#if LIBNOTMUCH_CHECK_VERSION(4, 2, 0)
  char *msg = NULL;
#endif

  mutt_debug(1, "nm: db open '%s' %s (timeout %d)\n", filename,
             writable ? "[WRITE]" : "[READ]", NmOpenTimeout);

  const notmuch_database_mode_t mode =
      writable ? NOTMUCH_DATABASE_MODE_READ_WRITE : NOTMUCH_DATABASE_MODE_READ_ONLY;

  const struct timespec wait = {
    .tv_sec = 0, .tv_nsec = 500000000, /* Half a second */
  };

  do
  {
#if LIBNOTMUCH_CHECK_VERSION(4, 2, 0)
    st = notmuch_database_open_verbose(filename, mode, &db, &msg);
#elif defined(NOTMUCH_API_3)
    st = notmuch_database_open(filename, mode, &db);
#else
    db = notmuch_database_open(filename, mode);
#endif
    if ((st == NOTMUCH_STATUS_FILE_ERROR) || db || !NmOpenTimeout || ((ct / 2) > NmOpenTimeout))
      break;

    if (verbose && ct && ((ct % 2) == 0))
      mutt_error(_("Waiting for notmuch DB... (%d sec)"), ct / 2);
    nanosleep(&wait, NULL);
    ct++;
  } while (true);

  if (verbose)
  {
    if (!db)
    {
#if LIBNOTMUCH_CHECK_VERSION(4, 2, 0)
      if (msg)
      {
        mutt_error(msg);
        FREE(&msg);
      }
      else
#endif
      {
        mutt_error(_("Cannot open notmuch database: %s: %s"), filename,
                   st ? notmuch_status_to_string(st) : _("unknown reason"));
      }
    }
    else if (ct > 1)
    {
      mutt_clear_error();
    }
  }
  return db;
}

/**
 * get_db - Get the Notmuch database
 * @param mdata Notmuch Mailbox data
 * @param writable Read/write?
 * @retval ptr Notmuch database
 */
static notmuch_database_t *get_db(struct NmMboxData *mdata, bool writable)
{
  if (!mdata)
    return NULL;
  if (mdata->db)
    return mdata->db;

  const char *db_filename = get_db_filename(mdata);
  if (db_filename)
    mdata->db = do_database_open(db_filename, writable, true);

  return mdata->db;
}

/**
 * release_db - Close the Notmuch database
 * @param mdata Notmuch Mailbox data
 * @retval  0 Success
 * @retval -1 Failure
 */
static int release_db(struct NmMboxData *mdata)
{
  if (!mdata || !mdata->db)
    return -1;

  mutt_debug(1, "nm: db close\n");
#ifdef NOTMUCH_API_3
  notmuch_database_destroy(mdata->db);
#else
  notmuch_database_close(mdata->db);
#endif
  mdata->db = NULL;
  mdata->longrun = false;
  return 0;
}

/**
 * db_trans_begin - Start a Notmuch database transaction
 * @param mdata Notmuch Mailbox data
 * @retval <0 error
 * @retval 1  new transaction started
 * @retval 0  already within transaction
 */
static int db_trans_begin(struct NmMboxData *mdata)
{
  if (!mdata || !mdata->db)
    return -1;

  if (mdata->trans)
    return 0;

  mutt_debug(2, "nm: db trans start\n");
  if (notmuch_database_begin_atomic(mdata->db))
    return -1;
  mdata->trans = true;
  return 1;
}

/**
 * db_trans_end - End a database transaction
 * @param mdata Notmuch Mailbox data
 * @retval  0 Success
 * @retval -1 Failure
 */
static int db_trans_end(struct NmMboxData *mdata)
{
  if (!mdata || !mdata->db)
    return -1;

  if (!mdata->trans)
    return 0;

  mutt_debug(2, "nm: db trans end\n");
  mdata->trans = false;
  if (notmuch_database_end_atomic(mdata->db))
    return -1;

  return 0;
}

/**
 * is_longrun - Is Notmuch in the middle of a long-running transaction
 * @param mdata Notmuch Mailbox data
 * @retval true if it is
 */
static bool is_longrun(struct NmMboxData *mdata)
{
  return mdata && mdata->longrun;
}

/**
 * get_database_mtime - Get the database modification time
 * @param[in]  mdata Struct holding database info
 * @param[out] mtime Save the modification time
 * @retval  0 Success (result in mtime)
 * @retval -1 Error
 *
 * Get the "mtime" (modification time) of the database file.
 * This is the time of the last update.
 */
static int get_database_mtime(struct NmMboxData *mdata, time_t *mtime)
{
  if (!mdata)
    return -1;

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/.notmuch/xapian", get_db_filename(mdata));
  mutt_debug(2, "nm: checking '%s' mtime\n", path);

  struct stat st;
  if (stat(path, &st))
    return -1;

  if (mtime)
    *mtime = st.st_mtime;

  return 0;
}

/**
 * apply_exclude_tags - Exclude the configured tags
 * @param query Notmuch query
 */
static void apply_exclude_tags(notmuch_query_t *query)
{
  if (!NmExcludeTags || !*NmExcludeTags)
    return;

  char *end = NULL, *tag = NULL;

  char *buf = mutt_str_strdup(NmExcludeTags);

  for (char *p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;
    *end = '\0';

    mutt_debug(2, "nm: query exclude tag '%s'\n", tag);
    notmuch_query_add_tag_exclude(query, tag);
    end = NULL;
    tag = NULL;
  }
  notmuch_query_set_omit_excluded(query, 1);
  FREE(&buf);
}

/**
 * get_query - Create a new query
 * @param mdata Notmuch Mailbox data
 * @param writable Should the query be updateable?
 * @retval ptr  Notmuch query
 * @retval NULL Error
 */
static notmuch_query_t *get_query(struct NmMboxData *mdata, bool writable)
{
  if (!mdata)
    return NULL;

  notmuch_database_t *db = get_db(mdata, writable);
  const char *str = get_query_string(mdata, true);

  if (!db || !str)
    goto err;

  notmuch_query_t *q = notmuch_query_create(db, str);
  if (!q)
    goto err;

  apply_exclude_tags(q);
  notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);
  mutt_debug(2, "nm: query successfully initialized (%s)\n", str);
  return q;
err:
  if (!is_longrun(mdata))
    release_db(mdata);
  return NULL;
}

/**
 * update_email_tags - Update the Email's tags from Notmuch
 * @param e   Email
 * @param msg Notmuch message
 * @retval 0 Success
 * @retval 1 Tags unchanged
 */
static int update_email_tags(struct Email *e, notmuch_message_t *msg)
{
  struct NmEmailData *edata = e->data;
  char *new_tags = NULL;
  char *old_tags = NULL;

  mutt_debug(2, "nm: tags update requested (%s)\n", edata->virtual_id);

  for (notmuch_tags_t *tags = notmuch_message_get_tags(msg);
       tags && notmuch_tags_valid(tags); notmuch_tags_move_to_next(tags))
  {
    const char *t = notmuch_tags_get(tags);
    if (!t || !*t)
      continue;

    mutt_str_append_item(&new_tags, t, ' ');
  }

  old_tags = driver_tags_get(&e->tags);

  if (new_tags && old_tags && (strcmp(old_tags, new_tags) == 0))
  {
    FREE(&old_tags);
    FREE(&new_tags);
    mutt_debug(2, "nm: tags unchanged\n");
    return 1;
  }

  /* new version */
  driver_tags_replace(&e->tags, new_tags);
  FREE(&new_tags);

  new_tags = driver_tags_get_transformed(&e->tags);
  mutt_debug(2, "nm: new tags: '%s'\n", new_tags);
  FREE(&new_tags);

  new_tags = driver_tags_get(&e->tags);
  mutt_debug(2, "nm: new tag transforms: '%s'\n", new_tags);
  FREE(&new_tags);

  return 0;
}

/**
 * update_message_path - Set the path for a message
 * @param e    Email
 * @param path Path
 * @retval 0 Success
 * @retval 1 Failure
 */
static int update_message_path(struct Email *e, const char *path)
{
  struct NmEmailData *edata = e->data;

  mutt_debug(2, "nm: path update requested path=%s, (%s)\n", path, edata->virtual_id);

  char *p = strrchr(path, '/');
  if (p && ((p - path) > 3) &&
      ((strncmp(p - 3, "cur", 3) == 0) || (strncmp(p - 3, "new", 3) == 0) ||
       (strncmp(p - 3, "tmp", 3) == 0)))
  {
    edata->magic = MUTT_MAILDIR;

    FREE(&e->path);
    FREE(&edata->folder);

    p -= 3; /* skip subfolder (e.g. "new") */
    e->path = mutt_str_strdup(p);

    for (; (p > path) && (*(p - 1) == '/'); p--)
      ;

    edata->folder = mutt_str_substr_dup(path, p);

    mutt_debug(2, "nm: folder='%s', file='%s'\n", edata->folder, e->path);
    return 0;
  }

  return 1;
}

/**
 * get_folder_from_path - Find an email's folder from its path
 * @param path Path
 * @retval ptr  Path string
 * @retval NULL Error
 */
static char *get_folder_from_path(const char *path)
{
  char *p = strrchr(path, '/');

  if (p && ((p - path) > 3) &&
      ((strncmp(p - 3, "cur", 3) == 0) || (strncmp(p - 3, "new", 3) == 0) ||
       (strncmp(p - 3, "tmp", 3) == 0)))
  {
    p -= 3;
    for (; (p > path) && (*(p - 1) == '/'); p--)
      ;

    return mutt_str_substr_dup(path, p);
  }

  return NULL;
}

/**
 * nm2mutt_message_id - converts notmuch message Id to neomutt message Id
 * @param id Notmuch ID to convert
 * @retval ptr NeoMutt message ID
 *
 * Caller must free the NeoMutt Message ID
 */
static char *nm2mutt_message_id(const char *id)
{
  size_t sz;
  char *mid = NULL;

  if (!id)
    return NULL;
  sz = strlen(id) + 3;
  mid = mutt_mem_malloc(sz);

  snprintf(mid, sz, "<%s>", id);
  return mid;
}

/**
 * init_email - Set up an email's Notmuch data
 * @param e    Email
 * @param path Path to email
 * @param msg  Notmuch message
 * @retval  0 Success
 * @retval -1 Failure
 */
static int init_email(struct Email *e, const char *path, notmuch_message_t *msg)
{
  if (e->data)
    return 0;

  struct NmEmailData *edata = new_emaildata();
  e->data = edata;
  e->free_data = free_emaildata;

  /* Notmuch ensures that message Id exists (if not notmuch Notmuch will
   * generate an ID), so it's more safe than use neomutt Email->env->id
   */
  const char *id = notmuch_message_get_message_id(msg);
  edata->virtual_id = mutt_str_strdup(id);

  mutt_debug(2, "nm: [e=%p, data=%p] (%s)\n", (void *) e, (void *) e->data, id);

  if (!e->env->message_id)
    e->env->message_id = nm2mutt_message_id(id);

  if (update_message_path(e, path) != 0)
    return -1;

  update_email_tags(e, msg);

  return 0;
}

/**
 * get_message_last_filename - Get a message's last filename
 * @param msg Notmuch message
 * @retval ptr  Filename
 * @retval NULL Error
 */
static const char *get_message_last_filename(notmuch_message_t *msg)
{
  const char *name = NULL;

  for (notmuch_filenames_t *ls = notmuch_message_get_filenames(msg);
       ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
  {
    name = notmuch_filenames_get(ls);
  }

  return name;
}

/**
 * progress_reset - Reset the progress counter
 * @param mailbox Mailbox
 */
static void progress_reset(struct Mailbox *mailbox)
{
  if (mailbox->quiet)
    return;

  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return;

  memset(&mdata->progress, 0, sizeof(mdata->progress));
  mdata->oldmsgcount = mailbox->msg_count;
  mdata->ignmsgcount = 0;
  mdata->noprogress = false;
  mdata->progress_ready = false;
}

/**
 * progress_update - Update the progress counter
 * @param mailbox Mailbox
 * @param q   Notmuch query
 */
static void progress_update(struct Mailbox *mailbox, notmuch_query_t *q)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);

  if (mailbox->quiet || !mdata || mdata->noprogress)
    return;

  if (!mdata->progress_ready && q)
  {
    unsigned int count;
    static char msg[STRING];
    snprintf(msg, sizeof(msg), _("Reading messages..."));

#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
    if (notmuch_query_count_messages(q, &count) != NOTMUCH_STATUS_SUCCESS)
      count = 0; /* may not be defined on error */
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
    if (notmuch_query_count_messages_st(q, &count) != NOTMUCH_STATUS_SUCCESS)
      count = 0; /* may not be defined on error */
#else
    count = notmuch_query_count_messages(q);
#endif
    mutt_progress_init(&mdata->progress, msg, MUTT_PROGRESS_MSG, ReadInc, count);
    mdata->progress_ready = true;
  }

  if (mdata->progress_ready)
  {
    mutt_progress_update(&mdata->progress,
                         mailbox->msg_count + mdata->ignmsgcount - mdata->oldmsgcount, -1);
  }
}

/**
 * get_mutt_email - Get the Email of a Notmuch message
 * @param mailbox Mailbox
 * @param msg Notmuch message
 * @retval ptr  Email
 * @retval NULL Error
 */
static struct Email *get_mutt_email(struct Mailbox *mailbox, notmuch_message_t *msg)
{
  if (!mailbox || !msg)
    return NULL;

  const char *id = notmuch_message_get_message_id(msg);
  if (!id)
    return NULL;

  mutt_debug(2, "nm: neomutt email, id='%s'\n", id);

  if (!mailbox->id_hash)
  {
    mutt_debug(2, "nm: init hash\n");
    mailbox->id_hash = mutt_make_id_hash(mailbox);
    if (!mailbox->id_hash)
      return NULL;
  }

  char *mid = nm2mutt_message_id(id);
  mutt_debug(2, "nm: neomutt id='%s'\n", mid);

  struct Email *e = mutt_hash_find(mailbox->id_hash, mid);
  FREE(&mid);
  return e;
}

/**
 * append_message - Associate a message
 * @param mailbox Mailbox
 * @param q       Notmuch query
 * @param msg     Notmuch message
 * @param dedup   De-duplicate results
 */
static void append_message(struct Mailbox *mailbox, notmuch_query_t *q,
                           notmuch_message_t *msg, bool dedup)
{
  char *newpath = NULL;
  struct Email *e = NULL;

  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return;

  /* deduplicate */
  if (dedup && get_mutt_email(mailbox, msg))
  {
    mdata->ignmsgcount++;
    progress_update(mailbox, q);
    mutt_debug(2, "nm: ignore id=%s, already in the mailbox\n",
               notmuch_message_get_message_id(msg));
    return;
  }

  const char *path = get_message_last_filename(msg);
  if (!path)
    return;

  mutt_debug(2, "nm: appending message, i=%d, id=%s, path=%s\n",
             mailbox->msg_count, notmuch_message_get_message_id(msg), path);

  if (mailbox->msg_count >= mailbox->hdrmax)
  {
    mutt_debug(2, "nm: allocate mx memory\n");
    mx_alloc_memory(mailbox);
  }
  if (access(path, F_OK) == 0)
    e = maildir_parse_message(MUTT_MAILDIR, path, false, NULL);
  else
  {
    /* maybe moved try find it... */
    char *folder = get_folder_from_path(path);

    if (folder)
    {
      FILE *f = maildir_open_find_message(folder, path, &newpath);
      if (f)
      {
        e = maildir_parse_stream(MUTT_MAILDIR, f, newpath, false, NULL);
        fclose(f);

        mutt_debug(1, "nm: not up-to-date: %s -> %s\n", path, newpath);
      }
    }
    FREE(&folder);
  }

  if (!e)
  {
    mutt_debug(1, "nm: failed to parse message: %s\n", path);
    goto done;
  }
  if (init_email(e, newpath ? newpath : path, msg) != 0)
  {
    mutt_email_free(&e);
    mutt_debug(1, "nm: failed to append email!\n");
    goto done;
  }

  e->active = true;
  e->index = mailbox->msg_count;
  mailbox->size += e->content->length + e->content->offset - e->content->hdr_offset;
  mailbox->hdrs[mailbox->msg_count] = e;
  mailbox->msg_count++;

  if (newpath)
  {
    /* remember that file has been moved -- nm_mbox_sync() will update the DB */
    struct NmEmailData *edata = e->data;

    if (edata)
    {
      mutt_debug(1, "nm: remember obsolete path: %s\n", path);
      edata->oldpath = mutt_str_strdup(path);
    }
  }
  progress_update(mailbox, q);
done:
  FREE(&newpath);
}

/**
 * append_replies - add all the replies to a given messages into the display
 * @param mailbox Mailbox
 * @param q       Notmuch query
 * @param top     Notmuch message
 * @param dedup   De-duplicate the results
 *
 * Careful, this calls itself recursively to make sure we get everything.
 */
static void append_replies(struct Mailbox *mailbox, notmuch_query_t *q,
                           notmuch_message_t *top, bool dedup)
{
  notmuch_messages_t *msgs = NULL;

  for (msgs = notmuch_message_get_replies(top); notmuch_messages_valid(msgs);
       notmuch_messages_move_to_next(msgs))
  {
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(mailbox, q, m, dedup);
    /* recurse through all the replies to this message too */
    append_replies(mailbox, q, m, dedup);
    notmuch_message_destroy(m);
  }
}

/**
 * append_thread - add each top level reply in the thread
 * @param mailbox Mailbox
 * @param q       Notmuch query
 * @param thread  Notmuch thread
 * @param dedup   De-duplicate the results
 *
 * add each top level reply in the thread, and then add each reply to the top
 * level replies
 */
static void append_thread(struct Mailbox *mailbox, notmuch_query_t *q,
                          notmuch_thread_t *thread, bool dedup)
{
  notmuch_messages_t *msgs = NULL;

  for (msgs = notmuch_thread_get_toplevel_messages(thread);
       notmuch_messages_valid(msgs); notmuch_messages_move_to_next(msgs))
  {
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(mailbox, q, m, dedup);
    append_replies(mailbox, q, m, dedup);
    notmuch_message_destroy(m);
  }
}

/**
 * read_mesgs_query - Search for matching messages
 * @param mailbox Mailbox
 * @param q       Notmuch query
 * @param dedup   De-duplicate the results
 * @retval true  Success
 * @retval false Failure
 */
static bool read_mesgs_query(struct Mailbox *mailbox, notmuch_query_t *q, bool dedup)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return false;

  int limit = get_limit(mdata);

  notmuch_messages_t *msgs = NULL;
#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
  if (notmuch_query_search_messages(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    return false;
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_messages_st(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    return false;
#else
  msgs = notmuch_query_search_messages(q);
#endif

  for (; notmuch_messages_valid(msgs) && ((limit == 0) || (mailbox->msg_count < limit));
       notmuch_messages_move_to_next(msgs))
  {
    if (SigInt == 1)
    {
      SigInt = 0;
      return false;
    }
    notmuch_message_t *m = notmuch_messages_get(msgs);
    append_message(mailbox, q, m, dedup);
    notmuch_message_destroy(m);
  }
  return true;
}

/**
 * read_threads_query - Perform a query with threads
 * @param mailbox Mailbox
 * @param q       Query type
 * @param dedup   Should the results be de-duped?
 * @param limit   Maximum number of results
 * @retval true  Success
 * @retval false Failure
 */
static bool read_threads_query(struct Mailbox *mailbox, notmuch_query_t *q,
                               bool dedup, int limit)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return false;

  notmuch_threads_t *threads = NULL;
#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
  if (notmuch_query_search_threads(q, &threads) != NOTMUCH_STATUS_SUCCESS)
    return false;
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_threads_st(q, &threads) != NOTMUCH_STATUS_SUCCESS)
    return false;
#else
  threads = notmuch_query_search_threads(q);
#endif

  for (; notmuch_threads_valid(threads) && ((limit == 0) || (mailbox->msg_count < limit));
       notmuch_threads_move_to_next(threads))
  {
    if (SigInt == 1)
    {
      SigInt = 0;
      return false;
    }
    notmuch_thread_t *thread = notmuch_threads_get(threads);
    append_thread(mailbox, q, thread, dedup);
    notmuch_thread_destroy(thread);
  }
  return true;
}

/**
 * get_nm_message - Find a Notmuch message
 * @param db  Notmuch database
 * @param e Email
 * @retval ptr Handle to the Notmuch message
 */
static notmuch_message_t *get_nm_message(notmuch_database_t *db, struct Email *e)
{
  notmuch_message_t *msg = NULL;
  char *id = email_get_id(e);

  mutt_debug(2, "nm: find message (%s)\n", id);

  if (id && db)
    notmuch_database_find_message(db, id, &msg);

  return msg;
}

/**
 * nm_message_has_tag - Does a message have this tag?
 * @param msg Notmuch message
 * @param tag Tag
 * @retval true It does
 */
static bool nm_message_has_tag(notmuch_message_t *msg, char *tag)
{
  const char *possible_match_tag = NULL;
  notmuch_tags_t *tags = NULL;

  for (tags = notmuch_message_get_tags(msg); notmuch_tags_valid(tags);
       notmuch_tags_move_to_next(tags))
  {
    possible_match_tag = notmuch_tags_get(tags);
    if (mutt_str_strcmp(possible_match_tag, tag) == 0)
    {
      return true;
    }
  }
  return false;
}

/**
 * update_tags - Update the tags on a message
 * @param msg  Notmuch message
 * @param tags String of tags (space separated)
 * @retval  0 Success
 * @retval -1 Failure
 */
static int update_tags(notmuch_message_t *msg, const char *tags)
{
  char *buf = mutt_str_strdup(tags);
  if (!buf)
    return -1;

  notmuch_message_freeze(msg);

  char *tag = NULL, *end = NULL;
  for (char *p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;

    *end = '\0';

    if (*tag == '-')
    {
      mutt_debug(1, "nm: remove tag: '%s'\n", tag + 1);
      notmuch_message_remove_tag(msg, tag + 1);
    }
    else if (*tag == '!')
    {
      mutt_debug(1, "nm: toggle tag: '%s'\n", tag + 1);
      if (nm_message_has_tag(msg, tag + 1))
      {
        notmuch_message_remove_tag(msg, tag + 1);
      }
      else
      {
        notmuch_message_add_tag(msg, tag + 1);
      }
    }
    else
    {
      mutt_debug(1, "nm: add tag: '%s'\n", (*tag == '+') ? tag + 1 : tag);
      notmuch_message_add_tag(msg, (*tag == '+') ? tag + 1 : tag);
    }
    end = NULL;
    tag = NULL;
  }

  notmuch_message_thaw(msg);
  FREE(&buf);
  return 0;
}

/**
 * update_email_flags - Update the Email's flags
 * @param ctx Mailbox
 * @param e   Email
 * @param tags String of tags (space separated)
 * @retval  0 Success
 * @retval -1 Failure
 *
 * TODO: extract parsing of string to separate function, join
 * update_email_tags and update_email_flags, which are given an array of
 * tags.
 */
static int update_email_flags(struct Context *ctx, struct Email *e, const char *tags)
{
  char *buf = mutt_str_strdup(tags);
  if (!buf)
    return -1;

  char *tag = NULL, *end = NULL;
  for (char *p = buf; p && *p; p++)
  {
    if (!tag && isspace(*p))
      continue;
    if (!tag)
      tag = p; /* begin of the tag */
    if ((*p == ',') || (*p == ' '))
      end = p; /* terminate the tag */
    else if (*(p + 1) == '\0')
      end = p + 1; /* end of optstr */
    if (!tag || !end)
      continue;
    if (tag >= end)
      break;

    *end = '\0';

    if (*tag == '-')
    {
      tag = tag + 1;
      if (strcmp(tag, "unread") == 0)
        mutt_set_flag(ctx, e, MUTT_READ, 1);
      else if (strcmp(tag, "replied") == 0)
        mutt_set_flag(ctx, e, MUTT_REPLIED, 0);
      else if (strcmp(tag, "flagged") == 0)
        mutt_set_flag(ctx, e, MUTT_FLAG, 0);
    }
    else
    {
      tag = (*tag == '+') ? tag + 1 : tag;
      if (strcmp(tag, "unread") == 0)
        mutt_set_flag(ctx, e, MUTT_READ, 0);
      else if (strcmp(tag, "replied") == 0)
        mutt_set_flag(ctx, e, MUTT_REPLIED, 1);
      else if (strcmp(tag, "flagged") == 0)
        mutt_set_flag(ctx, e, MUTT_FLAG, 1);
    }
    end = NULL;
    tag = NULL;
  }

  FREE(&buf);
  return 0;
}

/**
 * rename_maildir_filename - Rename a Maildir file
 * @param old    Old path
 * @param buf    Buffer for new path
 * @param buflen Length of buffer
 * @param e      Email
 * @retval  0 Success, renamed
 * @retval  1 Success, no change
 * @retval -1 Failure
 */
static int rename_maildir_filename(const char *old, char *buf, size_t buflen, struct Email *e)
{
  char filename[PATH_MAX];
  char suffix[PATH_MAX];
  char folder[PATH_MAX];

  mutt_str_strfcpy(folder, old, sizeof(folder));
  char *p = strrchr(folder, '/');
  if (p)
  {
    *p = '\0';
    p++;
  }
  else
    p = folder;

  mutt_str_strfcpy(filename, p, sizeof(filename));

  /* remove (new,cur,...) from folder path */
  p = strrchr(folder, '/');
  if (p)
    *p = '\0';

  /* remove old flags from filename */
  p = strchr(filename, ':');
  if (p)
    *p = '\0';

  /* compose new flags */
  maildir_gen_flags(suffix, sizeof(suffix), e);

  snprintf(buf, buflen, "%s/%s/%s%s", folder,
           (e->read || e->old) ? "cur" : "new", filename, suffix);

  if (strcmp(old, buf) == 0)
    return 1;

  if (rename(old, buf) != 0)
  {
    mutt_debug(1, "nm: rename(2) failed %s -> %s\n", old, buf);
    return -1;
  }

  return 0;
}

/**
 * remove_filename - Delete a file
 * @param mdata Notmuch Mailbox data
 * @param path Path of file
 * @retval  0 Success
 * @retval -1 Failure
 */
static int remove_filename(struct NmMboxData *mdata, const char *path)
{
  mutt_debug(2, "nm: remove filename '%s'\n", path);

  notmuch_database_t *db = get_db(mdata, true);
  if (!db)
    return -1;

  notmuch_message_t *msg = NULL;
  notmuch_status_t st = notmuch_database_find_message_by_filename(db, path, &msg);
  if (st || !msg)
    return -1;

  int trans = db_trans_begin(mdata);
  if (trans < 0)
    return -1;

  /* note that unlink() is probably unnecessary here, it's already removed
   * by mh_sync_mailbox_message(), but for sure...
   */
  notmuch_filenames_t *ls = NULL;
  st = notmuch_database_remove_message(db, path);
  switch (st)
  {
    case NOTMUCH_STATUS_SUCCESS:
      mutt_debug(2, "nm: remove success, call unlink\n");
      unlink(path);
      break;
    case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
      mutt_debug(2, "nm: remove success (duplicate), call unlink\n");
      unlink(path);
      for (ls = notmuch_message_get_filenames(msg);
           ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
      {
        path = notmuch_filenames_get(ls);

        mutt_debug(2, "nm: remove duplicate: '%s'\n", path);
        unlink(path);
        notmuch_database_remove_message(db, path);
      }
      break;
    default:
      mutt_debug(1, "nm: failed to remove '%s' [st=%d]\n", path, (int) st);
      break;
  }

  notmuch_message_destroy(msg);
  if (trans)
    db_trans_end(mdata);
  return 0;
}

/**
 * rename_filename - Rename the file
 * @param mdata Notmuch Mailbox data
 * @param old  Old filename
 * @param new  New filename
 * @param e    Email
 * @retval  0 Success
 * @retval -1 Failure
 */
static int rename_filename(struct NmMboxData *mdata, const char *old,
                           const char *new, struct Email *e)
{
  notmuch_database_t *db = get_db(mdata, true);
  if (!db || !new || !old || (access(new, F_OK) != 0))
    return -1;

  int rc = -1;
  notmuch_status_t st;
  notmuch_filenames_t *ls = NULL;
  notmuch_message_t *msg = NULL;

  mutt_debug(1, "nm: rename filename, %s -> %s\n", old, new);
  int trans = db_trans_begin(mdata);
  if (trans < 0)
    return -1;

  mutt_debug(2, "nm: rename: add '%s'\n", new);
#ifdef HAVE_NOTMUCH_DATABASE_INDEX_FILE
  st = notmuch_database_index_file(db, new, NULL, &msg);
#else
  st = notmuch_database_add_message(db, new, &msg);
#endif

  if ((st != NOTMUCH_STATUS_SUCCESS) && (st != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID))
  {
    mutt_debug(1, "nm: failed to add '%s' [st=%d]\n", new, (int) st);
    goto done;
  }

  mutt_debug(2, "nm: rename: rem '%s'\n", old);
  st = notmuch_database_remove_message(db, old);
  switch (st)
  {
    case NOTMUCH_STATUS_SUCCESS:
      break;
    case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
      mutt_debug(2, "nm: rename: syncing duplicate filename\n");
      notmuch_message_destroy(msg);
      msg = NULL;
      notmuch_database_find_message_by_filename(db, new, &msg);

      for (ls = notmuch_message_get_filenames(msg);
           msg && ls && notmuch_filenames_valid(ls); notmuch_filenames_move_to_next(ls))
      {
        const char *path = notmuch_filenames_get(ls);
        char newpath[PATH_MAX];

        if (strcmp(new, path) == 0)
          continue;

        mutt_debug(2, "nm: rename: syncing duplicate: %s\n", path);

        if (rename_maildir_filename(path, newpath, sizeof(newpath), e) == 0)
        {
          mutt_debug(2, "nm: rename dup %s -> %s\n", path, newpath);
          notmuch_database_remove_message(db, path);
#ifdef HAVE_NOTMUCH_DATABASE_INDEX_FILE
          notmuch_database_index_file(db, newpath, NULL, NULL);
#else
          notmuch_database_add_message(db, newpath, NULL);
#endif
        }
      }
      notmuch_message_destroy(msg);
      msg = NULL;
      notmuch_database_find_message_by_filename(db, new, &msg);
      st = NOTMUCH_STATUS_SUCCESS;
      break;
    default:
      mutt_debug(1, "nm: failed to remove '%s' [st=%d]\n", old, (int) st);
      break;
  }

  if ((st == NOTMUCH_STATUS_SUCCESS) && e && msg)
  {
    notmuch_message_maildir_flags_to_tags(msg);
    update_email_tags(e, msg);

    char *tags = driver_tags_get(&e->tags);
    update_tags(msg, tags);
    FREE(&tags);
  }

  rc = 0;
done:
  if (msg)
    notmuch_message_destroy(msg);
  if (trans)
    db_trans_end(mdata);
  return rc;
}

/**
 * count_query_thread_messages - Count the number of messages in all queried threads
 * @param q Executed query
 * @retval num Number of messages
 */
static unsigned int count_query_thread_messages(notmuch_query_t *q)
{
  unsigned int count = 0;
  notmuch_threads_t *threads = NULL;
  notmuch_thread_t *thread = NULL;

#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
  notmuch_query_search_threads(q, &threads);
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  notmuch_query_search_threads_st(q, &threads);
#else
  threads = notmuch_query_search_threads(q);
#endif

  for (; // Initialisation is done above to improve readability of for loop.
       notmuch_threads_valid(threads); notmuch_threads_move_to_next(threads))
  {
    thread = notmuch_threads_get(threads);

    count += notmuch_thread_get_total_messages(thread);

    notmuch_thread_destroy(thread);
  }

  return count;
}

/**
 * count_query_messages - Count the number of queried messages
 * @param q Executed query
 * @retval num Number of messages
 */
static unsigned int count_query_messages(notmuch_query_t *q)
{
  unsigned int count = 0;

#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
  if (notmuch_query_count_messages(q, &count) != NOTMUCH_STATUS_SUCCESS)
    count = 0; /* may not be defined on error */
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_count_messages_st(q, &count) != NOTMUCH_STATUS_SUCCESS)
    count = 0; /* may not be defined on error */
#else
  count = notmuch_query_count_messages(q);
#endif

  return count;
}

/**
 * count_query - Count the results of a query
 * @param db   Notmuch database
 * @param qstr Query to execute
 * @param type Query type, e.g. #NM_QUERY_TYPE_MESGS
 * @retval num Number of results
 */
static unsigned int count_query(notmuch_database_t *db, const char *qstr, enum NmQueryType type)
{
  notmuch_query_t *q = notmuch_query_create(db, qstr);
  if (!q)
    return 0;

  unsigned int res = 0;

  apply_exclude_tags(q);

  if (type == NM_QUERY_TYPE_MESGS)
    res = count_query_messages(q);
  else if (type == NM_QUERY_TYPE_THREADS)
    res = count_query_thread_messages(q);

  notmuch_query_destroy(q);
  mutt_debug(1, "nm: count '%s', result=%d\n", qstr, res);

  return res;
}

/**
 * nm_email_get_folder - Get the folder for a Email
 * @param e Email
 * @retval ptr  Folder containing email
 * @retval NULL Error
 */
char *nm_email_get_folder(struct Email *e)
{
  return (e && e->data) ? ((struct NmEmailData *) e->data)->folder : NULL;
}

/**
 * nm_longrun_init - Start a long transaction
 * @param mailbox  Mailbox
 * @param writable Read/write?
 */
void nm_longrun_init(struct Mailbox *mailbox, bool writable)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);

  if (mdata && get_db(mdata, writable))
  {
    mdata->longrun = true;
    mutt_debug(2, "nm: long run initialized\n");
  }
}

/**
 * nm_longrun_done - Finish a long transaction
 * @param mailbox Mailbox
 */
void nm_longrun_done(struct Mailbox *mailbox)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);

  if (mdata && (release_db(mdata) == 0))
    mutt_debug(2, "nm: long run deinitialized\n");
}

/**
 * nm_debug_check - Check if the database is open
 * @param mailbox Mailbox
 */
void nm_debug_check(struct Mailbox *mailbox)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return;

  if (mdata->db)
  {
    mutt_debug(1, "nm: ERROR: db is open, closing\n");
    release_db(mdata);
  }
}

/**
 * nm_read_entire_thread - Get the entire thread of an email
 * @param ctx Mailbox
 * @param e   Email
 * @retval  0 Success
 * @retval -1 Failure
 */
int nm_read_entire_thread(struct Context *ctx, struct Email *e)
{
  struct NmMboxData *mdata = get_mboxdata(ctx->mailbox);
  if (!mdata)
    return -1;

  notmuch_query_t *q = NULL;
  notmuch_database_t *db = NULL;
  notmuch_message_t *msg = NULL;
  int rc = -1;

  if (!(db = get_db(mdata, false)) || !(msg = get_nm_message(db, e)))
    goto done;

  mutt_debug(1, "nm: reading entire-thread messages...[current count=%d]\n",
             ctx->mailbox->msg_count);

  progress_reset(ctx->mailbox);
  const char *id = notmuch_message_get_thread_id(msg);
  if (!id)
    goto done;

  char *qstr = NULL;
  mutt_str_append_item(&qstr, "thread:", '\0');
  mutt_str_append_item(&qstr, id, '\0');

  q = notmuch_query_create(db, qstr);
  FREE(&qstr);
  if (!q)
    goto done;
  apply_exclude_tags(q);
  notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);

  read_threads_query(ctx->mailbox, q, true, 0);
  ctx->mailbox->mtime.tv_sec = time(NULL);
  ctx->mailbox->mtime.tv_nsec = 0;
  rc = 0;

  if (ctx->mailbox->msg_count > mdata->oldmsgcount)
    mx_update_context(ctx, ctx->mailbox->msg_count - mdata->oldmsgcount);
done:
  if (q)
    notmuch_query_destroy(q);
  if (!is_longrun(mdata))
    release_db(mdata);

  if (ctx->mailbox->msg_count == mdata->oldmsgcount)
    mutt_message(_("No more messages in the thread"));

  mdata->oldmsgcount = 0;
  mutt_debug(1, "nm: reading entire-thread messages... done [rc=%d, count=%d]\n",
             rc, ctx->mailbox->msg_count);
  return rc;
}

/**
 * Parse a query type out of a query.
 * @param data Mailbox, used for the query_type
 * @param buf  Buffer for URI
 *
 * If a user writes a query for a vfolder and includes a type= statement, that
 * type= will be encoded, which Notmuch will treat as part of the query=
 * statement. This method will remove the type= and set it within the Mailbox
 * struct.
 */
void nm_parse_type_from_query(struct NmMboxData *data, char *buf)
{
  // The six variations of how type= could appear.
  const char *variants[6] = { "&type=threads", "&type=messages",
                              "type=threads&", "type=messages&",
                              "type=threads",  "type=messages" };

  int variants_size = mutt_array_size(variants);
  for (int i = 0; i < variants_size; i++)
  {
    if (mutt_str_strcasestr(buf, variants[i]) != NULL)
    {
      // variants[] is setup such that type can be determined via modulo 2.
      data->query_type = (i % 2) == 0 ? NM_QUERY_TYPE_THREADS : NM_QUERY_TYPE_MESGS;

      mutt_str_remall_strcasestr(buf, variants[i]);
    }
  }
}

/**
 * nm_uri_from_query - Turn a query into a URI
 * @param mailbox Mailbox
 * @param buf     Buffer for URI
 * @param buflen  Length of buffer
 * @retval ptr  Query as a URI
 * @retval NULL Error
 */
char *nm_uri_from_query(struct Mailbox *mailbox, char *buf, size_t buflen)
{
  mutt_debug(2, "(%s)\n", buf);
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  char uri[PATH_MAX + LONG_STRING + 32]; /* path to DB + query + URI "decoration" */
  int added;
  bool using_default_data = false;

  // No existing data. Try to get a default NmMboxData.
  if (!mdata)
  {
    mdata = nm_get_default_data();
    using_default_data = true;
  }

  if (mdata)
  {
    nm_parse_type_from_query(mdata, buf);

    if (get_limit(mdata) != NmDbLimit)
    {
      added = snprintf(uri, sizeof(uri),
                       "notmuch://%s?type=%s&limit=%d&query=", get_db_filename(mdata),
                       query_type_to_string(mdata->query_type), get_limit(mdata));
    }
    else
    {
      added = snprintf(uri, sizeof(uri),
                       "notmuch://%s?type=%s&query=", get_db_filename(mdata),
                       query_type_to_string(mdata->query_type));
    }
  }
  else
    return NULL;

  if (added >= sizeof(uri))
  {
    // snprintf output was truncated, so can't create URI
    return NULL;
  }

  url_pct_encode(&uri[added], sizeof(uri) - added, buf);

  mutt_str_strfcpy(buf, uri, buflen);
  buf[buflen - 1] = '\0';

  if (using_default_data)
    free_mboxdata((void **) mdata);

  mutt_debug(1, "nm: uri from query '%s'\n", buf);
  return buf;
}

/**
 * nm_normalize_uri - takes a notmuch URI, parses it and reformat it in a canonical way
 * @param uri    Original URI to be parsed
 * @param buf    Buffer for the reformatted URI
 * @param buflen Size of the buffer
 * @retval true if buf contains a normalized version of the query
 * @retval false if uri contains an invalid query
 *
 * This function aims at making notmuch searches URI representations deterministic,
 * so that when comparing two equivalent searches they will be the same. It works
 * by building a notmuch context object from the original search string, and
 * building a new from the notmuch context object.
 *
 * It's aimed to be used by mailbox when parsing the virtual_mailboxes to make the
 * parsed user written search strings comparable to the internally generated ones.
 */
bool nm_normalize_uri(const char *uri, char *buf, size_t buflen)
{
  mutt_debug(2, "(%s)\n", uri);
  char tmp[PATH_MAX];
  int rc = -1;

  struct Mailbox tmp_mbox = { { 0 } };
  struct NmMboxData *tmp_mdata = new_mboxdata(uri);

  if (!tmp_mdata)
    return false;

  tmp_mbox.magic = MUTT_NOTMUCH;
  tmp_mbox.data = tmp_mdata;

  mutt_debug(2, "#1 () -> db_query: %s\n", tmp_mdata->db_query);

  if (!get_query_string(tmp_mdata, false))
    goto gone;

  mutt_debug(2, "#2 () -> db_query: %s\n", tmp_mdata->db_query);

  mutt_str_strfcpy(tmp, tmp_mdata->db_query, sizeof(tmp));

  if (!nm_uri_from_query(&tmp_mbox, tmp, sizeof(tmp)))
    goto gone;

  mutt_str_strfcpy(buf, tmp, buflen);

  mutt_debug(2, "#3 (%s) -> %s\n", uri, buf);

  rc = 0;
gone:
  url_free(&tmp_mdata->db_url);
  FREE(&tmp_mdata->db_url_holder);
  FREE(&tmp_mdata);
  if (rc < 0)
  {
    mutt_error(_("failed to parse notmuch uri: %s"), uri);
    mutt_debug(2, "() -> error\n");
    return false;
  }
  return true;
}

/**
 * nm_query_window_forward - Function to move the current search window forward in time
 *
 * Updates `nm_query_window_current_position` by decrementing it by 1, or does nothing
 * if the current window already is set to 0.
 *
 * The lower the value of `nm_query_window_current_position` is, the more recent the
 * result will be.
 */
void nm_query_window_forward(void)
{
  if (NmQueryWindowCurrentPosition != 0)
    NmQueryWindowCurrentPosition--;

  mutt_debug(2, "(%d)\n", NmQueryWindowCurrentPosition);
}

/**
 * nm_query_window_backward - Function to move the current search window backward in time
 *
 * Updates `nm_query_window_current_position` by incrementing it by 1
 *
 * The higher the value of `nm_query_window_current_position` is, the less recent the
 * result will be.
 */
void nm_query_window_backward(void)
{
  NmQueryWindowCurrentPosition++;
  mutt_debug(2, "(%d)\n", NmQueryWindowCurrentPosition);
}

/**
 * nm_message_is_still_queried - Is a message still visible in the query?
 * @param mailbox Mailbox
 * @param e     Email
 * @retval true Message is still in query
 */
bool nm_message_is_still_queried(struct Mailbox *mailbox, struct Email *e)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  notmuch_database_t *db = get_db(mdata, false);
  char *orig_str = get_query_string(mdata, true);

  if (!db || !orig_str)
    return false;

  char *new_str = NULL;
  bool result = false;
  if (safe_asprintf(&new_str, "id:%s and (%s)", email_get_id(e), orig_str) < 0)
    return false;

  mutt_debug(2, "nm: checking if message is still queried: %s\n", new_str);

  notmuch_query_t *q = notmuch_query_create(db, new_str);

  switch (mdata->query_type)
  {
    case NM_QUERY_TYPE_MESGS:
    {
      notmuch_messages_t *messages = NULL;
#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
      if (notmuch_query_search_messages(q, &messages) != NOTMUCH_STATUS_SUCCESS)
        return false;
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
      if (notmuch_query_search_messages_st(q, &messages) != NOTMUCH_STATUS_SUCCESS)
        return false;
#else
      messages = notmuch_query_search_messages(q);
#endif
      result = notmuch_messages_valid(messages);
      notmuch_messages_destroy(messages);
      break;
    }
    case NM_QUERY_TYPE_THREADS:
    {
      notmuch_threads_t *threads = NULL;
#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
      if (notmuch_query_search_threads(q, &threads) != NOTMUCH_STATUS_SUCCESS)
        return false;
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
      if (notmuch_query_search_threads_st(q, &threads) != NOTMUCH_STATUS_SUCCESS)
        return false;
#else
      threads = notmuch_query_search_threads(q);
#endif
      result = notmuch_threads_valid(threads);
      notmuch_threads_destroy(threads);
      break;
    }
  }

  notmuch_query_destroy(q);

  mutt_debug(2, "nm: checking if message is still queried: %s = %s\n", new_str,
             result ? "true" : "false");

  return result;
}

/**
 * nm_update_filename - Change the filename
 * @param mailbox Mailbox
 * @param old Old filename
 * @param new New filename
 * @param e   Email
 * @retval  0 Success
 * @retval -1 Failure
 */
int nm_update_filename(struct Mailbox *mailbox, const char *old,
                       const char *new, struct Email *e)
{
  char buf[PATH_MAX];
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata || !new)
    return -1;

  if (!old && e && e->data)
  {
    email_get_fullpath(e, buf, sizeof(buf));
    old = buf;
  }

  int rc = rename_filename(mdata, old, new, e);

  if (!is_longrun(mdata))
    release_db(mdata);
  mailbox->mtime.tv_sec = time(NULL);
  mailbox->mtime.tv_nsec = 0;
  return rc;
}

/**
 * nm_nonctx_get_count - Perform some queries without an open database
 * @param path Notmuch database path
 * @param all  Count of all emails
 * @param new  Count of new emails
 * @retval  0 Success
 * @retval -1 Failure
 */
int nm_nonctx_get_count(char *path, int *all, int *new)
{
  struct UrlQueryString *item = NULL;
  struct Url url = { U_UNKNOWN };
  char *url_holder = mutt_str_strdup(path);
  char *db_filename = NULL, *db_query = NULL;
  enum NmQueryType db_query_type = string_to_query_type(NmQueryType);
  notmuch_database_t *db = NULL;
  int rc = -1;
  mutt_debug(1, "nm: count\n");

  if (url_parse(&url, url_holder) < 0)
  {
    mutt_error(_("failed to parse notmuch uri: %s"), path);
    goto done;
  }

  STAILQ_FOREACH(item, &url.query_strings, entries)
  {
    if (item->value && (strcmp(item->name, "query") == 0))
    {
      db_query = item->value;
    }
    else if (item->value && (strcmp(item->name, "type") == 0))
    {
      db_query_type = string_to_query_type(item->value);
    }
  }

  if (!db_query)
    goto done;

  db_filename = url.path;
  if (!db_filename)
  {
    if (NmDefaultUri)
    {
      if (strncmp(NmDefaultUri, "notmuch://", 10) == 0)
        db_filename = NmDefaultUri + 10;
      else
        db_filename = NmDefaultUri;
    }
    else if (Folder)
      db_filename = Folder;
  }

  /* don't be verbose about connection, as we're called from
   * sidebar/mailbox very often */
  db = do_database_open(db_filename, false, false);
  if (!db)
    goto done;

  /* all emails */
  if (all)
    *all = count_query(db, db_query, db_query_type);

  /* new messages */
  if (new)
  {
    char *qstr = NULL;

    safe_asprintf(&qstr, "( %s ) tag:%s", db_query, NmUnreadTag);
    *new = count_query(db, qstr, db_query_type);
    FREE(&qstr);
  }

  rc = 0;
done:
  if (db)
  {
#ifdef NOTMUCH_API_3
    notmuch_database_destroy(db);
#else
    notmuch_database_close(db);
#endif
    mutt_debug(1, "nm: count close DB\n");
  }
  url_free(&url);
  FREE(&url_holder);

  mutt_debug(1, "nm: count done [rc=%d]\n", rc);
  return rc;
}

/**
 * nm_description_to_path - Find a path from a folder's description
 * @param desc   Description
 * @param buf    Buffer for path
 * @param buflen Length of buffer
 * @retval  0 Success
 * @retval <0 Failure
 */
int nm_description_to_path(const char *desc, char *buf, size_t buflen)
{
  if (!desc || !buf || (buflen == 0))
    return -EINVAL;

  struct MailboxNode *np = NULL;
  STAILQ_FOREACH(np, &AllMailboxes, entries)
  {
    if ((np->m->magic == MUTT_NOTMUCH) && np->m->desc && (strcmp(desc, np->m->desc) == 0))
    {
      mutt_str_strfcpy(buf, np->m->path, buflen);
      buf[buflen - 1] = '\0';
      return 0;
    }
  }

  return -1;
}

/**
 * nm_record_message - Add a message to the Notmuch database
 * @param mailbox Mailbox
 * @param path    Path of the email
 * @param e       Email
 * @retval  0 Success
 * @retval -1 Failure
 */
int nm_record_message(struct Mailbox *mailbox, char *path, struct Email *e)
{
  notmuch_database_t *db = NULL;
  notmuch_status_t st;
  notmuch_message_t *msg = NULL;
  int rc = -1;
  struct NmMboxData *mdata = get_mboxdata(mailbox);

  if (!path || !mdata || (access(path, F_OK) != 0))
    return 0;
  db = get_db(mdata, true);
  if (!db)
    return -1;

  mutt_debug(1, "nm: record message: %s\n", path);
  int trans = db_trans_begin(mdata);
  if (trans < 0)
    goto done;

#ifdef HAVE_NOTMUCH_DATABASE_INDEX_FILE
  st = notmuch_database_index_file(db, path, NULL, &msg);
#else
  st = notmuch_database_add_message(db, path, &msg);
#endif

  if ((st != NOTMUCH_STATUS_SUCCESS) && (st != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID))
  {
    mutt_debug(1, "nm: failed to add '%s' [st=%d]\n", path, (int) st);
    goto done;
  }

  if (st == NOTMUCH_STATUS_SUCCESS && msg)
  {
    notmuch_message_maildir_flags_to_tags(msg);
    if (e)
    {
      char *tags = driver_tags_get(&e->tags);
      update_tags(msg, tags);
      FREE(&tags);
    }
    if (NmRecordTags)
      update_tags(msg, NmRecordTags);
  }

  rc = 0;
done:
  if (msg)
    notmuch_message_destroy(msg);
  if (trans == 1)
    db_trans_end(mdata);
  if (!is_longrun(mdata))
    release_db(mdata);
  return rc;
}

/**
 * nm_get_all_tags - Fill a list with all notmuch tags
 * @param mailbox   Mailbox
 * @param tag_list  List of tags
 * @param tag_count Number of tags
 * @retval  0 Success
 * @retval -1 Failure
 *
 * If tag_list is NULL, just count the tags.
 */
int nm_get_all_tags(struct Mailbox *mailbox, char **tag_list, int *tag_count)
{
  struct NmMboxData *mdata = get_mboxdata(mailbox);
  if (!mdata)
    return -1;

  notmuch_database_t *db = NULL;
  notmuch_tags_t *tags = NULL;
  const char *tag = NULL;
  int rc = -1;

  if (!(db = get_db(mdata, false)) || !(tags = notmuch_database_get_all_tags(db)))
    goto done;

  *tag_count = 0;
  mutt_debug(1, "nm: get all tags\n");

  while (notmuch_tags_valid(tags))
  {
    tag = notmuch_tags_get(tags);
    /* Skip empty string */
    if (*tag)
    {
      if (tag_list)
        tag_list[*tag_count] = mutt_str_strdup(tag);
      (*tag_count)++;
    }
    notmuch_tags_move_to_next(tags);
  }

  rc = 0;
done:
  if (tags)
    notmuch_tags_destroy(tags);

  if (!is_longrun(mdata))
    release_db(mdata);

  mutt_debug(1, "nm: get all tags done [rc=%d tag_count=%u]\n", rc, *tag_count);
  return rc;
}

/**
 * nm_mbox_open - Implements MxOps::mbox_open()
 */
static int nm_mbox_open(struct Context *ctx)
{
  int rc = -1;

  if (init_mailbox(ctx->mailbox) != 0)
    return -1;

  struct NmMboxData *mdata = get_mboxdata(ctx->mailbox);
  if (!mdata)
    return -1;

  mutt_debug(1, "nm: reading messages...[current count=%d]\n", ctx->mailbox->msg_count);

  progress_reset(ctx->mailbox);

  notmuch_query_t *q = get_query(mdata, false);
  if (q)
  {
    rc = 0;
    switch (mdata->query_type)
    {
      case NM_QUERY_TYPE_MESGS:
        if (!read_mesgs_query(ctx->mailbox, q, false))
          rc = -2;
        break;
      case NM_QUERY_TYPE_THREADS:
        if (!read_threads_query(ctx->mailbox, q, false, get_limit(mdata)))
          rc = -2;
        break;
    }
    notmuch_query_destroy(q);
  }

  if (!is_longrun(mdata))
    release_db(mdata);

  ctx->mailbox->mtime.tv_sec = time(NULL);
  ctx->mailbox->mtime.tv_nsec = 0;

  mx_update_context(ctx, ctx->mailbox->msg_count);
  mdata->oldmsgcount = 0;

  mutt_debug(1, "nm: reading messages... done [rc=%d, count=%d]\n", rc,
             ctx->mailbox->msg_count);
  return rc;
}

/**
 * nm_mbox_check - Implements MxOps::mbox_check()
 * @param ctx         Mailbox
 * @param index_hint  Remember our place in the index
 * @retval -1 Error
 * @retval  0 Success
 * @retval #MUTT_NEW_MAIL New mail has arrived
 * @retval #MUTT_REOPENED Mailbox closed and reopened
 * @retval #MUTT_FLAGS    Flags have changed
 */
static int nm_mbox_check(struct Context *ctx, int *index_hint)
{
  struct NmMboxData *mdata = get_mboxdata(ctx->mailbox);
  time_t mtime = 0;
  if (!mdata || (get_database_mtime(mdata, &mtime) != 0))
    return -1;

  int new_flags = 0;
  bool occult = false;

  if (ctx->mailbox->mtime.tv_sec >= mtime)
  {
    mutt_debug(2, "nm: check unnecessary (db=%lu mailbox=%lu)\n", mtime,
               ctx->mailbox->mtime);
    return 0;
  }

  mutt_debug(1, "nm: checking (db=%lu mailbox=%lu)\n", mtime, ctx->mailbox->mtime);

  notmuch_query_t *q = get_query(mdata, false);
  if (!q)
    goto done;

  mutt_debug(1, "nm: start checking (count=%d)\n", ctx->mailbox->msg_count);
  mdata->oldmsgcount = ctx->mailbox->msg_count;
  mdata->noprogress = true;

  for (int i = 0; i < ctx->mailbox->msg_count; i++)
    ctx->mailbox->hdrs[i]->active = false;

  int limit = get_limit(mdata);

  notmuch_messages_t *msgs = NULL;
#if LIBNOTMUCH_CHECK_VERSION(5, 0, 0)
  if (notmuch_query_search_messages(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    return false;
#elif LIBNOTMUCH_CHECK_VERSION(4, 3, 0)
  if (notmuch_query_search_messages_st(q, &msgs) != NOTMUCH_STATUS_SUCCESS)
    goto done;
#else
  msgs = notmuch_query_search_messages(q);
#endif

  for (int i = 0; notmuch_messages_valid(msgs) && ((limit == 0) || (i < limit));
       notmuch_messages_move_to_next(msgs), i++)
  {
    char old[PATH_MAX];
    const char *new = NULL;

    notmuch_message_t *m = notmuch_messages_get(msgs);
    struct Email *e = get_mutt_email(ctx->mailbox, m);

    if (!e)
    {
      /* new email */
      append_message(ctx->mailbox, NULL, m, 0);
      notmuch_message_destroy(m);
      continue;
    }

    /* message already exists, merge flags */
    e->active = true;

    /* Check to see if the message has moved to a different subdirectory.
     * If so, update the associated filename.
     */
    new = get_message_last_filename(m);
    email_get_fullpath(e, old, sizeof(old));

    if (mutt_str_strcmp(old, new) != 0)
      update_message_path(e, new);

    if (!e->changed)
    {
      /* if the user hasn't modified the flags on
       * this message, update the flags we just
       * detected.
       */
      struct Email tmp = { 0 };
      maildir_parse_flags(&tmp, new);
      maildir_update_flags(ctx, e, &tmp);
    }

    if (update_email_tags(e, m) == 0)
      new_flags++;

    notmuch_message_destroy(m);
  }

  for (int i = 0; i < ctx->mailbox->msg_count; i++)
  {
    if (!ctx->mailbox->hdrs[i]->active)
    {
      occult = true;
      break;
    }
  }

  if (ctx->mailbox->msg_count > mdata->oldmsgcount)
    mx_update_context(ctx, ctx->mailbox->msg_count - mdata->oldmsgcount);
done:
  if (q)
    notmuch_query_destroy(q);

  if (!is_longrun(mdata))
    release_db(mdata);

  ctx->mailbox->mtime.tv_sec = time(NULL);
  ctx->mailbox->mtime.tv_nsec = 0;

  mutt_debug(1, "nm: ... check done [count=%d, new_flags=%d, occult=%d]\n",
             ctx->mailbox->msg_count, new_flags, occult);

  return occult ? MUTT_REOPENED :
                  (ctx->mailbox->msg_count > mdata->oldmsgcount) ?
                  MUTT_NEW_MAIL :
                  new_flags ? MUTT_FLAGS : 0;
}

/**
 * nm_mbox_sync - Implements MxOps::mbox_sync()
 */
static int nm_mbox_sync(struct Context *ctx, int *index_hint)
{
  struct NmMboxData *mdata = get_mboxdata(ctx->mailbox);
  if (!mdata)
    return -1;

  int rc = 0;
  struct Progress progress;
  char *uri = ctx->mailbox->path;
  bool changed = false;
  char msgbuf[PATH_MAX + 64];

  mutt_debug(1, "nm: sync start ...\n");

  if (!ctx->mailbox->quiet)
  {
    /* all is in this function so we don't use data->progress here */
    snprintf(msgbuf, sizeof(msgbuf), _("Writing %s..."), ctx->mailbox->path);
    mutt_progress_init(&progress, msgbuf, MUTT_PROGRESS_MSG, WriteInc,
                       ctx->mailbox->msg_count);
  }

  for (int i = 0; i < ctx->mailbox->msg_count; i++)
  {
    char old[PATH_MAX], new[PATH_MAX];
    struct Email *e = ctx->mailbox->hdrs[i];
    struct NmEmailData *edata = e->data;

    if (!ctx->mailbox->quiet)
      mutt_progress_update(&progress, i, -1);

    *old = '\0';
    *new = '\0';

    if (edata->oldpath)
    {
      mutt_str_strfcpy(old, edata->oldpath, sizeof(old));
      old[sizeof(old) - 1] = '\0';
      mutt_debug(2, "nm: fixing obsolete path '%s'\n", old);
    }
    else
      email_get_fullpath(e, old, sizeof(old));

    mutt_str_strfcpy(ctx->mailbox->path, edata->folder, sizeof(ctx->mailbox->path));
    ctx->mailbox->magic = edata->magic;
#ifdef USE_HCACHE
    rc = mh_sync_mailbox_message(ctx, i, NULL);
#else
    rc = mh_sync_mailbox_message(ctx, i);
#endif
    mutt_str_strfcpy(ctx->mailbox->path, uri, sizeof(ctx->mailbox->path));
    ctx->mailbox->magic = MUTT_NOTMUCH;

    if (rc)
      break;

    if (!e->deleted)
      email_get_fullpath(e, new, sizeof(new));

    if (e->deleted || (strcmp(old, new) != 0))
    {
      if (e->deleted && (remove_filename(mdata, old) == 0))
        changed = true;
      else if (*new &&*old && (rename_filename(mdata, old, new, e) == 0))
        changed = true;
    }

    FREE(&edata->oldpath);
  }

  mutt_str_strfcpy(ctx->mailbox->path, uri, sizeof(ctx->mailbox->path));
  ctx->mailbox->magic = MUTT_NOTMUCH;

  if (!is_longrun(mdata))
    release_db(mdata);
  if (changed)
  {
    ctx->mailbox->mtime.tv_sec = time(NULL);
    ctx->mailbox->mtime.tv_nsec = 0;
  }

  mutt_debug(1, "nm: .... sync done [rc=%d]\n", rc);
  return rc;
}

/**
 * nm_mbox_close - Implements MxOps::mbox_close()
 *
 * Nothing to do.
 */
static int nm_mbox_close(struct Context *ctx)
{
  return 0;
}

/**
 * nm_msg_open - Implements MxOps::msg_open()
 */
static int nm_msg_open(struct Context *ctx, struct Message *msg, int msgno)
{
  if (!ctx || !msg)
    return 1;
  struct Email *cur = ctx->mailbox->hdrs[msgno];
  char path[PATH_MAX];
  char *folder = nm_email_get_folder(cur);

  snprintf(path, sizeof(path), "%s/%s", folder, cur->path);

  msg->fp = fopen(path, "r");
  if (!msg->fp && (errno == ENOENT) &&
      ((ctx->mailbox->magic == MUTT_MAILDIR) || (ctx->mailbox->magic == MUTT_NOTMUCH)))
  {
    msg->fp = maildir_open_find_message(folder, cur->path, NULL);
  }

  mutt_debug(1, "%s\n", __func__);
  return !msg->fp;
}

/**
 * nm_msg_commit - Implements MxOps::msg_commit()
 * @retval -1 Always
 */
static int nm_msg_commit(struct Context *ctx, struct Message *msg)
{
  mutt_error(_("Can't write to virtual folder"));
  return -1;
}

/**
 * nm_msg_close - Implements MxOps::msg_close()
 */
static int nm_msg_close(struct Context *ctx, struct Message *msg)
{
  if (!msg)
    return -1;
  mutt_file_fclose(&(msg->fp));
  return 0;
}

/**
 * nm_tags_edit - Implements MxOps::tags_edit()
 */
static int nm_tags_edit(struct Context *ctx, const char *tags, char *buf, size_t buflen)
{
  *buf = '\0';
  if (mutt_get_field("Add/remove labels: ", buf, buflen, MUTT_NM_TAG) != 0)
    return -1;
  return 1;
}

/**
 * nm_tags_commit - Implements MxOps::tags_commit()
 */
static int nm_tags_commit(struct Context *ctx, struct Email *e, char *buf)
{
  struct NmMboxData *mdata = get_mboxdata(ctx->mailbox);
  if (!buf || !*buf || !mdata)
    return -1;

  notmuch_database_t *db = NULL;
  notmuch_message_t *msg = NULL;
  int rc = -1;

  if (!(db = get_db(mdata, true)) || !(msg = get_nm_message(db, e)))
    goto done;

  mutt_debug(1, "nm: tags modify: '%s'\n", buf);

  update_tags(msg, buf);
  update_email_flags(ctx, e, buf);
  update_email_tags(e, msg);
  mutt_set_header_color(ctx, e);

  rc = 0;
  e->changed = true;
done:
  if (!is_longrun(mdata))
    release_db(mdata);
  if (e->changed)
  {
    ctx->mailbox->mtime.tv_sec = time(NULL);
    ctx->mailbox->mtime.tv_nsec = 0;
  }
  mutt_debug(1, "nm: tags modify done [rc=%d]\n", rc);
  return rc;
}

/**
 * nm_path_probe - Is this a Notmuch mailbox? - Implements MxOps::path_probe()
 */
int nm_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (mutt_str_strncasecmp(path, "notmuch://", 10) == 0)
    return MUTT_NOTMUCH;

  return MUTT_UNKNOWN;
}

/**
 * nm_path_canon - Canonicalise a mailbox path - Implements MxOps::path_canon()
 */
int nm_path_canon(char *buf, size_t buflen, const char *folder)
{
  if (!buf)
    return -1;

  if ((buf[0] == '+') || (buf[0] == '='))
  {
    if (!folder)
      return -1;

    size_t flen = mutt_str_strlen(folder);
    if ((flen > 0) && (folder[flen - 1] != '/'))
    {
      buf[0] = '/';
      mutt_str_inline_replace(buf, buflen, 0, folder);
    }
    else
    {
      mutt_str_inline_replace(buf, buflen, 1, folder);
    }
  }

  return 0;
}

/**
 * nm_path_pretty - Implements MxOps::path_pretty()
 */
int nm_path_pretty(char *buf, size_t buflen, const char *folder)
{
  /* Succeed, but don't do anything, for now */
  return 0;
}

/**
 * nm_path_parent - Implements MxOps::path_parent()
 */
int nm_path_parent(char *buf, size_t buflen)
{
  /* Succeed, but don't do anything, for now */
  return 0;
}

// clang-format off
/**
 * struct mx_notmuch_ops - Notmuch mailbox - Implements ::MxOps
 */
struct MxOps mx_notmuch_ops = {
  .magic            = MUTT_NOTMUCH,
  .name             = "notmuch",
  .mbox_open        = nm_mbox_open,
  .mbox_open_append = NULL,
  .mbox_check       = nm_mbox_check,
  .mbox_sync        = nm_mbox_sync,
  .mbox_close       = nm_mbox_close,
  .msg_open         = nm_msg_open,
  .msg_open_new     = NULL,
  .msg_commit       = nm_msg_commit,
  .msg_close        = nm_msg_close,
  .msg_padding_size = NULL,
  .tags_edit        = nm_tags_edit,
  .tags_commit      = nm_tags_commit,
  .path_probe       = nm_path_probe,
  .path_canon       = nm_path_canon,
  .path_pretty      = nm_path_pretty,
  .path_parent      = nm_path_parent,
};
// clang-format on
