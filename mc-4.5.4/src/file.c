/* {{{ Copyright */

/* File managing.  Important notes on this file:
   
   About the use of dialogs in this file:
     If you want to add a new dialog box (or call a routine that pops
     up a dialog box), you have to provide a wrapper for background
     operations (ie, background operations have to up-call to the parent
     process).

     For example, instead of using the message() routine, in this
     file, you should use one of the stubs that call message with the
     proper number of arguments (ie, message_1s, message_2s and so on).

     Actually, that is a rule that should be followed by any routines
     that may be called from this module.

*/

/* File managing
   Copyright (C) 1994, 1995, 1996 The Free Software Foundation
   
   Written by: 1994, 1995       Janne Kukonlehto
               1994, 1995       Fred Leeflang
               1994, 1995, 1996 Miguel de Icaza
               1995, 1996       Jakub Jelinek
	       1997             Norbert Warmuth
	       1998		Pavel Machek

   The copy code was based in GNU's cp, and was written by:
   Torbjorn Granlund, David MacKenzie, and Jim Meyering.

   The move code was based in GNU's mv, and was written by:
   Mike Parker and David MacKenzie.

   Janne Kukonlehto added much error recovery to them for being used
   in an interactive program.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* }}} */

/* {{{ Include files */

#include <config.h>
/* Hack: the vfs code should not rely on this */
#define WITH_FULL_PATHS 1

#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#ifdef OS2_NT
#    include <io.h>
#endif 

#include <errno.h>
#include "tty.h"
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#ifdef SCO_FLAVOR
#	include <sys/timeb.h>	/* alex: for struct timeb, used in time.h */
#endif /* SCO_FLAVOR */
#include <time.h>
#include <utime.h>
#include "mad.h"
#include "regex.h"
#include "util.h"
#include "dialog.h"
#include "global.h"
#include "setup.h"
/* Needed by query_replace */
#include "color.h"
#include "win.h"
#include "dlg.h"
#include "widget.h"
#define WANT_WIDGETS
#include "main.h"		/* WANT_WIDGETS-> we get the the_hint def */
#include "background.h"
#include "layout.h"
#include "widget.h"
#include "wtools.h"

/* Needed for current_panel, other_panel and WTree */
#include "dir.h"
#include "panel.h"
#include "file.h"
#include "filegui.h"
#include "tree.h"
#include "key.h"
#include "../vfs/vfs.h"

#include "x.h"

/* }}} */

/* rcsid [] = "$Id: file.c,v 1.37 1998/12/30 02:51:11 unammx Exp $" */
int verbose = 1;

/* Recursive operation on subdirectories */
int dive_into_subdirs = 0;

/*
 * When moving directories cross filesystem boundaries delete the successfull
 * copied files when all files below the directory and its subdirectories 
 * were processed. 
 * If erase_at_end is zero files will be deleted immediately after their
 * successful copy (Note: this behaviour is not tested and at the moment
 * it can't be changed at runtime)
 */
int erase_at_end = 1;

/*
 * Preserve the original files' owner, group, permissions, and
 * timestamps (owner, group only as root).
 */
int file_mask_preserve;

/*
 * Whether the Midnight Commander tries to provide more
 * information about copy/move sizes and bytes transfered
 * at the expense of some speed
 */
int file_op_compute_totals = 1;

/*
 * If running as root, preserve the original uid/gid
 * (we don't want to try chwon for non root)
 * preserve_uidgid = preserve && uid == 0 */
int file_mask_preserve_uidgid = 0;

/* The bits to preserve in created files' modes on file copy */
int file_mask_umask_kill = 0777777;

/* If on, it gets a little scrict with dangerous operations */
int know_not_what_am_i_doing = 0;

int file_mask_stable_symlinks = 0;

/* The next two are not static, since they are used on background.c */
/* Controls appending to files, shared with filequery.c */
int file_progress_do_append = 0;

/* result from the recursive query */
int file_progress_recursive_result;

/* The estimated time of arrival in seconds */
double file_progress_eta_secs;

/* The reget flag */
int file_progress_do_reget = 1;

/* Status reporting flags */
int file_progress_totals_computed; /* panel total has been computed */
long file_progress_count;
double file_progress_bytes;

/* mapping operations into names */
char *operation_names [] = { "Copy", "Move", "Delete" };

/* This is a hard link cache */
struct link {
    struct link *next;
    vfs *vfs;
    dev_t dev;
    ino_t ino;
    short linkcount;
    umode_t st_mode;
    char name[1];
};

/* the hard link cache */
struct link *linklist = NULL;

/* the files-to-be-erased list */
struct link *erase_list;

/*
 * In copy_dir_dir we use two additional single linked lists: The first - 
 * variable name `parent_dirs' - holds information about already copied 
 * directories and is used to detect cyclic symbolic links. 
 * The second (`dest_dirs' below) holds information about just created 
 * target directories and is used to detect when an directory is copied 
 * into itself (we don't want to copy infinitly). 
 * Both lists don't use the linkcount and name structure members of struct
 * link.
 */
struct link *dest_dirs = 0;

struct re_pattern_buffer file_mask_rx;
struct re_registers regs;

char *file_mask_dest_mask = NULL;

/*
 * To symlinks the difference between `follow Links' checked and not
 * checked is the stat call used (mc_stat resp. mc_lstat)
 */
int (*file_mask_xstat)(char *, struct stat *) = mc_lstat;

int   file_mask_op_follow_links = 0;

#ifndef HAVE_GNOME
/* we don't need these in the GNOME version */
char *file_progress_replace_filename;
int   file_progress_replace_result;
#endif
unsigned long file_progress_bps = 0, file_progress_bps_time = 0;

char *op_names [3] = {
	N_(" Copy "),
	N_(" Move "),
	N_(" Delete ")
};

static int recursive_erase (char *s, long *progress_count, double *progress_bytes);

/* }}} */


enum CaseConvs { NO_CONV=0, UP_CHAR=1, LOW_CHAR=2, UP_SECT=4, LOW_SECT=8 };

static int
convert_case (int c, enum CaseConvs *conversion)
{
    if (*conversion & UP_CHAR){
	*conversion &= ~UP_CHAR;
	return toupper (c);
    } else if (*conversion & LOW_CHAR){
	*conversion &= ~LOW_CHAR;
	return tolower (c);
    } else if (*conversion & UP_SECT){
	return toupper (c);
    } else if (*conversion & LOW_SECT){
	return tolower (c);
    } else
	return c;
}

static int transform_error = 0;

static char *
do_transform_source (char *source)
{
    int j, k, l, len;
    char *fnsource = x_basename (source);
    int next_reg;
    enum CaseConvs case_conv = NO_CONV;
    static char fntarget [MC_MAXPATHLEN];
    
    len = strlen (fnsource);
    j = re_match (&file_mask_rx, fnsource, len, 0, &regs);
    if (j != len){
        transform_error = FILE_SKIP;
    	return NULL;
    }
    for (next_reg = 1, j = 0, k = 0; j < strlen (file_mask_dest_mask); j++){
        switch (file_mask_dest_mask [j]){
	case '\\':
	    j++;
	    if (! isdigit (file_mask_dest_mask [j])){
		/* Backslash followed by non-digit */
		switch (file_mask_dest_mask [j]){
		case 'U':
		    case_conv |= UP_SECT;
		    case_conv &= ~LOW_SECT;
		    break;
		case 'u':
		    case_conv |= UP_CHAR;
		    break;
		case 'L':
		    case_conv |= LOW_SECT;
		    case_conv &= ~UP_SECT;
		    break;
		case 'l':
		    case_conv |= LOW_CHAR;
		    break;
		case 'E':
		    case_conv = NO_CONV;
		    break;
		default:
		    /* Backslash as quote mark */
		    fntarget [k++] = convert_case (file_mask_dest_mask [j], &case_conv);
		}
		break;
	    } else {
		/* Backslash followed by digit */
		next_reg = file_mask_dest_mask [j] - '0';
		/* Fall through */
	    }
                
	case '*':
	    if (next_reg < 0 || next_reg >= RE_NREGS
		|| regs.start [next_reg] < 0){
		message_1s (1, MSG_ERROR, _(" Invalid target mask "));
		transform_error = FILE_ABORT;
		return NULL;
	    }
	    for (l = regs.start [next_reg]; l < regs.end [next_reg]; l++)
		fntarget [k++] = convert_case (fnsource [l], &case_conv);
	    next_reg ++;
	    break;
            	
	default:
	    fntarget [k++] = convert_case (file_mask_dest_mask [j], &case_conv);
	    break;
        }
    }
    fntarget [k] = 0;
    return fntarget;
}

