/* Declarations for the extfs.

   Copyright (C) 1995 The Free Software Foundation
   
   Written by: 1995 Jakub Jelinek

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include <sys/types.h>

struct inode;

struct entry {
    int has_changed;
    struct entry *next_in_dir;
    struct entry *dir;
    char *name;
    struct inode *inode;
};

struct archive;

struct inode {
    int has_changed;
    nlink_t nlink;
    struct entry *first_in_subdir; /* only used if this is a directory */
    struct entry *last_in_subdir;
    ino_t inode;        /* This is inode # */
    dev_t dev;		/* This is an internal identification of the extfs archive */
    struct archive *archive; /* And this is an archive structure */
    dev_t rdev;
    umode_t mode;
    uid_t uid;
    gid_t gid;
    int size;
    time_t mtime;
    char linkflag;
    char *linkname;
    time_t atime;
    time_t ctime;
    char *local_filename;
};

struct archive {
    int fstype;
    char *name;
    char *local_name;
    struct stat extfsstat;
    struct stat local_stat;
    dev_t rdev;
    int fd_usage;
    ino_t __inode_counter;
    struct entry *root_entry;
    struct entry *current_dir;
    struct archive *next;
};

typedef struct archive extfs_archive; 	/* Do _not_ use this inside extfs.c */
