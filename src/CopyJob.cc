/*
 * lftp and utils
 *
 * Copyright (c) 1999-2000 by Alexander V. Lukyanov (lav@yars.free.net)
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
#include "CopyJob.h"
#include "ArgV.h"
#include "plural.h"
#include "misc.h"

int CopyJob::Do()
{
   if(!fg_data)
      fg_data=c->GetFgData(fg);
   if(done)
      return STALL;
   if(c->Error())
   {
      eprintf("%s: %s\n",op,c->ErrorText());
      done=true;
      return MOVED;
   }
   if(c->Done())
   {
      done=true;
      return MOVED;
   }
   if(!c->WriteAllowed() && c->WritePending())
   {
      if(no_status_on_write)
      {
	 // clear status.
	 const char *empty="";
	 eprintf(empty);
	 // disable status.
	 NoStatus();
      }
      c->AllowWrite();
      return MOVED;
   }
   return STALL;
}
int CopyJob::Done()
{
   return done;
}
int CopyJob::ExitCode()
{
   if(c->Error())
      return 1;
   return 0;
}

const char *CopyJob::SqueezeName(int w)
{
   return squeeze_file_name(name,w);
}

// xgettext:c-format
static const char copy_status_format[]=N_("`%s' at %lld %s%s%s%s");
#define COPY_STATUS _(copy_status_format),name,\
      (long long)c->GetPos(),c->GetPercentDoneStr(),c->GetRateStr(),\
      c->GetETAStr(),c->GetStatus()

void CopyJob::ShowRunStatus(StatusLine *s)
{
   if(no_status)
      return;
   if(c->Done() || c->Error())
      return;

   const char *name=SqueezeName(s->GetWidthDelayed()-50);
   s->Show(COPY_STATUS);
}
void CopyJob::PrintStatus(int v)
{
   if(c->Done() || c->Error())
      return;

   printf("\t");
   const char *name=GetName();
   printf(COPY_STATUS);
   printf("\n");
}

int CopyJob::AcceptSig(int sig)
{
   if(c==0 || GetProcGroup()==0)
   {
      if(sig==SIGINT || sig==SIGTERM)
	 return WANTDIE;
      return STALL;
   }
   c->Kill(sig);
   if(sig!=SIGCONT)
      c->Kill(SIGCONT);
   return MOVED;
}

CopyJob::CopyJob(FileCopy *c1,const char *name1,const char *op1)
{
   c=c1;
   name=xstrdup(name1);
   op=xstrdup(op1);
   done=false;
   no_status=false;
   no_status_on_write=false;
}
CopyJob::~CopyJob()
{
   Delete(c);
   xfree(name);
   xfree(op);
}

CopyJob *CopyJob::NewEcho(const char *str,int len,FDStream *o,const char *op)
{
   if(o==0)
      o=new FDStream(1,"<stdout>");
   CopyJob *j=new CopyJob(FileCopy::New(
	 new FileCopyPeerString(str,len),
	 new FileCopyPeerFDStream(o,FileCopyPeer::PUT),
	 false
      ),o->name,op);
   if(o->usesfd(1))
      j->NoStatus();
   return j;
}

// CopyJobEnv
CopyJobEnv::CopyJobEnv(FileAccess *s,ArgV *a,bool cont1)
   : SessionJob(s)
{
   args=a;
   args->rewind();
   op=args?args->a0():"?";
   done=false;
   cp=0;
   errors=0;
   count=0;
   bytes=0;
   time_spent=0;
   no_status=false;
   cont=cont1;
   ascii=false;
   cwd=xgetcwd();
}
CopyJobEnv::~CopyJobEnv()
{
   SetCopier(0,0);
   if(args)
      delete args;
   xfree(cwd);
}
int CopyJobEnv::Do()
{
   int m=STALL;
   if(done)
      return STALL;
   if(waiting_num<1)
   {
      NextFile();
      if(waiting_num==0)
      {
	 done=true;
	 m=MOVED;
      }
      else if(cp==0)
	 cp=(CopyJob*)waiting[0];
   }
   CopyJob *j=(CopyJob*)FindDoneAwaitedJob();	// we start only CopyJob's.
   if(j==0)
      return m;
   RemoveWaiting(j);
   if(j->ExitCode()!=0)
      errors++;
   count++;
   bytes+=j->GetBytesCount();
   time_spent+=j->GetTimeSpent();
   return MOVED;
}
void CopyJobEnv::AddCopier(FileCopy *c,const char *n)
{
   if(c==0)
      return;
   if(ascii)
      c->Ascii();
   cp=new CopyJob(c,n,op);
   cp->SetParentFg(this);
   AddWaiting(cp);
}
void CopyJobEnv::SetCopier(FileCopy *c,const char *n)
{
   while(waiting_num>0)
   {
      Job *j=waiting[0];
      RemoveWaiting(j);
      Delete(j);
   }
   cp=0;
   AddCopier(c,n);
}

void CopyJobEnv::SayFinalWithPrefix(const char *p)
{
   if(no_status)
      return;
   if(count==errors)
      return;
   if(bytes)
   {
      printf("%s",p);
      if(time_spent>=1)
      {
	 printf(plural("%lld $#ll#byte|bytes$ transferred"
			" in %ld $#l#second|seconds$",
			(long long)bytes,long(time_spent+.5)),
			(long long)bytes,long(time_spent+.5));
	 double rate=bytes/time_spent;
	 if(rate>=1)
	    printf(" (%s)\n",Speedometer::GetStr(rate));
	 else
	    printf("\n");
      }
      else
      {
	 printf(plural("%lld $#ll#byte|bytes$ transferred\n",
			(long long)bytes),(long long)bytes);
      }
   }
   if(errors>0)
   {
      printf("%s",p);
      printf(plural("Transfer of %d of %d $file|files$ failed\n",count),
	 errors,count);
   }
   else if(count>1)
   {
      printf("%s",p);
      printf(plural("Total %d $file|files$ transferred\n",count),count);
   }
}
void CopyJobEnv::PrintStatus(int v)
{
   SessionJob::PrintStatus(v);
   if(Done())
      SayFinalWithPrefix("\t");
}

int CopyJobEnv::AcceptSig(int sig)
{
   if(cp==0)
   {
      if(sig==SIGINT || sig==SIGTERM)
	 return WANTDIE;
      return STALL;
   }
   int total;
   if(sig==SIGINT || sig==SIGTERM)
      total=WANTDIE;
   else
      total=STALL;
   for(int i=0; i<waiting_num; i++)
   {
      Job *j=waiting[i];
      int res=j->AcceptSig(sig);
      if(res==WANTDIE)
      {
	 RemoveWaiting(j);
	 Delete(j);
	 if(cp==j)
	    cp=0;
      }
      else if(res==MOVED)
	 total=MOVED;
      else if(res==STALL)
      {
	 if(total==WANTDIE)
	    total=MOVED;
      }
   }
   if(waiting_num>0 && cp==0)
      cp=(CopyJob*)waiting[0];
   return total;
}

int CopyJobEnv::Done()
{
   return done;
}