static char *
transform_source (char *source)
{
    char *s = strdup (source);
    char *q;

    /* We remove \n from the filename since regex routines would use \n as an anchor */
    /* this is just to be allowed to maniupulate file names with \n on it */
    for (q = s; *q; q++){
	if (*q == '\n')
	    *q = ' ';
    }
    q = do_transform_source (s);
    free (s);
    return q;
}

static void
free_linklist (struct link **linklist)
{
    struct link *lp, *lp2;
    
    for (lp = *linklist; lp != NULL; lp = lp2){
    	lp2 = lp -> next;
    	free (lp);
    }
    *linklist = NULL;
}

static int 
is_in_linklist (struct link *lp, char *path, struct stat *sb)
{
   ino_t ino = sb->st_ino;
   dev_t dev = sb->st_dev;
#ifdef USE_VFS
   vfs *vfs = vfs_type (path);
#endif
   
   while (lp){
#ifdef USE_VFS
      if (lp->vfs == vfs)
#endif
	  if (lp->ino == ino && lp->dev == dev )
	      return 1;
      lp = lp->next;
   }
   return 0;
}

/*
 * Returns 0 if the inode wasn't found in the cache and 1 if it was found
 * and a hardlink was succesfully made
 */
static int
check_hardlinks (char *src_name, char *dst_name, struct stat *pstat)
{
    struct link *lp;
    vfs *my_vfs = vfs_type (src_name);
    ino_t ino = pstat->st_ino;
    dev_t dev = pstat->st_dev;
    struct stat link_stat;
    char *p;

#if 1	/* What will happen if we kill this line? mc_link() will fail on this and it is right behaviour... */
    if (vfs_file_is_ftp (src_name))
        return 0;
#endif
    for (lp = linklist; lp != NULL; lp = lp -> next)
        if (lp->vfs == my_vfs && lp->ino == ino && lp->dev == dev){
            if (!mc_stat (lp->name, &link_stat) && link_stat.st_ino == ino &&
                link_stat.st_dev == dev && vfs_type (lp->name) == my_vfs){
                p = strchr (lp->name, 0) + 1; /* i.e. where the `name' file
            				         was copied to */
                if (vfs_type (dst_name) == vfs_type (p)){
            	    if (!mc_stat (p, &link_stat)){
            	    	if (!mc_link (p, dst_name))
            	    	    return 1;
            	    }
            	}
            }
	    message_1s (1, MSG_ERROR, _(" Could not make the hardlink "));
            return 0;
        }
    lp = (struct link *) xmalloc (sizeof (struct link) + strlen (src_name) 
                                  + strlen (dst_name) + 1, "Hardlink cache");
    if (lp){
    	lp->vfs = my_vfs;
    	lp->ino = ino;
    	lp->dev = dev;
    	strcpy (lp->name, src_name);
    	p = strchr (lp->name, 0) + 1;
    	strcpy (p, dst_name);
    	lp->next = linklist;
    	linklist = lp;
    }
    return 0;
}

/*
 * Duplicate the contents of the symbolic link src_path in dst_path.
 * Try to make a stable symlink if the option "stable symlink" was
 * set in the file mask dialog.
 * If dst_path is an existing symlink it will be deleted silently
 * (upper levels take already care of existing files at dst_path).
 */
static int 
make_symlink (char *src_path, char *dst_path)
{
    char link_target[MC_MAXPATHLEN];
    int len;
    int return_status;
    struct stat sb;
    int dst_is_symlink;
    
    if (mc_lstat (dst_path, &sb) == 0 && S_ISLNK (sb.st_mode)) 
	dst_is_symlink = 1;
    else
	dst_is_symlink = 0;

  retry_src_readlink:
    len = mc_readlink (src_path, link_target, MC_MAXPATHLEN);
    if (len < 0){
	return_status = file_error
	    (_(" Cannot read source link \"%s\" \n %s "), src_path);
	if (return_status == FILE_RETRY)
	    goto retry_src_readlink;
	return return_status;
    }
    link_target[len] = 0;

    if (file_mask_stable_symlinks)
	if ((!vfs_file_is_local (src_path)||!vfs_file_is_local (dst_path))){
	    message_1s (1, MSG_ERROR, _(
		" Cannot make stable symlinks across "
		"non-local filesystems: \n\n"
		" Option Stable Symlinks will be disabled "));
	    file_mask_stable_symlinks = 0;
    }
    
    if (file_mask_stable_symlinks && *link_target != PATH_SEP){
	char *p, *q, *r, *s;

	p = strdup (src_path);
	r = strrchr (p, PATH_SEP);

	if (r){
	    r[1] = 0;
	    if (*dst_path == PATH_SEP)
		q = strdup (dst_path);
	    else
		q = copy_strings (p, dst_path, 0);
	    r = strrchr (q, PATH_SEP);
	    if (r){
		r[1] = 0;
		s = copy_strings (p, link_target, NULL);
		strcpy (link_target, s);
		free (s);
		s = diff_two_paths (q, link_target);
		if (s){
		    strcpy (link_target, s);
		    free (s);
		}
	    }
	    free (q);
	}
	free (p);
    }
  retry_dst_symlink:
    if (mc_symlink (link_target, dst_path) == 0)
	/* Success */
	return FILE_CONT;
    /*
     * if dst_exists, it is obvious that this had failed.
     * We can delete the old symlink and try again...
     */
    if (dst_is_symlink){
	if (!mc_unlink (dst_path))
	    if (mc_symlink (link_target, dst_path) == 0)
		/* Success */
		return FILE_CONT;
    }
    return_status = file_error
	(_(" Cannot create target symlink \"%s\" \n %s "), dst_path);
    if (return_status == FILE_RETRY)
	goto retry_dst_symlink;
    return return_status;
}

static int
progress_update_one (long *progress_count, 
                     double *progress_bytes, 
                     int add,
                     int is_toplevel_file)
{
    int ret;

    if (is_toplevel_file || file_progress_totals_computed) {
        (*progress_count)++;
        (*progress_bytes) += add;
    }

    /* Apply some heuristic here to not call the update stuff very often */
    ret = file_progress_show_count (*progress_count, file_progress_count);

    if (ret != FILE_CONT)
	    return ret;
    
    ret = file_progress_show_bytes (*progress_bytes, file_progress_bytes);

    return ret;
}

