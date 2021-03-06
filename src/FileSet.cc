/*
 * lftp - file transfer program
 *
 * Copyright (c) 1996-2012 by Alexander V. Lukyanov (lav@yars.free.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "FileSet.h"

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utime.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <assert.h>

#include <grp.h>
#include <pwd.h>
#include <ctype.h>

#include "misc.h"
#include "ResMgr.h"
#include "StringPool.h"
#include "IdNameCache.h"
#include "PatternSet.h"

#ifdef HAVE_SYS_STATFS_H
# include <sys/statfs.h>
#endif

#define fnum files.count()

void  FileInfo::Merge(const FileInfo& f)
{
   if(strcmp(name,f.name))
      return;
// int sim=defined&f.defined;
   int dif=(~defined)&f.defined;
   if(dif&MODE)
      SetMode(f.mode);
   if(dif&DATE || (defined&DATE && f.defined&DATE && f.date.ts_prec<date.ts_prec))
      SetDate(f.date,f.date.ts_prec);
   if(dif&TYPE)
      SetType(f.filetype);
   if(dif&SYMLINK)
      SetSymlink(f.symlink);
   if(dif&USER)
      SetUser(f.user);
   if(dif&GROUP)
      SetGroup(f.group);
   if(dif&NLINKS)
      SetNlink(f.nlinks);
}

void FileInfo::SetUser(const char *u)
{
   if(u==user)
      return;
   user=StringPool::Get(u);
   def(USER);
}

void FileInfo::SetGroup(const char *g)
{
   if(g==group)
      return;
   group=StringPool::Get(g);
   def(GROUP);
}

void FileSet::add_before(int pos,FileInfo *fi)
{
   files.insert(fi,pos);
}
void FileSet::Add(FileInfo *fi)
{
   assert(!sorted);
   if(!fi->name)
   {
      delete fi;
      return;
   }
   /* add sorted */
   int pos = FindGEIndByName(fi->name);
   if(pos < fnum && !strcmp(files[pos]->name,fi->name)) {
      files[pos]->Merge(*fi);
      delete fi;
      return;
   }
   add_before(pos,fi);
}

void FileSet::Sub(int i)
{
   assert(!sorted);
   files.remove(i);
   if(ind>i)
      ind--;
}

void FileSet::Merge(const FileSet *set)
{
   for(int i=0; i<set->fnum; i++)
   {
      const Ref<FileInfo>& fi=set->files[i];
      int pos = FindGEIndByName(fi->name);
      if(pos < fnum && !strcmp(files[pos]->name,fi->name))
	 files[pos]->Merge(*fi);
      else
	 add_before(pos,new FileInfo(*fi));
   }
}

void FileSet::PrependPath(const char *path)
{
   for(int i=0; i<fnum; i++)
      files[i]->SetName(dir_file(path, files[i]->name));
}

FileSet::FileSet()
   : ind(0)
{
}

FileSet::~FileSet()
{
}

/* we don't copy the sort state--nothing needs it, and it'd
 * be a bit of a pain to implement. */
FileSet::FileSet(FileSet const *set)
{
   ind=set->ind;
   for(int i=0; i<set->fnum; i++)
      files.append(new FileInfo(*(set->files[i])));
}

static int (*compare)(const char *s1, const char *s2);
static int rev_cmp;
static RefArray<FileInfo> *files_cmp;

static int sort_name(const void *s1, const void *s2)
{
   const FileInfo *p1=(*files_cmp)[*static_cast<const int*>(s1)];
   const FileInfo *p2=(*files_cmp)[*static_cast<const int*>(s2)];
   return compare(p1->name, p2->name) * rev_cmp;
}

static int sort_size(const void *s1, const void *s2)
{
   const FileInfo *p1=(*files_cmp)[*static_cast<const int*>(s1)];
   const FileInfo *p2=(*files_cmp)[*static_cast<const int*>(s2)];
   if(p1->size > p2->size) return -rev_cmp;
   if(p1->size < p2->size) return rev_cmp;
   return 0;
}

static int sort_dirs(const void *s1, const void *s2)
{
   const FileInfo *p1=(*files_cmp)[*static_cast<const int*>(s1)];
   const FileInfo *p2=(*files_cmp)[*static_cast<const int*>(s2)];
   if(p1->filetype == FileInfo::DIRECTORY && !p2->filetype == FileInfo::DIRECTORY) return -rev_cmp;
   if(!p1->filetype == FileInfo::DIRECTORY && p2->filetype == FileInfo::DIRECTORY) return rev_cmp;
   return 0;
}

