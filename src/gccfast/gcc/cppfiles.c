/* Part of CPP library.  (include file handling)
   Copyright (C) 1986, 1987, 1989, 1992, 1993, 1994, 1995, 1998,
   1999, 2000, 2001, 2002 Free Software Foundation, Inc.
   Written by Per Bothner, 1994.
   Based on CCCP program by Paul Rubin, June 1986
   Adapted to ANSI C, Richard Stallman, Jan 1987
   Split out of cpplib.c, Zack Weinberg, Oct 1998

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include <dirent.h>
#include "cpplib.h"
#include "cpphash.h"
#include "intl.h"
#include "mkdeps.h"
#include "splay-tree.h"
#ifdef ENABLE_VALGRIND_CHECKING
#include <valgrind.h>
#else
/* Avoid #ifdef:s when we can help it.  */
#define VALGRIND_DISCARD(x)
#endif

/* APPLE LOCAL indexing dpatel */
#include "genindex.h"

#ifdef HAVE_MMAP_FILE
# include <sys/mman.h>
# ifndef MMAP_THRESHOLD
#  define MMAP_THRESHOLD 3 /* Minimum page count to mmap the file.  */
# endif
# if MMAP_THRESHOLD
#  define TEST_THRESHOLD(size, pagesize) \
     (size / pagesize >= MMAP_THRESHOLD && (size % pagesize) != 0)
   /* Use mmap if the file is big enough to be worth it (controlled
      by MMAP_THRESHOLD) and if we can safely count on there being
      at least one readable NUL byte after the end of the file's
      contents.  This is true for all tested operating systems when
      the file size is not an exact multiple of the page size.  */
#  ifndef __CYGWIN__
#   define SHOULD_MMAP(size, pagesize) TEST_THRESHOLD (size, pagesize)
#  else
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
    /* Cygwin can't correctly emulate mmap under Windows 9x style systems so
       disallow use of mmap on those systems.  Windows 9x does not zero fill
       memory at EOF and beyond, as required.  */
#   define SHOULD_MMAP(size, pagesize) ((GetVersion() & 0x80000000) \
    					? 0 : TEST_THRESHOLD (size, pagesize))
#  endif
# endif

#else  /* No MMAP_FILE */
#  undef MMAP_THRESHOLD
#  define MMAP_THRESHOLD 0
#endif

/* APPLE LOCAL begin darwin mmap bug workaround  ilr */
#ifndef USING_DARWIN_MMAP
#define USING_DARWIN_MMAP 0
#endif
#if MMAP_THRESHOLD && USING_DARWIN_MMAP
#include <mach/mach_init.h>
#include <mach/mach_host.h>
static kernel_version_t darwin_kernel_version;
static int darwin_mmap_allowed = 1;
#endif
/* APPLE LOCAL end darwin mmap bug workaround  ilr */

#ifndef O_BINARY
# define O_BINARY 0
#endif

/* If errno is inspected immediately after a system call fails, it will be
   nonzero, and no error number will ever be zero.  */
#ifndef ENOENT
# define ENOENT 0
#endif
#ifndef ENOTDIR
# define ENOTDIR 0
#endif

/* Suppress warning about function macros used w/o arguments in traditional
   C.  It is unlikely that glibc's strcmp macro helps this file at all.  */
#undef strcmp

/* This structure is used for the table of all includes.  */
struct include_file {
  const char *name;		/* actual path name of file */
  const char *header_name;	/* the original header found */
  const cpp_hashnode *cmacro;	/* macro, if any, preventing reinclusion.  */
  const struct search_path *foundhere;
				/* location in search path where file was
				   found, for #include_next and sysp.  */
  const unsigned char *buffer;	/* pointer to cached file contents */
  struct stat st;		/* copy of stat(2) data for file */
  int fd;			/* fd open on file (short term storage only) */
  int err_no;			/* errno obtained if opening a file failed */
  unsigned short include_count;	/* number of times file has been read */
  unsigned short refcnt;	/* number of stacked buffers using this file */
  unsigned char mapped;		/* file buffer is mmapped */
  /* APPLE LOCAL begin PCH */
  unsigned char pch;		/* 0: file not known to be a PCH.
				   1: file is a PCH 
				      (on return from find_include_file).
				   2: file is not and never will be a valid
				      precompiled header.
				   3: file is always a valid precompiled
				      header.  */
  /* APPLE LOCAL end PCH */
  /* APPLE LOCAL begin Symbol Separation */
  unsigned char suppress_dbg;   /* 0: Default. Do nothing.
				   1: file has separate debugging information
				      available. Do not generate again.  */
  /* APPLE LOCAL end Symbol Separation */
};

/* Variable length record files on VMS will have a stat size that includes
   record control characters that won't be included in the read size.  */
#ifdef VMS
# define FAB_C_VAR 2 /* variable length records (see Starlet fabdef.h) */
# define STAT_SIZE_TOO_BIG(ST) ((ST).st_fab_rfm == FAB_C_VAR)
#else
# define STAT_SIZE_TOO_BIG(ST) 0
#endif

/* The cmacro works like this: If it's NULL, the file is to be
   included again.  If it's NEVER_REREAD, the file is never to be
   included again.  Otherwise it is a macro hashnode, and the file is
   to be included again if the macro is defined.  */
#define NEVER_REREAD ((const cpp_hashnode *) -1)
#define DO_NOT_REREAD(inc) \
((inc)->cmacro && ((inc)->cmacro == NEVER_REREAD \
		   || (inc)->cmacro->type == NT_MACRO))
#define NO_INCLUDE_PATH ((struct include_file *) -1)
#define INCLUDE_PCH_P(F) (((F)->pch & 1) != 0)

static struct file_name_map *read_name_map
				PARAMS ((cpp_reader *, const char *));
static char *read_filename_string PARAMS ((int, FILE *));
static char *remap_filename 	PARAMS ((cpp_reader *, char *,
					 struct search_path *));
static struct search_path *search_from PARAMS ((cpp_reader *,
						enum include_type));
/* APPLE LOCAL begin read-from-stdin */
static bool more_to_read_from_stdin = false;
/* APPLE LOCAL end read-from-stdin */

/* APPLE LOCAL framework headers */
#ifdef FRAMEWORK_HEADERS
struct framework_header {const char * dirName; int dirNameLen; };
static struct framework_header framework_header_dirs[] = {
   { "PrivateHeaders", 14 },
   { "Headers", 7 },
   { NULL, 0 }
};

static struct include_file *
find_subframework_file PARAMS ((cpp_reader *, const cpp_token *,
                              struct search_path *));
static struct include_file *
	find_framework_file PARAMS ((cpp_reader *, const cpp_token *,
				   enum include_type));
#endif /* FRAMEWORK_HEADERS  */

/* APPLE LOCAL begin header search */
static void _init_include_hash PARAMS ((cpp_reader *, char *));
static struct include_file * 
        find_include_file_in_hashtable PARAMS ((cpp_reader *, const char *, char *, 
                                                enum include_type, struct search_path *));
static void  synthesize_name_from_path PARAMS ((char *, struct search_path *, const char *));
/* APPLE LOCAL end header search */

static struct include_file *
	find_include_file PARAMS ((cpp_reader *, const cpp_token *,
				   enum include_type));
static struct include_file *open_file PARAMS ((cpp_reader *, const char *));
static struct include_file *validate_pch PARAMS ((cpp_reader *,
						  const char *,
						  const char *));
static struct include_file *open_file_pch PARAMS ((cpp_reader *, 
						   const char *));
static int read_include_file	PARAMS ((cpp_reader *, struct include_file *));
static bool stack_include_file	PARAMS ((cpp_reader *, struct include_file *));
static void purge_cache 	PARAMS ((struct include_file *));
static void destroy_node	PARAMS ((splay_tree_value));
static int report_missing_guard		PARAMS ((splay_tree_node, void *));
static splay_tree_node find_or_create_entry PARAMS ((cpp_reader *,
						     const char *));
static void handle_missing_header PARAMS ((cpp_reader *, const char *, int));
static int remove_component_p	PARAMS ((const char *));

/* APPLE LOCAL begin Symbol Separation */
static const char context_ident[8] = "gContext";

/* File pointer for context information file.  */
static FILE *cinfo_file;

/* Hold checksum for the include file. It is included with BINCL and EINCL stabs.  */
static struct cpp_stab_checksum stabs_checksum;

/* See if valid .cinfo file exists for given filename.
   Use cpp_valid_state() (from PCH) to validate preprocessor state.  */
int c_valid_cinfo PARAMS ((cpp_reader *, const char *));
/* APPLE LOCAL end Symbol Separation */

/* APPLE LOCAL begin distcc pch */
static int init_indirect_pipes PARAMS ((int *, int*));
static int read_from_parent PARAMS ((int, char *, int));
static int write_to_parent PARAMS ((int, const char *));
static char *indirect_file PARAMS ((char *, const int));

static const char message_terminator = '\n';

/* Communications routine to communicate with filename translation
   server for distributed builds.  This routine reads data from the
   server.  */

static int
read_from_parent (fd, buffer, size)
     int fd;
     char *buffer;
     int size;
{
  int index = 0;
  int result;

  if (size <= 0)
    return 0;

  do {
    result = read (fd, &buffer[index], size - index);

    if (result <= 0 || index >= size )
      return 0;
    else
      index += result;
  } while (buffer[index - 1] != message_terminator);

  /* Straighten out the string termination. */
  buffer[index - 1] = '\0';

  return 1;
}

/* Communications routine to communicate with filename translation server
   for distributed builds.  This routine writes data to the server.  */

static int
write_to_parent (fd, message)
     int fd;
     const char *message;
{
  int result;

  if (message) {
    const int length = strlen (message);
    int index = 0;

    while (index < length) {
      result = write (fd, &message[index], length - index);

      if (result < 0)
        return 0;
      else
        index += result;
    }
  }

  result = write (fd, &message_terminator, 1);

  if (result < 0)
    return 0;

  return 1;
}


/* Initialize the filename translation service.  */

static int
init_indirect_pipes (read_fd, write_fd)
     int *read_fd;
     int *write_fd;
{
  const char *file_indirect_pipes = getenv ("GCC_INDIRECT_FILES");
  const char *protocol_operation = "VERS";
  const char *protocol_version = "1";
  char response[MAXPATHLEN];

  if (!file_indirect_pipes)
    return -1;

  /* The environment variable indicates that the process that invoked
     gcc would like to provide a different path for certain files.
     This is mainly intended to be used with PCH headers and symbol
     separation files (.cinfo) files under certain circumstances. */

  if (sscanf (file_indirect_pipes, "%d, %d", read_fd, write_fd) != 2)
    return -1;

  /* Verify the protocol version. */
  if (write_to_parent (*write_fd, protocol_operation))
    if (write_to_parent (*write_fd, protocol_version))
      if (read_from_parent (*read_fd, response, MAXPATHLEN))
	if (strcmp ("OK", response) == 0)
	  return 1;

  return -1;
}

/* Redirect file I/O at the direction of a translation server.  fname
   is the filename to transform.  OPERATION is:

   0 for reading
   1 for writing
   2 for reading and writing  */