int
copy_file_file (char *src_path, char *dst_path, int ask_overwrite,
		long  *progress_count, double *progress_bytes, 
                int is_toplevel_file)
{
#ifndef OS2_NT
    uid_t src_uid;
    gid_t src_gid;
#endif
    char *buf = 0;
    int  buf_size = 8*1024;
    int  src_desc, dest_desc = 0;
    int  n_read, n_written;
    int  src_mode;		/* The mode of the source file */
    struct stat sb, sb2;
    struct utimbuf utb;
    int  dst_exists = 0, appending = 0;
    long n_read_total = 0, file_size;
    int  return_status, temp_status;
    struct timeval tv_transfer_start;

    /* bitmask used to remember which resourses we should release on return 
       A single goto label is much easier to handle than a bunch of gotos ;-). */ 
    unsigned resources = 0; 

    /* FIXME: We should not be using global variables! */
    file_progress_do_reget = 0; 
    return_status = FILE_RETRY;

    if (file_progress_show_source (src_path) == FILE_ABORT ||
	file_progress_show_target (dst_path) == FILE_ABORT)
	return FILE_ABORT;
    mc_refresh ();

 retry_dst_stat:
    if (mc_stat (dst_path, &sb2) == 0){
	if (S_ISDIR (sb2.st_mode)){
	    return_status = file_error (_(" Cannot overwrite directory \"%s\" \n %s "),
					dst_path);
	    if (return_status == FILE_RETRY)
		goto retry_dst_stat;
	    return return_status;
	}
	dst_exists = 1;
    }

    while ((*file_mask_xstat)(src_path, &sb)){
	return_status = file_error (_(" Cannot stat source file \"%s\" \n %s "),
				    src_path);
	if (return_status == FILE_RETRY)
	    continue;
	return return_status;
    } 
    
    if (dst_exists){
	    /* .ado: For OS/2 or NT: no st_ino exists, it is better to just try to
	     * overwrite the target file
	     */
#ifndef OS2_NT
	/* Destination already exists */
	if (sb.st_dev == sb2.st_dev && sb.st_ino == sb2.st_ino){
	    message_3s (1, MSG_ERROR, _(" `%s' and `%s' are the same file. "),
		     src_path, dst_path);
	    do_refresh ();
	    return FILE_SKIP;
	}
#endif

	/* Should we replace destination? */
	if (ask_overwrite){
	    file_progress_do_reget = 0;
		    
	    return_status = query_replace (dst_path, &sb, &sb2);
	    if (return_status != FILE_CONT)
	        return return_status;
	}
    }

    if (!file_progress_do_append){
       /* .ado: OS2 and NT don't have hardlinks */
#ifndef OS2_NT    
        /* Check the hardlinks */
        if (!file_mask_op_follow_links && sb.st_nlink > 1 && 
	     check_hardlinks (src_path, dst_path, &sb) == 1){
    	    /* We have made a hardlink - no more processing is necessary */
    	    return return_status;
        }
	
        if (S_ISLNK (sb.st_mode))
	    return make_symlink (src_path, dst_path);
	    
#endif /* !OS_NT */

        if (S_ISCHR (sb.st_mode) || S_ISBLK (sb.st_mode) || S_ISFIFO (sb.st_mode)
            || S_ISSOCK (sb.st_mode)){
            while (mc_mknod (dst_path, sb.st_mode & file_mask_umask_kill, sb.st_rdev) < 0){
	        return_status = file_error
		    (_(" Cannot create special file \"%s\" \n %s "), dst_path);
	        if (return_status == FILE_RETRY)
		    continue;
		return return_status;
	    }
	    /* Success */
	    
#ifndef OS2_NT
	    while (file_mask_preserve_uidgid && mc_chown (dst_path, sb.st_uid, sb.st_gid)){
		temp_status = file_error
		    (_(" Cannot chown target file \"%s\" \n %s "), dst_path);
		if (temp_status == FILE_RETRY)
		    continue;
		return temp_status;
	    }
#endif
#ifndef __os2__
	    while (file_mask_preserve &&
		(mc_chmod (dst_path, sb.st_mode & file_mask_umask_kill) < 0)){
		temp_status = file_error (_(" Cannot chmod target file \"%s\" \n %s "), dst_path);
		if (temp_status == FILE_RETRY)
		    continue;
		return temp_status;
	    }
#endif
	    return FILE_CONT;
        }
    }
    
    gettimeofday (&tv_transfer_start, (struct timezone *) NULL);

    while ((src_desc = mc_open (src_path, O_RDONLY | O_LINEAR)) < 0){
	return_status = file_error
	    (_(" Cannot open source file \"%s\" \n %s "), src_path);
	if (return_status == FILE_RETRY)
	    continue;
	file_progress_do_append = 0;
	return return_status;
    }

    resources |= 1;
    if (file_progress_do_reget){
        if (mc_lseek (src_desc, file_progress_do_reget, SEEK_SET) != file_progress_do_reget){
	    message_1s (1, _(" Warning "), _(" Reget failed, about to overwrite file "));
	    file_progress_do_reget = file_progress_do_append = 0;
	}
    }
    
    while (mc_fstat (src_desc, &sb)){
        return_status = file_error
	    (_(" Cannot fstat source file \"%s\" \n %s "), src_path);
	if (return_status == FILE_RETRY)
	    continue;
	file_progress_do_append = 0;
	goto ret;
    }
    src_mode = sb.st_mode;
#ifndef OS2_NT
    src_uid = sb.st_uid;
    src_gid = sb.st_gid;
#endif
    utb.actime = sb.st_atime;
    utb.modtime = sb.st_mtime;
    file_size = sb.st_size;

    /* Create the new regular file with small permissions initially,
       do not create a security hole.  FIXME: You have security hole
       here, btw. Imagine copying to /tmp and symlink attack :-( */

    while ((dest_desc = mc_open (dst_path, O_WRONLY | 
      (file_progress_do_append ? O_APPEND : (O_CREAT | O_TRUNC)), 0600)) < 0){
        return_status = file_error
	    (_(" Cannot create target file \"%s\" \n %s "), dst_path);
	if (return_status == FILE_RETRY)
	    continue;
	file_progress_do_append = 0;
	goto ret;
    }
    resources |= 2; /* dst_path exists/dst_path opened */
    resources |= 4; /* remove short file */

    appending = file_progress_do_append;
    file_progress_do_append = 0;

    /* Find out the optimal buffer size.  */
    while (mc_fstat (dest_desc, &sb)){
        return_status = file_error
	    (_(" Cannot fstat target file \"%s\" \n %s "), dst_path);
	if (return_status == FILE_RETRY)
	    continue;
	goto ret;
    }
    buf = (char *) xmalloc (buf_size, "copy_file_file");

    file_progress_eta_secs = 0.0;
    file_progress_bps = 0;

    return_status = file_progress_show (0, file_size);

    mc_refresh ();

    if (return_status != FILE_CONT)
	goto ret;

    {
	struct timeval tv_current, tv_last_update, tv_last_input;
        int    secs, update_secs;
	long   dt;
	char   *stalled_msg;

	tv_last_update = tv_transfer_start;
      
        for (;;){
 /* src_read */
	    if (mc_ctl (src_desc, MCCTL_IS_NOTREADY, 0))
	        n_read = -1;
	    else
	        while ((n_read = mc_read (src_desc, buf, buf_size))<0){
		    return_status = file_error(_(" Cannot read source file \"%s\" \n %s "), src_path);
		    if (return_status == FILE_RETRY)
		        continue;
		    goto ret;
		}
	    if (n_read == 0)
	        break;

	    gettimeofday (&tv_current, NULL);

	    if (n_read>0){
	        n_read_total += n_read;

		/* Windows NT ftp servers report that files have no
		 * permissions: -------, so if we happen to have actually
		 * read something, we should fix the permissions.
		 */
		if (!(src_mode &
		      ((S_IRUSR|S_IWUSR|S_IXUSR)    /* user */
		       |(S_IXOTH|S_IWOTH|S_IROTH)  /* other */
		       |(S_IXGRP|S_IWGRP|S_IRGRP)))) /* group */
		    src_mode = S_IRUSR|S_IWUSR|S_IROTH|S_IRGRP;
		gettimeofday (&tv_last_input, NULL);

 /* dst_write */
		while ((n_written = mc_write (dest_desc, buf, n_read)) < n_read){
		    if (n_written>0){
		        n_read -= n_written;
			continue;
		    }
		    return_status = file_error(_(" Cannot write target file \"%s\" \n %s "), dst_path);
		    if (return_status == FILE_RETRY)
		        continue;
		    goto ret;
		}
	    }

	    /* 1. Update rotating dash after some time (hardcoded to 2 seconds) */
	    secs = (tv_current.tv_sec - tv_last_update.tv_sec);
	    if (secs > 2){
		rotate_dash ();
		tv_last_update = tv_current;
	    }

	    /* 2. Check for a stalled condition */
	    update_secs = (tv_current.tv_sec - tv_last_input.tv_sec);
	    stalled_msg = "";
	    if (update_secs > 4){
		stalled_msg = _("(stalled)");
	    }

	    /* 3. Compute ETA */
	    if (secs > 2){
		dt = (tv_current.tv_sec - tv_transfer_start.tv_sec);
		
		if (n_read_total){
		    file_progress_eta_secs = ((dt / (double) n_read_total) * file_size) - dt;
		    file_progress_bps = n_read_total / ((dt < 1) ? 1 : dt);
		} else
		    file_progress_eta_secs = 0.0;
	    }

	    /* 4. Compute BPS rate */
	    if (secs > 2){
		file_progress_bps_time = (tv_current.tv_sec - tv_transfer_start.tv_sec);
		if (file_progress_bps_time < 1)
		    file_progress_bps_time = 1;
		file_progress_bps = n_read_total / file_progress_bps_time;
	    }

	    file_progress_set_stalled_label (stalled_msg);

	    return_status = file_progress_show (n_read_total, file_size);
	    mc_refresh ();
	    if (return_status != FILE_CONT)
	        goto ret;
        }
    }
   
    resources &= ~4; /* copy successful, don't remove target file */

ret:
    if (buf)
	free (buf);
	
    while ((resources & 1) && mc_close (src_desc) < 0){
	temp_status = file_error
	    (_(" Cannot close source file \"%s\" \n %s "), src_path);
	if (temp_status == FILE_RETRY)
	    continue;
	if (temp_status == FILE_ABORT)
	    return_status = temp_status;
	break;
    }

    while ((resources & 2) && mc_close (dest_desc) < 0){
	temp_status = file_error
	    (_(" Cannot close target file \"%s\" \n %s "), dst_path);
	if (temp_status == FILE_RETRY)
	    continue;
	return_status = temp_status;
	break;
    }
	
    if (resources & 4){
        /* Remove short file */
        int result;
        result = query_dialog ("Copy", _("Incomplete file was retrieved. Keep it?"), D_ERROR, 2, _("&Delete"), _("&Keep"));
	if (!result)
	    mc_unlink (dst_path);
    } else if (resources & (2|8)){
        /* no short file and destination file exists */
#ifndef OS2_NT 
	if (!appending && file_mask_preserve_uidgid){
    	    while (mc_chown (dst_path, src_uid, src_gid)){
		temp_status = file_error
	    	    (_(" Cannot chown target file \"%s\" \n %s "), dst_path);
		if (temp_status == FILE_RETRY)
	    	    continue;
		return_status = temp_status;
		break;
    	    }
	}
#endif

     /*
      * .ado: according to the XPG4 standard, the file must be closed before
      * chmod can be invoked
      */
     retry_dst_chmod:
	if (!appending && mc_chmod (dst_path, src_mode & file_mask_umask_kill)){
	    temp_status = file_error
		(_(" Cannot chmod target file \"%s\" \n %s "), dst_path);
	    if (temp_status == FILE_RETRY)
		goto retry_dst_chmod;
	    return_status = temp_status;
	}
    
	if (!appending && file_mask_preserve)
	    mc_utime (dst_path, &utb);
    }

    if (return_status == FILE_CONT)
        return_status = progress_update_one (
                                progress_count, 
                                progress_bytes, 
                                file_size, is_toplevel_file);
    
    return return_status;
}

