/* Copyright (C) 2006 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* Testing of the basic functions of a MARIA table */

#include "maria.h"
#include <my_getopt.h>
#include <m_string.h>
#include "ma_control_file.h"
#include "ma_loghandler.h"

extern PAGECACHE *maria_log_pagecache;
extern const char *maria_data_root;

#define MAX_REC_LENGTH 1024

static void usage();

static int rec_pointer_size=0, flags[50];
static int key_field=FIELD_SKIP_PRESPACE,extra_field=FIELD_SKIP_ENDSPACE;
static int key_type=HA_KEYTYPE_NUM;
static int create_flag=0;
static enum data_file_type record_type= DYNAMIC_RECORD;

static uint insert_count, update_count, remove_count;
static uint pack_keys=0, pack_seg=0, key_length;
static uint unique_key=HA_NOSAME;
static my_bool pagecacheing, null_fields, silent, skip_update, opt_unique,
  verbose, skip_delete, transactional, die_in_middle_of_transaction;
static MARIA_COLUMNDEF recinfo[4];
static MARIA_KEYDEF keyinfo[10];
static HA_KEYSEG keyseg[10];
static HA_KEYSEG uniqueseg[10];

static int run_test(const char *filename);
static void get_options(int argc, char *argv[]);
static void create_key(char *key,uint rownr);
static void create_record(char *record,uint rownr);
static void update_record(char *record);


/*
  These are here only for testing of recovery with undo. We are not
  including maria_def.h here as this test is also to be an example of
  how to use maria outside of the maria directory
*/

extern int _ma_flush_table_files(MARIA_HA *info, uint flush_data_or_index,
                                 enum flush_type flush_type_for_data,
                                 enum flush_type flush_type_for_index);
#define MARIA_FLUSH_DATA  1


int main(int argc,char *argv[])
{
  MY_INIT(argv[0]);
  my_init();
  get_options(argc,argv);
  maria_data_root= ".";
  /* Maria requires that we always have a page cache */
  if (maria_init() ||
      (init_pagecache(maria_pagecache, IO_SIZE*16, 0, 0,
                      maria_block_size) == 0) ||
      ma_control_file_create_or_open(TRUE) ||
      (init_pagecache(maria_log_pagecache,
                      TRANSLOG_PAGECACHE_SIZE, 0, 0,
                      TRANSLOG_PAGE_SIZE) == 0) ||
      translog_init(maria_data_root, TRANSLOG_FILE_SIZE,
                    0, 0, maria_log_pagecache,
                    TRANSLOG_DEFAULT_FLAGS) ||
      (transactional && trnman_init()))
  {
    fprintf(stderr, "Error in initialization");
    exit(1);
  }

  exit(run_test("test1"));
}


