#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include "bmvr_xfer.h"
int main(int argc, char **argv){
  char path[512]; snprintf(path,sizeof path,"%s/%s",argv[1],BMVR_XFER_SOCKET_NAME);
  int s=socket(AF_UNIX,SOCK_STREAM,0); struct sockaddr_un a={.sun_family=AF_UNIX}; strcpy(a.sun_path,path);
  if(connect(s,(struct sockaddr*)&a,sizeof a)<0){perror("connect");return 1;}
  struct bmvr_xfer_hello h; struct iovec iov={&h,sizeof h}; char ctl[CMSG_SPACE(3*sizeof(int))];
  struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ctl,.msg_controllen=sizeof ctl};
  ssize_t g=recvmsg(s,&m,0); int nfds=0; struct cmsghdr*c=CMSG_FIRSTHDR(&m);
  if(c) nfds=(int)((c->cmsg_len-CMSG_LEN(0))/sizeof(int));
  printf("client: hello bytes=%zd version=%u caps=%u fds=%d size=%ux%u\n",g,h.version,h.caps,nfds,h.width,h.height);
  int semframe = argc>2 && !strcmp(argv[2],"sem");
  const char *names[]={"InternalRT0","_rt_FullFrameFB","weird/name with spaces!"};
  for(int i=0;i<3;i++){
    struct bmvr_xfer_frame f; memset(&f,0,sizeof f); f.magic=BMVR_XFER_MAGIC_FRAME; f.tick=(uint32_t)(100+i); f.src_w=3840; f.src_h=2160;
    f.flags=(i==1?BMVR_XFER_FLAG_FLIPPED:0)|(semframe?BMVR_XFER_FLAG_SEMAPHORE:0); f.gl_name=42; snprintf(f.name,sizeof f.name,"%s",names[i]);
    if(send(s,&f,sizeof f,0)!=(ssize_t)sizeof f){printf("client: send failed\n");break;}
    struct bmvr_xfer_ack ack; ssize_t r=recv(s,&ack,sizeof ack,MSG_WAITALL);
    printf("client: frame %d (%s) -> ack %s\n",i,names[i],r==(ssize_t)sizeof ack&&ack.magic==BMVR_XFER_MAGIC_ACK?"yes":"NO (connection dropped)");
    if(r!=(ssize_t)sizeof ack) break;
  }
  return 0;}