/*
 * I think these copy_*_* functions should have a return type.
 * anyway, this function *must* have two directories as arguments.
 */
/* FIXME: This function needs to check the return values of the
   function calls */
int
copy_dir_dir (char *s, char *d, int toplevel,
	      int move_over, int delete,
              struct link *parent_dirs,
	      long *progress_count,
	      double *progress_bytes)
{
#ifdef __os2__
    DIR    *next;
#else
    struct dirent *next;
#endif
    struct stat   buf, cbuf;
    DIR    *reading;
    char   *path, *mdpath, *dest_file, *dest_dir;
    int    return_status = FILE_CONT;
    struct utimbuf utb;
    struct link *lp;

    /* First get the mode of the source dir */
 retry_src_stat:
    if ((*file_mask_xstat) (s, &cbuf)){
	return_status = file_error (_(" Cannot stat source directory \"%s\" \n %s "), s);
	if (return_status == FILE_RETRY)
	    goto retry_src_stat;
	return return_status;
    }
    
    if (is_in_linklist (dest_dirs, s, &cbuf)){
	/* Don't copy a directory we created before (we don't want to copy 
	   infinitely if a directory is copied into itself) */
	/* FIXME: should there be an error message and FILE_SKIP? - Norbert */
	return FILE_CONT;
    }

/* Hmm, hardlink to directory??? - Norbert */    
/* FIXME: In this step we should do something
   in case the destination already exist */    
    /* Check the hardlinks */
    if (file_mask_preserve && cbuf.st_nlink > 1 && check_hardlinks (s, d, &cbuf) == 1){
    	/* We have made a hardlink - no more processing is necessary */
    	return return_status;
    }

    if (!S_ISDIR (cbuf.st_mode)){
	return_status = file_error (_(" Source directory \"%s\" is not a directory \n %s "), s);
	if (return_status == FILE_RETRY)
	    goto retry_src_stat;
	return return_status;
    }
    
    if (is_in_linklist (parent_dirs, s, &cbuf)){
 	/* we found a cyclic symbolic link */
	   message_2s (1, MSG_ERROR, _(" Cannot copy cyclic symbolic link \n `%s' "), s);
	return FILE_SKIP;
    }
    
    lp = xmalloc (sizeof (struct link), "parent_dirs");
    lp->vfs = vfs_type (s);
    lp->ino = cbuf.st_ino;
    lp->dev = cbuf.st_dev;
    lp->next = parent_dirs;
    parent_dirs = lp;

    /* Now, check if the dest dir exists, if not, create it. */
    if (mc_stat (d, &buf)){
    	/* Here the dir doesn't exist : make it !*/

    	if (move_over){
            if (mc_rename (s, d) == 0){
		free (parent_dirs);
		return FILE_CONT;
	    }
	}
	dest_dir = copy_strings (d, 0);
    } else {
        /*
         * If the destination directory exists, we want to copy the whole
         * directory, but we only want this to happen once.
	 *
	 * Escape sequences added to the * to compiler warnings.
         * so, say /bla exists, if we copy /tmp/\* to /bla, we get /bla/tmp/\*
         * or ( /bla doesn't exist )       /tmp/\* to /bla     ->  /bla/\*
         */
#if 1
/* Again, I'm getting curious. Is not d already what we wanted, incl.
 *  masked source basename? Is not this just a relict of the past versions? 
 *  I'm afraid this will lead into a two level deep dive :(
 *
 * I think this is indeed the problem.  I can not remember any case where
 * we actually would like that behaviour -miguel
 *
 * It's a documented feature (option `Dive into subdir if exists' in the
 * copy/move dialog). -Norbert
 */
        if (toplevel && dive_into_subdirs){
	    dest_dir = concat_dir_and_file (d, x_basename (s));
	} else 
#endif
	{
	    dest_dir = copy_strings (d, 0);
	    goto dont_mkdir;
	}
    }
 retry_dst_mkdir:
    if (my_mkdir (dest_dir, (cbuf.st_mode & file_mask_umask_kill) | S_IRWXU)){
	return_status = file_error (_(" Cannot create target directory \"%s\" \n %s "), dest_dir);
	if (return_status == FILE_RETRY)
	    goto retry_dst_mkdir;
	goto ret;
    }
    
    lp = xmalloc (sizeof (struct link), "dest_dirs");
    mc_stat (dest_dir, &buf);
    lp->vfs = vfs_type (dest_dir);
    lp->ino = buf.st_ino;
    lp->dev = buf.st_dev;
    lp->next = dest_dirs;
    dest_dirs = lp;
    
#ifndef OS2_NT 
    if (file_mask_preserve_uidgid){
     retry_dst_chown:
        if (mc_chown (dest_dir, cbuf.st_uid, cbuf.st_gid)){
	    return_status = file_error
	        (_(" Cannot chown target directory \"%s\" \n %s "), dest_dir);
	    if (return_status == FILE_RETRY)
	        goto retry_dst_chown;
	    goto ret;
        }
    }
#endif

 dont_mkdir:
    /* open the source dir for reading */
    if ((reading = mc_opendir (s)) == 0){
	goto ret;
    }
    
    while ((next = mc_readdir (reading)) && return_status != FILE_ABORT){
        /*
         * Now, we don't want '.' and '..' to be created / copied at any time 
         */
        if (!strcmp (next->d_name, "."))
            continue;
        if (!strcmp (next->d_name, ".."))
           continue;

        /* get the filename and add it to the src directory */
	path = concat_dir_and_file (s, next->d_name);
	
        (*file_mask_xstat)(path, &buf);
        if (S_ISDIR (buf.st_mode)){
            mdpath = concat_dir_and_file (dest_dir, next->d_name);
            /*
             * From here, we just intend to recursively copy subdirs, not
             * the double functionality of copying different when the target
             * dir already exists. So, we give the recursive call the flag 0
             * meaning no toplevel.
             */
            return_status = copy_dir_dir (
		    path, mdpath, 0, 0, delete, parent_dirs,
		    progress_count, progress_bytes);
	    free (mdpath);
	} else {
	    dest_file = concat_dir_and_file (dest_dir, x_basename (path));
            return_status = copy_file_file (
		    path, dest_file, 1,
		    progress_count, progress_bytes, 0);
	    free (dest_file);
	}    
	if (delete && return_status == FILE_CONT){
	    if (erase_at_end){
                static struct link *tail;
		lp = xmalloc (sizeof (struct link) + strlen (path), "erase_list");
		strcpy (lp->name, path);
		lp->st_mode = buf.st_mode;
		lp->next = 0;
                if (erase_list){
                   tail->next = lp;
                   tail = lp;
                } else 
		   erase_list = tail = lp;
	    } else {
	        if (S_ISDIR (buf.st_mode)){
		    return_status = erase_dir_iff_empty (path);
		} else
		    return_status = erase_file (path, 0, 0, 0);
	    }
	}
	
#ifdef __os2__
	/* The OS/2 mc_readdir returns a block of memory DIR
	 * next should be freed: .ado
	 */
	if (!next)
		free (next);
#endif
        free (path);
    }
    mc_closedir (reading);
    
    /* .ado: Directories can not have permission set in OS/2 */
#ifndef __os2__
    if (file_mask_preserve){
	mc_chmod (dest_dir, cbuf.st_mode & file_mask_umask_kill);
	utb.actime = cbuf.st_atime;
	utb.modtime = cbuf.st_mtime;
	mc_utime(dest_dir, &utb);
    }

#endif
ret:
    free (dest_dir);
    free (parent_dirs);
    return return_status;
}

/* }}} */

/* {{{ Move routines */