static int run_test(const char *filename)
{
  MARIA_HA *file;
  int i,j,error,deleted,rec_length,uniques=0;
  ha_rows found,row_count;
  char record[MAX_REC_LENGTH],key[MAX_REC_LENGTH],read_record[MAX_REC_LENGTH];
  MARIA_UNIQUEDEF uniquedef;
  MARIA_CREATE_INFO create_info;

  if (die_in_middle_of_transaction)
    null_fields= 1;

  bzero((char*) recinfo,sizeof(recinfo));
  bzero((char*) &create_info,sizeof(create_info));

  /* First define 2 columns */
  create_info.null_bytes= 1;
  recinfo[0].type= key_field;
  recinfo[0].length= (key_field == FIELD_BLOB ? 4+portable_sizeof_char_ptr :
		      key_length);
  if (key_field == FIELD_VARCHAR)
    recinfo[0].length+= HA_VARCHAR_PACKLENGTH(key_length);
  recinfo[1].type=extra_field;
  recinfo[1].length= (extra_field == FIELD_BLOB ? 4 + portable_sizeof_char_ptr : 24);
  if (extra_field == FIELD_VARCHAR)
    recinfo[1].length+= HA_VARCHAR_PACKLENGTH(recinfo[1].length);
  if (opt_unique)
  {
    recinfo[2].type=FIELD_CHECK;
    recinfo[2].length=MARIA_UNIQUE_HASH_LENGTH;
  }
  rec_length= recinfo[0].length+recinfo[1].length+recinfo[2].length;

  if (key_type == HA_KEYTYPE_VARTEXT1 &&
      key_length > 255)
    key_type= HA_KEYTYPE_VARTEXT2;

  /* Define a key over the first column */
  keyinfo[0].seg=keyseg;
  keyinfo[0].keysegs=1;
  keyinfo[0].block_length= 0;                   /* Default block length */
  keyinfo[0].key_alg=HA_KEY_ALG_BTREE;
  keyinfo[0].seg[0].type= key_type;
  keyinfo[0].seg[0].flag= pack_seg;
  keyinfo[0].seg[0].start=1;
  keyinfo[0].seg[0].length=key_length;
  keyinfo[0].seg[0].null_bit= null_fields ? 2 : 0;
  keyinfo[0].seg[0].null_pos=0;
  keyinfo[0].seg[0].language= default_charset_info->number;
  if (pack_seg & HA_BLOB_PART)
  {
    keyinfo[0].seg[0].bit_start=4;		/* Length of blob length */
  }
  keyinfo[0].flag = (uint8) (pack_keys | unique_key);

  bzero((uchar*) flags,sizeof(flags));
  if (opt_unique)
  {
    uint start;
    uniques=1;
    bzero((char*) &uniquedef,sizeof(uniquedef));
    bzero((char*) uniqueseg,sizeof(uniqueseg));
    uniquedef.seg=uniqueseg;
    uniquedef.keysegs=2;

    /* Make a unique over all columns (except first NULL fields) */
    for (i=0, start=1 ; i < 2 ; i++)
    {
      uniqueseg[i].start=start;
      start+=recinfo[i].length;
      uniqueseg[i].length=recinfo[i].length;
      uniqueseg[i].language= default_charset_info->number;
    }
    uniqueseg[0].type= key_type;
    uniqueseg[0].null_bit= null_fields ? 2 : 0;
    uniqueseg[1].type= HA_KEYTYPE_TEXT;
    if (extra_field == FIELD_BLOB)
    {
      uniqueseg[1].length=0;			/* The whole blob */
      uniqueseg[1].bit_start=4;			/* long blob */
      uniqueseg[1].flag|= HA_BLOB_PART;
    }
    else if (extra_field == FIELD_VARCHAR)
    {
      uniqueseg[1].flag|= HA_VAR_LENGTH_PART;
      uniqueseg[1].type= (HA_VARCHAR_PACKLENGTH(recinfo[1].length-1) == 1 ?
                          HA_KEYTYPE_VARTEXT1 : HA_KEYTYPE_VARTEXT2);
    }
  }
  else
    uniques=0;

  if (!silent)
    printf("- Creating maria file\n");
  create_info.max_rows=(ulong) (rec_pointer_size ?
				(1L << (rec_pointer_size*8))/40 :
				0);
  create_info.transactional= transactional;
  if (maria_create(filename, record_type, 1, keyinfo,2+opt_unique,recinfo,
		uniques, &uniquedef, &create_info,
		create_flag))
    goto err;
  if (!(file=maria_open(filename,2,HA_OPEN_ABORT_IF_LOCKED)))
    goto err;
  if (!silent)
    printf("- Writing key:s\n");

  if (maria_begin(file))
    goto err;
  my_errno=0;
  row_count=deleted=0;
  for (i=49 ; i>=1 ; i-=2 )
  {
    if (insert_count-- == 0) { VOID(maria_close(file)) ; exit(0) ; }
    j=i%25 +1;
    create_record(record,j);
    error=maria_write(file,record);
    if (!error)
      row_count++;
    flags[j]=1;
    if (verbose || error)
      printf("J= %2d  maria_write: %d  errno: %d\n", j,error,my_errno);
  }

  if (maria_commit(file) || maria_begin(file))
    goto err;

  /* Insert 2 rows with null values */
  if (null_fields)
  {
    create_record(record,0);
    error=maria_write(file,record);
    if (!error)
      row_count++;
    if (verbose || error)
      printf("J= NULL  maria_write: %d  errno: %d\n", error,my_errno);
    error=maria_write(file,record);
    if (!error)
      row_count++;
    if (verbose || error)
      printf("J= NULL  maria_write: %d  errno: %d\n", error,my_errno);
    flags[0]=2;
  }

  if (die_in_middle_of_transaction)
  {
    /*
      Ensure we get changed pages and log to disk
      As commit record is not done, the undo entries needs to be rolled back.
    */
    _ma_flush_table_files(file, MARIA_FLUSH_DATA, FLUSH_RELEASE,
                          FLUSH_RELEASE);
    exit(1);
  }

  if (!skip_update)
  {
    if (opt_unique)
    {
      if (!silent)
	printf("- Checking unique constraint\n");
      create_record(record,j);
      if (!maria_write(file,record) || my_errno != HA_ERR_FOUND_DUPP_UNIQUE)
      {
	printf("unique check failed\n");
      }
    }
    if (!silent)
      printf("- Updating rows\n");

    /* Update first last row to force extend of file */
    if (maria_rsame(file,read_record,-1))
    {
      printf("Can't find last row with maria_rsame\n");
    }
    else
    {
      memcpy(record,read_record,rec_length);
      update_record(record);
      if (maria_update(file,read_record,record))
      {
	printf("Can't update last row: %.*s\n",
	       keyinfo[0].seg[0].length,read_record+1);
      }
    }

    /* Read through all rows and update them */
    assert(maria_scan_init(file) == 0);

    found=0;
    while ((error= maria_scan(file,read_record)) == 0)
    {
      if (update_count-- == 0) { VOID(maria_close(file)) ; exit(0) ; }
      memcpy(record,read_record,rec_length);
      update_record(record);
      if (maria_update(file,read_record,record))
      {
	printf("Can't update row: %.*s, error: %d\n",
	       keyinfo[0].seg[0].length,record+1,my_errno);
      }
      found++;
    }
    if (found != row_count)
      printf("Found %ld of %ld rows\n", (ulong) found, (ulong) row_count);
    maria_scan_end(file);
  }

  if (!silent)
    printf("- Reopening file\n");
  if (maria_commit(file))
    goto err;
  if (maria_close(file))
    goto err;
  if (!(file=maria_open(filename,2,HA_OPEN_ABORT_IF_LOCKED)))
    goto err;
  if (maria_begin(file))
    goto err;
  if (!skip_delete)
  {
    if (!silent)
      printf("- Removing keys\n");

    for (i=0 ; i <= 10 ; i++)
    {
      /* testing */
      if (remove_count-- == 0)
      {
        fprintf(stderr,
                "delete-rows number of rows deleted; Going down hard!\n");
        VOID(maria_close(file));
        exit(0) ;
      }
      j=i*2;
      if (!flags[j])
	continue;
      create_key(key,j);
      my_errno=0;
      if ((error = maria_rkey(file, read_record, 0, key,
                              HA_WHOLE_KEY, HA_READ_KEY_EXACT)))
      {
	if (verbose || (flags[j] >= 1 ||
			(error && my_errno != HA_ERR_KEY_NOT_FOUND)))
	  printf("key: '%.*s'  maria_rkey:  %3d  errno: %3d\n",
		 (int) key_length,key+test(null_fields),error,my_errno);
      }
      else
      {
	error=maria_delete(file,read_record);
	if (verbose || error)
	  printf("key: '%.*s'  maria_delete: %3d  errno: %3d\n",
		 (int) key_length, key+test(null_fields), error, my_errno);
	if (! error)
	{
	  deleted++;
	  flags[j]--;
	}
      }
    }
  }
  if (!silent)
    printf("- Reading rows with key\n");
  record[1]= 0;                                 /* For nicer printf */
  for (i=0 ; i <= 25 ; i++)
  {
    create_key(key,i);
    my_errno=0;
    error=maria_rkey(file,read_record,0,key,HA_WHOLE_KEY,HA_READ_KEY_EXACT);
    if (verbose ||
	(error == 0 && flags[i] == 0 && unique_key) ||
	(error && (flags[i] != 0 || my_errno != HA_ERR_KEY_NOT_FOUND)))
    {
      printf("key: '%.*s'  maria_rkey: %3d  errno: %3d  record: %s\n",
	     (int) key_length,key+test(null_fields),error,my_errno,record+1);
    }
  }

  if (!silent)
    printf("- Reading rows with position\n");
  if (maria_scan_init(file))
  {
    fprintf(stderr, "maria_scan_init failed\n");
    goto err;
  }

  for (i=1,found=0 ; i <= 30 ; i++)
  {
    my_errno=0;
    if ((error= maria_scan(file, read_record)) == HA_ERR_END_OF_FILE)
    {
      if (found != row_count-deleted)
	printf("Found only %ld of %ld rows\n", (ulong) found,
	       (ulong) (row_count - deleted));
      break;
    }
    if (!error)
      found++;
    if (verbose || (error != 0 && error != HA_ERR_RECORD_DELETED &&
		    error != HA_ERR_END_OF_FILE))
    {
      printf("pos: %2d  maria_rrnd: %3d  errno: %3d  record: %s\n",
	     i-1,error,my_errno,read_record+1);
    }
  }
  if (maria_commit(file))
    goto err;
  if (maria_close(file))
    goto err;
  maria_end();
  my_end(MY_CHECK_ERROR);

  return (0);
err:
  printf("got error: %3d when using maria-database\n",my_errno);
  return 1;			/* skip warning */
}