static int sort_rank(const void *s1, const void *s2)
{
   const FileInfo *p1=(*files_cmp)[*static_cast<const int*>(s1)];
   const FileInfo *p2=(*files_cmp)[*static_cast<const int*>(s2)];
   if(p1->GetRank()==p2->GetRank())
      return sort_name(s1,s2);
   return p1->GetRank()<p2->GetRank() ? -rev_cmp : rev_cmp;
}

static int sort_date(const void *s1, const void *s2)
{
   const FileInfo *p1=(*files_cmp)[*static_cast<const int*>(s1)];
   const FileInfo *p2=(*files_cmp)[*static_cast<const int*>(s2)];
   if(p1->date==p2->date)
      return sort_name(s1,s2);
   return p1->date>p2->date ? -rev_cmp : rev_cmp;
}

void FileSet::Sort(sort_e newsort, bool casefold, bool reverse)
{
   if(newsort == BYNAME && !casefold && !reverse) {
      Unsort();
      return;
   }

   if(casefold) compare = strcasecmp;
   else compare = strcmp;

   rev_cmp=(reverse?-1:1);
   files_cmp=&files;

   sorted.truncate();
   for(int i=0; i<fnum; i++)
      sorted.append(i);

   int *s=sorted.get_non_const();
   int sz=sorted.get_element_size();
   switch(newsort) {
   case BYNAME: qsort(s, fnum, sz, sort_name); break;
   case BYSIZE: qsort(s, fnum, sz, sort_size); break;
   case DIRSFIRST: qsort(s, fnum, sz, sort_dirs); break;
   case BYRANK: qsort(s, fnum, sz, sort_rank); break;
   case BYDATE: qsort(s, fnum, sz, sort_date); break;
   }
}

/* Remove the current sort, allowing new entries to be added.
 * (Nothing uses this ... */
void FileSet::Unsort()
{
   sorted.unset();
}

void FileSet::Empty()
{
   Unsort();
   files.unset();
   ind=0;
}

void FileSet::SubtractSame(const FileSet *set,int ignore)
{
   for(int i=0; i<fnum; i++)
   {
      FileInfo *f=set->FindByName(files[i]->name);
      if(f && files[i]->SameAs(f,ignore))
	 Sub(i--);
   }
}

void FileSet::SubtractAny(const FileSet *set)
{
   for(int i=0; i<fnum; i++)
      if(set->FindByName(files[i]->name))
	 Sub(i--);
}

void FileSet::SubtractNotIn(const FileSet *set)
{
   for(int i=0; i<fnum; i++)
      if(!set->FindByName(files[i]->name))
	 Sub(i--);
}
void FileSet::SubtractSameType(const FileSet *set)
{
   for(int i=0; i<fnum; i++)
   {
      FileInfo *f=set->FindByName(files[i]->name);
      if(f && files[i]->defined&FileInfo::TYPE && f->defined&FileInfo::TYPE
      && files[i]->filetype==f->filetype)
	 Sub(i--);
   }
}

void FileSet::SubtractTimeCmp(bool (FileInfo::*cmp)(time_t) const,time_t t)
{
   for(int i=0; i<fnum; i++)
   {
      if(files[i]->defined&FileInfo::TYPE
      && files[i]->filetype!=FileInfo::NORMAL)
	 continue;
      if((files[i].get()->*cmp)(t))
      {
	 Sub(i);
	 i--;
      }
   }
}

void FileSet::SubtractSizeOutside(const Range *r)
{
   for(int i=0; i<fnum; i++)
   {
      if(files[i]->defined&FileInfo::TYPE
      && files[i]->filetype!=FileInfo::NORMAL)
	 continue;
      if(files[i]->SizeOutside(r))
      {
	 Sub(i);
	 i--;
      }
   }
}
void FileSet::SubtractDirs()
{
   for(int i=0; i<fnum; i++)
   {
      if(files[i]->defined&FileInfo::TYPE
      && files[i]->filetype==FileInfo::DIRECTORY)
      {
	 Sub(i);
	 i--;
      }
   }
}
void FileSet::SubtractNotDirs()
{
   for(int i=0; i<fnum; i++)
   {
      if(!(files[i]->defined&FileInfo::TYPE)
      || files[i]->filetype!=FileInfo::DIRECTORY)
      {
	 Sub(i);
	 i--;
      }
   }
}