int
move_file_file (char *s, 
                char *d, 
                long *progress_count, 
                double *progress_bytes)
{
    struct stat src_stats, dst_stats;
    int return_status = FILE_CONT;

    if (file_progress_show_source (s) == FILE_ABORT
	|| file_progress_show_target (d) == FILE_ABORT)
	return FILE_ABORT;

    mc_refresh ();

 retry_src_lstat:
    if (mc_lstat (s, &src_stats) != 0){
	/* Source doesn't exist */
	return_status = file_error (_(" Cannot stat file \"%s\" \n %s "), s);
	if (return_status == FILE_RETRY)
	    goto retry_src_lstat;
	return return_status;
    }

    if (mc_lstat (d, &dst_stats) == 0){
	/* Destination already exists */
	/* .ado: for OS/2 and NT, no st_ino exists */
#ifndef OS2_NT
	if (src_stats.st_dev == dst_stats.st_dev
	    && src_stats.st_ino == dst_stats.st_ino){
	    int msize = COLS - 36;
            char st[MC_MAXPATHLEN];
            char dt[MC_MAXPATHLEN];

	    if (msize < 0)
		msize = 40;
	    msize /= 2;

	    strcpy (st, name_trunc (s, msize));
	    strcpy (dt, name_trunc (d, msize));
	    message_3s (1, MSG_ERROR, _(" `%s' and `%s' are the same file "),
		     st, dt );
	    do_refresh ();
	    return FILE_SKIP;
	}
#endif /* OS2_NT */
	if (S_ISDIR (dst_stats.st_mode)){
	    message_2s (1, MSG_ERROR, _(" Cannot overwrite directory `%s' "), d);
	    do_refresh ();
	    return FILE_SKIP;
	}

	if (confirm_overwrite){
	    return_status = query_replace (d, &src_stats, &dst_stats);
	    if (return_status != FILE_CONT)
		return return_status;
	}
	/* Ok to overwrite */
    }

    if (!file_progress_do_append){
	if (S_ISLNK (src_stats.st_mode) && file_mask_stable_symlinks){
	    if ((return_status = make_symlink (s, d)) == FILE_CONT)
		goto retry_src_remove;
	    else
		return return_status;
	}

        if (mc_rename (s, d) == 0)
	    return FILE_CONT;
    }
#if 0
/* Comparison to EXDEV seems not to work in nfs if you're moving from
   one nfs to the same, but on the server it is on two different
   filesystems. Then nfs returns EIO instead of EXDEV. 
   Hope it will not hurt if we always in case of error try to copy/delete. */
     else
    	errno = EXDEV; /* Hack to copy (append) the file and then delete it */

    if (errno != EXDEV){
	return_status = files_error (_(" Cannot move file \"%s\" to \"%s\" \n %s "), s, d);
	if (return_status == FILE_RETRY)
	    goto retry_rename;
	return return_status;
    }
#endif    

    /* Failed because filesystem boundary -> copy the file instead */
    return_status = copy_file_file (s, d, 0, 
                             progress_count, progress_bytes, 1);
    if (return_status != FILE_CONT)
	return return_status;

    if ((return_status = file_progress_show_source (NULL)) != FILE_CONT
	|| (return_status = file_progress_show (0, 0)) != FILE_CONT)
	return return_status;

    mc_refresh ();

 retry_src_remove:
    if (mc_unlink (s)){
	return_status = file_error (_(" Cannot remove file \"%s\" \n %s "), s);
	if (return_status == FILE_RETRY)
	    goto retry_src_remove;
	return return_status;
    }

    if (return_status == FILE_CONT)
        return_status = progress_update_one (progress_count, 
                                  progress_bytes, src_stats.st_size, 1);
    
    return return_status;
}

int
move_dir_dir (char *s, char *d, long *progress_count, double *progress_bytes)
{
    struct stat sbuf, dbuf, destbuf;
    struct link *lp;
    char *destdir;
    int return_status;
    int move_over = 0;

    if (file_progress_show_source (s) == FILE_ABORT || 
	file_progress_show_target (d) == FILE_ABORT)
	return FILE_ABORT;

    mc_refresh ();

    mc_stat (s, &sbuf);
    if (mc_stat (d, &dbuf))
	destdir = copy_strings (d, 0);  /* destination doesn't exist */
    else if (!dive_into_subdirs){
	destdir = copy_strings (d, 0);
	move_over = 1;
    } else
	destdir = concat_dir_and_file (d, x_basename (s));

    /* Check if the user inputted an existing dir */
 retry_dst_stat:
    if (!mc_stat (destdir, &destbuf)){
	if (move_over){
	    return_status = copy_dir_dir (
		    s, destdir, 0, 1, 1, 0,
		    progress_count, progress_bytes);
	    
	    if (return_status != FILE_CONT)
		goto ret;
	    goto oktoret;
	} else {
	    if (S_ISDIR (destbuf.st_mode))
	        return_status = file_error (_(" Cannot overwrite directory \"%s\" %s "), destdir);
	    else
	        return_status = file_error (_(" Cannot overwrite file \"%s\" %s "), destdir);
	    if (return_status == FILE_RETRY)
	        goto retry_dst_stat;
	}
        free (destdir);
        return return_status;
    }
    
 retry_rename:
    if (mc_rename (s, destdir) == 0){
	return_status = FILE_CONT;
	goto ret;
    }
/* .ado: Drive, Do we need this anymore? */
#ifdef WIN32
        else {
        /* EXDEV: cross device; does not work everywhere */
                if (toupper(s[0]) != toupper(destdir[0]))
			goto w32try;
        }
#endif

    if (errno != EXDEV){
	return_status = files_error (_(" Cannot move directory \"%s\" to \"%s\" \n %s "), s, d);
	if (return_status == FILE_RETRY)
	    goto retry_rename;
	goto ret;
    }

w32try:
    /* Failed because of filesystem boundary -> copy dir instead */
    return_status = copy_dir_dir (s, destdir, 0, 0, 1, 0, progress_count, progress_bytes);
    
    if (return_status != FILE_CONT)
	goto ret;
oktoret:
    if ((return_status = file_progress_show_source (NULL)) != FILE_CONT
	|| (return_status = file_progress_show (0, 0)) != FILE_CONT)
	goto ret;

    mc_refresh ();
    if (erase_at_end){
	for ( ; erase_list && return_status != FILE_ABORT; ){
    	    if (S_ISDIR (erase_list->st_mode)){
		return_status = erase_dir_iff_empty (erase_list->name);
	    } else
		return_status = erase_file (erase_list->name, 0, 0, 0);
	    lp = erase_list;
	    erase_list = erase_list->next;
	    free (lp);
	}
    }
    erase_dir_iff_empty (s);

 ret:
    free (destdir);
    for ( ; erase_list; ){
       lp = erase_list;
       erase_list = erase_list->next;
       free (lp);
    }
    return return_status;
}

/* }}} */

/* {{{ Erase routines */
/* Don't update progress status if progress_count==NULL */
int
erase_file (char *s, 
            long *progress_count, 
            double *progress_bytes, 
            int is_toplevel_file)
{
    int return_status;
    struct stat buf;

    if (file_progress_show_deleting (s) == FILE_ABORT)
	return FILE_ABORT;
    mc_refresh ();
    
    if (progress_count && mc_stat (s, &buf)) {
        /* ignore, most likely the mc_unlink fails, too */
        buf.st_size = 0;
    }

    while (mc_unlink (s)){
	return_status = file_error (_(" Cannot delete file \"%s\" \n %s "), s);
	if (return_status != FILE_RETRY)
	    return return_status;
    }
    if (progress_count)
        return progress_update_one (
                     progress_count, progress_bytes, 
                     buf.st_size, is_toplevel_file);
    else
        return FILE_CONT;
}

static int
recursive_erase (char *s, long *progress_count, double *progress_bytes)
{
    struct dirent *next;
    struct stat	buf;
    DIR    *reading;
    char   *path;
    int    return_status = FILE_CONT;

    if (!strcmp (s, ".."))
	return 1;
    
    reading = mc_opendir (s);
    
    if (!reading)
	return 1;
	
    while ((next = mc_readdir (reading)) && return_status == FILE_CONT){
	if (!strcmp (next->d_name, "."))
	    continue;
   	if (!strcmp (next->d_name, ".."))
	    continue;
	path = concat_dir_and_file (s, next->d_name);
   	if (mc_lstat (path, &buf)){
	    free (path);
	    return 1;
	} 
	if (S_ISDIR (buf.st_mode))
	    return_status = (recursive_erase (path, progress_count, progress_bytes) != FILE_CONT);
	else
	    return_status = erase_file (
                                  path, progress_count, progress_bytes, 0);
	free (path);
	/* .ado: OS/2 returns a block of memory DIR to next and must be freed */
#ifdef __os2__
	if (!next)
	    free (next);
#endif
    }
    mc_closedir (reading);
    if (return_status != FILE_CONT)
	return return_status;
    if (file_progress_show_deleting (s) == FILE_ABORT)
	return FILE_ABORT;
    mc_refresh ();
 retry_rmdir:
    if (my_rmdir (s)){
	return_status = file_error (_(" Cannot remove directory \"%s\" \n %s "), s);
	if (return_status == FILE_RETRY)
	    goto retry_rmdir;
	return return_status;
    }
    return FILE_CONT;
}

