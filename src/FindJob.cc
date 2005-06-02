/*
 * lftp and utils
 *
 * Copyright (c) 1998-2000 by Alexander V. Lukyanov (lav@yars.free.net)
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id$ */

#include <config.h>
#include <assert.h>
#include <fnmatch.h>

#include "FindJob.h"
#include "CmdExec.h"
#include "misc.h"
#include "GetFileInfo.h"
#include "url.h"
#include "PatternSet.h"
#include "buffer_std.h"

#define top (*stack[stack_ptr])
#define super SessionJob

int FinderJob::Do()
{
   int m=STALL;
   prf_res pres;
   Job *j;

   switch(state)
   {
   case START_INFO:
   {
      if(stack_ptr==-1)
      {
	 ParsedURL u(dir,true);
	 if(u.proto)
	 {
	    session=FileAccess::New(&u);
	    session->SetPriority(fg?1:0);
	    init_dir=xstrdup(session->GetCwd());
	    Down(u.path?u.path:init_dir);
	 }
      }

      /* If we're not validating, and this is an argument (first-level path),
       * pretend the file exists. */
      if((file_info_need|FileInfo::NAME) == FileInfo::NAME &&
	    !validate_args && stack_ptr == -1)
      {
	 FileSet *fs = new FileSet();
	 fs->Add(new FileInfo(dir));
	 Push(fs);
	 state=LOOP;
	 return MOVED;
      }

      /* The first time we get here (stack_ptr == -1), dir is an actual
       * argument, so it might be a file.  (Every other time, it's guaranteed
       * to be a directory.)  Set show_dirs to true, so it'll end up actually
       * being on the stack, with type information. */
      li=new GetFileInfo(session, dir, stack_ptr == -1);

      /* Prepend for the argument level entry only: */
      if(stack_ptr != -1)
	 li->DontPrependPath();

      int need = file_info_need|FileInfo::NAME;

      /* We only explicitely need the type if we're recursing further. */
      if(stack_ptr+1 < maxdepth)
	 need |= FileInfo::TYPE;

      li->Need(need);
      if(use_cache)
	 li->UseCache();
      state=INFO;
      m=MOVED;
   }
   case INFO:
      if(!li->Done())
	 return m;
      if(li->Error())
      {
	 if(!quiet)
	    eprintf("%s: %s\n",op,li->ErrorText());
	 Delete(li);
	 li=0;
	 errors++;
	 depth_done=true;
	 state=LOOP;
	 return MOVED;
      }

      if(stack_ptr != -1 && li->WasDirectory())
	 Enter(dir);

      Push(li->GetResult());
      top.fset->rewind();

      Delete(li);
      li=0;
      state=LOOP;
      m=MOVED;
   case LOOP:
      if(stack_ptr==-1 || top.fset->curr()==0)
      {
	 Up();
	 return MOVED;
      }

      session->SetCwd(init_dir);
      session->Chdir(top.path,false);
      // at this point either is true:
      // 1. we just process another file (!depth_done)
      // 2. we just returned from a subdir (depth_done)
      if(depth_first && !depth_done && (maxdepth == -1 || stack_ptr+1 < maxdepth))
      {
	 FileInfo *f=top.fset->curr();
	 if((f->defined&f->TYPE) && f->filetype==f->DIRECTORY)
	 {
	    Down(f->name);
	    return MOVED;
	 }
      }
      pres=ProcessFile(top.path,top.fset->curr());

      if(pres==PRF_LATER)
	 return m;

      depth_done=false;

      switch(pres)
      {
      case(PRF_FATAL):
	 errors++;
	 state=DONE;
	 m=MOVED;
	 return m;
      case(PRF_ERR):
	 errors++;
	 break;
      case(PRF_WAIT):
	 state=WAIT;
	 return MOVED;
      case(PRF_OK):
	 break;
      case(PRF_LATER):
	 abort();
      }
   post_WAIT:
      if(stack_ptr==-1)
	 return m;
      if(!depth_first && (maxdepth == -1 || stack_ptr+1 < maxdepth))
      {
	 FileInfo *f=top.fset->curr();
	 if((f->defined&f->TYPE) && f->filetype==f->DIRECTORY)
	 {
	    top.fset->next();
	    Down(f->name);
	    return MOVED;
	 }
      }
      top.fset->next();
      return MOVED;

   case WAIT:
      j=FindDoneAwaitedJob();
      if(!j)
	 return m;
      RemoveWaiting(j);
      Delete(j);
      state=LOOP;
      m=MOVED;
      goto post_WAIT;

   case DONE:
      return m;
   }
   return m;
}