static void create_key_part(char *key,uint rownr)
{
  if (!unique_key)
    rownr&=7;					/* Some identical keys */
  if (keyinfo[0].seg[0].type == HA_KEYTYPE_NUM)
  {
    sprintf(key,"%*d",keyinfo[0].seg[0].length,rownr);
  }
  else if (keyinfo[0].seg[0].type == HA_KEYTYPE_VARTEXT1 ||
           keyinfo[0].seg[0].type == HA_KEYTYPE_VARTEXT2)
  {						/* Alpha record */
    /* Create a key that may be easily packed */
    bfill(key,keyinfo[0].seg[0].length,rownr < 10 ? 'A' : 'B');
    sprintf(key+keyinfo[0].seg[0].length-2,"%-2d",rownr);
    if ((rownr & 7) == 0)
    {
      /* Change the key to force a unpack of the next key */
      bfill(key+3,keyinfo[0].seg[0].length-5,rownr < 10 ? 'a' : 'b');
    }
  }
  else
  {						/* Alpha record */
    if (keyinfo[0].seg[0].flag & HA_SPACE_PACK)
      sprintf(key,"%-*d",keyinfo[0].seg[0].length,rownr);
    else
    {
      /* Create a key that may be easily packed */
      bfill(key,keyinfo[0].seg[0].length,rownr < 10 ? 'A' : 'B');
      sprintf(key+keyinfo[0].seg[0].length-2,"%-2d",rownr);
      if ((rownr & 7) == 0)
      {
	/* Change the key to force a unpack of the next key */
	key[1]= (rownr < 10 ? 'a' : 'b');
      }
    }
  }
}