/* Return -1 on error, 1 if there are no entries besides "." and ".." 
   in the directory path points to, 0 else. */
static int 
check_dir_is_empty(char *path)
{
    DIR *dir;
    struct dirent *d;
    int i;

    dir = mc_opendir (path);
    if (!dir)
	return -1;

    for (i = 1, d = mc_readdir (dir); d; d = mc_readdir (dir)){
	if (d->d_name[0] == '.' && (d->d_name[1] == '\0' ||
            (d->d_name[1] == '.' && d->d_name[2] == '\0')))
	    continue; /* "." or ".." */
	i = 0;
	break;
    }

    mc_closedir (dir);
    return i;
}

int
erase_dir (char *s, long *progress_count, double *progress_bytes)
{
    int error;

    if (strcmp (s, "..") == 0)
	return FILE_SKIP;

    if (strcmp (s, ".") == 0)
	return FILE_SKIP;

    if (file_progress_show_deleting (s) == FILE_ABORT)
	return FILE_ABORT;
    mc_refresh ();

    /* The old way to detect a non empty directory was:
            error = my_rmdir (s);
            if (error && (errno == ENOTEMPTY || errno == EEXIST))){
       For the linux user space nfs server (nfs-server-2.2beta29-2)
       we would have to check also for EIO. I hope the new way is
       fool proof. (Norbert)
     */
    error = check_dir_is_empty (s);
    if (error == 0){ /* not empty */
	error = query_recursive (s);
	if (error == FILE_CONT)
	    return recursive_erase (s, progress_count, progress_bytes);
	else
	    return error;
    }

 retry_rmdir:
    error = my_rmdir (s);
    if (error == -1){
	error = file_error (_(" Cannot remove directory \"%s\" \n %s "), s);
	if (error == FILE_RETRY)
	    goto retry_rmdir;
	return error;
    }
    return FILE_CONT;
}

int
erase_dir_iff_empty (char *s)
{
    int error;

    if (strcmp (s, "..") == 0)
	return FILE_SKIP;

    if (strcmp (s, ".") == 0)
	return FILE_SKIP;

    if (file_progress_show_deleting (s) == FILE_ABORT)
	return FILE_ABORT;
    mc_refresh ();

    if (1 != check_dir_is_empty (s)) /* not empty or error */
	return FILE_CONT;

 retry_rmdir:
    error = my_rmdir (s);
    if (error){
	error = file_error (_(" Cannot remove directory \"%s\" \n %s "), s);
	if (error == FILE_RETRY)
	    goto retry_rmdir;
	return error;
    }
    return FILE_CONT;
}

/* }}} */

/* {{{ Panel operate routines */

/* Returns currently selected file or the first marked file if there is one */
char *
panel_get_file (WPanel *panel, struct stat *stat_buf)
{
    int i;

    /* No problem with Gnome, as get_current_type never returns view_tree there */
    if (get_current_type () == view_tree){
      WTree *tree = (WTree *)get_panel_widget (get_current_index ());
	
	mc_stat (tree->selected_ptr->name, stat_buf);
	return tree->selected_ptr->name;
    } 

    if (panel->marked){
	for (i = 0; i < panel->count; i++)
	    if (panel->dir.list [i].f.marked){
		*stat_buf = panel->dir.list [i].buf;
		return panel->dir.list [i].fname;
	    }
    } else {
	*stat_buf = panel->dir.list [panel->selected].buf;
	return panel->dir.list [panel->selected].fname;
    }
    fprintf (stderr, _(" Internal error: get_file \n"));
    mi_getch ();
    return "";
}

int
is_wildcarded (char *p)
{
    for (; *p; p++){
        if (*p == '*')
            return 1;
        else if (*p == '\\' && p [1] >= '1' && p [1] <= '9')
            return 1;
    }
    return 0;
}

/* Sets all global variables used by copy_file_file/move_file_file to a 
   resonable default
   (file_mask_dialog sets these global variables interactively)
 */
void
file_mask_defaults (void)
{
    file_mask_stable_symlinks = 0;
    file_mask_op_follow_links = 0;
    dive_into_subdirs = 0;
    file_mask_xstat = mc_lstat;
    
    file_mask_preserve = 1;
    file_mask_umask_kill = 0777777;
    file_mask_preserve_uidgid = (geteuid () == 0) ? 1 : 0;
}

/**
 * compute_dir_size:
 *
 * Computes the number of bytes used by the files in a directory
 */
void
compute_dir_size (char *dirname, long *ret_marked, double *ret_total)
{
	DIR *dir;
	struct dirent *dirent;
	
	dir = mc_opendir (dirname);

	if (!dir)
		return;

	while ((dirent = mc_readdir (dir)) != NULL){
		struct stat s;
		char *fullname;
		int res;
		
		if (strcmp (dirent->d_name, ".") == 0)
			continue;
		if (strcmp (dirent->d_name, "..") == 0)
			continue;

		fullname = concat_dir_and_file (dirname, dirent->d_name); 
		
		res = mc_lstat (fullname, &s);

		if (res != 0){
			free (fullname);
			continue;
		}

		if (S_ISDIR (s.st_mode)){
			long   subdir_count = 0;
			double subdir_bytes = 0;

			compute_dir_size (fullname, &subdir_count, &subdir_bytes);

			*ret_marked += subdir_count;
			*ret_total  += subdir_bytes;
		} else {
			(*ret_marked)++;
			*ret_total += s.st_size;
		}
		free (fullname);
	}
	
	mc_closedir (dir);
}

/**
 * panel_compute_totals:
 *
 * compute the number of files and the number of bytes
 * used up by the whole selection, recursing directories
 * as required.  In addition, it checks to see if it will
 * overwrite any files by doing the copy.
 */
static void
panel_compute_totals (WPanel *panel, long *ret_marked, double *ret_total)
{
	int i;
	
        *ret_marked = 0;
        *ret_total = 0.0;

	for (i = 0; i < panel->count; i++){
		struct stat *s;
		
		if (!panel->dir.list [i].f.marked)
			continue;

		s = &panel->dir.list [i].buf;

		if (S_ISDIR (s->st_mode)){
			char   *dir_name;
			long   subdir_count = 0;
			double subdir_bytes = 0;

			dir_name = concat_dir_and_file (panel->cwd, panel->dir.list [i].fname);
			compute_dir_size (dir_name, &subdir_count, &subdir_bytes);

			*ret_marked += subdir_count;
			*ret_total  += subdir_bytes;
			free (dir_name);
		} else {
			(*ret_marked)++;
			*ret_total += s->st_size;
		}
	}
}

/**
 * panel_operate:
 *
 * Performs one of the operations on the selection on the source_panel
 * (copy, delete, move).  
 *
 * Returns 1 if did change the directory
 * structure, Returns 0 if user aborted
 */