static char *
indirect_file (fname, operation)
     char *fname;
     int operation;
{
  static int indirection_initialized;
  static int read_fd;
  static int write_fd;
  const char *operation_identifier = NULL;

  if (!indirection_initialized)
    indirection_initialized = init_indirect_pipes (&read_fd, &write_fd);

  if (indirection_initialized != 1)
    return fname;

  switch (operation)
    {
    case 0:
      operation_identifier = "PULL";
      break;
    case 1:
      operation_identifier = "PUSH";
      break;
    case 2:
      operation_identifier = "BOTH";
      break;
    default:
      return fname;
    }

  if (write_to_parent (write_fd, operation_identifier))
    if (write_to_parent (write_fd, fname))
      {
	char response[MAXPATHLEN];

	if (read_from_parent (read_fd, response, MAXPATHLEN))
	  fname = xstrdup (response);
      }

  return fname;
}
/* APPLE LOCAL end distcc pch */

/* Set up the splay tree we use to store information about all the
   file names seen in this compilation.  We also have entries for each
   file we tried to open but failed; this saves system calls since we
   don't try to open it again in future.

   The key of each node is the file name, after processing by
   _cpp_simplify_pathname.  The path name may or may not be absolute.
   The path string has been malloced, as is automatically freed by
   registering free () as the splay tree key deletion function.

   A node's value is a pointer to a struct include_file, and is never
   NULL.  */
void
_cpp_init_includes (pfile)
     cpp_reader *pfile;
{
  pfile->all_include_files
    = splay_tree_new ((splay_tree_compare_fn) strcmp,
		      (splay_tree_delete_key_fn) free,
		      destroy_node);
}

/* Tear down the splay tree.  */
void
_cpp_cleanup_includes (pfile)
     cpp_reader *pfile;
{
  splay_tree_delete (pfile->all_include_files);
}

/* Free a node.  The path string is automatically freed.  */
static void
destroy_node (v)
     splay_tree_value v;
{
  struct include_file *f = (struct include_file *) v;

  if (f)
    {
      purge_cache (f);
      free (f);
    }
}

/* Mark a file to not be reread (e.g. #import, read failure).  */
void
_cpp_never_reread (file)
     struct include_file *file;
{
  file->cmacro = NEVER_REREAD;
}

/* Lookup a filename, which is simplified after making a copy, and
   create an entry if none exists.  errno is nonzero iff a (reported)
   stat() error occurred during simplification.  */
static splay_tree_node
find_or_create_entry (pfile, fname)
     cpp_reader *pfile;
     const char *fname;
{
  splay_tree_node node;
  struct include_file *file;
  char *name = xstrdup (fname);

  _cpp_simplify_pathname (name);
  node = splay_tree_lookup (pfile->all_include_files, (splay_tree_key) name);
  if (node)
    free (name);
  else
    {
      file = xcnew (struct include_file);
      file->name = name;
      file->header_name = name;
      file->err_no = errno;
      node = splay_tree_insert (pfile->all_include_files,
				(splay_tree_key) file->name,
				(splay_tree_value) file);
    }

  return node;
}

/* Enter a file name in the splay tree, for the sake of cpp_included.  */
void
_cpp_fake_include (pfile, fname)
     cpp_reader *pfile;
     const char *fname;
{
  find_or_create_entry (pfile, fname);
}

/* APPLE LOCAL begin #import inode hack 2001-10-29 sts */
static ino_t new_inode;
static dev_t new_device;
static struct include_file *repl_file;

static int inode_finder PARAMS ((splay_tree_node, void *data));

static int
inode_finder (x, data)
     splay_tree_node x;
     void *data ATTRIBUTE_UNUSED;
{
  ino_t inode = 0;
  dev_t device = 0;

  if ((repl_file = (struct include_file *)x->value))
    {
      inode = repl_file->st.st_ino;
      device = repl_file->st.st_dev;
      if (inode == new_inode && device == new_device && DO_NOT_REREAD (repl_file))
	return 1;
    }
  return 0;
}

/* APPLE LOCAL begin pch #import hack */
#include <md5.h>
static struct pchf_data {

  size_t count;
  int have_once_only;
  
  struct pchf_entry {
    off_t size;
    unsigned char sum[16];
    char once_only;
  } entries[1];

} *pchf;

static int
pchf_counter (x, data)
     splay_tree_node x;
     void *data;
{
  struct include_file *f = (struct include_file *)x->value;
  if (f != NULL && f->include_count != 0)
    ++ *(size_t *)data;
  return 0;
}

static int
pchf_adder (x, data)
     splay_tree_node x;
     void *data;
{
  struct include_file *f = (struct include_file *)x->value;
  struct pchf_data *d = (struct pchf_data *)data;
  if (f != NULL && f->include_count != 0)
    {
      size_t count = d->count++;
      
      d->entries[count].size = f->st.st_size;
      d->entries[count].once_only = f->cmacro == NEVER_REREAD;
      d->have_once_only |= d->entries[count].once_only;
      if (f->buffer)
	md5_buffer ((const char *)f->buffer, 
		    f->st.st_size, d->entries[count].sum);
      else
	{
	  FILE *ff;
	  ff = fopen (f->name, "rb");
	  md5_stream (ff, d->entries[count].sum);
	  fclose (ff);
	}
    }
  return 0;
}

static int
pchf_save_compare (e1, e2)
     const void *e1;
     const void *e2;
{
  return memcmp (e1, e2, sizeof (struct pchf_entry));
}

int
_cpp_save_file_entries (pfile, f)
     cpp_reader *pfile;
     FILE *f;
{
  size_t count = 0;
  struct pchf_data *result;
  size_t result_size;
  
  splay_tree_foreach (pfile->all_include_files, pchf_counter, &count);
  result_size = (sizeof (struct pchf_data) 
		 + sizeof (struct pchf_entry) * (count - 1));
  result = xmalloc (result_size);
  
  result->count = 0;
  result->have_once_only = false;
  
  splay_tree_foreach (pfile->all_include_files, pchf_adder, result);

  qsort (result->entries, count, sizeof (struct pchf_entry), 
	 pchf_save_compare);

  return fwrite (result, result_size, 1, f) == 1 ? 0 : -1;
}

int
_cpp_read_file_entries (pfile, f)
     cpp_reader *pfile ATTRIBUTE_UNUSED;
     FILE *f;
{
  struct pchf_data d;
  
  if (fread (&d, sizeof (struct pchf_data) - sizeof (struct pchf_entry), 1, f)
       != 1)
    return -1;
  
  pchf = xmalloc (sizeof (struct pchf_data)
		  + sizeof (struct pchf_entry) * (d.count - 1));
  memcpy (pchf, &d, sizeof (struct pchf_data) - sizeof (struct pchf_entry));
  if (fread (pchf->entries, sizeof (struct pchf_entry), d.count, f)
      != d.count)
    return -1;
  return 0;
}

struct pchf_compare_data 
{
  off_t size;
  unsigned char sum[16];
  bool sum_computed;
  bool check_included;
  struct include_file *f;
};

static int
pchf_compare (d_p, e_p)
     const void *d_p;
     const void *e_p;
{
  const struct pchf_entry *e = (const struct pchf_entry *)e_p;
  struct pchf_compare_data *d = (struct pchf_compare_data *)d_p;
  int result;
  
  result = memcmp (&d->size, &e->size, sizeof (off_t));
  if (result != 0)
    return result;
  
  if (! d->sum_computed)
    {
      struct include_file *const f = d->f;
      
      if (f->buffer)
	md5_buffer ((const char *)f->buffer, f->st.st_size, d->sum);
      else
	{
	  FILE *ff;
	  ff = fopen (f->name, "rb");
	  md5_stream (ff, d->sum);
	  fclose (ff);
	}
      d->sum_computed = true;
    }

  result = memcmp (d->sum, e->sum, 16);
  if (result != 0)
    return result;

  if (d->check_included || e->once_only)
    return 0;
  else
    return 1;
}

static int
check_file_against_entries (pfile, f, check_included)
     cpp_reader *pfile ATTRIBUTE_UNUSED;
     struct include_file *f;
     int check_included;
{
  struct pchf_compare_data d;
  
  if (pchf == NULL
      || (! check_included && ! pchf->have_once_only))
    return 0;
  
  d.size = f->st.st_size;
  d.sum_computed = false;
  d.f = f;
  d.check_included = check_included;
  return bsearch (&d, pchf->entries, pchf->count, sizeof (struct pchf_entry),
		  pchf_compare) != NULL;
}

/* APPLE LOCAL end pch #import hack */
/* APPLE LOCAL end #import inode hack 2001-10-29 sts */

/* Given a file name, look it up in the cache; if there is no entry,
   create one with a non-NULL value (regardless of success in opening
   the file).  If the file doesn't exist or is inaccessible, this
   entry is flagged so we don't attempt to open it again in the
   future.  If the file isn't open, open it.  The empty string is
   interpreted as stdin.

   Returns an include_file structure with an open file descriptor on
   success, or NULL on failure.  */
static struct include_file *
open_file (pfile, filename)
     cpp_reader *pfile;
     const char *filename;
{
  splay_tree_node nd = find_or_create_entry (pfile, filename);
  struct include_file *file = (struct include_file *) nd->value;

  if (file->err_no)
    {
      /* Ugh.  handle_missing_header () needs errno to be set.  */
      errno = file->err_no;
      return 0;
    }

  /* Don't reopen an idempotent file.  */
  if (DO_NOT_REREAD (file))
    return file;

  /* Don't reopen one which is already loaded.  */
  if (file->buffer != NULL)
    return file;

  /* We used to open files in nonblocking mode, but that caused more
     problems than it solved.  Do take care not to acquire a
     controlling terminal by mistake (this can't happen on sane
     systems, but paranoia is a virtue).

     Use the three-argument form of open even though we aren't
     specifying O_CREAT, to defend against broken system headers.

     O_BINARY tells some runtime libraries (notably DJGPP) not to do
     newline translation; we can handle DOS line breaks just fine
     ourselves.

     Special case: the empty string is translated to stdin.  */

  if (filename[0] == '\0')
    {
      file->fd = 0;
#ifdef __DJGPP__
      /* For DJGPP redirected input is opened in text mode. Change it
         to binary mode.  */
      if (! isatty (file->fd))
	setmode (file->fd, O_BINARY);
#endif
    }
  else
    file->fd = open (file->name, O_RDONLY | O_NOCTTY | O_BINARY, 0666);

  if (file->fd != -1 && fstat (file->fd, &file->st) == 0)
    {
      /* APPLE LOCAL begin #import inode hack 2001-10-29 sts */
      new_inode = file->st.st_ino;
      new_device = file->st.st_dev;
      if (splay_tree_foreach (pfile->all_include_files, inode_finder, 0))
	{
	  close (file->fd);
	  file->fd  = -1;
	  return repl_file;
	}
      /* APPLE LOCAL end #import inode hack 2001-10-29 sts */

      if (!S_ISDIR (file->st.st_mode))
	return file;

      /* If it's a directory, we return null and continue the search
	 as the file we're looking for may appear elsewhere in the
	 search path.  */
      errno = ENOENT;
      close (file->fd);
      file->fd = -1;
    }

  file->err_no = errno;
  return 0;
}