static void create_key(char *key,uint rownr)
{
  if (keyinfo[0].seg[0].null_bit)
  {
    if (rownr == 0)
    {
      key[0]=1;					/* null key */
      key[1]=0;					/* For easy print of key */
      return;
    }
    *key++=0;
  }
  if (keyinfo[0].seg[0].flag & (HA_BLOB_PART | HA_VAR_LENGTH_PART))
  {
    uint tmp;
    create_key_part(key+2,rownr);
    tmp=strlen(key+2);
    int2store(key,tmp);
  }
  else
    create_key_part(key,rownr);
}


static char blob_key[MAX_REC_LENGTH];
static char blob_record[MAX_REC_LENGTH+20*20];


static void create_record(char *record,uint rownr)
{
  char *pos;
  bzero((char*) record,MAX_REC_LENGTH);
  record[0]=1;					/* delete marker */
  if (rownr == 0 && keyinfo[0].seg[0].null_bit)
    record[0]|=keyinfo[0].seg[0].null_bit;	/* Null key */

  pos=record+1;
  if (recinfo[0].type == FIELD_BLOB)
  {
    uint tmp;
    char *ptr;
    create_key_part(blob_key,rownr);
    tmp=strlen(blob_key);
    int4store(pos,tmp);
    ptr=blob_key;
    memcpy_fixed(pos+4,&ptr,sizeof(char*));
    pos+=recinfo[0].length;
  }
  else if (recinfo[0].type == FIELD_VARCHAR)
  {
    uint tmp, pack_length= HA_VARCHAR_PACKLENGTH(recinfo[0].length-1);
    create_key_part(pos+pack_length,rownr);
    tmp= strlen(pos+pack_length);
    if (pack_length == 1)
      *(uchar*) pos= (uchar) tmp;
    else
      int2store(pos,tmp);
    pos+= recinfo[0].length;
  }
  else
  {
    create_key_part(pos,rownr);
    pos+=recinfo[0].length;
  }
  if (recinfo[1].type == FIELD_BLOB)
  {
    uint tmp;
    char *ptr;;
    sprintf(blob_record,"... row: %d", rownr);
    strappend(blob_record,max(MAX_REC_LENGTH-rownr,10),' ');
    tmp=strlen(blob_record);
    int4store(pos,tmp);
    ptr=blob_record;
    memcpy_fixed(pos+4,&ptr,sizeof(char*));
  }
  else if (recinfo[1].type == FIELD_VARCHAR)
  {
    uint tmp, pack_length= HA_VARCHAR_PACKLENGTH(recinfo[1].length-1);
    sprintf(pos+pack_length, "... row: %d", rownr);
    tmp= strlen(pos+pack_length);
    if (pack_length == 1)
      *(uchar*) pos= (uchar) tmp;
    else
      int2store(pos,tmp);
  }
  else
  {
    sprintf(pos,"... row: %d", rownr);
    strappend(pos,recinfo[1].length,' ');
  }
}