void FileSet::ExcludeDots()
{
   for(int i=0; i<fnum; i++)
   {
      if(!strcmp(files[i]->name,".") || !strcmp(files[i]->name,".."))
      {
	 Sub(i);
	 i--;
      }
   }
}

void FileSet::ExcludeUnaccessible()
{
   for(int i=0; i<fnum; i++)
   {
      if(!(files[i]->defined&FileInfo::MODE) || !(files[i]->defined&FileInfo::TYPE))
	 continue;
      if((files[i]->filetype==FileInfo::NORMAL    && !(files[i]->mode&0444))
      || (files[i]->filetype==FileInfo::DIRECTORY && !(files[i]->mode&0444&(files[i]->mode<<2))))
      {
	 Sub(i);
	 i--;
      }
   }
}

bool  FileInfo::SameAs(const FileInfo *fi,int ignore) const
{
   if(defined&NAME && fi->defined&NAME)
      if(strcmp(name,fi->name))
	 return false;
   if(defined&TYPE && fi->defined&TYPE)
      if(filetype!=fi->filetype)
	 return false;

   if((defined&TYPE && filetype==DIRECTORY)
   || (fi->defined&TYPE && fi->filetype==DIRECTORY))
      return false;  // can't guarantee directory is the same (recursively)

   if(defined&SYMLINK_DEF && fi->defined&SYMLINK_DEF)
      return (strcmp(symlink,fi->symlink)==0);

   if(defined&DATE && fi->defined&DATE && !(ignore&DATE))
   {
      time_t p=date.ts_prec;
      if(p<fi->date.ts_prec)
	 p=fi->date.ts_prec;
      if(!(ignore&IGNORE_DATE_IF_OLDER && date<fi->date)
      && labs(date-fi->date)>p)
	 return false;
   }

   if(defined&SIZE && fi->defined&SIZE && !(ignore&SIZE))
   {
      if(!(ignore&IGNORE_SIZE_IF_OLDER && defined&DATE && fi->defined&DATE
	   && date<fi->date)
      && (size!=fi->size))
	 return false;
   }

   return true;
}

bool  FileInfo::NotOlderThan(time_t t) const
{
   return((defined&DATE) && date>=t);
}
bool  FileInfo::NotNewerThan(time_t t) const
{
   return((defined&DATE) && date<=t);
}
bool  FileInfo::OlderThan(time_t t) const
{
   return((defined&DATE) && date<t);
}
bool  FileInfo::NewerThan(time_t t) const
{
   return((defined&DATE) && date>t);
}
bool  FileInfo::SizeOutside(const Range *r) const
{
   return((defined&SIZE) && !r->Match(size));
}

void FileSet::Count(int *d,int *f,int *s,int *o) const
{
   for(int i=0; i<fnum; i++)
   {
      switch(files[i]->filetype)
      {
      case(FileInfo::DIRECTORY):
	 if(d) (*d)++; break;
      case(FileInfo::NORMAL):
	 if(f) (*f)++; break;
      case(FileInfo::SYMLINK):
	 if(s) (*s)++; break;
      case(FileInfo::UNKNOWN):
	 if(o) (*o)++;
      }
   }
}

/* assumes sorted by name. binary search for name, returning the first name
 * >= name; returns fnum if name is greater than all names. */
int FileSet::FindGEIndByName(const char *name) const
{
   int l = 0, u = fnum - 1;

   /* no files or name is greater than the max file: */
   if(!fnum || strcmp(files[u]->name, name) < 0)
      return fnum;

   /* we have files, and u >= name (meaning l <= name <= u); loop while
    * this is true: */
   while(l < u) {
      /* find the midpoint: */
      int m = (l + u) / 2;
      int cmp = strcmp(files[m]->name, name);

      /* if files[m]->name > name, update the upper bound: */
      if (cmp > 0)
	 u = m;
      /* if files[m]->name < name, update the lower bound: */
      else if (cmp < 0)
	 l = m+1;
      else /* otherwise found exact match */
	 return m;
   }

   return u;
}

FileInfo *FileSet::FindByName(const char *name) const
{
   int n = FindGEIndByName(name);

   if(n < fnum && !strcmp(files[n]->name,name))
      return files[n].get_non_const();

   return 0;
}

static bool do_exclude_match(const char *prefix,const FileInfo *fi,const PatternSet *x)
{
   const char *name=dir_file(prefix,fi->name);
   if(fi->defined&fi->TYPE && fi->filetype==fi->DIRECTORY)
   {
      char *name1=alloca_strdup2(name,1);
      strcat(name1,"/");
      name=name1;
   }
   return x->MatchExclude(name);
}