void FinderJob::Up()
{
   if(stack_ptr==-1)
   {
   done:
      state=DONE;
      Finish();
      return;
   }
   /* stack[0] is the dir entry for the argument (ie. ls -d dir), and
    * stack[1] is the contents (ls dir); don't exit for the first. */
   if(stack_ptr)
      Exit();
   delete stack[stack_ptr--];
   if(stack_ptr==-1)
      goto done;
   depth_done=true;
   state=LOOP;
}

void FinderJob::Push(FileSet *fset)
{
   const char *old_path=0;
   if(stack_ptr>=0)
      old_path=top.path;

   stack_ptr++;
   if(stack_allocated<=stack_ptr)
   {
      stack_allocated=stack_ptr+8;
      stack=(place**)xrealloc(stack,sizeof(*stack)*stack_allocated);
   }

   if(stack_ptr != 0)
      fset->ExcludeDots(); /* don't need . and .. (except for stack[0]) */

   const char *new_path="";
   if(old_path) // the first path will be empty
      new_path=alloca_strdup(dir_file(old_path,dir));

   /* matching exclusions don't include the path, so they operate
    * on the filename portion only */
   if(exclude)
      fset->Exclude(0, exclude);
   stack[stack_ptr]=new place(new_path,fset);

   /* give a chance to operate on the list as a whole, and
    * possibly sort it */
   ProcessList(fset);
}

FinderJob::place::place(const char *p,FileSet *f)
{
   path=xstrdup(p);
   fset=f;
}
FinderJob::place::~place()
{
   xfree(path);
   if(fset) delete fset;
}

void FinderJob::Down(const char *p)
{
#ifdef FIND_DEBUG
   printf("Down(%s)\n",p);
#endif
   xfree(dir);
   dir=xstrdup(p);
   state=START_INFO;
}

FinderJob::prf_res FinderJob::ProcessFile(const char *d,const FileInfo *f)
{
   return PRF_OK;
}

void FinderJob::Init()
{
   orig_session=0;
   orig_init_dir=0;

   op="find";
   init_dir=0;
   dir=0;
   errors=0;
   li=0;

   stack_allocated=0;
   stack_ptr=-1;
   stack=0;

   show_sl=true;

   depth_first=false; // useful for rm -r
   depth_done=false;

   file_info_need=0;
   use_cache=true;
   validate_args=false;

   quiet=false;
   maxdepth=-1;
   exclude=0;

   state=START_INFO;
}

FinderJob::FinderJob(FileAccess *s)
   : SessionJob(s)
{
   Init();
   init_dir=xstrdup(session->GetCwd());
   orig_session=session;
   orig_init_dir=init_dir;
}

void FinderJob::NextDir(const char *d)
{
   if(session!=orig_session)
   {
      Reuse(session);
      session=orig_session;
      assert(orig_init_dir!=init_dir);
      xfree(init_dir);
      init_dir=orig_init_dir;
   }
   session->SetCwd(init_dir);
   Down(d);
}

FinderJob::~FinderJob()
{
   while(stack_ptr>=0)
      Up();
   if(orig_session!=session)
      Reuse(orig_session);
   if(orig_init_dir!=init_dir)
      xfree(orig_init_dir);
   xfree(stack);
   xfree(init_dir);
   delete exclude;
   xfree(dir);
   Delete(li);
}

void FinderJob::ShowRunStatus(StatusLine *sl)
{
   if(!show_sl)
      return;

   char *path=0;
   switch(state)
   {
   case INFO:
      if(stack_ptr>=0)
	 path=top.path;
      sl->Show("%s: %s",dir_file(path,dir),li->Status());
      break;
   case WAIT:
      Job::ShowRunStatus(sl);
      break;
   default:
      sl->Clear();
      break;
   }
}

void FinderJob::PrintStatus(int v,const char *prefix)
{
   SessionJob::PrintStatus(v,prefix);

   char *path=0;
   switch(state)
   {
   case INFO:
      if(stack_ptr>=0)
	 path=top.path;
      printf("\t%s: %s\n",dir_file(path,dir),li->Status());
      break;
   case WAIT:
      break;
   default:
      break;
   }
}

