#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
double now(){struct timespec t;clock_gettime(1,&t);return t.tv_sec+t.tv_nsec*1e-9;}
volatile unsigned K;double T;
#define P(n,m) printf("memb: %s %.3f MB/s\n",n,m/(now()-T));T=now();
#define L(n,a) for(i=0;i<131072;i++){x=x*1103515245u+12345u;a;}P(n,0.5)
int main(){unsigned *b=malloc(16<<20),*p,*e=b+(1<<19),v=7,s=0,i,x=1;char*c=b;memset(b,1,16<<20);T=now();
for(p=b;p<e;p+=8){p[0]=v;p[1]=v;p[2]=v;p[3]=v;p[4]=v;p[5]=v;p[6]=v;p[7]=v;}P("store-seq-word",2)
for(p=b;p<e;p+=8)s+=p[0]+p[1]+p[2]+p[3]+p[4]+p[5]+p[6]+p[7];K=s;P("load-seq-word",2)
for(i=0;i<1<<20;i+=4){c[i]=1;c[i+1]=2;c[i+2]=3;c[i+3]=4;}P("store-seq-byte",1)
for(i=0;i<4;i++)memcpy(c+(8<<20),c,1<<20);P("memcpy-1MB",4)
for(i=0;i<4;i++)memset(c+(8<<20),i,1<<20);P("memset-1MB",4)
for(i=0;i<256;i++)memcpy(c+(8<<20),c,8192);P("memcpy-8KB",2)
L("store-rand-64KB",b[(x>>8)&16383]=i)L("store-rand-1MB",b[(x>>8)&262143]=i)L("store-rand-16MB",b[(x>>6)&4194303]=i)
L("load-rand-16MB",s+=b[(x>>6)&4194303])L("no-memory-same-loop",s+=x>>8)K=s;printf("memb: %s\n","finished");}
