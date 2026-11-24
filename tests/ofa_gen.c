/*ofa_gen.c: Generate OF-A, a two-link chained Ogg Opus file whose second
   link is truncated in a way that actually makes op_pcm_seek() fail.

   A plain truncation does not fail a seek: at open time opusfile scans back
   to the last complete page and shrinks the link's pcm_end to match, so the
   truncated region simply becomes unseekable via op_get_granulepos()
   (op_pcm_seek() rejects it up front with OP_EINVAL).

   To make a seek fail *inside* the seek machinery we instead keep a complete
   page whose granule position claims more audio than the surviving data can
   provide:
     link1: 70 frames of 960 samples (20 ms) each are encoded.
     The page ending at frame 38 gets its granule position inflated by 9600
      samples (200 ms), i.e. a gap larger than the 80 ms (3840 sample) seek
      pre-roll.
     All pages after that one are cut from the file.
   The inflated page survives, so the open-time scan keeps pcm_end at the
   inflated value; seeks into the claimed-but-missing region pass the
   op_get_granulepos() check, bisect to the inflated page, then hit EOF while
   scanning forward for the packets the granule position claims, and fail with
   OP_EBADLINK after having already moved cur_link/tell to link 1.

   Layout (48 kHz, mono, 960-sample frames):
     link0: 25 frames -> file PCM 0..23999 (24000 samples).
     link1: 70 nominal frames; gap of 9600 samples starting after frame 38;
             cut immediately after the page containing frame 38.
     Anchor seek target: file PCM 48000 (inside link 1, well before the cut,
      ~13440 readable samples remain from there).
     First unreadable target: file PCM 64320.
     Nominal total duration of the uncut file: 71040 samples.

   The pages are framed by hand (instead of via ogg_stream) so that the
   granule position of the inflated page can be rewritten after encoding,
   with the Ogg CRC recomputed as required by libogg's page validation.*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>
#include <opus.h>

#define OFA_FRAME_SIZE 960
#define OFA_SAMPLE_RATE 48000
/*A 9600-sample (200 ms) gap: larger than the 80 ms seek pre-roll.*/
#define OFA_GAP_SAMPLES 9600
/*Link geometry.*/
#define OFA_LINK0_FRAMES 25
#define OFA_LINK1_FRAMES 70
#define OFA_GAP_AFTER_FRAME 38
/*One frame per Ogg page keeps the bisection geometry simple and makes every
   granule position explicit.*/
#define OFA_FRAMES_PER_PAGE 1

/*Ogg CRC-32 (polynomial 0x04c11db7), per RFC 3533, section A.6.*/
static ogg_uint32_t ofa_crc_table[256];

static void ofa_crc_init(void){
  ogg_uint32_t i;
  for(i=0;i<256;i++){
    ogg_uint32_t r;
    int          bit;
    r=i<<24;
    for(bit=0;bit<8;bit++){
      r=(r&0x80000000U)?(r<<1)^0x04c11db7U:r<<1;
    }
    ofa_crc_table[i]=r;
  }
}

static ogg_uint32_t ofa_crc(const unsigned char *_data,size_t _len){
  ogg_uint32_t crc;
  size_t       i;
  crc=0;
  for(i=0;i<_len;i++)crc=(crc<<8)^ofa_crc_table[((crc>>24)&0xff)^_data[i]];
  return crc;
}

static void ofa_put_le64(unsigned char *_p,ogg_int64_t _v){
  ogg_uint64_t u;
  memcpy(&u,&_v,sizeof(u));
  _p[0]=(unsigned char)(u&0xff);
  _p[1]=(unsigned char)((u>>8)&0xff);
  _p[2]=(unsigned char)((u>>16)&0xff);
  _p[3]=(unsigned char)((u>>24)&0xff);
  _p[4]=(unsigned char)((u>>32)&0xff);
  _p[5]=(unsigned char)((u>>40)&0xff);
  _p[6]=(unsigned char)((u>>48)&0xff);
  _p[7]=(unsigned char)((u>>56)&0xff);
}

static void ofa_put_le32(unsigned char *_p,ogg_uint32_t _v){
  _p[0]=(unsigned char)(_v&0xff);
  _p[1]=(unsigned char)((_v>>8)&0xff);
  _p[2]=(unsigned char)((_v>>16)&0xff);
  _p[3]=(unsigned char)((_v>>24)&0xff);
}

static void ofa_put_le16(unsigned char *_p,ogg_uint16_t _v){
  _p[0]=(unsigned char)(_v&0xff);
  _p[1]=(unsigned char)((_v>>8)&0xff);
}

/*Build a complete Ogg page around a single (header or audio) packet.
  Returns the page length, or -1 on error.*/