static bool invalid_pch_found;

static struct include_file *
validate_pch (pfile, filename, pchname)
     cpp_reader *pfile;
     const char *filename;
     const char *pchname;
{
  struct include_file * file;
  
  file = open_file (pfile, pchname);
  if (file == NULL)
    return NULL;
  if ((file->pch & 2) == 0)
    file->pch = pfile->cb.valid_pch (pfile, pchname, file->fd);
  if (! INCLUDE_PCH_P (file))
    invalid_pch_found = true;
  if (CPP_OPTION (pfile, print_include_names))
    {
      unsigned int i;
      for (i = 1; i < pfile->line_maps.depth; i++)
	putc ('.', stderr);
      fprintf (stderr, "%c %s\n", INCLUDE_PCH_P (file) ? '!' : 'x', pchname);
    }
  if (INCLUDE_PCH_P (file))
    {
      file->header_name = _cpp_simplify_pathname (xstrdup (filename));
      return file;
    }
  close (file->fd);
  file->fd = -1;
  return NULL;
}

/* Like open_file, but also look for a precompiled header if (a) one exists
   and (b) it is valid.  */
static struct include_file *
open_file_pch (pfile, filename)
     cpp_reader *pfile;
     const char *filename;
{
  if (filename[0] != '\0'
      && pfile->cb.valid_pch != NULL)
    {
      size_t namelen = strlen (filename);
      char *pchname = alloca (namelen + 5);
      struct include_file * file;
      splay_tree_node nd;
      
      /* APPLE LOCAL .gch and .cinfo suffix */
      if (namelen > 6 && strcmp(&filename[namelen-6], ".cinfo") == 0)
        return open_file (pfile, filename);
      
      memcpy (pchname, filename, namelen);
      /* APPLE LOCAL .gch suffix */
      memcpy (pchname + namelen, ".gch", 5);

      /* APPLE LOCAL begin distcc pch */
      pchname = indirect_file (pchname, 0);
      namelen = strlen (pchname) - 4;
      /* APPLE LOCAL end distcc pch */

      nd = find_or_create_entry (pfile, pchname);
      file = (struct include_file *) nd->value;

      if (file != NULL)
	{
	  if (stat (file->name, &file->st) == 0 && S_ISDIR (file->st.st_mode))
	    {
	      DIR * thedir;
	      struct dirent *d;
	      size_t subname_len = namelen + 64;
	      char *subname = xmalloc (subname_len);
	      
	      thedir = opendir (pchname);
	      if (thedir == NULL)
		return NULL;
	      memcpy (subname, pchname, namelen + 4);
	      subname[namelen+4] = '/';
	      while ((d = readdir (thedir)) != NULL)
		{
		  if (strlen (d->d_name) + namelen + 7 > subname_len)
		    {
		      subname_len = strlen (d->d_name) + namelen + 64;
		      subname = xrealloc (subname, subname_len);
		    }
		  strcpy (subname + namelen + 5, d->d_name);
		  file = validate_pch (pfile, filename, subname);
		  if (file)
		    break;
		}
	      closedir (thedir);
	      free (subname);
	    }
	  else
	    file = validate_pch (pfile, filename, pchname);
	  if (file)
	    return file;
	}
    }
  return open_file (pfile, filename);
}

/* Place the file referenced by INC into a new buffer on the buffer
   stack, unless there are errors, or the file is not re-included
   because of e.g. multiple-include guards.  Returns true if a buffer
   is stacked.  */
static bool
stack_include_file (pfile, inc)
     cpp_reader *pfile;
     struct include_file *inc;
{
  cpp_buffer *fp;
  int sysp;
  const char *filename;

  if (DO_NOT_REREAD (inc))
    return false;

  sysp = MAX ((pfile->map ? pfile->map->sysp : 0),
	      (inc->foundhere ? inc->foundhere->sysp : 0));

  /* Add the file to the dependencies on its first inclusion.  */
  if (CPP_OPTION (pfile, deps.style) > !!sysp && !inc->include_count)
    {
      if (pfile->buffer || CPP_OPTION (pfile, deps.ignore_main_file) == 0)
	deps_add_dep (pfile->deps, inc->name);
    }

  /* PCH files get dealt with immediately.  */
  if (INCLUDE_PCH_P (inc))
    {
      pfile->cb.read_pch (pfile, inc->name, inc->fd, inc->header_name);
      close (inc->fd);
      inc->fd = -1;
      return false;
    }

  /* Not in cache?  */
  if (! inc->buffer)
    {
      if (read_include_file (pfile, inc))
	{
	  /* If an error occurs, do not try to read this file again.  */
	  _cpp_never_reread (inc);
	  return false;
	}
      /* Mark a regular, zero-length file never-reread.  We read it,
	 NUL-terminate it, and stack it once, so preprocessing a main
	 file of zero length does not raise an error.  */
      if (S_ISREG (inc->st.st_mode) && inc->st.st_size == 0)
	_cpp_never_reread (inc);
      /* APPLE LOCAL read-from-stdin */
      if (inc->fd != 0)  /* Don't close stdin */
      {
        close (inc->fd);
        inc->fd = -1;
      }
      /* APPLE LOCAL end read-from-stdin */
    }

  if (pfile->buffer)
    /* We don't want MI guard advice for the main file.  */
    inc->include_count++;

  /* APPLE LOCAL begin Symbol Separation */
  if (pfile->cinfo_state == CINFO_VALID)
    {
      inc->suppress_dbg = 1;
      pfile->cinfo_state = CINFO_READ;
      if (pfile->cb.clear_write_symbols)
	  pfile->cb.clear_write_symbols (inc->name, cpp_get_stabs_checksum ());
    }
  /* APPLE LOCAL end Symbol Separation */

  /* Push a buffer.  */
  fp = cpp_push_buffer (pfile, inc->buffer, inc->st.st_size,
			/* from_stage3 */ CPP_OPTION (pfile, preprocessed), 0);
  fp->inc = inc;
  fp->inc->refcnt++;

  /* Initialize controlling macro state.  */
  pfile->mi_valid = true;
  pfile->mi_cmacro = 0;

  /* Generate the call back.  */
  filename = inc->name;
  if (*filename == '\0') 
  {
    /* APPLE LOCAL begin read-from-stdin */
    filename = CPP_OPTION(pfile, stdin_diag_filename);
    if (filename == NULL)
    {
      filename = "<stdin>";
    }
    /* APPLE LOCAL end read-from-stdin */
  }
  _cpp_do_file_change (pfile, LC_ENTER, filename, 1, sysp);

  return true;
}

/* Read the file referenced by INC into the file cache.

   If fd points to a plain file, we might be able to mmap it; we can
   definitely allocate the buffer all at once.  If fd is a pipe or
   terminal, we can't do either.  If fd is something weird, like a
   block device, we don't want to read it at all.

   Unfortunately, different systems use different st.st_mode values
   for pipes: some have S_ISFIFO, some S_ISSOCK, some are buggy and
   zero the entire struct stat except a couple fields.  Hence we don't
   even try to figure out what something is, except for plain files
   and block devices.

   FIXME: Flush file cache and try again if we run out of memory.  */
static int
read_include_file (pfile, inc)
     cpp_reader *pfile;
     struct include_file *inc;
{
  ssize_t size, offset, count;
  uchar *buf;
#if MMAP_THRESHOLD
  static int pagesize = -1;
#endif

  if (S_ISREG (inc->st.st_mode))
    {
      /* off_t might have a wider range than ssize_t - in other words,
	 the max size of a file might be bigger than the address
	 space.  We can't handle a file that large.  (Anyone with
	 a single source file bigger than 2GB needs to rethink
	 their coding style.)  Some systems (e.g. AIX 4.1) define
	 SSIZE_MAX to be much smaller than the actual range of the
	 type.  Use INTTYPE_MAXIMUM unconditionally to ensure this
	 does not bite us.  */
      if (inc->st.st_size > INTTYPE_MAXIMUM (ssize_t))
	{
	  cpp_error (pfile, DL_ERROR, "%s is too large", inc->name);
	  goto fail;
	}
      size = inc->st.st_size;

      inc->mapped = 0;
#if MMAP_THRESHOLD
/* APPLE LOCAL begin darwin mmap bug workaround  ilr */
#if USING_DARWIN_MMAP
      if (pagesize == -1)
        {
	  pagesize = getpagesize ();

	  /* Darwin kernels with a version number 5.Y or earlier have a
	     bug in mmap where there is no guarantee that the bytes
	     beyond a file's logical eof are zero (actually the bytes
	     up to the next 512-byte boundary -- 512 is the I/O block
	     size).  Since using mmap here depends on a 0 at the eof we
	     can't reliably use it when running under those kernels.
	     But we don't want to stop using mmaps on kernels from 6.Y
	     and beyond (some measurements show it does improve things
	     a bit).  We can do this by checking the kernel version
	     number.  A call to host_kernel_version() gives us the 
	     version info (note that USING_DARWIN_MMAP is for Darwin
	     only as set in config.gcc).
	     
	     The host_kernel_version() call generates as string that 
	     looks like "Darwin Kernel Version X.Y...more stuff...",
	     where X.Y is the Darwin kernel version.  We look for the
	     '.' and then for a blank and digit before it.  If the
	     digit is less then 6 we assume we cannot use mmap.  It's
	     a "brain dead" test but good enough.  Even if future 
	     kernels change the format the test will fail and we'll
	     think it's a newer kernel.  
	     
	     Maybe someday we can remove all of this.  But it's better
	     to have a slightly slower but working compiler than one
	     that can fail because of a bug on the kernel.  */

	  if (host_kernel_version (mach_host_self (), darwin_kernel_version))
	    darwin_mmap_allowed = 0;
	  else
	    {
	      char *s = strchr(darwin_kernel_version, '.');
	      if (s && *(s-2) == ' ' && *(s-1) < '6')
	      	darwin_mmap_allowed = 0;
	      else
	      	darwin_mmap_allowed = 1;
	    }
	}

      /* Use mmap if the file is big enough to be worth it (controlled
	 by MMAP_THRESHOLD) and if we can safely count on there being
	 at least one readable NUL byte after the end of the file's
	 contents.  This is true for all tested operating systems when
	 the file size is not an exact multiple of the page size.  */
      if (darwin_mmap_allowed &&
      	  size / pagesize >= MMAP_THRESHOLD
	  && (size % pagesize) != 0)
	{
	  buf = (unsigned char *) mmap (0, size, PROT_READ, MAP_PRIVATE, inc->fd, 0);
	  if (buf == (unsigned char *)-1)
	    goto perror_fail;
	  inc->mapped = 1;
	}
      else
#else /* !USING_DARWIN_MMAP */
/* APPLE LOCAL end darwin mmap bug workaround  ilr */
      if (pagesize == -1)
	pagesize = getpagesize ();

      if (SHOULD_MMAP (size, pagesize))
	{
	  buf = (uchar *) mmap (0, size, PROT_READ, MAP_PRIVATE, inc->fd, 0);
	  if (buf == (uchar *) -1)
	    goto perror_fail;

	  /* We must tell Valgrind that the byte at buf[size] is actually
	     readable.  Discard the handle to avoid handle leak.  */
	  VALGRIND_DISCARD (VALGRIND_MAKE_READABLE (buf + size, 1));

	  inc->mapped = 1;
	}
      else
/* APPLE LOCAL darwin mmap bug workaround  ilr */
#endif /* !USING_DARWIN_MMAP */
#endif
	{
	  buf = (uchar *) xmalloc (size + 1);
	  offset = 0;
	  while (offset < size)
	    {
	      count = read (inc->fd, buf + offset, size - offset);
	      if (count < 0)
		goto perror_fail;
	      if (count == 0)
		{
		  if (!STAT_SIZE_TOO_BIG (inc->st))
		    cpp_error (pfile, DL_WARNING,
			       "%s is shorter than expected", inc->name);
		  size = offset;
		  buf = xrealloc (buf, size + 1);
		  inc->st.st_size = size;
		  break;
		}
	      offset += count;
	    }
	  /* The lexer requires that the buffer be NUL-terminated.  */
	  buf[size] = '\0';
	}
    }
  else if (S_ISBLK (inc->st.st_mode))
    {
      cpp_error (pfile, DL_ERROR, "%s is a block device", inc->name);
      goto fail;
    }
  /* APPLE LOCAL begin read-from-stdin */
  else if ( CPP_OPTION(pfile, stdin_diag_filename) != NULL )
    {
      if (CPP_OPTION(pfile, predictive_compilation_size) >= 0)
      {
	count = 0;
	size = CPP_OPTION(pfile, predictive_compilation_size);
	offset = 0;
	buf = (uchar *) xmalloc (size + 1);
	if (size > 0)
	{
	    while ((count = read (inc->fd, buf + offset, size - offset)) > 0)
	    {
	      offset += count;
	      if (offset == size)
		break;
	    }
	} 
	CPP_OPTION(pfile, predictive_compilation_size) = -1;
        if (count == 0)
          count = 1;
      }
      else
      {
        size = 8 * 1024;
        buf = (uchar *) xmalloc (size + 1);
        offset = 0;
        while ((count = read (inc->fd, buf + offset, size - offset)) > 0)
        {
	    offset += count;
	    if (offset == size)
	      {
	        size *= 2;
	        buf = xrealloc (buf, size + 1);
	      }
        }
      }
      more_to_read_from_stdin = (count > 0);

      if (offset + 1 < size)
	buf = xrealloc (buf, offset + 1);

      /* The lexer requires that the buffer be NUL-terminated.  */
      buf[offset] = '\0';
      inc->st.st_size = offset;
    }
  /* APPLE LOCAL end read-from-stdin */
  else
    {
      /* 8 kilobytes is a sensible starting size.  It ought to be
	 bigger than the kernel pipe buffer, and it's definitely
	 bigger than the majority of C source files.  */
      size = 8 * 1024;

      buf = (uchar *) xmalloc (size + 1);
      offset = 0;
      while ((count = read (inc->fd, buf + offset, size - offset)) > 0)
	{
	  offset += count;
	  if (offset == size)
	    {
	      size *= 2;
	      buf = xrealloc (buf, size + 1);
	    }
	}
      if (count < 0)
	goto perror_fail;

      if (offset + 1 < size)
	buf = xrealloc (buf, offset + 1);

      /* The lexer requires that the buffer be NUL-terminated.  */
      buf[offset] = '\0';
      inc->st.st_size = offset;
    }