void FinderJob::Fg()
{
   super::Fg();
   if(orig_session!=session)
      orig_session->SetPriority(1);
}
void FinderJob::Bg()
{
   if(orig_session!=session)
      orig_session->SetPriority(0);
   super::Bg();
}

// FinderJob_List implementation
// find files and write list to a stream
FinderJob::prf_res FinderJob_List::ProcessFile(const char *d,const FileInfo *fi)
{
   if(buf->Broken())
      return PRF_FATAL;
   if(buf->Error())
   {
      eprintf("%s: %s\n",op,buf->ErrorText());
      return PRF_FATAL;
   }
   if(fg_data==0)
      fg_data=buf->GetFgData(fg);
   if(buf->Size()>0x10000)
      return PRF_LATER;
   if(ProcessingURL())
   {
      char *old_cwd=alloca_strdup(session->GetCwd());
      session->SetCwd(init_dir);
      session->Chdir(dir_file(d,fi->name),false);
      buf->Put(session->GetConnectURL());
      session->SetCwd(old_cwd);
   }
   else
      buf->Put(dir_file(d,fi->name));
   if((fi->defined&fi->TYPE) && fi->filetype==fi->DIRECTORY && strcmp(fi->name,"/"))
      buf->Put("/");
   buf->Put("\n");
   return FinderJob::ProcessFile(d,fi);
}

FinderJob_List::FinderJob_List(FileAccess *s,ArgV *a,FDStream *o)
   : FinderJob(s), args(a)
{
   if(o)
      buf=new IOBufferFDStream(o,IOBuffer::PUT);
   else
      buf=new IOBuffer_STDOUT(this);
   show_sl = !o || !o->usesfd(1);
   NextDir(a->getcurr());
   ValidateArgs();
}

FinderJob_List::~FinderJob_List()
{
   Delete(buf);
   delete args;
}

void FinderJob_List::Finish()
{
   const char *d=args->getnext();
   if(!d) {
      buf->PutEOF();
      return;
   }
   NextDir(d);
}

#if 0 // unused
// FinderJob_Cmd implementation
// process directory tree
#undef super
#define super FinderJob

FinderJob::prf_res FinderJob_Cmd::ProcessFile(const char *d,const FileInfo *f)
{
#define ISDIR  ((f->defined&f->TYPE) && f->filetype==f->DIRECTORY)
#define ISLINK ((f->defined&f->TYPE) && f->filetype==f->SYMLINK)

   FileAccess *s=session->Clone();

   CmdExec *exec=new CmdExec(s);
   exec->SetParentFg(this);
   exec->SetCWD(saved_cwd);

   char *file=f->name;

   switch(cmd)
   {
   case RM:
      if(ISDIR)
      {
	 exec->FeedCmd("rmdir ");
	 if(quiet)
	    exec->FeedCmd("-f ");
	 exec->FeedCmd("-- ");
	 exec->FeedQuoted(file);
	 exec->FeedCmd("\n");
      }
      else
      {
	 exec->FeedCmd("rm ");
	 if(quiet)
	    exec->FeedCmd("-f ");
	 exec->FeedCmd("-- ");
	 exec->FeedQuoted(file);
	 exec->FeedCmd("\n");
      }
      break;
   case GET:
#if 0
      if(ISDIR)
      {
// 	 mkdir(dir_file(saved_cwd,
      }
      else if(ISLINK)
      {
      }
      else
      {
      }
#endif
      break;
   }
   AddWaiting(exec);
   return PRF_WAIT;

#undef ISDIR
#undef ISLINK
}

FinderJob_Cmd::FinderJob_Cmd(FileAccess *s,ArgV *a,cmd_t c)
   : FinderJob(s)
{
   cmd=c;
   args=a;
   op=args->a0();
   use_cache=false;
   if(cmd==RM)
      depth_first=true;
   saved_cwd=xgetcwd();
   removing_last=false;
   NextDir(a->getcurr());
}
FinderJob_Cmd::~FinderJob_Cmd()
{
   xfree(saved_cwd);
   delete args;
   while(waiting_num>0)
   {
      Job *r=waiting[0];
      for(int k=0; k<r->waiting_num; k++)
	 r->waiting[k]->jobno=-1; // prevent reparenting
      RemoveWaiting(r);
      Delete(r);
   }
}

void FinderJob_Cmd::Finish()
{
   char *d=args->getnext();
   if(!d)
      return;
   FinderJob::NextDir(d);
}

int FinderJob_Cmd::Done()
{
   return FinderJob::Done() && args->getcurr()==0;
}
#endif