static int ofa_make_page(unsigned char *_out,size_t _out_size,
 ogg_uint32_t _serialno,ogg_int64_t _granpos,ogg_uint32_t _page_seq,
 int _bos,int _eos,const unsigned char *_packet,long _packet_bytes){
  unsigned char header[27+255];
  unsigned char segtab[255];
  int           nseg;
  long          remaining;
  size_t        header_len;
  size_t        page_len;
  ogg_uint32_t  crc;
  if(_packet_bytes>255*255)return -1;
  nseg=0;
  remaining=_packet_bytes;
  /*255 lacing values continue the packet; a final smaller value ends it.
    A zero-length packet still gets one zero lacing value.*/
  if(remaining==0){
    segtab[nseg++]=0;
  }
  while(remaining>0){
    if(remaining>=255){
      segtab[nseg++]=255;
      remaining-=255;
    }
    else{
      segtab[nseg++]=(unsigned char)remaining;
      remaining=0;
    }
  }
  header_len=27+(size_t)nseg;
  page_len=header_len+(size_t)_packet_bytes;
  if(page_len>_out_size)return -1;
  memcpy(header,"OggS",4);
  header[4]=0;                         /*stream structure version*/
  header[5]=(unsigned char)((_bos?0x02:0)|(_eos?0x04:0));
  ofa_put_le64(header+6,_granpos);     /*8-byte granule position*/
  ofa_put_le32(header+14,_serialno);
  ofa_put_le32(header+18,_page_seq);
  ofa_put_le32(header+22,0);           /*CRC, filled in below (LE)*/
  header[26]=(unsigned char)nseg;
  memcpy(header+27,segtab,nseg);
  memcpy(_out,header,header_len);
  memcpy(_out+header_len,_packet,_packet_bytes);
  /*Checksum over the whole page, with the CRC field (bytes 22..25) zero.
    RFC 3533 stores every multi-byte header field, including the CRC, in
     little-endian byte order.*/
  crc=ofa_crc(_out,page_len);
  ofa_put_le32(_out+22,crc);
  return (int)page_len;
}

/*Build an OpusHead packet (mapping family 0, mono): 19 bytes.*/
static int ofa_make_head(unsigned char *_out,opus_int32 _pre_skip){
  memcpy(_out,"OpusHead",8);
  _out[8]=1;                           /*version*/
  _out[9]=1;                           /*1 channel (mono)*/
  ofa_put_le16(_out+10,(ogg_uint16_t)_pre_skip);
  ofa_put_le32(_out+12,OFA_SAMPLE_RATE);
  ofa_put_le16(_out+16,0);             /*output gain*/
  _out[18]=0;                          /*mapping family 0*/
  return 19;
}

/*Build a minimal OpusTags packet: vendor string + zero comments.*/
static int ofa_make_tags(unsigned char *_out){
  static const char vendor[]="ofa_gen";
  int p;
  memcpy(_out,"OpusTags",8);
  p=8;
  ofa_put_le32(_out+p,(ogg_uint32_t)(sizeof(vendor)-1));
  p+=4;
  memcpy(_out+p,vendor,sizeof(vendor)-1);
  p+=(int)sizeof(vendor)-1;
  ofa_put_le32(_out+p,0);             /*no user comments*/
  p+=4;
  return p;
}

/*Encode one link: BOS/head page, tags page, then one audio page per frame.
  Writes pages into _out and returns the total number of bytes.
  If _cut_after_frame>=0, audio pages for frames after that index are omitted
   (the returned bytes end with the page for that frame), and the granule
   position of the last retained page is inflated by OFA_GAP_SAMPLES.
  *_next_page_seq is updated so the next link can continue page sequencing.
  Per the Ogg Opus spec, every link's granule-position origin restarts at 0;
   opusfile maps links onto file-relative PCM via pcm_file_offset itself.*/