  inc->buffer = buf;
  return 0;

 perror_fail:
  cpp_errno (pfile, DL_ERROR, inc->name);
 fail:
  return 1;
}

/* APPLE LOCAL begin read-from-stdin */
bool read_from_stdin(pfile)
cpp_reader *pfile;
{
  cpp_buffer *buffer;
  struct include_file *inc;

  if (!more_to_read_from_stdin)
    return false;

  buffer = pfile->buffer;
  inc = buffer->inc;
  if (inc == NULL || inc->name[0] != '\0')
    return false;
  purge_cache(inc);

  if (!read_include_file(pfile, inc))
  {
    buffer->line_base = buffer->buf = buffer->cur = inc->buffer;
    buffer->rlimit = buffer->buf + inc->st.st_size;
    return true;
  }
  return false;
}
/* APPLE LOCAL end read-from-stdin */

/* Drop INC's buffer from memory, if we are unlikely to need it again.  */
static void
purge_cache (inc)
     struct include_file *inc;
{
  if (inc->buffer)
    {
#if MMAP_THRESHOLD
      if (inc->mapped)
	{
	  /* Undo the previous annotation for the
	     known-zero-byte-after-mmap.  Discard the handle to avoid
	     handle leak.  */
	  VALGRIND_DISCARD (VALGRIND_MAKE_NOACCESS (inc->buffer
						    + inc->st.st_size, 1));
	  munmap ((PTR) inc->buffer, inc->st.st_size);
	}
      else
#endif
	free ((PTR) inc->buffer);
      inc->buffer = NULL;
    }
}

/* Return 1 if the file named by FNAME has been included before in
   any context, 0 otherwise.  */
int
cpp_included (pfile, fname)
     cpp_reader *pfile;
     const char *fname;
{
  struct search_path *path;
  char *name, *n;
  splay_tree_node nd;

  if (IS_ABSOLUTE_PATHNAME (fname))
    {
      /* Just look it up.  */
      nd = splay_tree_lookup (pfile->all_include_files, (splay_tree_key) fname);
      return (nd && nd->value);
    }

  /* Search directory path for the file.  */
  name = (char *) alloca (strlen (fname) + pfile->max_include_len + 2);
  for (path = CPP_OPTION (pfile, quote_include); path; path = path->next)
    {
      memcpy (name, path->name, path->len);
      name[path->len] = '/';
      strcpy (&name[path->len + 1], fname);
      if (CPP_OPTION (pfile, remap))
	n = remap_filename (pfile, name, path);
      else
	n = name;

      nd = splay_tree_lookup (pfile->all_include_files, (splay_tree_key) n);
      if (nd && nd->value)
	return 1;
    }
  return 0;
}

/* APPLE LOCAL begin Symbol Separation */
void
find_include_cinfo (pfile, in_name)
     cpp_reader *pfile;
     const char *in_name;
{
  struct search_path *path;
  struct include_file *file;
  size_t len, namelen;
  char *name, *cinfoname;
  const char *fname = NULL;

  if (!in_name)
    return;

  len = namelen = strlen (in_name);
  cinfoname = alloca (namelen + 8);

  /* Extract header name from path */
  while (len > 0 && in_name[len] != '/')
    --len;

  if (in_name[len] == '/')
    ++len;

  if (!in_name[len])
    return;

  fname = &in_name[len];

  pfile->cinfo_candidate_file = fname;
  pfile->cinfo_src_file = fname;

  path = search_from (pfile, IT_INCLUDE);

  if (path == NULL)
    return;

  memcpy (cinfoname, fname, namelen);
  memcpy (cinfoname + namelen, ".cinfo", 7);
  cinfoname[namelen+7] = NULL;
  
  name = (char *) alloca (strlen (fname) + pfile->max_include_len + 2 + 500);
  /* handle everything but gcc's include_next and -I- extensions */
  if ((file = find_include_file_in_hashtable (pfile, cinfoname, name, IT_INCLUDE, path)))
    {
      size_t dirlen = strlen ((file->foundhere)->name);
      char *cinfo_fullname = xmalloc (namelen + dirlen + 1);
      memcpy (cinfo_fullname,  (file->foundhere)->name, dirlen);
      memcpy (cinfo_fullname + dirlen, "/", 1);
      memcpy (cinfo_fullname + dirlen + 1, fname, namelen);
      cinfo_fullname [dirlen + namelen + 1] = NULL;
      pfile->cinfo_state = CINFO_FOUND;
      pfile->cinfo_candidate_file = cinfo_fullname;
      file = NULL;
    }
}
/* APPLE LOCAL end Symbol Separation */

/* Search for HEADER.  Return 0 if there is no such file (or it's
   un-openable), in which case an error code will be in errno.  If
   there is no include path to use it returns NO_INCLUDE_PATH,
   otherwise an include_file structure.  If this request originates
   from a directive of TYPE #include_next, set INCLUDE_NEXT to true.  */
static struct include_file *
find_include_file (pfile, header, type)
     cpp_reader *pfile;
     const cpp_token *header;
     enum include_type type;
{
  const char *fname = (const char *) header->val.str.text;
  struct search_path *path;
  /* APPLE LOCAL begin -header-mapfile */
  struct search_path *saved_path;
  /* APPLE LOCAL end -header-mapfile */
  struct include_file *file;
  char *name, *n;

  /* APPLE LOCAL begin Symbol Separation */
  if (c_valid_cinfo (pfile, fname))
    pfile->cinfo_state = CINFO_VALID;
  /* APPLE LOCAL end Symbol Separtion */

  /* APPLE LOCAL begin distcc pch */
  if (type == IT_INCLUDE_PCH)
    {
      struct include_file * file;
      splay_tree_node nd;

      cpp_get_options (pfile)->warn_invalid_pch = 1;
      cpp_get_options (pfile)->pch_preprocess = 1;

      fname = indirect_file (fname, 0);
      nd = find_or_create_entry (pfile, fname);
      file = (struct include_file *) nd->value;
      if (file != NULL)
	{
	  file = open_file (pfile, fname);
	  if (file)
	    {
	      if ((file->pch & 2) == 0)
		file->pch = pfile->cb.valid_pch (pfile, fname, file->fd);
	      if (INCLUDE_PCH_P (file))
		{
		  file->header_name = _cpp_simplify_pathname (xstrdup (fname));
		  return file;
		}
	      close (file->fd);
	      file->fd = -1;
	    }
	}
      cpp_error (pfile, DL_ERROR, "invalid pch file %s", fname);
      return 0;
    }
  /* APPLE LOCAL begin distcc pch */

  if (IS_ABSOLUTE_PATHNAME (fname))
    return open_file_pch (pfile, fname);
  
  /* For #include_next, skip in the search path past the dir in which
     the current file was found, but if it was found via an absolute
     path use the normal search logic.  */
  if (type == IT_INCLUDE_NEXT && pfile->buffer->inc->foundhere)
    path = pfile->buffer->inc->foundhere->next;
  else if (header->type == CPP_HEADER_NAME)
    {
      path = CPP_OPTION (pfile, bracket_include);
/* APPLE LOCAL begin framework headers */
#ifdef FRAMEWORK_HEADERS
      if (path == NULL)
	path = CPP_OPTION (pfile, framework_include);
#endif
/* APPLE LOCAL end framework headers */
    }
  else
    path = search_from (pfile, type);

  if (path == NULL)
    {
      cpp_error (pfile, DL_ERROR, "no include path in which to find %s",
		 fname);
      return NO_INCLUDE_PATH;
    }

  /* Search directory path for the file.  */
  /* APPLE LOCAL -header-mapfile bandaid for buffer overflows */
  name = (char *) alloca (strlen (fname) + pfile->max_include_len + 2 + 500);