static int
panel_operate_flags (void *source_panel, FileOperation operation, char *thedefault, int ask_user)
{
    WPanel *panel = source_panel;
#ifdef WITH_FULL_PATHS
    char *source_with_path = NULL;
#else
#   define source_with_path source
#endif
    char *source = NULL;
    char *dest = NULL;
    char *temp = NULL;
    char *save_cwd = NULL, *save_dest = NULL;
    int only_one = (get_current_type () == view_tree) || (panel->marked <= 1);
    struct stat src_stat, dst_stat;
    int i, value;

    long   count = 0;
    double bytes = 0;

    int  dst_result;
    int  do_bg;			/* do background operation? */

    do_bg = 0;
    file_mask_rx.buffer = NULL;
    free_linklist (&linklist);
    free_linklist (&dest_dirs);
    if (get_current_type () == view_listing)
	if (!panel->marked && !strcmp (selection (panel)->fname, "..")){
	    message (1, MSG_ERROR, _(" Can't operate on \"..\"! "));
	    return 0;
	}
    
    if (operation < OP_COPY || operation > OP_DELETE)
	return 0;
    
    /* Generate confirmation prompt */
    source = panel_operate_generate_prompt (cmd_buf, panel, operation, only_one, &src_stat);   
    
    /* Show confirmation dialog */
    if (operation == OP_DELETE && confirm_delete){
        if (know_not_what_am_i_doing)
	    query_set_sel (1);
#ifdef HAVE_GNOME
	i = query_dialog (_(op_names [operation]), cmd_buf,
			  D_ERROR, 2, _("Yes"), _("No"));
#else
	i = query_dialog (_(op_names [operation]), cmd_buf,
			  D_ERROR, 2, _("&Yes"), _("&No"));
#endif
	if (i != 0)
	    return 0;
    } else if (operation != OP_DELETE){
	file_mask_rx.buffer = (char *) xmalloc (MC_MAXPATHLEN, "mask copying");
	file_mask_rx.allocated = MC_MAXPATHLEN;
	file_mask_rx.translate = 0;
	
        if (ask_user){
	    char *dest_dir;
	    
	    if (thedefault != NULL)
		dest_dir = thedefault;
	    else if (get_other_type () == view_listing)
		dest_dir = opanel->cwd;
	    else
		dest_dir = panel->cwd;
	    
	    dest = file_mask_dialog (operation, cmd_buf, dest_dir, only_one, &do_bg);
	    if (!dest){
		free (file_mask_rx.buffer);
		return 0;
	    }
	    if (!*dest){
		free (file_mask_rx.buffer);
		free (dest);
		return 0;
	    }
	} else {
	    char *all = "^\\(.*\\)$";
	    
	    re_compile_pattern (all, strlen (all), &file_mask_rx);
	    file_mask_dest_mask = strdup ("*");
	    do_bg = FALSE;
	    dest = strdup (thedefault);
	    file_mask_defaults ();
	}
    }

#ifdef WITH_BACKGROUND
    /* Did the user select to do a background operation? */
    if (do_bg){
	int v;
	
	v = do_background (copy_strings (operation_names [operation], ": ", panel->cwd, 0));
	if (v == -1){
	    message (1, MSG_ERROR, _(" Sorry, I could not put the job in background "));
	}

	/* If we are the parent */
	if (v == 1){
	    mc_setctl (panel->cwd, MCCTL_FORGET_ABOUT, NULL);
	    mc_setctl (dest,       MCCTL_FORGET_ABOUT, NULL);
	    return 0;
	}
    }
#endif

    /* Initialize things */
    /* We do not want to trash cache every time file is
       created/touched. However, this will make our cache contain
       invalid data. */
    if (dest) {
	if (mc_setctl (dest, MCCTL_WANT_STALE_DATA, NULL))
	    save_dest = strdup(dest);
    }
    if (panel->cwd) {
	if (mc_setctl (panel->cwd, MCCTL_WANT_STALE_DATA, NULL))
	    save_cwd = strdup(panel->cwd);
    }
	
    ftpfs_hint_reread (0);
    
    /* Now, let's do the job */

    /* This code is only called by the tree and panel code */
    if (only_one){
      /* We now have ETA in all cases */
      create_op_win (operation, 1);

	/* One file: FIXME mc_chdir will take user out of any vfs */
	if (operation != OP_COPY && get_current_type () == view_tree)
	    mc_chdir (PATH_SEP_STR);
	
	/* The source and src_stat variables have been initialized before */
#ifdef WITH_FULL_PATHS
	source_with_path = concat_dir_and_file (panel->cwd, source);
#endif
	
	if (operation == OP_DELETE)
	{
	    if (S_ISDIR (src_stat.st_mode))
		value = erase_dir (source_with_path, &count, &bytes);
	    else
		value = erase_file (source_with_path, &count, &bytes, 1);
	}
	else
	{
	    temp = transform_source (source_with_path);

	    if (temp == NULL){
		value = transform_error;
	    }
	    else
	    {
		temp = get_full_name (dest, temp);
		free (dest);
		dest = temp;
		temp = 0;

	        switch (operation){
	        case OP_COPY:
		    /*
		     * we use file_mask_op_follow_links only with OP_COPY,
		      */
		    (*file_mask_xstat) (source_with_path, &src_stat);

		    if (S_ISDIR (src_stat.st_mode))
		        value = copy_dir_dir (
				source_with_path, dest, 1, 0, 0, 0, &count, &bytes);
		    else
		        value = copy_file_file (
				source_with_path, dest, 1, &count, &bytes, 1);
		    break;

	        case OP_MOVE:
		    if (S_ISDIR (src_stat.st_mode))
		        value = move_dir_dir (
				source_with_path, dest, &count, &bytes);
		    else
		        value = move_file_file (
				source_with_path, dest, &count, &bytes);
		    break;

	        default:
		    value = FILE_CONT;
		    message_1s (1, _(" Internal failure "), _(" Unknown file operation "));
	        }
	    }
	} /* Copy or move operation */

	if (value == FILE_CONT)
	    unmark_files (panel);

    } else {
        /* Many files */
	/* Need to determine this.*/
	int policy_over_write_necessary = 1;

	/* Check destination for copy or move operation */
	if (operation != OP_DELETE){
	retry_many_dst_stat:
	    dst_result = mc_stat (dest, &dst_stat);
	    if (dst_result == 0 && !S_ISDIR (dst_stat.st_mode)){
		if (file_error (_(" Destination \"%s\" must be a directory \n %s "), dest) == FILE_RETRY)
		    goto retry_many_dst_stat;
		goto clean_up;
	    }
	}

	/* Initialize variables for progress bars */
	if (operation != OP_MOVE && verbose && file_op_compute_totals) {
		panel_compute_totals (
			panel, &file_progress_count, &file_progress_bytes);
                file_progress_totals_computed = 1;
	} else {
                file_progress_totals_computed = 0;
		file_progress_count = panel->marked;
		file_progress_bytes = panel->total;
	}
	

#ifdef HAVE_GNOME
	/* FIXME: we need to determine if this dialog is actually needed. */
	/* We need to pre-copy all the files and see if there are any 
	 * over-writes.  Ugh, this sounds yucky.  )-: */
	if (policy_over_write_necessary) {
		if (file_progress_query_replace_policy (TRUE) == FILE_ABORT)
			goto clean_up;
		else
			/* this will initialize some variables */
			file_progress_query_replace_policy (FALSE);
	}
#endif
	/* We now have ETA in all cases */
	create_op_win (operation, 1);
      
	/* Loop for every file, perform the actual copy operation */
	for (i = 0; i < panel->count; i++){

	    if (!panel->dir.list [i].f.marked)
		continue;	/* Skip the unmarked ones */

	    source = panel->dir.list [i].fname;
	    src_stat = panel->dir.list [i].buf;	/* Inefficient, should we use pointers? */

#ifdef WITH_FULL_PATHS
	    if (source_with_path)
	    	free (source_with_path);
	    source_with_path = concat_dir_and_file (panel->cwd, source);
#endif

	    if (operation == OP_DELETE){
		if (S_ISDIR (src_stat.st_mode))
		    value = erase_dir (source_with_path, &count, &bytes);
		else
		    value = erase_file (source_with_path, &count, &bytes, 1);
	    }
	    else
	    {
		if (temp)
		    free (temp);

		temp = transform_source (source_with_path);
		if (temp == NULL)
		    value = transform_error;
		else
		{
		    temp = get_full_name (dest, temp);

		    switch (operation){
		    case OP_COPY:
		       /*
			* we use file_mask_op_follow_links only with OP_COPY,
		        */
			(*file_mask_xstat) (source_with_path, &src_stat);
		    	if (S_ISDIR (src_stat.st_mode))
			    value = copy_dir_dir (
				    source_with_path, temp, 1, 0, 0, 0,
				    &count, &bytes);
		    	else
			    value = copy_file_file (
				    source_with_path, temp, 1,
				    &count, &bytes, 1);
			free_linklist (&dest_dirs);
		    	break;

		    case OP_MOVE:
		    	if (S_ISDIR (src_stat.st_mode))
			    value = move_dir_dir (
				    source_with_path, temp,
				    &count, &bytes);
		    	else
		            value = move_file_file (
				    source_with_path, temp,
				    &count, &bytes);
		        break;

		    default:
		    	message_1s (1, _(" Internal failure "),
			         _(" Unknown file operation "));
		    	goto clean_up;
		    }
		}

	    } /* Copy or move operation */

	    if (value == FILE_ABORT)
		goto clean_up;

	    if (value == FILE_CONT)
		do_file_mark (panel, i, 0);

	    if (file_progress_show_count (count, file_progress_count) == FILE_ABORT)
		goto clean_up;

	    if (verbose &&
	        file_progress_show_bytes (bytes, file_progress_bytes) == FILE_ABORT)
		goto clean_up;

	    if (operation != OP_DELETE && verbose
		&& file_progress_show (0, 0) == FILE_ABORT)
		goto clean_up;

	    mc_refresh ();
	} /* Loop for every file */
    } /* Many files */

 clean_up:
    /* Clean up */
    destroy_op_win ();
    if (save_cwd) {
        mc_setctl (save_cwd, MCCTL_NO_STALE_DATA, NULL);
	free(save_cwd);
    }
    if (save_dest) {
        mc_setctl (save_dest, MCCTL_NO_STALE_DATA, NULL);
	free(save_dest);
    }
    ftpfs_hint_reread (1);

    free_linklist (&linklist);
    free_linklist (&dest_dirs);
#if WITH_FULL_PATHS
    if (source_with_path)
    	free (source_with_path);
#endif

    if (dest)
	free (dest);

    if (temp)
	free (temp);

    if (file_mask_rx.buffer){
	free (file_mask_rx.buffer);
	file_mask_rx.buffer = NULL;
    }

    if (file_mask_dest_mask){
	free (file_mask_dest_mask);
	file_mask_dest_mask = NULL;
    }

#ifdef WITH_BACKGROUND
    /* Let our parent know we are saying bye bye */
    if (we_are_background){
	vfs_shut ();
	tell_parent (MSG_CHILD_EXITING);
	exit (1);
    } 
#endif
    return 1;
}