static int ofa_encode_link(unsigned char *_out,size_t _out_size,
 ogg_uint32_t _serialno,int _nframes,
 int _cut_after_frame,ogg_uint32_t *_next_page_seq,unsigned _seed){
  OpusEncoder *enc;
  opus_int16   pcm[OFA_FRAME_SIZE];
  int          err;
  int          frame;
  size_t       used;
  ogg_uint32_t page_seq;
  unsigned char packet[4000];
  unsigned char page[27+255+sizeof(packet)];
  int          packet_bytes;
  int          page_len;
  ogg_int64_t  granpos;
  int          pre_skip;
  unsigned char head[19];
  unsigned char tags[64];
  int          tags_len;
  enc=opus_encoder_create(OFA_SAMPLE_RATE,1,OPUS_APPLICATION_AUDIO,&err);
  if(enc==NULL){
    fprintf(stderr,"opus_encoder_create failed: %d\n",err);
    return -1;
  }
  /*A deterministic, frame-dependent signal so different positions decode to
     different, reproducible samples.*/
  opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(10));
  pre_skip=0;
  err=opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&pre_skip));
  if(err!=OPUS_OK){
    fprintf(stderr,"OPUS_GET_LOOKAHEAD failed: %d\n",err);
    opus_encoder_destroy(enc);
    return -1;
  }
  used=0;
  page_seq=*_next_page_seq;
  /*BOS + OpusHead.*/
  ofa_make_head(head,pre_skip);
  page_len=ofa_make_page(page,sizeof(page),_serialno,0,page_seq++,1,0,
   head,19);
  if(page_len<0||used+(size_t)page_len>_out_size)goto fail;
  memcpy(_out+used,page,page_len);
  used+=page_len;
  /*OpusTags (not BOS, not EOS).*/
  tags_len=ofa_make_tags(tags);
  page_len=ofa_make_page(page,sizeof(page),_serialno,0,page_seq++,0,0,
   tags,tags_len);
  if(page_len<0||used+(size_t)page_len>_out_size)goto fail;
  memcpy(_out+used,page,page_len);
  used+=page_len;
  /*Audio pages: one frame per page.
    Granule positions count decoded samples (including pre-skip) from the
     link's granule origin, so frame f ends at pre_skip+(f+1)*960.*/
  for(frame=0;frame<_nframes;frame++){
    int i;
    for(i=0;i<OFA_FRAME_SIZE;i++){
      /*Simple deterministic tone mixture with frame-dependent phase.*/
      unsigned v;
      v=_seed+ (unsigned)frame*2654435761U+(unsigned)i*40503U;
      pcm[i]=(opus_int16)((v&0xffff)-32768);
    }
    packet_bytes=opus_encode(enc,pcm,OFA_FRAME_SIZE,packet,sizeof(packet));
    if(packet_bytes<0){
      fprintf(stderr,"opus_encode failed at frame %d: %d\n",frame,
       packet_bytes);
      goto fail;
    }
    granpos=pre_skip+(ogg_int64_t)(frame+1)*OFA_FRAME_SIZE;
    if(_cut_after_frame>=0&&frame==_cut_after_frame){
      /*Claim OFA_GAP_SAMPLES more samples than the data actually covers.
        Later pages (which would have backed this claim) are cut.*/
      granpos+=OFA_GAP_SAMPLES;
    }
    page_len=ofa_make_page(page,sizeof(page),_serialno,granpos,page_seq++,0,
     /*Only the final page of an uncut link sets EOS.*/
     (_cut_after_frame<0&&frame==_nframes-1),packet,packet_bytes);
    if(page_len<0)goto fail;
    if(_cut_after_frame>=0&&frame>_cut_after_frame)break;
    if(used+(size_t)page_len>_out_size)goto fail;
    memcpy(_out+used,page,page_len);
    used+=page_len;
  }
  opus_encoder_destroy(enc);
  *_next_page_seq=page_seq;
  return (int)used;
fail:
  opus_encoder_destroy(enc);
  return -1;
}

int main(int _argc,char **_argv){
  const char *path;
  FILE       *fp;
  unsigned char *buf;
  size_t       buf_size;
  size_t       used;
  int          n;
  ogg_uint32_t page_seq;
  ogg_uint32_t serial0;
  ogg_uint32_t serial1;
  if(_argc!=2){
    fprintf(stderr,"usage: %s OUTPUT.oga\n",_argv[0]);
    return 2;
  }
  path=_argv[1];
  ofa_crc_init();
  buf_size=256*1024;
  buf=(unsigned char *)malloc(buf_size);
  if(buf==NULL){
    fprintf(stderr,"malloc failed\n");
    return 1;
  }
  used=0;
  page_seq=0;
  serial0=0x4f464130;                 /*"OFA0"*/
  serial1=0x4f464131;                 /*"OFA1"*/
  /*Link 0: complete, 25 frames, granule origin 0.*/
  n=ofa_encode_link(buf+used,buf_size-used,serial0,OFA_LINK0_FRAMES,-1,
   &page_seq,1);
  if(n<0)return 1;
  used+=(size_t)n;
  /*Link 1: 70 nominal frames, inflated gap after frame 38, cut right there.
    Its file-relative PCM starts at 25*960 = 24000.*/
  n=ofa_encode_link(buf+used,buf_size-used,serial1,
   OFA_LINK1_FRAMES,OFA_GAP_AFTER_FRAME,&page_seq,2);
  if(n<0)return 1;
  used+=(size_t)n;
  fp=fopen(path,"wb");
  if(fp==NULL){
    fprintf(stderr,"fopen %s: %s\n",path,strerror(errno));
    return 1;
  }
  if(fwrite(buf,1,used,fp)!=used){
    fprintf(stderr,"short write\n");
    fclose(fp);
    return 1;
  }
  if(fclose(fp)!=0){
    fprintf(stderr,"fclose: %s\n",strerror(errno));
    return 1;
  }
  printf("wrote %s: %zu bytes, link0=%d frames, link1 cut after frame %d, "
   "gap=%d samples\n",path,used,OFA_LINK0_FRAMES,OFA_GAP_AFTER_FRAME,
   OFA_GAP_SAMPLES);
  printf("anchor seek: 48000 (link1 file PCM); link1 readable region ends at "
   "file PCM %lld; inflated pcm_end claims %lld\n",
   (long long)(OFA_LINK0_FRAMES*OFA_FRAME_SIZE
   +(OFA_GAP_AFTER_FRAME+1)*OFA_FRAME_SIZE),
   (long long)(OFA_LINK0_FRAMES*OFA_FRAME_SIZE
   +(OFA_GAP_AFTER_FRAME+1)*OFA_FRAME_SIZE+OFA_GAP_SAMPLES));
  free(buf);
  return 0;
}