  /* APPLE LOCAL begin header search */
  if ((type != IT_INCLUDE_NEXT) && 
      (CPP_OPTION (pfile, bracket_include) == CPP_OPTION (pfile, quote_include)))
    {
      /* APPLE LOCAL begin Symbol Separation */
      if (CPP_OPTION (pfile, use_ss))
	{
	  size_t namelen = strlen (fname);
	  char *cinfoname = alloca (namelen + 8);
	  memcpy (cinfoname, fname, namelen);
	  memcpy (cinfoname + namelen, ".cinfo", 7);
	  cinfoname[namelen+7] = NULL;
	  /* handle everything but gcc's include_next and -I- extensions */
	  if ((file = find_include_file_in_hashtable (pfile, cinfoname, name, header->type, path)))
	    {
	      size_t dirlen = strlen ((file->foundhere)->name);
	      char *cinfo_fullname = alloca (namelen + dirlen + 1);
	      memcpy (cinfo_fullname,  (file->foundhere)->name, dirlen);
	      memcpy (cinfo_fullname + dirlen, "/", 1);
	      memcpy (cinfo_fullname + dirlen + 1, fname, namelen);
	      cinfo_fullname [dirlen + namelen + 1] = NULL;
	      if (c_valid_cinfo (pfile, cinfo_fullname))
		pfile->cinfo_state = CINFO_VALID;
	      file = NULL;
	    }
	}
      /* APPLE LOCAL end Symbol Separation */
      /* handle everything but gcc's include_next and -I- extensions */
      if ((file = find_include_file_in_hashtable (pfile, fname, name, header->type, path)))
	return file;
    }
  /* APPLE LOCAL end header search */

  for (; path; path = path->next)
    {
      /* APPLE LOCAL begin -header-mapfile */
      saved_path = path;
      if (path == CPP_OPTION (pfile, bracket_include)
	  && CPP_OPTION (pfile, header_map))
	path = hmap_lookup_path (pfile, &fname);
      {
      /* APPLE LOCAL end -header-mapfile */
      int len = path->len;
      memcpy (name, path->name, len);
      /* Don't turn / into // or // into ///; // may be a namespace
	 escape.  */
      if (name[len-1] == '/')
	len--;
      name[len] = '/';
      strcpy (&name[len + 1], fname);
      /* APPLE LOCAL -header-mapfile */
      }
      if (CPP_OPTION (pfile, remap))
	n = remap_filename (pfile, name, path);
      else
	n = name;

      /* APPLE LOCAL begin Symbol Separation */
      if (c_valid_cinfo (pfile, fname))
	pfile->cinfo_state = CINFO_VALID;
      /* APPLE LOCAL end Symbol Separtion */

      file = open_file_pch (pfile, n);
      if (file)
	{
	  file->foundhere = path;
	  return file;
	}
      /* APPLE LOCAL begin -header-mapfile */
      else
        {
          if (saved_path == CPP_OPTION (pfile, bracket_include)
              && CPP_OPTION (pfile, header_map))
            {
              /* Path was collected from header_map buckets.
                 No point in using it again, if file does not
                 exist. Otherwise we will never come out of
                 for (; path; path = path->next) loop.  */
              path = saved_path;    
            }
        }
      /* APPLE LOCAL end -header-mapfile */
    }

  return 0;
}

/* APPLE LOCAL begin framework headers */
#ifdef FRAMEWORK_HEADERS
/* Search for HEADER frameworks.  Return 0 if there is no such file
   (or it's un-openable), in which case an error code will be in
   errno. Translates the header file name suitable for frameworks from
   "Foundation/Foundation.h" into
   "Foundation.frameworks/PrivateHeaders/Foundation.h".  */

static struct include_file *
find_framework_file (pfile, header, type)
     cpp_reader *pfile;
     const cpp_token *header;
     enum include_type type ATTRIBUTE_UNUSED;
{
  struct include_file *inc = NULL;
  const char *fname = (const char *) header->val.str.text;
  struct search_path *path;
  struct include_file *file;
  char *n, *frname;
  unsigned char *bufptr;
  unsigned position;
  int frname_len, slash_position, index; 

  if (fname == NULL)
    return 0; 

  /* For #include_next, skip in the search path past the dir in which
     the current file was found, but if it was found via an absolute
     path use the normal search logic.  */

  path = NULL;
  path = CPP_OPTION (pfile, framework_include);

  if (path == NULL)
    {
      cpp_error (pfile, DL_ERROR, "No include path in which to find %s", fname);
      return NO_INCLUDE_PATH;
    }

  /* Framework header filename includes framework name and header name in the
     "Foundation/Foundation.h" form. If it does not include slash it is not a
     framework include.  */
  bufptr = NULL;
  slash_position = -1;
  position = 0;
  for (bufptr = (unsigned char *) fname; bufptr != NULL;  bufptr++)
    {
       if (position > strlen(fname))
         break;
       if (*bufptr == '/')
         {
           slash_position = position;
           break;
         }
       position++;
    }
  if (slash_position == -1)
     return 0;

  /* 25 = strlen(".framework/") + strlen("PrivateHeaders") */
  frname = (char *) alloca (strlen (fname) + pfile->max_include_len + 2 + 25);
 
  for (; path; path = path->next)
    {
      strncpy (&frname[0], path->name, path->len);
      frname[path->len] = '/';
      frname_len = path->len + 1;
      strncpy (&frname[frname_len], fname, slash_position);
      frname_len += slash_position;
      strncpy (&frname[frname_len], ".framework/", 11);
      frname_len += 11;

      /* Append framework_header_dirs and header file name */
      for (index = 0; framework_header_dirs[index].dirName; index++)
        {
          strncpy (&frname[frname_len], 
                   framework_header_dirs[index].dirName,
                   framework_header_dirs[index].dirNameLen);
          strcpy (&frname[frname_len + framework_header_dirs[index].dirNameLen],
                  &fname[slash_position]);

          if (CPP_OPTION (pfile, remap))
            n = remap_filename (pfile, frname, path);
          else
	    n = frname;

	  /* APPLE LOCAL begin Symbol Separation */
	  if (c_valid_cinfo (pfile, fname))
	    pfile->cinfo_state = CINFO_VALID;
	  /* APPLE LOCAL end Symbol Separtion */

          file = open_file_pch (pfile, n);
          if (file)
	    {
	      file->foundhere = path;
	      return file;
	    }
        }
    }

  path = CPP_OPTION (pfile, framework_include);

  /* Find in subframework */
  for (; path; path = path->next) 
    {
      inc = find_subframework_file (pfile, header, path);
      if (inc)
        return inc;
    }

  return 0;
}

/* Search for HEADER in sub frameworks.  Return 0 if there is no such
   file (or it's un-openable), in which case an error code will be in
   errno.  */

static struct include_file *
find_subframework_file (pfile, header, path)
     cpp_reader *pfile;
     const cpp_token *header;
     struct search_path *path;
{
  const char *fname = (const char *) header->val.str.text;
  const char *pname; /* Parent header name */
  struct include_file *file;
  char *n, *sfrname;
  const char *dot_framework = ".framework/";
  char *bufptr; 
  int sfrname_len, position, index; 
  struct cpp_buffer *b;

  for (b = pfile->buffer; b && b->inc && b->inc->name; b = b->prev)
    {
      pname = b->inc->name;
      /* Sub framework header filename includes parent framework name and
         header name in the "CarbonCore/OSUtils.h" form. If it does not
         include slash it is not a sub framework include.  */
      bufptr = NULL;
      position = 0;
      for (bufptr = (char *) fname; bufptr != NULL; bufptr++)
        {
           if (*bufptr == '/')
              break;
           position++;
        }
      if (position == 0)
         return 0;

      bufptr = NULL;
      bufptr = strstr (pname, dot_framework);

      /* If the parent header is not of any framework, then this header
         can not be part of any subframework.  */
      if (!bufptr)
        return 0;

      /* Now translate. For example,                   +- bufptr
          fname = CarbonCore/OSUtils.h                 | 
          pname = /System/Library/Frameworks/Foundation.framework/Headers/Foundation.h into
          sfrname = /System/Library/Frameworks/Foundation.framework/Frameworks/CarbonCore.framework/Headers/OSUtils.h
      */

      /* 36 = strlen ("Frameworks/") + strlen (".framework/") + strlen ("PrivateHeaders") */
      sfrname = (char *) alloca (strlen (pname) + strlen (fname) + 2 + 36);
 
      /* Advance bufptr */
      bufptr += strlen (dot_framework);

      sfrname_len = bufptr - pname; 

      strncpy (&sfrname[0], pname, sfrname_len);

      strncpy (&sfrname[sfrname_len], "Frameworks/", 11);
      sfrname_len += 11;

      strncpy (&sfrname[sfrname_len], fname, position);
      sfrname_len += position;

      strncpy (&sfrname[sfrname_len], ".framework/", 11);
      sfrname_len += 11;

      /* Append framework_header_dirs and header file name */
      for (index = 0; framework_header_dirs[index].dirName; index++)
        {
          strncpy (&sfrname[sfrname_len], 
                   framework_header_dirs[index].dirName,
                   framework_header_dirs[index].dirNameLen);
          strcpy (&sfrname[sfrname_len + framework_header_dirs[index].dirNameLen],
                  &fname[position]);
    
          if (CPP_OPTION (pfile, remap))
            n = remap_filename (pfile, sfrname, path);
          else
            n = sfrname;
   
	  /* APPLE LOCAL begin Symbol Separation */
	  if (c_valid_cinfo (pfile, n))
	    pfile->cinfo_state = CINFO_VALID;
	  /* APPLE LOCAL end Symbol Separtion */
 
          file = open_file_pch (pfile, n);
          if (file)
            {
              file->foundhere = path;
              return file;
            }
        }
    }
  return 0;
}
#endif /* FRAMEWORK_HEADERS  */
/* APPLE LOCAL end framework headers */

/* Not everyone who wants to set system-header-ness on a buffer can
   see the details of a buffer.  This is an exported interface because
   fix-header needs it.  */
void
cpp_make_system_header (pfile, syshdr, externc)
     cpp_reader *pfile;
     int syshdr, externc;
{
  int flags = 0;

  /* 1 = system header, 2 = system header to be treated as C.  */
  if (syshdr)
    flags = 1 + (externc != 0);
  _cpp_do_file_change (pfile, LC_RENAME, pfile->map->to_file,
		       SOURCE_LINE (pfile->map, pfile->line), flags);
}

/* Report on all files that might benefit from a multiple include guard.
   Triggered by -H.  */
void
_cpp_report_missing_guards (pfile)
     cpp_reader *pfile;
{
  int banner = 0;
  splay_tree_foreach (pfile->all_include_files, report_missing_guard,
		      (PTR) &banner);
}

/* Callback function for splay_tree_foreach().  */
static int
report_missing_guard (n, b)
     splay_tree_node n;
     void *b;
{
  struct include_file *f = (struct include_file *) n->value;
  int *bannerp = (int *) b;

  if (f && f->cmacro == 0 && f->include_count == 1)
    {
      if (*bannerp == 0)
	{
	  fputs (_("Multiple include guards may be useful for:\n"), stderr);
	  *bannerp = 1;
	}
      fputs (f->name, stderr);
      putc ('\n', stderr);
    }
  return 0;
}