void  FileSet::Exclude(const char *prefix,const PatternSet *x)
{
   if(!x)
      return;
   for(int i=0; i<fnum; i++)
   {
      if(do_exclude_match(prefix,files[i],x))
      {
	 Sub(i);
	 i--;
      }
   }
}

#if 0
void FileSet::Dump(const char *tag) const
{
   printf("%s:",tag);
   for(int i=0; i<fnum; i++)
      printf(" %s",files[i]->name.get());
   printf("\n");
}
#endif

// *** Manipulations with set of local files

void FileSet::LocalUtime(const char *dir,bool only_dirs)
{
   for(int i=0; i<fnum; i++)
   {
      const Ref<FileInfo>& file=files[i];
      if(file->defined & file->DATE)
      {
	 if(!(file->defined & file->TYPE))
	    continue;
	 if(file->filetype==file->SYMLINK)
	    continue;
	 if(only_dirs && file->filetype!=file->DIRECTORY)
	    continue;

	 const char *local_name=dir_file(dir,file->name);
	 struct utimbuf ut;
	 struct stat st;
	 ut.actime=ut.modtime=file->date;

	 if(stat(local_name,&st)!=-1 && labs(st.st_mtime-file->date)>file->date.ts_prec)
	    utime(local_name,&ut);
      }
   }
}
void FileSet::LocalChmod(const char *dir,mode_t mask)
{
   for(int i=0; i<fnum; i++)
   {
      const Ref<FileInfo>& file=files[i];
      if(file->defined & file->MODE)
      {
	 if(file->defined & file->TYPE
	 && file->filetype==file->SYMLINK)
	    continue;

	 const char *local_name=dir_file(dir,file->name);

	 struct stat st;
	 mode_t new_mode=file->mode&~mask;

	 if(stat(local_name,&st)!=-1 && (st.st_mode&07777)!=new_mode)
	    chmod(local_name,new_mode);
      }
   }
}
void FileSet::LocalChown(const char *dir)
{
   for(int i=0; i<fnum; i++)
   {
      const Ref<FileInfo>& file=files[i];
      if(file->defined & (file->USER|file->GROUP))
      {
#ifndef HAVE_LCHOWN
	 if(file->defined & file->TYPE
	 && file->filetype==file->SYMLINK)
	    continue;
#define lchown chown
#endif

	 const char *local_name=dir_file(dir,file->name);

	 struct stat st;

	 if(lstat(local_name,&st)==-1)
	    continue;
	 uid_t new_uid=st.st_uid;
	 gid_t new_gid=st.st_gid;
	 if(file->defined&file->USER)
	 {
	    int u=PasswdCache::LookupS(file->user);
	    if(u!=-1)
	       new_uid=u;
	 }
	 if(file->defined&file->GROUP)
	 {
	    int g=GroupCache::LookupS(file->group);
	    if(g!=-1)
	       new_gid=g;
	 }
	 if(new_uid!=st.st_uid || new_gid!=st.st_gid)
	 {
	    if(lchown(local_name,new_uid,new_gid)==-1)
	       /* don't care */;
	 }
      }
   }
}

FileInfo * FileSet::operator[](int i) const
{
   if(i>=fnum || i<0)
      return 0;
   if(sorted)
      i=sorted[i];
   return files[i].get_non_const();
}

FileInfo *FileSet::curr()
{
   return (*this)[ind];
}
FileInfo *FileSet::next()
{
   if(ind<fnum)
   {
      ind++;
      return curr();
   }
   return 0;
}
void FileSet::SubtractCurr()
{
   Sub(ind--);
}

void FileInfo::Init()
{
   filetype=UNKNOWN;
   mode=(mode_t)-1;
   date=NO_DATE;
   size=NO_SIZE;
   nlinks=0;
   defined=0;
   need=0;
   user=0; group=0;
   rank=0;
}

FileInfo::FileInfo(const FileInfo &fi)
{
   Init();
   name.set(fi.name);
   symlink.set(fi.symlink);
   user=fi.user;
   group=fi.group;
   defined=fi.defined;
   filetype=fi.filetype;
   mode=fi.mode;
   date=fi.date;
   size=fi.size;
   nlinks=fi.nlinks;
   longname.set(fi.longname);
}
FileInfo::FileInfo(const char *n)
{
   Init();
   SetName(n);
}

FileInfo::~FileInfo()
{
}

#ifndef S_ISLNK
# define S_ISLNK(mode) (S_IFLNK==(mode&S_IFMT))
#endif

