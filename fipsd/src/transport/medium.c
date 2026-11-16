#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/medium.h"
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#ifdef __APPLE__
#include <net/if_media.h>
#elif defined(__linux__)
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <linux/wireless.h>
#include <stdio.h>
#endif
fips_path_medium fipsd_medium_media_word(int w)
{
    switch (w & 0xe0) {
    case 0x20: return FIPS_PATH_Wired;
    case 0x80: return FIPS_PATH_Wireless;
    default: return FIPS_PATH_Unknown;
    }
}
fips_path_medium fipsd_medium_linux(unsigned type, bool wireless, bool phy,
                                   const char *devtype, bool device)
{
    if (wireless || phy || (devtype && !strcmp(devtype, "wlan"))) return FIPS_PATH_Wireless;
    return type == 1 && device && !devtype ? FIPS_PATH_Wired : FIPS_PATH_Unknown;
}
static bool valid_name(const char *s)
{ return s && *s && strlen(s) < IFNAMSIZ && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, ".."); }
static int native_ioctl(void *unused, int fd, unsigned long op, void *arg)
{ (void)unused; return ioctl(fd, op, arg); }
fips_path_medium fipsd_medium_read_ioctl(const char *name, int fd, fipsd_ioctl_fn call, void *u)
{
    if (!valid_name(name) || !call) return FIPS_PATH_Unknown;
    struct ifreq flags = {0}; strcpy(flags.ifr_name, name);
    if (call(u, fd, SIOCGIFFLAGS, &flags) < 0 || (flags.ifr_flags & IFF_LOOPBACK)) return FIPS_PATH_Unknown;
#ifdef __APPLE__
    struct ifmediareq req = {0}; strcpy(req.ifm_name, name);
    if (call(u, fd, SIOCGIFMEDIA, &req) < 0) return FIPS_PATH_Unknown;
    return fipsd_medium_media_word(req.ifm_active ? req.ifm_active : req.ifm_current);
#elif defined(__linux__)
    struct iwreq wireless = {0}; strcpy(wireless.ifr_name, name);
    if (call(u, fd, SIOCGIWNAME, &wireless) == 0) return FIPS_PATH_Wireless;
    /* ethtool confirms a physical Ethernet port; virtual or indeterminate
     * ports stay unknown. Sysfs below also covers modern nl80211-only radios. */
    struct ethtool_cmd cmd = {.cmd = ETHTOOL_GSET};
    struct ifreq req = {0}; strcpy(req.ifr_name, name); req.ifr_data = (void *)&cmd;
    if (call(u, fd, SIOCETHTOOL, &req) == 0 &&
        (cmd.port == PORT_TP || cmd.port == PORT_FIBRE || cmd.port == PORT_BNC || cmd.port == PORT_AUI))
        return FIPS_PATH_Wired;
#else
    (void)fd; (void)u;
#endif
    return FIPS_PATH_Unknown;
}
fips_path_medium fipsd_medium_read(const char *name)
{
    if (!valid_name(name)) return FIPS_PATH_Unknown;
#ifdef __linux__
    char path[256], devtype[64] = {0}, line[256]; unsigned type = 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/type", name);
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%u", &type) != 1) type = 0; fclose(f); }
    snprintf(path, sizeof path, "/sys/class/net/%s/wireless", name); bool wireless = access(path, F_OK) == 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/phy80211", name); bool phy = access(path, F_OK) == 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/device", name); bool device = access(path, F_OK) == 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/uevent", name); f = fopen(path, "r");
    if (f) { while (fgets(line, sizeof line, f)) if (sscanf(line, "DEVTYPE=%63s", devtype) == 1) break; fclose(f); }
    fips_path_medium facts = fipsd_medium_linux(type, wireless, phy, *devtype ? devtype : NULL, device);
    if (facts == FIPS_PATH_Wireless) return facts;
    /* A VLAN/bridge/veth may answer ethtool but not identify its underlying
     * medium. Do not promote it to wired. */
    if (!device || *devtype) return FIPS_PATH_Unknown;
#endif
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return FIPS_PATH_Unknown;
    fips_path_medium result = fipsd_medium_read_ioctl(name, fd, native_ioctl, NULL);
    close(fd);
#ifdef __linux__
    if (result == FIPS_PATH_Unknown) result = facts;
#endif
    return result;
}
const char *fipsd_medium_name(fips_path_medium m)
{
    switch (m) {
    case FIPS_PATH_Wired: return "wired";
    case FIPS_PATH_Wireless: return "wireless";
    case FIPS_PATH_Ble: return "ble";
    default: return "unknown";
    }
}