/* Create a dependency for file FNAME, or issue an error message as
   appropriate.  ANGLE_BRACKETS is nonzero if the file was bracketed
   like <..>.  */
static void
handle_missing_header (pfile, fname, angle_brackets)
     cpp_reader *pfile;
     const char *fname;
     int angle_brackets;
{
  bool print_dep
    = CPP_OPTION (pfile, deps.style) > (angle_brackets || pfile->map->sysp);

  if (CPP_OPTION (pfile, deps.missing_files) && print_dep)
    deps_add_dep (pfile->deps, fname);
  /* If -M was specified, then don't count this as an error, because
     we can still produce correct output.  Otherwise, we can't produce
     correct output, because there may be dependencies we need inside
     the missing file, and we don't know what directory this missing
     file exists in.  */
  else
    {
      cpp_errno (pfile, (CPP_OPTION (pfile, deps.style) && !print_dep
			 ? DL_WARNING : DL_ERROR), fname);
      if (invalid_pch_found 
	  && !(CPP_OPTION (pfile, deps.style) && !print_dep))
	{
	  cpp_error (pfile, DL_ERROR, 
	     "one or more PCH files were found, but they were invalid");
	  if (!cpp_get_options (pfile)->warn_invalid_pch)
	    cpp_error (pfile, DL_ERROR, 
		       "use -Winvalid-pch for more information");
	}
    }
}

/* Handles #include-family directives (distinguished by TYPE),
   including HEADER, and the command line -imacros and -include.
   Returns true if a buffer was stacked.  */
bool
_cpp_execute_include (pfile, header, type)
     cpp_reader *pfile;
     const cpp_token *header;
     enum include_type type;
{
  bool stacked = false;
  struct include_file *inc;

  invalid_pch_found = false;

  inc = find_include_file (pfile, header, type);

  /* APPLE LOCAL begin framework headers */
#ifdef FRAMEWORK_HEADERS
  /* Search framework */
  if (type != IT_INCLUDE_PCH && inc == 0)
    inc = find_framework_file (pfile, header, type);
#endif
  /* APPLE LOCAL end framework headers */

  if (inc == 0)
    handle_missing_header (pfile, (const char *) header->val.str.text,
			   header->type == CPP_HEADER_NAME);
  /* APPLE LOCAL unnecessary test? */
  if (inc && inc != NO_INCLUDE_PATH)
    {
      /* Catch #import after #include */
      if (type == IT_IMPORT && inc->include_count)
	_cpp_never_reread (inc);

      /* APPLE LOCAL begin pch #import hack */
      if (inc->include_count == 0
	  && check_file_against_entries (pfile, inc, type == IT_IMPORT))
	_cpp_never_reread (inc);
      /* APPLE LOCAL end pch #import hack */

      stacked = stack_include_file (pfile, inc);

      if (type == IT_IMPORT)
	_cpp_never_reread (inc);
    }

  /* APPLE LOCAL begin indexing dpatel */
  if (stacked && flag_gen_index_original)
    process_header_indexing ((char *)inc->name, PB_INDEX_BEGIN);
  /* APPLE LOCAL end indexing dpatel */

  return stacked;
}

/* Locate HEADER, and determine whether it is newer than the current
   file.  If it cannot be located or dated, return -1, if it is newer
   newer, return 1, otherwise 0.  */
int
_cpp_compare_file_date (pfile, header)
     cpp_reader *pfile;
     const cpp_token *header;
{
  struct include_file *inc = find_include_file (pfile, header, 0);

  if (inc == NULL || inc == NO_INCLUDE_PATH)
    return -1;

  if (inc->fd > 0)
    {
      close (inc->fd);
      inc->fd = -1;
    }

  return inc->st.st_mtime > pfile->buffer->inc->st.st_mtime;
}


/* Push an input buffer and load it up with the contents of FNAME.  If
   FNAME is "", read standard input.  Return true if a buffer was
   stacked.  */
bool
_cpp_read_file (pfile, fname)
     cpp_reader *pfile;
     const char *fname;
{
  /* This uses open_file, because we don't allow a PCH to be used as
     the toplevel compilation (that would prevent re-compiling an
     existing PCH without deleting it first).  */
  struct include_file *f = open_file (pfile, fname);

  if (f == NULL)
    {
      cpp_errno (pfile, DL_ERROR, fname);
      return false;
    }

  return stack_include_file (pfile, f);
}

/* Do appropriate cleanup when a file INC's buffer is popped off the
   input stack.  */
void
_cpp_pop_file_buffer (pfile, inc)
     cpp_reader *pfile;
     struct include_file *inc;
{
  /* Record the inclusion-preventing macro, which could be NULL
     meaning no controlling macro.  */
  if (pfile->mi_valid && inc->cmacro == NULL)
    inc->cmacro = pfile->mi_cmacro;

  /* Invalidate control macros in the #including file.  */
  pfile->mi_valid = false;

  inc->refcnt--;
  if (inc->refcnt == 0 && DO_NOT_REREAD (inc))
    purge_cache (inc);
}

/* Returns the first place in the include chain to start searching for
   "" includes.  This involves stripping away the basename of the
   current file, unless -I- was specified.

   If we're handling -include or -imacros, use the "" chain, but with
   the preprocessor's cwd prepended.  */
static struct search_path *
search_from (pfile, type)
     cpp_reader *pfile;
     enum include_type type;
{
  cpp_buffer *buffer = pfile->buffer;
  unsigned int dlen;

  /* Command line uses the cwd, and does not cache the result.  */
  if (type == IT_CMDLINE)
    goto use_cwd;

  /* Ignore the current file's directory if -I- was given.  */
  if (CPP_OPTION (pfile, ignore_srcdir))
    return CPP_OPTION (pfile, quote_include);

  if (! buffer->search_cached)
    {
      buffer->search_cached = 1;

      dlen = lbasename (buffer->inc->name) - buffer->inc->name;

      if (dlen)
	{
	  /* We don't guarantee NAME is null-terminated.  This saves
	     allocating and freeing memory.  Drop a trailing '/'.  */
	  buffer->dir.name = buffer->inc->name;
	  if (dlen > 1)
	    dlen--;
	}
      /* APPLE LOCAL begin read-from-stdin */
      else if (buffer->inc->name && strlen(buffer->inc->name) == 0
	       && CPP_OPTION(pfile, stdin_diag_filename) != NULL)
        {
          buffer->dir.name = CPP_OPTION(pfile, stdin_diag_filename);
          dlen = lbasename (buffer->dir.name) - buffer->dir.name;
          if (dlen <= 0)
            goto use_cwd;      
	  /* Drop a trailing '/'.  */
	  if (dlen > 1)
	    dlen--;
        }
      /* APPLE LOCAL end read-from-stdin */
      else
	{
	use_cwd:
	  buffer->dir.name = ".";
	  dlen = 1;
	}

      if (dlen > pfile->max_include_len)
	pfile->max_include_len = dlen;

      buffer->dir.len = dlen;
      buffer->dir.next = CPP_OPTION (pfile, quote_include);
      buffer->dir.sysp = pfile->map->sysp;
    }

  return &buffer->dir;
}

/* The file_name_map structure holds a mapping of file names for a
   particular directory.  This mapping is read from the file named
   FILE_NAME_MAP_FILE in that directory.  Such a file can be used to
   map filenames on a file system with severe filename restrictions,
   such as DOS.  The format of the file name map file is just a series
   of lines with two tokens on each line.  The first token is the name
   to map, and the second token is the actual name to use.  */
struct file_name_map {
  struct file_name_map *map_next;
  char *map_from;
  char *map_to;
};

#define FILE_NAME_MAP_FILE "header.gcc"

/* Read a space delimited string of unlimited length from a stdio
   file F.  */
static char *
read_filename_string (ch, f)
     int ch;
     FILE *f;
{
  char *alloc, *set;
  int len;

  len = 20;
  set = alloc = xmalloc (len + 1);
  if (! is_space (ch))
    {
      *set++ = ch;
      while ((ch = getc (f)) != EOF && ! is_space (ch))
	{
	  if (set - alloc == len)
	    {
	      len *= 2;
	      alloc = xrealloc (alloc, len + 1);
	      set = alloc + len / 2;
	    }
	  *set++ = ch;
	}
    }
  *set = '\0';
  ungetc (ch, f);
  return alloc;
}

/* This structure holds a linked list of file name maps, one per directory.  */
struct file_name_map_list {
  struct file_name_map_list *map_list_next;
  char *map_list_name;
  struct file_name_map *map_list_map;
};

/* Read the file name map file for DIRNAME.  */
static struct file_name_map *
read_name_map (pfile, dirname)
     cpp_reader *pfile;
     const char *dirname;
{
  struct file_name_map_list *map_list_ptr;
  char *name;
  FILE *f;

  /* Check the cache of directories, and mappings in their remap file.  */
  for (map_list_ptr = CPP_OPTION (pfile, map_list); map_list_ptr;
       map_list_ptr = map_list_ptr->map_list_next)
    if (! strcmp (map_list_ptr->map_list_name, dirname))
      return map_list_ptr->map_list_map;

  map_list_ptr = ((struct file_name_map_list *)
		  xmalloc (sizeof (struct file_name_map_list)));
  map_list_ptr->map_list_name = xstrdup (dirname);

  /* The end of the list ends in NULL.  */
  map_list_ptr->map_list_map = NULL;

  name = (char *) alloca (strlen (dirname) + strlen (FILE_NAME_MAP_FILE) + 2);
  strcpy (name, dirname);
  if (*dirname)
    strcat (name, "/");
  strcat (name, FILE_NAME_MAP_FILE);
  f = fopen (name, "r");

  /* Silently return NULL if we cannot open.  */
  if (f)
    {
      int ch;

      while ((ch = getc (f)) != EOF)
	{
	  char *from, *to;
	  struct file_name_map *ptr;

	  if (is_space (ch))
	    continue;
	  from = read_filename_string (ch, f);
	  while ((ch = getc (f)) != EOF && is_hspace (ch))
	    ;
	  to = read_filename_string (ch, f);

	  ptr = ((struct file_name_map *)
		 xmalloc (sizeof (struct file_name_map)));
	  ptr->map_from = from;

	  /* Make the real filename absolute.  */
	  if (IS_ABSOLUTE_PATHNAME (to))
	    ptr->map_to = to;
	  else
	    {
	      ptr->map_to = concat (dirname, "/", to, NULL);
	      free (to);
	    }

	  ptr->map_next = map_list_ptr->map_list_map;
	  map_list_ptr->map_list_map = ptr;

	  while ((ch = getc (f)) != '\n')
	    if (ch == EOF)
	      break;
	}
      fclose (f);
    }

  /* Add this information to the cache.  */
  map_list_ptr->map_list_next = CPP_OPTION (pfile, map_list);
  CPP_OPTION (pfile, map_list) = map_list_ptr;

  return map_list_ptr->map_list_map;
}