void FileInfo::LocalFile(const char *name, bool follow_symlinks)
{
   if(!this->name)
      SetName(name);

   struct stat st;
   if(lstat(name,&st)==-1)
      return;

check_again:
   FileInfo::type t;
   if(S_ISDIR(st.st_mode))
      t=FileInfo::DIRECTORY;
   else if(S_ISREG(st.st_mode))
      t=FileInfo::NORMAL;
   else if(S_ISLNK(st.st_mode))
   {
      if(follow_symlinks)
      {
	 if(stat(name,&st)!=-1)
	    goto check_again;
	 // dangling symlink, don't follow it.
      }
      t=FileInfo::SYMLINK;
   }
   else
      return;   // ignore other type files

   SetSize(st.st_size);
   int prec=0;
#if defined(HAVE_STATFS) && defined(MSDOS_SUPER_MAGIC)
   struct statfs stfs;
   if(statfs(name,&stfs)!=-1 && stfs.f_type==MSDOS_SUPER_MAGIC)
      prec=1;  // MS-DOS fs has 2-second resolution
#endif
   SetDate(st.st_mtime,prec);
   SetMode(st.st_mode&07777);
   SetType(t);
   SetNlink(st.st_nlink);

   SetUser(PasswdCache::LookupS(st.st_uid));
   SetGroup(GroupCache::LookupS(st.st_gid));

   if(t==SYMLINK)
   {
      char *buf=string_alloca(st.st_size+1);
      int res=readlink(name,buf,st.st_size);
      if(res!=-1)
      {
	 buf[res]=0;
	 SetSymlink(buf);
      }
   }
}

/* parse_ls_line: too common procedure to make it protocol specific */
/*
-rwxr-xr-x   1 lav      root         4771 Sep 12  1996 install-sh
-rw-r--r--   1 lav      root         1349 Feb  2 14:10 lftp.lsm
drwxr-xr-x   4 lav      root         1024 Feb 22 15:32 lib
lrwxrwxrwx   1 lav      root           33 Feb 14 17:45 ltconfig -> /usr/share/libtool/ltconfig
NOTE: group may be missing.
*/
FileInfo *FileInfo::parse_ls_line(const char *line_c,const char *tz)
{
   char *line=alloca_strdup(line_c);
   char *next=0;
   FileInfo *fi=0; /* don't instantiate until we at least have something */
#define FIRST_TOKEN strtok_r(line," \t",&next)
#define NEXT_TOKEN  strtok_r(NULL," \t",&next)
#define ERR do{delete fi;return(0);}while(0)

   /* parse perms */
   char *t = FIRST_TOKEN;
   if(t==0)
      ERR;

   fi = new FileInfo;
   switch(t[0])
   {
   case('l'):  // symlink
      fi->SetType(fi->SYMLINK);
      break;
   case('d'):  // directory
      fi->SetType(fi->DIRECTORY);
      break;
   case('-'):  // plain file
      fi->SetType(fi->NORMAL);
      break;
   case('b'): // block
   case('c'): // char
   case('p'): // pipe
   case('s'): // sock
   case('D'): // Door
      return 0;  // ignore
   default:
      ERR;
   }
   mode_t mode=parse_perms(t+1);
   if(mode!=(mode_t)-1)
      fi->SetMode(mode);

   // link count
   t = NEXT_TOKEN;
   if(!t)
      ERR;
   fi->SetNlink(atoi(t));

   // user
   t = NEXT_TOKEN;
   if(!t)
      ERR;
   fi->SetUser(t);

   // group or size
   char *group_or_size = NEXT_TOKEN;

   // size or month
   t = NEXT_TOKEN;
   if(!t)
      ERR;
   if(isdigit((unsigned char)*t))
   {
      // it's size, so the previous was group:
      fi->SetGroup(group_or_size);
      long long size;
      if(sscanf(t,"%lld",&size)==1)
	 fi->SetSize(size);
      t = NEXT_TOKEN;
      if(!t)
	 ERR;
   }
   else
   {
      // it was month, so the previous was size:
      long long size;
      if(sscanf(group_or_size,"%lld",&size)==1)
	 fi->SetSize(size);
   }

   struct tm date;
   memset(&date,0,sizeof(date));

   date.tm_mon=parse_month(t);
   if(date.tm_mon==-1)
      date.tm_mon=0;

   const char *day_of_month = NEXT_TOKEN;
   if(!day_of_month)
      ERR;
   date.tm_mday=atoi(day_of_month);

   bool year_anomaly=false;

   // time or year
   t = NEXT_TOKEN;
   if(!t)
      ERR;
   date.tm_isdst=-1;
   date.tm_hour=date.tm_min=0;
   date.tm_sec=30;
   int prec=30;

   if(sscanf(t,"%2d:%2d",&date.tm_hour,&date.tm_min)==2)
      date.tm_year=guess_year(date.tm_mon,date.tm_mday,date.tm_hour,date.tm_min) - 1900;
   else
   {
      if(day_of_month+strlen(day_of_month)+1 == t)
	 year_anomaly=true;
      date.tm_year=atoi(t)-1900;
      /* We don't know the hour.  Set it to something other than 0, or
       * DST -1 will end up changing the date. */
      date.tm_hour = 12;
      date.tm_min=0;
      date.tm_sec=0;
      prec=12*HOUR;
   }

   fi->SetDate(mktime_from_tz(&date,tz),prec);

   char *name=strtok_r(NULL,"",&next);
   if(!name)
      ERR;

   // there are ls which output extra space after year.
   if(year_anomaly && *name==' ')
      name++;

   if(fi->filetype==fi->SYMLINK)
   {
      char *arrow=name;
      while((arrow=strstr(arrow," -> "))!=0)
      {
	 if(arrow!=name && arrow[4]!=0)
	 {
	    *arrow=0;
	    fi->SetSymlink(arrow+4);
	    break;
	 }
	 arrow++;
      }
   }
   fi->SetName(name);
   fi->SetLongName(line_c);

   return fi;
}