/* change row to test re-packing of rows and reallocation of keys */

static void update_record(char *record)
{
  char *pos=record+1;
  if (recinfo[0].type == FIELD_BLOB)
  {
    char *column,*ptr;
    int length;
    length=uint4korr(pos);			/* Long blob */
    memcpy_fixed(&column,pos+4,sizeof(char*));
    memcpy(blob_key,column,length);		/* Move old key */
    ptr=blob_key;
    memcpy_fixed(pos+4,&ptr,sizeof(char*));	/* Store pointer to new key */
    if (keyinfo[0].seg[0].type != HA_KEYTYPE_NUM)
      default_charset_info->cset->casedn(default_charset_info,
                                         blob_key, length, blob_key, length);
    pos+=recinfo[0].length;
  }
  else if (recinfo[0].type == FIELD_VARCHAR)
  {
    uint pack_length= HA_VARCHAR_PACKLENGTH(recinfo[0].length-1);
    uint length= pack_length == 1 ? (uint) *(uchar*) pos : uint2korr(pos);
    default_charset_info->cset->casedn(default_charset_info,
                                       pos + pack_length, length,
                                       pos + pack_length, length);
    pos+=recinfo[0].length;
  }
  else
  {
    if (keyinfo[0].seg[0].type != HA_KEYTYPE_NUM)
      default_charset_info->cset->casedn(default_charset_info,
                                         pos, keyinfo[0].seg[0].length,
                                         pos, keyinfo[0].seg[0].length);
    pos+=recinfo[0].length;
  }

  if (recinfo[1].type == FIELD_BLOB)
  {
    char *column;
    int length;
    length=uint4korr(pos);
    memcpy_fixed(&column,pos+4,sizeof(char*));
    memcpy(blob_record,column,length);
    bfill(blob_record+length,20,'.');	/* Make it larger */
    length+=20;
    int4store(pos,length);
    column=blob_record;
    memcpy_fixed(pos+4,&column,sizeof(char*));
  }
  else if (recinfo[1].type == FIELD_VARCHAR)
  {
    /* Second field is longer than 10 characters */
    uint pack_length= HA_VARCHAR_PACKLENGTH(recinfo[1].length-1);
    uint length= pack_length == 1 ? (uint) *(uchar*) pos : uint2korr(pos);
    pos= record+ recinfo[1].offset;
    bfill(pos+pack_length+length,recinfo[1].length-length-pack_length,'.');
    length=recinfo[1].length-pack_length;
    if (pack_length == 1)
      *(uchar*) pos= (uchar) length;
    else
      int2store(pos,length);
  }
  else
  {
    bfill(pos+recinfo[1].length-10,10,'.');
  }
}