int
panel_operate_def (void *source_panel, FileOperation operation, char *thedefault)
{
    return panel_operate_flags (source_panel, operation, thedefault, FALSE);
}

int
panel_operate (void *source_panel, FileOperation operation, char *thedefault)
{
    return panel_operate_flags (source_panel, operation, thedefault, TRUE);
}

/* }}} */

/* {{{ Query/status report routines */

static int
real_do_file_error (enum OperationMode mode, char *error)
{
    int result;
    char *msg;

    msg = mode == Foreground ? MSG_ERROR : _(" Background process error ");
    result = query_dialog (msg, error, D_ERROR, 3, _("&Skip"), _("&Retry"), _("&Abort"));

    switch (result){
    case 0:
	do_refresh ();
	return FILE_SKIP;

    case 1:
	do_refresh ();
	return FILE_RETRY;

    case 2:
    default:
	return FILE_ABORT;
    }
}

/* Report error with one file */
int
file_error (char *format, char *file)
{
    sprintf (cmd_buf, format,
	     name_trunc (file, 30), unix_error_string (errno));

    return do_file_error (cmd_buf);
}

/* Report error with two files */
int
files_error (char *format, char *file1, char *file2)
{
    char nfile1 [16];
    char nfile2 [16];
    
    strcpy (nfile1, name_trunc (file1, 15));
    strcpy (nfile2, name_trunc (file2, 15));
    
    sprintf (cmd_buf, format, nfile1, nfile2, unix_error_string (errno));

    return do_file_error (cmd_buf);
}

static int
real_query_recursive (enum OperationMode mode, char *s)
{
    char *confirm, *text;

    if (file_progress_recursive_result < RECURSIVE_ALWAYS){
	char *msg =
	    mode == Foreground ? _("\n   Directory not empty.   \n   Delete it recursively? ")
	                       : _("\n   Background process: Directory not empty \n   Delete it recursively? ");
	text = copy_strings (" Delete: ", name_trunc (s, 30), " ", 0);

        if (know_not_what_am_i_doing)
	    query_set_sel (1);
        file_progress_recursive_result = query_dialog (text, msg, D_ERROR, 5,
						       _("&Yes"), _("&No"),
						       _("a&ll"), _("non&E"),
						       _("&Abort"));
	
	if (file_progress_recursive_result != RECURSIVE_ABORT)
	    do_refresh ();
	free (text);
	if (know_not_what_am_i_doing){
	    if (file_progress_recursive_result == RECURSIVE_YES ||
		file_progress_recursive_result == RECURSIVE_ALWAYS){
		text = copy_strings (
		    _(" Type 'yes' if you REALLY want to delete "),
		    file_progress_recursive_result == RECURSIVE_YES
		    ? name_trunc (s, 19) : _("all the directories "), " ", 0);
		confirm = input_dialog (
		    mode == Foreground ? _(" Recursive Delete ")
		    : _(" Background process: Recursive Delete "),
		    text, "no");
		do_refresh ();
		if (!confirm || strcmp (confirm, "yes"))
		    file_progress_recursive_result = RECURSIVE_NEVER;
		free (confirm);
		free (text);
	    }
	}
    }

    switch (file_progress_recursive_result){
    case RECURSIVE_YES:
    case RECURSIVE_ALWAYS:
	return FILE_CONT;

    case RECURSIVE_NO:
    case RECURSIVE_NEVER:
	return FILE_SKIP;

    case RECURSIVE_ABORT:

    default:
	return FILE_ABORT;
    }
}

#ifdef WITH_BACKGROUND
int
do_file_error (char *str)
{
    return call_1s (real_do_file_error, str);
}

int
query_recursive (char *s)
{
    return call_1s (real_query_recursive, s);
}

int
query_replace (char *destname, struct stat *_s_stat, struct stat *_d_stat)
{
    if (we_are_background)
	return parent_call ((void *)file_progress_real_query_replace,
			    3,
			    strlen (destname), destname,
			    sizeof (struct stat), _s_stat,
			    sizeof(struct stat), _d_stat);
    else
	return file_progress_real_query_replace (Foreground, destname, _s_stat, _d_stat);
}

#else
do_file_error (char *str)
{
    return real_do_file_error (Foreground, str);
}

int
query_recursive (char *s)
{
    return real_query_recursive (Foreground, s);
}

int
query_replace (char *destname, struct stat *_s_stat, struct stat *_d_stat)
{
    return file_progress_real_query_replace (Foreground, destname, _s_stat, _d_stat);
}

#endif

/*
  Cause emacs to enter folding mode for this file:
  Local variables:
  end:
*/

/*
 * This array introduced to avoid translation problems. The former (op_names)
 * is assumed to be nouns, suitable in dialog box titles; this one should
 * contain whatever is used in prompt itself (i.e. in russian, it's verb).
 * Notice first symbol - it is to fool gettext and force these strings to
 * be different for it. First symbol is skipped while building a prompt.
 * (I don't use spaces around the words, because someday they could be
 * dropped, when widgets get smarter)
 */
static char *op_names1 [] = { N_("1Copy"), N_("1Move"), N_("1Delete") };
#define	FMD_XLEN 64

int fmd_xlen = FMD_XLEN, fmd_i18n_flag = 0;
/*
 * These are formats for building a prompt. Parts encoded as follows:
 * %o - operation from op_names1
 * %f - file/files or files/directories, as appropriate
 * %m - "with source mask" or question mark for delete
 * %s - source name (truncated)
 * %d - number of marked files
 * %e - "to:" or question mark for delete
 */

#ifndef HAVE_GNOME
static char* one_format  = N_("%o %f \"%s\"%m");
static char* many_format = N_("%o %d %f%m");
#else
static char* one_format  = N_("%o %f \"%s\"%e");
static char* many_format = N_("%o %d %f%e");
#endif
static char* prompt_parts [] =
{
	N_("file"), N_("files"), N_("directory"), N_("directories"),
	N_("files/directories"), N_(" with source mask:"), N_(" to:")
};

char*
panel_operate_generate_prompt (char* cmd_buf, WPanel* panel, int operation, int only_one,
			       struct stat* src_stat)
{
	register char *sp, *cp;
	register int i;
	char format_string [200];
	char *dp = format_string;
	char* source = NULL;

#ifdef ENABLE_NLS
	static int i18n_flag = 0;
	if (!i18n_flag)
	{
		if (!fmd_i18n_flag)
			fmd_init_i18n(); /* to get proper fmd_xlen */

		for (i = sizeof (op_names1) / sizeof (op_names1 [0]); i--;)
			op_names1 [i] = _(op_names1 [i]);

		for (i = sizeof (prompt_parts) / sizeof (prompt_parts [0]); i--;)
			prompt_parts [i] = _(prompt_parts [i]);

		one_format = _(one_format);
		many_format = _(many_format);
		i18n_flag = 1;
	}
#endif /* ENABLE_NLS */

	sp = only_one ? one_format : many_format;

	if (only_one)
		source = panel_get_file (panel, src_stat);

	while (*sp)
	{
		switch (*sp)
		{
			case '%':
				cp = NULL;
				switch (sp[1])
				{
					case 'o':
						cp = op_names1 [operation] + 1;
						break;
					case 'm':
						cp = operation == OP_DELETE ? "?" : prompt_parts [5];
						break;
					case 'e':
						cp = operation == OP_DELETE ? "?" : prompt_parts [6];
						break;
					case 'f':
						if (only_one)
						{
							cp = S_ISDIR (src_stat->st_mode) ? 
								prompt_parts [2] : prompt_parts [0];
						}
						else
						{
							cp = (panel->marked == panel->dirs_marked) 
							? prompt_parts [3] 
							: (panel->dirs_marked ? prompt_parts [4] 
							: prompt_parts [1]);
						}
						break;
					default:
						*dp++ = *sp++;
				}
				if (cp)
				{
					sp += 2;
					while (*cp)
						*dp++ = *cp++;
				}
				break;
			default:
				*dp++ = *sp++;
		}
	}
	*dp = '\0';

	if (only_one)
	{
		i = fmd_xlen - strlen(format_string) - 4;
		sprintf (cmd_buf, format_string, name_trunc (source, i));
	}
	else
	{
		sprintf (cmd_buf, format_string, panel->marked);
		i = strlen (cmd_buf) + 6 - fmd_xlen;
		if (i > 0)
		{
			fmd_xlen += i;
			fmd_init_i18n(); /* to recalculate positions of child widgets */
		}
	}

	return source;
}



