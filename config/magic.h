/**
 * @file
 * Type representing a mailbox
 *
 * @authors
 * Copyright (C) 2017-2018 Richard Russon <rich@flatcap.org>
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
 */

#ifndef MUTT_CONFIG_MAGIC_H
#define MUTT_CONFIG_MAGIC_H

struct ConfigSet;

extern const char *magic_values[];

/**
 * enum MailboxType - Supported mailbox formats
 */
enum MailboxType
{
  MUTT_MAILBOX_ERROR = -1,
  MUTT_UNKNOWN = 0,
  MUTT_MBOX,
  MUTT_MMDF,
  MUTT_MH,
  MUTT_MAILDIR,
  MUTT_NNTP,
  MUTT_IMAP,
  MUTT_NOTMUCH,
  MUTT_POP,
  MUTT_COMPRESSED,
};

void magic_init(struct ConfigSet *cs);

#endif /* MUTT_CONFIG_MAGIC_H */
