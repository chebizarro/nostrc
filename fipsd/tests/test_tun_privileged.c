#define _POSIX_C_SOURCE 200809L
#include "tun/tun.h"
#include "tun/status.h"
#include "host_os.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
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
    if (!started) {
        fipsd_tun_status status=fipsd_tun_status_read(t);
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
    fipsd_tun_destroy(t); fips_identity_destroy(id); fips_identity_destroy_secp(secp);
    return started || ping || stopped ? 1 : 0;
}