/* Remap an unsimplified path NAME based on the file_name_map (if any)
   for LOC.  */
static char *
remap_filename (pfile, name, loc)
     cpp_reader *pfile;
     char *name;
     struct search_path *loc;
{
  struct file_name_map *map;
  const char *from, *p;
  char *dir;

  if (! loc->name_map)
    {
      /* Get a null-terminated path.  */
      char *dname = alloca (loc->len + 1);
      memcpy (dname, loc->name, loc->len);
      dname[loc->len] = '\0';

      loc->name_map = read_name_map (pfile, dname);
      if (! loc->name_map)
	return name;
    }

  /* This works since NAME has not been simplified yet.  */
  from = name + loc->len + 1;

  for (map = loc->name_map; map; map = map->map_next)
    if (!strcmp (map->map_from, from))
      return map->map_to;

  /* Try to find a mapping file for the particular directory we are
     looking in.  Thus #include <sys/types.h> will look up sys/types.h
     in /usr/include/header.gcc and look up types.h in
     /usr/include/sys/header.gcc.  */
  p = strrchr (name, '/');
  if (!p)
    return name;

  /* We know p != name as absolute paths don't call remap_filename.  */
  if (p == name)
    cpp_error (pfile, DL_ICE, "absolute file name in remap_filename");

  dir = (char *) alloca (p - name + 1);
  memcpy (dir, name, p - name);
  dir[p - name] = '\0';
  from = p + 1;

  for (map = read_name_map (pfile, dir); map; map = map->map_next)
    if (! strcmp (map->map_from, from))
      return map->map_to;

  return name;
}

/* Returns true if it is safe to remove the final component of path,
   when it is followed by a ".." component.  We use lstat to avoid
   symlinks if we have it.  If not, we can still catch errors with
   stat ().  */
static int
remove_component_p (path)
     const char *path;
{
  struct stat s;
  int result;

#ifdef HAVE_LSTAT
  result = lstat (path, &s);
#else
  result = stat (path, &s);
#endif

  /* There's no guarantee that errno will be unchanged, even on
     success.  Cygwin's lstat(), for example, will often set errno to
     ENOSYS.  In case of success, reset errno to zero.  */
  if (result == 0)
    errno = 0;

  return result == 0 && S_ISDIR (s.st_mode);
}

/* Simplify a path name in place, deleting redundant components.  This
   reduces OS overhead and guarantees that equivalent paths compare
   the same (modulo symlinks).

   Transforms made:
   foo/bar/../quux	foo/quux
   foo/./bar		foo/bar
   foo//bar		foo/bar
   /../quux		/quux
   //quux		//quux  (POSIX allows leading // as a namespace escape)

   Guarantees no trailing slashes.  All transforms reduce the length
   of the string.  Returns PATH.  errno is 0 if no error occurred;
   nonzero if an error occurred when using stat () or lstat ().  */
char *
_cpp_simplify_pathname (path)
     char *path;
{
#ifndef VMS
  char *from, *to;
  char *base, *orig_base;
  int absolute = 0;

  errno = 0;
  /* Don't overflow the empty path by putting a '.' in it below.  */
  if (*path == '\0')
    return path;

#if defined (HAVE_DOS_BASED_FILE_SYSTEM)
  /* Convert all backslashes to slashes.  */
  for (from = path; *from; from++)
    if (*from == '\\') *from = '/';

  /* Skip over leading drive letter if present.  */
  if (ISALPHA (path[0]) && path[1] == ':')
    from = to = &path[2];
  else
    from = to = path;
#else
  from = to = path;
#endif

  /* Remove redundant leading /s.  */
  if (*from == '/')
    {
      absolute = 1;
      to++;
      from++;
      if (*from == '/')
	{
	  if (*++from == '/')
	    /* 3 or more initial /s are equivalent to 1 /.  */
	    while (*++from == '/');
	  else
	    /* On some hosts // differs from /; Posix allows this.  */
	    to++;
	}
    }

  base = orig_base = to;
  for (;;)
    {
      int move_base = 0;

      while (*from == '/')
	from++;

      if (*from == '\0')
	break;

      if (*from == '.')
	{
	  if (from[1] == '\0')
	    break;
	  if (from[1] == '/')
	    {
	      from += 2;
	      continue;
	    }
	  else if (from[1] == '.' && (from[2] == '/' || from[2] == '\0'))
	    {
	      /* Don't simplify if there was no previous component.  */
	      if (absolute && orig_base == to)
		{
		  from += 2;
		  continue;
		}
	      /* Don't simplify if the previous component was "../",
		 or if an error has already occurred with (l)stat.  */
	      if (base != to && errno == 0)
		{
		  /* We don't back up if it's a symlink.  */
		  *to = '\0';
		  if (remove_component_p (path))
		    {
		      while (to > base && *to != '/')
			to--;
		      from += 2;
		      continue;
		    }
		}
	      move_base = 1;
	    }
	}

      /* Add the component separator.  */
      if (to > orig_base)
	*to++ = '/';

      /* Copy this component until the trailing null or '/'.  */
      while (*from != '\0' && *from != '/')
	*to++ = *from++;

      if (move_base)
	base = to;
    }

  /* Change the empty string to "." so that it is not treated as stdin.
     Null terminate.  */
  if (to == path)
    *to++ = '.';
  *to = '\0';

  return path;
#else /* VMS  */
  errno = 0;
  return path;
#endif /* !VMS  */
}

/* APPLE LOCAL begin header search */
#include <dirent.h>
#include "hashtab.h"

static htab_t header_htab = 0;

struct hashed_attribute
{
  struct search_path *include_directory;
  struct hashed_attribute *next;
};
struct hashed_entry
{
  char file_name[MAXNAMLEN + 1];
  struct search_path *include_directory;
  struct hashed_attribute *list;
};

static hashval_t hashed_entry_hash    PARAMS ((const void *x));
static int hashed_entry_eq            PARAMS ((const void *x, const void *y));
static void hash_enter PARAMS ((struct hashed_entry **, const char *, struct search_path *));
static void hash_add_attr PARAMS ((struct hashed_entry *, struct search_path *));
static void _load_include_headers PARAMS ((cpp_reader *, htab_t, char *));
#ifdef FRAMEWORK_HEADERS
static void _load_framework_directories PARAMS ((cpp_reader *, htab_t, char *));
#endif

static hashval_t
hashed_entry_hash (p)
     const PTR p;
{
  const struct hashed_entry *old = p;
  return htab_hash_string (old->file_name);
}

static int
hashed_entry_eq (p1, p2)
     const PTR p1;
     const PTR p2;   
{
  const struct hashed_entry *old = p1;
  const char *new = p2;  

  return strcmp (old->file_name, new) == 0;
}

static void
hash_enter (slot, file_name, include_directory)
     struct hashed_entry **slot;
     const char *file_name;
     struct search_path *include_directory;
{
  struct hashed_entry *obj = (struct hashed_entry *) xmalloc (sizeof (struct hashed_entry));
  *slot = obj;
  obj->list = 0;
  strcpy(obj->file_name, file_name);
  obj->include_directory = include_directory;
}

static void
hash_add_attr (entry, value)
     struct hashed_entry * entry;
     struct search_path *value;
{
  struct hashed_attribute *obj = (struct hashed_attribute *) xmalloc (sizeof (struct hashed_attribute));
  struct hashed_attribute *list = entry->list;

  obj->include_directory = value;
  obj->next = 0;
  if (list)
    { /* append to read */
      while (list->next) list = list->next;
      list->next = obj;
    }
  else
    entry->list = obj;		
}

static void
_load_include_headers (pfile, includehash, name)
     cpp_reader *pfile;
     htab_t includehash;
     char *name;
{
  struct search_path *path = CPP_OPTION (pfile, bracket_include);
  for (; path; path = path->next)
    {
    DIR    *directory;
    struct dirent *directory_entry;

    /* cannot depend on path->name being null terminated (weird) */
    memcpy (name, path->name, path->len);
    name[path->len] = 0;
    
    /* There was a comment here that said "no need to check for
       existence...already done by append_include_chain()", but it's possible
       that the directory might have existed before, but was deleted
       since then.  (That, of course, is a fatal error.)  */
    directory = opendir(name);
    if (directory == NULL)
      {
	cpp_error (pfile, DL_ERROR, 
		   "couldn't open %s: %s", name, xstrerror (errno));
	return;
      }
    while ((directory_entry = readdir(directory)) != NULL) 
      {
	if (directory_entry->d_name[0] == '.') /* for now, assume headers cannot begin with "."  */
	  continue;
      else
        {
        struct hashed_entry **slot, *entry;
        slot = (struct hashed_entry **)htab_find_slot_with_hash (includehash, directory_entry->d_name,
                                                 htab_hash_string (directory_entry->d_name), INSERT);
        entry = *slot;
        if (!entry)
          hash_enter (slot, directory_entry->d_name, path);
        else 
          hash_add_attr (entry, path);
        }
      }
    closedir(directory);
    }
}

#ifdef FRAMEWORK_HEADERS
static void
_load_framework_directories (pfile, frameworkhash, name)
     cpp_reader *pfile;
     htab_t frameworkhash;
     char *name;
{
  struct search_path *path;
  
  path = CPP_OPTION (pfile, framework_include);
  if (path)
    {
    for (; path; path = path->next)
      {
      DIR    *directory;
      struct dirent *directory_entry;

      /* cannot depend on path->name being null terminated (weird) */
      memcpy (name, path->name, path->len);
      name[path->len] = 0;
      /* There was a comment here that said "no need to check for
	 existence...already done by append_include_chain()", but it's
	 possible that the directory might have existed before, but
	 was deleted since then.  (That, of course, is a fatal
	 error.)  */
      directory = opendir(name);
      if (directory == NULL)
	{
	  cpp_error (pfile, DL_ERROR, 
		     "couldn't open %s: %s", name, xstrerror (errno));
	  return;
	}
      while ((directory_entry = readdir(directory)) != NULL) 
        {
	  int len = strlen (directory_entry->d_name);
	  int framework_dir = -1;
	  /* 10 = strlen (".framework");  */
	  if (len > 10)
	    framework_dir = len - 10;
	  if (directory_entry->d_name[0] == '.') /* for now, assume headers cannot begin with "." */
	    continue;
	  /* just add directories, *not* all the headers        */
	else if (directory_entry->d_type == DT_DIR ||
		 ((framework_dir != -1) 
		  && !strcmp ((directory_entry->d_name + framework_dir), ".framework")))
          {
          struct hashed_entry **slot, *entry;
          slot = (struct hashed_entry **)htab_find_slot_with_hash (frameworkhash, directory_entry->d_name,
                                                 htab_hash_string (directory_entry->d_name), INSERT);
          entry = *slot;
          if (!entry)
            hash_enter (slot, directory_entry->d_name, path);
          else
            hash_add_attr (entry, path);
          }
        }
      closedir(directory);
      } /* for */
    }
}
#endif