int FileSet::Have() const
{
   int bits=0;

   for(int i=0; i<fnum; i++)
      bits |= files[i]->defined;

   return bits;
}

static bool fnmatch_dir(const char *pattern,const FileInfo *file)
{
   const char *name=file->name;
   if(file->defined&file->TYPE && file->filetype==file->DIRECTORY)
   {
      char *n=alloca_strdup2(name,1);
      strcat(n,"/");
      name=n;
   }
   return fnmatch(pattern,name,FNM_PATHNAME|FNM_CASEFOLD);
}

void FileSet::SortByPatternList(const char *list_c)
{
   const int max_rank=1000000;
   for(int i=0; i<fnum; i++)
      files[i]->SetRank(max_rank);
   char *list=alloca_strdup(list_c);
   int rank=0;
   for(char *p=strtok(list," "); p; p=strtok(0," "), rank++)
      for(int i=0; i<fnum; i++)
	 if(files[i]->GetRank()==max_rank && !fnmatch_dir(p,files[i]))
	    files[i]->SetRank(rank);
   Sort(BYRANK);
}

void FileInfo::MakeLongName()
{
   char filetype_s[2]="-";
   char &filetype_c=filetype_s[0];
   switch(filetype)
   {
   case NORMAL:	   break;
   case UNKNOWN:   break;
   case DIRECTORY: filetype_c='d'; break;
   case SYMLINK:   filetype_c='l'; break;
   }
   int mode1=(defined&MODE?mode:
      (filetype_c=='d'?0755:(filetype_c=='l'?0777:0644)));

   const char *usergroup="";
   if(defined&(USER|GROUP)) {
      usergroup=xstring::format("%.16s%s%.16s",defined&USER?user:"?",
		  defined&GROUP?"/":"",defined&GROUP?group:"");
   }

   int w=20-strlen(usergroup);
   if(w<1)
      w=1;
   char size_str[21];
   if(defined&SIZE)
      snprintf(size_str,sizeof(size_str),"%*lld",w,(long long)size);
   else
      snprintf(size_str,sizeof(size_str),"%*s",w,"-");

   const char *date_str="-";
   if(defined&DATE)
      date_str=TimeDate(date).IsoDateTime();

   longname.vset(filetype_s,format_perms(mode1),"  ",usergroup," ",size_str,
      " ",date_str," ",name.get(),NULL);

   if(defined&SYMLINK_DEF)
      longname.vappend(" -> ",symlink.get(),NULL);
}

size_t FileSet::EstimateMemory() const
{
   size_t size=sizeof(FileSet)
      +files.count()*files.get_element_size()
      +sorted.count()*sorted.get_element_size();
   for(int i=0; i<fnum; i++)
   {
      size+=sizeof(FileInfo);
      size+=xstrlen(files[i]->name);
      size+=xstrlen(files[i]->symlink);
      size+=xstrlen(files[i]->longname);
   }
   return size;
}