static struct my_option my_long_options[] =
{
  {"checksum", 'c', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Undocumented",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"delete-rows", 'd', "Abort after this many rows has been deleted",
   (uchar**) &remove_count, (uchar**) &remove_count, 0, GET_UINT, REQUIRED_ARG,
   1000, 0, 0, 0, 0, 0},
  {"help", '?', "Display help and exit",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"insert-rows", 'i', "Undocumented", (uchar**) &insert_count,
   (uchar**) &insert_count, 0, GET_UINT, REQUIRED_ARG, 1000, 0, 0, 0, 0, 0},
  {"key-alpha", 'a', "Use a key of type HA_KEYTYPE_TEXT",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-binary-pack", 'B', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-blob", 'b', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-cache", 'K', "Undocumented", (uchar**) &pagecacheing,
   (uchar**) &pagecacheing, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-length", 'k', "Undocumented", (uchar**) &key_length,
   (uchar**) &key_length, 0, GET_UINT, REQUIRED_ARG, 6, 0, 0, 0, 0, 0},
  {"key-multiple", 'm', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-prefix_pack", 'P', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-space_pack", 'p', "Undocumented",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"key-varchar", 'w', "Test VARCHAR keys",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"null-fields", 'N', "Define fields with NULL",
   (uchar**) &null_fields, (uchar**) &null_fields, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
  {"row-fixed-size", 'S', "Fixed size records",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"rows-in-block", 'M', "Store rows in block format",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"row-pointer-size", 'R', "Undocumented", (uchar**) &rec_pointer_size,
   (uchar**) &rec_pointer_size, 0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"silent", 's', "Undocumented",
   (uchar**) &silent, (uchar**) &silent, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"skip-delete", 'U', "Don't test deletes", (uchar**) &skip_delete,
   (uchar**) &skip_delete, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-update", 'D', "Don't test updates", (uchar**) &skip_update,
   (uchar**) &skip_update, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"test-undo", 'A',
   "Abort hard after doing inserts. Used for testing recovery with undo",
   (uchar**) &die_in_middle_of_transaction,
   (uchar**) &die_in_middle_of_transaction,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"transactional", 'T',
   "Test in transactional mode. (Only works with block format)",
   (uchar**) &transactional, (uchar**) &transactional, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
  {"unique", 'C', "Undocumented", (uchar**) &opt_unique,
   (uchar**) &opt_unique, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"update-rows", 'u', "Max number of rows to update", (uchar**) &update_count,
   (uchar**) &update_count, 0, GET_UINT, REQUIRED_ARG, 1000, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Be more verbose", (uchar**) &verbose,
   (uchar**) &verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Print version number and exit",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument __attribute__((unused)))
{
  switch(optid) {
  case 'a':
    key_type= HA_KEYTYPE_TEXT;
    break;
  case 'c':
    create_flag|= HA_CREATE_CHECKSUM;
    break;
  case 'R':				/* Length of record pointer */
    if (rec_pointer_size > 3)
      rec_pointer_size=0;
    break;
  case 'P':
    pack_keys= HA_PACK_KEY;		/* Use prefix compression */
    break;
  case 'B':
    pack_keys= HA_BINARY_PACK_KEY;	/* Use binary compression */
    break;
  case 'M':
    record_type= BLOCK_RECORD;
    break;
  case 'S':
    if (key_field == FIELD_VARCHAR)
    {
      create_flag=0;			/* Static sized varchar */
      record_type= STATIC_RECORD;
    }
    else if (key_field != FIELD_BLOB)
    {
      key_field=FIELD_NORMAL;		/* static-size record */
      extra_field=FIELD_NORMAL;
      record_type= STATIC_RECORD;
    }
    break;
  case 'p':
    pack_keys=HA_PACK_KEY;		/* Use prefix + space packing */
    pack_seg=HA_SPACE_PACK;
    key_type=HA_KEYTYPE_TEXT;
    break;
  case 'm':
    unique_key=0;
    break;
  case 'b':
    key_field=FIELD_BLOB;			/* blob key */
    extra_field= FIELD_BLOB;
    pack_seg|= HA_BLOB_PART;
    key_type= HA_KEYTYPE_VARTEXT1;
    if (record_type == STATIC_RECORD)
      record_type= DYNAMIC_RECORD;
    break;
  case 'k':
    if (key_length < 4 || key_length > HA_MAX_KEY_LENGTH)
    {
      fprintf(stderr,"Wrong key length\n");
      exit(1);
    }
    break;
  case 'w':
    key_field=FIELD_VARCHAR;			/* varchar keys */
    extra_field= FIELD_VARCHAR;
    key_type= HA_KEYTYPE_VARTEXT1;
    pack_seg|= HA_VAR_LENGTH_PART;
    if (record_type == STATIC_RECORD)
      record_type= DYNAMIC_RECORD;
    break;
  case 'K':                                     /* Use key cacheing */
    pagecacheing=1;
    break;
  case 'V':
    printf("test1 Ver 1.2 \n");
    exit(0);
  case '#':
    DBUG_PUSH (argument);
    break;
  case '?':
    usage();
    exit(1);
  }
  return 0;
}


/* Read options */

static void get_options(int argc, char *argv[])
{
  int ho_error;

  if ((ho_error=handle_options(&argc, &argv, my_long_options, get_one_option)))
    exit(ho_error);

  return;
} /* get options */


static void usage()
{
  printf("Usage: %s [options]\n\n", my_progname);
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
}
