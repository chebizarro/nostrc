#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include "tun/tun.h"
#include "tun/status.h"
#include "host_os.h"
#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
/* The device is not persistent, so a clean stop leaves neither the interface
 * nor its address on any interface: the end state, whatever order the kernel
 * removed them in. Returns the number of leftovers. */
static int leftovers(const char *name,const char *address)
{
    struct in6_addr want;
    if (inet_pton(AF_INET6,address,&want)!=1) { fprintf(stderr,"TUN status address '%s' is not IPv6\n",address); return 1; }
    int left=0;
    if (if_nametoindex(name)) { fprintf(stderr,"TUN teardown left interface %s\n",name); ++left; }
    struct ifaddrs *all=NULL;
    if (getifaddrs(&all)<0) { perror("getifaddrs"); return left+1; }
    for (const struct ifaddrs *a=all;a;a=a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family!=AF_INET6) continue;
        const struct sockaddr_in6 *in6=(const struct sockaddr_in6 *)(const void *)a->ifa_addr;
        if (!memcmp(&in6->sin6_addr,&want,sizeof(want))) { fprintf(stderr,"TUN teardown left %s on %s\n",address,a->ifa_name); ++left; }
    }
    freeifaddrs(all);
    return left;
}
int main(void)
{
    if (geteuid()!=0) {
        puts("SKIP: real TUN creation/address/MTU/ping requires root (geteuid()!=0); fake ops cover ordinary tests");
        return 77;
    }
    const char *opt=getenv("FIPSD_TUN_PRIVILEGED_SMOKE");
    if (!opt || strcmp(opt,"1")) {
        puts("SKIP: privileged smoke also requires explicit FIPSD_TUN_PRIVILEGED_SMOKE=1"); return 77;
    }
    fips_secp *secp=NULL; fips_identity *id=NULL;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(),&secp)==FIPS_IDENTITY_OK);
    assert(fips_identity_generate(secp,fipsd_host_os_entropy(),&id)==FIPS_IDENTITY_OK);
    fipsd_tun_config cfg={.name="fipsSmoke0",.mtu=1280,.transport_mtu=1357};
    fipsd_tun *t=fipsd_tun_create(&cfg,NULL,NULL); assert(t);
    int started=fipsd_tun_start(t,id),ping=-1;
    fipsd_tun_status status={0};
    if (!started) {
        status=fipsd_tun_status_read(t);
        assert(status.mtu==1280 && status.state==FIPSD_TUN_UP);
        pid_t child=fork(); assert(child>=0);
        if (!child) {
#ifdef __linux__
            execlp("ping","ping","-6","-c","1","-W","2",status.address,(char *)NULL);
#else
            execl("/sbin/ping6","ping6","-c","1",status.address,(char *)NULL);
#endif
            _exit(127);
        }
        int result=0; assert(waitpid(child,&result,0)==child);
        if (WIFEXITED(result)) ping=WEXITSTATUS(result);
    } else perror("TUN bring-up");
    int stopped=fipsd_tun_stop(t);
    if (stopped) perror("TUN teardown");
    int left=started ? 0 : leftovers(status.name,status.address);
    if (!started && !stopped && fipsd_tun_status_read(t).state!=FIPSD_TUN_STOPPED) { fputs("TUN not reported stopped\n",stderr); ++left; }
    fipsd_tun_destroy(t); fips_identity_destroy(id); fips_identity_destroy_secp(secp);
    return started || ping || stopped || left ? 1 : 0;
}
