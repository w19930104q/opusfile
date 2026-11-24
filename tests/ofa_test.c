/*ofa_test.c: Verify that a failing op_pcm_seek() rolls playback back to the
   last successfully committed seek position instead of leaving it at the
   bisection's stopping point.

   Scenario (see ofa_gen.c for the fixture construction):
     1. Open OF-A, a two-link chained Ogg Opus file whose second link is
         truncated behind an inflated granule position.
     2. op_pcm_seek(48000): a legal seek into link 1. Must succeed and leave
         cur_link==1 and op_pcm_tell()==48000.
     3. Read 240 float samples and record them (the "anchor" samples).
     4. op_pcm_seek() into the claimed-but-missing region: must fail with
         OP_EBADLINK. Repeating the same seek must fail with the same error
         code.
     5. After the failure: cur_link must still be 1 and op_pcm_tell() must
         still be 48000.
     6. Read another 240 float samples: they must be bit-identical to the
         anchor samples, proving the decoder/framing state was deterministically
         rebuilt at 48000.
     7. A subsequent legal seek (back to 24000, start of link 1) must succeed,
         and reading must continue without error.*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opusfile.h>

#define OFA_ANCHOR_PCM   48000
#define OFA_LEGAL_PCM    24000
#define OFA_SAMPLES_N    240
/*Inflated pcm_end of link 1 (link0 24000 + 39 frames*960 + 9600 gap).
  Any target in (61440, 71040] passes op_get_granulepos() but cannot be
   decoded because the backing pages were cut.*/
#define OFA_MISSING_PCM  64320

static int ofa_fail(const char *_fmt,...){
  va_list ap;
  va_start(ap,_fmt);
  fprintf(stderr,"FAIL: ");
  vfprintf(stderr,_fmt,ap);
  fputc('\n',stderr);
  va_end(ap);
  return 1;
}

/*Read exactly _want samples via op_read_float(), tolerating link changes.
  Returns 0 on success, 1 on error/EOF.*/
static int ofa_read_exact(OggOpusFile *_of,float *_out,int _want){
  int got;
  got=0;
  while(got<_want){
    int li;
    int n;
    n=op_read_float(_of,_out+got,_want-got,&li);
    if(n<0)return ofa_fail("op_read_float returned %d after %d samples",n,got);
    if(n==0)return ofa_fail("unexpected EOF after %d of %d samples",got,_want);
    got+=n;
  }
  return 0;
}

int main(int _argc,char **_argv){
  const char    *path;
  OggOpusFile   *of;
  int            err;
  int            ret;
  int            li;
  ogg_int64_t    tell;
  float          anchor[OFA_SAMPLES_N];
  float          after[OFA_SAMPLES_N];
  int            i;
  if(_argc!=2){
    fprintf(stderr,"usage: %s OFA.oga\n",_argv[0]);
    return 2;
  }
  path=_argv[1];
  of=op_open_file(path,&err);
  if(of==NULL)return ofa_fail("op_open_file(%s) failed: %d",path,err);
  if(op_link_count(of)!=2){
    return ofa_fail("expected 2 links, got %d",op_link_count(of));
  }
  /*Step 2: legal anchor seek to 48000 (link 1).*/
  ret=op_pcm_seek(of,OFA_ANCHOR_PCM);
  if(ret!=0)return ofa_fail("anchor seek to %d failed: %d",
   OFA_ANCHOR_PCM,ret);
  li=op_current_link(of);
  if(li!=1)return ofa_fail("after anchor seek cur_link=%d, expected 1",li);
  tell=op_pcm_tell(of);
  if(tell!=OFA_ANCHOR_PCM){
    return ofa_fail("after anchor seek op_pcm_tell=%lld, expected %d",
     (long long)tell,OFA_ANCHOR_PCM);
  }
  /*Step 3: record the first 240 samples at the anchor.*/
  if(ofa_read_exact(of,anchor,OFA_SAMPLES_N))return 1;
  /*Step 4: seek into the claimed-but-missing region. Must fail.*/
  ret=op_pcm_seek(of,OFA_MISSING_PCM);
  if(ret!=OP_EBADLINK){
    return ofa_fail("seek to missing region returned %d, expected "
     "OP_EBADLINK(%d)",ret,OP_EBADLINK);
  }
  /*The error code must not change when the same failed seek is retried.*/
  err=op_pcm_seek(of,OFA_MISSING_PCM);
  if(err!=ret){
    return ofa_fail("retry of failing seek returned %d, expected %d",err,
     ret);
  }
  /*Step 5: position must be restored to the anchor.*/
  li=op_current_link(of);
  if(li!=1)return ofa_fail("after failed seek cur_link=%d, expected 1",li);
  tell=op_pcm_tell(of);
  if(tell!=OFA_ANCHOR_PCM){
    return ofa_fail("after failed seek op_pcm_tell=%lld, expected %d",
     (long long)tell,OFA_ANCHOR_PCM);
  }
  /*Step 6: the next 240 samples must bit-match the anchor samples.*/
  if(ofa_read_exact(of,after,OFA_SAMPLES_N))return 1;
  for(i=0;i<OFA_SAMPLES_N;i++){
    if(memcmp(anchor+i,after+i,sizeof(*anchor))!=0){
      return ofa_fail("sample %d differs after rollback: %.9g vs %.9g",i,
       (double)anchor[i],(double)after[i]);
    }
  }
  /*Step 7: the next legal seek must still work, and reads must continue.*/
  ret=op_pcm_seek(of,OFA_LEGAL_PCM);
  if(ret!=0)return ofa_fail("legal recovery seek to %d failed: %d",
   OFA_LEGAL_PCM,ret);
  tell=op_pcm_tell(of);
  if(tell!=OFA_LEGAL_PCM){
    return ofa_fail("after recovery seek op_pcm_tell=%lld, expected %d",
     (long long)tell,OFA_LEGAL_PCM);
  }
  if(ofa_read_exact(of,after,OFA_SAMPLES_N))return 1;
  /*And a failing seek after the recovery seek must still roll back cleanly
     and keep the same error code.*/
  ret=op_pcm_seek(of,OFA_MISSING_PCM);
  if(ret!=OP_EBADLINK){
    return ofa_fail("post-recovery failing seek returned %d, expected %d",
     ret,OP_EBADLINK);
  }
  tell=op_pcm_tell(of);
  if(tell!=OFA_LEGAL_PCM){
    return ofa_fail("after post-recovery failed seek op_pcm_tell=%lld, "
     "expected %d",(long long)tell,OFA_LEGAL_PCM);
  }
  op_free(of);
  printf("PASS: failed seek rolled back to link 1 / PCM %d; %d float samples "
   "bit-identical; retry error code stable; legal seek still works\n",
   OFA_ANCHOR_PCM,OFA_SAMPLES_N);
  return 0;
}