static void
_init_include_hash(pfile, name)
     cpp_reader *pfile;
     char *name;
{
  header_htab = htab_create (2039, hashed_entry_hash, hashed_entry_eq, NULL);
  
  _load_include_headers(pfile, header_htab, name);
#ifdef FRAMEWORK_HEADERS
  _load_framework_directories(pfile, header_htab, name);      
#endif
}

static void 
synthesize_name_from_path(name, path, fname)
     char *name;
     struct search_path *path;
     const char *fname;
{
  int len = path->len;
  memcpy(name, path->name, len);
  /* Don't turn / into // or // into ///; // may be a namespace escape.  */
  if (name[len-1] == '/')
    len--;
  name[len] = '/';
  strcpy (&name[len + 1], fname);
}

static struct include_file *
find_include_file_in_hashtable (pfile, fname, name, type, path)
     cpp_reader *pfile;
     const char *fname;
     char *name;
     enum include_type type;
     struct search_path *path;
{
    char *slash_in_fname = strrchr(fname, '/');
    struct hashed_entry *entry;
    struct include_file *file;

    /* should be called from _cpp_init_includes ()...unfortunately, _cpp_init_includes()
       it is called before the -I/-F flags are processed. Would be nice to find a
       better place to do this. Checking on every call is obviously silly */
    if (!header_htab)
      _init_include_hash(pfile, name);
    
    if ((type == CPP_STRING) && (path != CPP_OPTION (pfile, bracket_include)))
      {
      synthesize_name_from_path(name, path, fname);
      if ((file = open_file_pch (pfile, name)))
        {
        file->foundhere = path;
        return file;
        }
      }
    if (CPP_OPTION (pfile, header_map))
      {
      const char *saved_fname = fname;
      struct search_path *hmap_path = hmap_lookup_path (pfile, &fname);
      
      if (hmap_path != CPP_OPTION (pfile, bracket_include))
        {
        synthesize_name_from_path(name, hmap_path, fname);
        if ((file = open_file_pch (pfile, name)))
          {
          file->foundhere = hmap_path;
          return file;
          }
        }
      /* The header map search was not successful - restore clobbered fname. */
      fname = saved_fname;
      }
    if (!slash_in_fname)
      {
      if ((entry = htab_find(header_htab, fname))) 
        {
        synthesize_name_from_path(name, entry->include_directory, fname);
        if ((file = open_file_pch (pfile, name)))
          {
          file->foundhere = entry->include_directory;
          return file;
          }
        }
      }
    else /* we have a slash... */
      {
      char *basename = strchr(fname, '/');

      /* first, look for a directory */
      memcpy(name, fname, basename-fname);
      name[basename-fname] = 0;

      if ((entry = htab_find(header_htab, name)))
        {
        synthesize_name_from_path(name, entry->include_directory, fname);
        if ((file = open_file_pch (pfile, name)))
          {
          file->foundhere = entry->include_directory;
          return file;
          }
        else if (entry->list) /* ambiguous...look in multiple places... */
          { 
          struct hashed_attribute *hattr = entry->list;
          do 
            { 
            synthesize_name_from_path(name, hattr->include_directory, fname);
            if ((file = open_file_pch (pfile, name)))
              {
              file->foundhere = hattr->include_directory;
              return file;
              }   
            } 
          while ((hattr = hattr->next));
          }
        }
      /* now, look for a framework */
      memcpy(name, fname, basename-fname);
      name[basename-fname] = 0;
      strcat(name, ".framework");
      if ((entry = htab_find(header_htab, name)))
        {
        int len = entry->include_directory->len;
        memcpy(name, entry->include_directory->name, entry->include_directory->len);
        name[len] = '/';
        strcpy (&name[len + 1], entry->file_name);
        strcat (name, "/Headers");
        strcat (name, basename); /* add the base name */
        if ((file = open_file_pch (pfile, name)))
          {
            file->foundhere = entry->include_directory;
            return file;
          }
        memcpy(name, entry->include_directory->name, entry->include_directory->len);
        name[len] = '/';
        strcpy (&name[len + 1], entry->file_name);
        strcat (name, "/PrivateHeaders");
        strcat (name, basename); /* add the base name */
        if ((file = open_file_pch (pfile, name)))
          {
            file->foundhere = entry->include_directory;
            return file;
          }
        }
    }
  return 0;
}

#ifdef DEBUG

extern cpp_reader *parse_in;

static int found = 0, not_found = 0;
static int total_size = 0;

static int
splay_func (n, b)
     splay_tree_node n;
     void *b;
{
  struct include_file *f = (struct include_file *) n->value;
  int *bannerp = (int *)b;
  ssize_t size = f->st.st_size;
  
  if (f->st.st_ino)
    {
    if (parse_in->buffer->inc == f)
      {
	; /* printf("module %s %d\n",f->name, size); */
      }
    else
      {
      total_size += size;
      /* printf("header %s %d %d\n",f->name, size, total_size); */
      }
    found++;
    }
  else
    {
    not_found++;
    }
  return 0;
}

/* include call to cpp_log() in toplev.c:compile_file() if you want diagnostics */

void
cpp_log (pfile)
{
  int banner = 0;
  struct include_file *f = parse_in->buffer->inc;
  ssize_t size = f->st.st_size;
  
  found = 0; not_found = 0;
  splay_tree_foreach (parse_in->all_include_files, splay_func, (PTR) &banner);
  fprintf(stderr, "=> module %s %d header %d found %d nfound %d\n", f->name, size, total_size,
                found, not_found);
}
#endif
/* APPLE LOCAL end header search */

/* APPLE LOCAL begin Symbol Separation */

/* See if valid .cinfo file exists for given filename.
   Use cpp_valid_state() (from PCH) to validate preprocessor state.  */
int
c_valid_cinfo (pfile, filename)
     cpp_reader *pfile;
     const char *filename;
{

  if (!CPP_OPTION (pfile, use_ss))
    return 0;

  /* If already using repository, no need to use anymore until we are done using current
     repository.  */
  if (pfile->cinfo_state == CINFO_READ)
    return 0;

  if (1)
    {
      size_t namelen = strlen (filename);
      char *cinfoname = alloca (namelen + 7);
      int fd, sizeread, result;
      char ident[8];

      memcpy (cinfoname, filename, namelen);
      memcpy (cinfoname + namelen, ".cinfo", 7);

      /* file = validate_context (pfile, filename, pchname);*/
      fd = open (cinfoname, O_RDONLY, S_IRWXU);
      if (fd == -1)
	return 0;

      sizeread = read (fd, ident, sizeof (context_ident));
      if (sizeread == -1)
	{
	  cpp_error (pfile, DL_WARNING, "can't read %s", cinfoname); 
	  return 0;
	}
      else if (sizeread != sizeof (context_ident))
	return 0;
  
      if (memcmp (ident, context_ident, sizeof (context_ident)) != 0)
	{
	  /*
	    if (cpp_get_options (pfile)->warn_invalid_context)
	      {
	      }
	  */
	  return 0;
	}

      /* Check the preprocessor macros are the same as when the PCH was
	 generated.  */
  
      result = cpp_valid_state (pfile, cinfoname, fd);
      if (result == 0)
	{
	  /* printf ("using ss for %s\n", filename); */
	  cpp_read_stabs_checksum (pfile, fd);
	  pfile->cinfo_state = CINFO_VALID;
	  return 1;
	}
      else 
	{
	  pfile->cinfo_state = CINFO_NONE;
	  return 0;
	}
    }
}

/* Return 1 if include has suppress_dbg set.  */
int
suppress_dbg_info (inc)
     struct include_file *inc;
{
  if (!inc)
    return 0;
  if (inc->suppress_dbg)
    {
      inc->suppress_dbg = 0;
      return 1;
    }
  return 0;
}

/* Initialize symbol separtion. 
   If creating new symbol repository then write preprocessor state  in .cinfo file.
   Create .cinfo file based on main input file name in the location specified by 
   dbg_dir.  */
const char *
cpp_symbol_separation_init (pfile, dbg_dir, main_input_filename)
     struct cpp_reader *pfile;
     const char * dbg_dir;
     const char * main_input_filename;
{
  FILE *f;
  int dir_len, len;
  if (dbg_dir)
    {
      const char *basename;
      char *cinfo_file_name;
      int main_inputfilename_len, i = strlen (main_input_filename);
      main_inputfilename_len = i;
      dir_len = strlen (dbg_dir);

      /* Search backward for first '/' */
      while (main_input_filename[i] != '/' && i > 0)
	i--;

      basename = &main_input_filename[i];
      len = main_inputfilename_len - i;

      /* 6 = strlen (".cinfo") */
      cinfo_file_name = (char *) xmalloc (dir_len + len + 6 + 2);
      strcpy (cinfo_file_name, dbg_dir);
      memcpy (&cinfo_file_name[dir_len], "/", 1);
      memcpy (&cinfo_file_name[dir_len + 1], basename, len);
      memcpy (&cinfo_file_name[dir_len + 1 + len], ".cinfo", 6);
      cinfo_file_name[dir_len + 1 + len + 6] = NULL;

      f = fopen (cinfo_file_name, "w+b");
      if (f == NULL)
	{
	  cpp_error (pfile, DL_ERROR, "can't open %s", cinfo_file_name);
	  return NULL;
	}

      cinfo_file = f;
      cpp_calculate_stabs_checksum (main_input_filename);

      /* Write identification bytes.  */
      if (fwrite (context_ident, sizeof (context_ident), 1, f) != 1)
	{
	  cpp_error (pfile, DL_ERROR, "can't write to %s", cinfo_file_name);
	  return NULL;
	}

      cpp_save_state (pfile, f);
    }
  return dbg_dir;
}

/* Write checksum in to input file (.cinfo).  */
void
cpp_write_symbol_deps (pfile)
     struct cpp_reader *pfile;
{
  pfile->cinfo_state = CINFO_WRITE;
  cpp_write_pch_deps (pfile, cinfo_file);
  if (fwrite (&stabs_checksum, sizeof (struct cpp_stab_checksum), 1, cinfo_file) != 1)
    cpp_errno (pfile, DL_ERROR, "while writing checksum for context info file");
  fclose (cinfo_file);
  pfile->cinfo_state = CINFO_NONE;
}

/* Read checksum using input file descriptor (.cifno).  */
void
cpp_read_stabs_checksum (pfile, fd)
     struct cpp_reader *pfile;
     int fd;
{
  size_t len = sizeof (struct cpp_stab_checksum);

  stabs_checksum.checksum = 0;
  if (read (fd, &stabs_checksum, len) == -1)
    cpp_errno (pfile, DL_ERROR, "while reading checksum from context info file");
}

/* Return checksum */
unsigned long
cpp_get_stabs_checksum ()
{
  return stabs_checksum.checksum;
}

/* Calculate checksum for the header represented by input name.
   Use inode value as checksum.  */
void
cpp_calculate_stabs_checksum (name)
     const char *name;
{
  struct stat st;
  if (stat (name, &st) == 0)
    {
      /* Use timestamp from stat and mask one bit.  */
      stabs_checksum.checksum = (unsigned long) st.st_mtime | 0x10000000;
    }
  else
    stabs_checksum.checksum = 0xFF;
}

/* APPLE LOCAL end Symbol Separation */
