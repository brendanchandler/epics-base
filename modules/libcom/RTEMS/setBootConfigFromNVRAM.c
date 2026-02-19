/*************************************************************************\
* Copyright (c) 2008 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef RTEMS_LEGACY_STACK
#include <rtems/rtems_bsdnet.h>
#else
#include <stdio.h>
#include <net/if.h>
#include <sysexits.h>
#include <rtems/bsd/bsd.h>
#endif
#include <bsp.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <epicsStdlib.h>
#include <epicsStdio.h>
#include <epicsString.h>
#include <envDefs.h>

char *env_nfsServer;
char *env_nfsPath;
char *env_nfsMountPoint;

extern char *nvram_dhcpena;
extern char *nvram_if ;
extern char *nvram_ip ;
extern char *nvram_netmask ;
extern char *nvram_gateway;
extern char *nvram_bootserver; /* TFTP/boot server IP, also used as DNS/NTP fallback */
extern char *nvram_ntpserver;
extern char *nvram_epics_script;
extern char *nvram_hostname;
extern char *nvram_boot_file_name;

/*
 * Split argument string of form nfs_server:nfs_export:<path>
 * The nfs_export component will be used as:
 *      - the path to the directory exported from the NFS server
 *      - the local mount point
 *      - a prefix of <path>
 * For example, the argument string:
 *       romeo:/export/users:smith/ioc/iocexample/st.cmd
 * would:
 *       - mount /export/users from NFS server romeo on /export/users
 *       - chdir to /export/users/smith/ioc/iocexample
 *       - read commands from st.cmd
 */
static void
splitRtemsBootpCmdline(char * Cmdline)
{
    char *cp1, *cp2, *cp3;

    if ((cp1 = Cmdline) == NULL)
        return;
    if (((cp2 = strchr(cp1, ':')) != NULL)
     && (((cp3 = strchr(cp2+1, ' ')) != NULL)
      || ((cp3 = strchr(cp2+1, ':')) != NULL))) {
        int l1 = cp2 - cp1;
        int l2 = cp3 - cp2 - 1;
        int l3 = strlen(cp3) - 1;
        if (l1 && l2 && l3) {
            *cp2++ = '\0';
            *cp3 = '\0';
            env_nfsServer = cp1;
            env_nfsMountPoint = env_nfsPath = epicsStrDup(cp2);
            *cp3 = '/';
            Cmdline = cp2;
        }
    }
}

/*
 * Split NFS mount information of the form nfs_server:host_path:local_path
 */
static void
splitNfsMountPath(char *nfsString)
{
    char *cp2, *cp3;

    if (nfsString == NULL)
        return;
    if (((cp2 = strchr(nfsString, ':')) != NULL)
     && (((cp3 = strchr(cp2+1, ' ')) != NULL)
      || ((cp3 = strchr(cp2+1, ':')) != NULL))) {
        int l1 = cp2 - nfsString;
        int l2 = cp3 - cp2 - 1;
        int l3 = strlen(cp3) - 1;
        if (l1 && l2 && l3) {
            *cp2++ = '\0';
            *cp3++ = '\0';
            env_nfsServer = nfsString;
            env_nfsPath = cp2;
            env_nfsMountPoint = cp3;
        }
    }
}

#if defined(HAVE_MOTLOAD)

/*
 * Motorola MOTLOAD NVRAM Access
 */
static char *
gev(const char *parm, volatile char *nvp)
{
    const char *val;
    const char *name;
    char *ret;
    char c;

    for (;;) {
        if (*nvp == '\0')
            return NULL;
        name = parm;
        while ((c = *nvp++) != '\0') {
            if ((c == '=') && (*name == '\0')) {
                val = (char *)nvp;
                while (*nvp++ != '\0')
                    continue;
                ret = malloc(nvp - val);
                if (ret == NULL)
                    return NULL;
                strcpy(ret, val);
                return ret;
            }
            if (c != *name++) {
                while (*nvp++ != '\0')
                    continue;
                break;
            }
        }
    }
}

static char *
motScriptParm(const char *mot_script_boot, char parm)
{
    const char *cp;
    char *ret;
    int l;

    while (*mot_script_boot != '\0') {
        if (isspace(*(unsigned char *)mot_script_boot)
         && (*(mot_script_boot+1) == '-')
         && (*(mot_script_boot+2) == parm)) {
            mot_script_boot += 3;
            cp = mot_script_boot;
            while ((*mot_script_boot != '\0') &&
                   !isspace(*(unsigned char *)mot_script_boot))
                mot_script_boot++;
            l = mot_script_boot - cp;
            ret = malloc(l+1);
            if (ret == NULL)
                return NULL;
            strncpy(ret, cp, l);
            *(ret+l) = '\0';
            return ret;
         }
        mot_script_boot++;
    }
    return NULL;
}

int
setBootConfigFromNVRAM()
{
    const char *mot_script_boot;
    volatile char *nvp;
    char *cp;

# if defined(BSP_NVRAM_BASE_ADDR)
    nvp = (volatile char *)(BSP_NVRAM_BASE_ADDR+0x70f8);
# elif defined(BSP_I2C_VPD_EEPROM_DEV_NAME)
    char gev_buf[3592];
    int fd;
    if ((fd = open(BSP_I2C_VPD_EEPROM_DEV_NAME, 0)) < 0) {
        printf("Can't open %s: %s\n", BSP_I2C_VPD_EEPROM_DEV_NAME, strerror(errno));
        return -1;
    }
    lseek(fd, 0x10f8, SEEK_SET);
    if (read(fd, gev_buf, sizeof gev_buf) != sizeof gev_buf) {
        printf("Can't read %s: %s\n", BSP_I2C_VPD_EEPROM_DEV_NAME, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    nvp = gev_buf;
# else
#  error "No way to read GEV!"
# endif

    mot_script_boot = gev("mot-script-boot", nvp);

#ifdef RTEMS_LEGACY_STACK
    if (rtems_bsdnet_config.bootp != NULL)
        return -1;

    if ((rtems_bsdnet_bootp_server_name = gev("mot-/dev/enet0-sipa", nvp)) == NULL)
        rtems_bsdnet_bootp_server_name = motScriptParm(mot_script_boot, 's');
    if ((rtems_bsdnet_config.gateway = gev("mot-/dev/enet0-gipa", nvp)) == NULL)
        rtems_bsdnet_config.gateway = motScriptParm(mot_script_boot, 'g');
    if  ((rtems_bsdnet_config.ifconfig->ip_netmask = gev("mot-/dev/enet0-snma", nvp)) == NULL)
        rtems_bsdnet_config.ifconfig->ip_netmask = motScriptParm(mot_script_boot, 'm');

    rtems_bsdnet_config.name_server[0] = gev("rtems-dns-server", nvp);
    if (rtems_bsdnet_config.name_server[0] == NULL)
        rtems_bsdnet_config.name_server[0] = rtems_bsdnet_bootp_server_name;
    cp = gev("rtems-dns-domainname", nvp);
    if (cp)
        rtems_bsdnet_config.domainname = cp;

    if ((rtems_bsdnet_config.ifconfig->ip_address = gev("mot-/dev/enet0-cipa", nvp)) == NULL)
        rtems_bsdnet_config.ifconfig->ip_address = motScriptParm(mot_script_boot, 'c');
    rtems_bsdnet_config.hostname = gev("rtems-client-name", nvp);
    if (rtems_bsdnet_config.hostname == NULL)
        rtems_bsdnet_config.hostname = rtems_bsdnet_config.ifconfig->ip_address;

    if ((rtems_bsdnet_bootp_boot_file_name = gev("mot-/dev/enet0-file", nvp)) == NULL)
        rtems_bsdnet_bootp_boot_file_name = motScriptParm(mot_script_boot, 'f');
    rtems_bsdnet_bootp_cmdline = gev("epics-script", nvp);
    splitRtemsBootpCmdline(rtems_bsdnet_bootp_cmdline);
    splitNfsMountPath(gev("epics-nfsmount", nvp));
    rtems_bsdnet_config.ntp_server[0] = gev("epics-ntpserver", nvp);
    if (rtems_bsdnet_config.ntp_server[0] == NULL)
        rtems_bsdnet_config.ntp_server[0] = rtems_bsdnet_bootp_server_name;
    if ((cp = gev("epics-tz", nvp)) != NULL)
        epicsEnvSet("TZ", cp);
#else
    /*
     * Read network parameters from NVRAM
     */
    if ((nvram_dhcpena = gev("dhcp_ena",nvp)) == NULL){
        nvram_dhcpena = "yes";
    }

    if ((nvram_if = gev("bootif",nvp)) == NULL){
        char ifnamebuf[IF_NAMESIZE];
        /* Assumes loopback interface is already brought up on
         * index 0, and index 1 is the first hardware device. */
        nvram_if = if_indextoname(1, ifnamebuf);
        if (nvram_if == NULL) {
            printf("No network interface found\n");
        }
    }
    if ((nvram_ip = gev("ipaddr", nvp)) == NULL) {
        if ((nvram_ip = gev("mot-/dev/enet0-cipa", nvp)) == NULL) {
            nvram_ip = motScriptParm(mot_script_boot, 'c');
        }
    }

    if ((nvram_netmask = gev("netmask", nvp)) == NULL) {
        if ((nvram_netmask = gev("mot-/dev/enet0-snma", nvp)) == NULL) {
            nvram_netmask = motScriptParm(mot_script_boot, 'm');
        }
    }

    if ((nvram_gateway = gev("gatewayip", nvp)) == NULL) {
        if ((nvram_gateway = gev("mot-/dev/enet0-gipa", nvp)) == NULL) {
            nvram_gateway = motScriptParm(mot_script_boot, 'g');
        }
    }
    
    if ((nvram_bootserver = gev("bootserverip", nvp)) == NULL) {
        if ((nvram_bootserver = gev("mot-/dev/enet0-sipa", nvp)) == NULL) {
            nvram_bootserver = motScriptParm(mot_script_boot, 's');
        }
    }

    if ((nvram_boot_file_name = gev("bootfile", nvp)) == NULL) {
        if ((nvram_boot_file_name = gev("mot-/dev/enet0-file", nvp)) == NULL) {
            nvram_boot_file_name = motScriptParm(mot_script_boot, 'f');
	}
    }

    if ((nvram_ntpserver = gev("epics-ntpserver", nvp)) == NULL) {
        nvram_ntpserver = nvram_bootserver;
    }

    nvram_epics_script = gev("epics-script",nvp);

    if ((nvram_hostname = gev("hostname",nvp)) == NULL) {
        nvram_hostname = gev("rtems-client-name", nvp);
    }

    splitRtemsBootpCmdline(nvram_epics_script);
    splitNfsMountPath(gev("epics-nfsmount", nvp));

    if ((cp = gev("epics-tz", nvp)) != NULL)
        epicsEnvSet("TZ", cp);
#endif
    return 0;
}

#elif defined(HAVE_PPCBUG)
/*
 * Motorola PPCBUG NVRAM Access
 */
struct ppcbug_nvram {
    uint32_t    PacketVersionIdentifier;
    uint32_t    NodeControlMemoryAddress;
    uint32_t    BootFileLoadAddress;
    uint32_t    BootFileExecutionAddress;
    uint32_t    BootFileExecutionDelay;
    uint32_t    BootFileLength;
    uint32_t    BootFileByteOffset;
    uint32_t    TraceBufferAddress;
    uint32_t    ClientIPAddress;
    uint32_t    ServerIPAddress;
    uint32_t    SubnetIPAddressMask;
    uint32_t    BroadcastIPAddressMask;
    uint32_t    GatewayIPAddress;
    uint8_t     BootpRarpRetry;
    uint8_t     TftpRarpRetry;
    uint8_t     BootpRarpControl;
    uint8_t     UpdateControl;
    char                BootFilenameString[64];
    char                ArgumentFilenameString[64];
};

static char *addr(char *cbuf, uint32_t addr)
{
    struct in_addr a;
    if ((a.s_addr = addr) == 0)
        return NULL;
    return (char *)inet_ntop(AF_INET, &a, cbuf, INET_ADDRSTRLEN);
}

int
setBootConfigFromNVRAM()
{
#ifdef RTEMS_LEGACY_STACK
    static struct ppcbug_nvram nvram;
    static char ip_address[INET_ADDRSTRLEN];
    static char ip_netmask[INET_ADDRSTRLEN];
    static char server[INET_ADDRSTRLEN];
    static char gateway[INET_ADDRSTRLEN];

    if (rtems_bsdnet_config.bootp != NULL)
        return 0;

    /*
     * Get network configuration from PPCBUG.
     * The 'correct' way to do this would be to issue a .NETCFIG PPCBUG
     * system call.  Unfortunately it is very difficult to issue such a
     * call once RTEMS is up and running so we just copy from the 'known'
     * location of the network configuration parameters.
     * Care must be taken to access the NVRAM a byte at a time.
     */

#if defined(NVRAM_INDIRECT)
   {
      volatile char *addrLo = (volatile char *)0x80000074;
      volatile char *addrHi = (volatile char *)0x80000075;
      volatile char *data = (volatile char *)0x80000077;
      int addr =  0x1000;
      char *d = (char *)&nvram;

      while (d < ((char *)&nvram + sizeof nvram)) {
         *addrLo = addr & 0xFF;
         *addrHi = (addr >> 8) & 0xFF;
         *d++ = *data;
         addr++;
      }
   }
#else
    {
    volatile char *s = (volatile char *)0xFFE81000;
    char *d = (char *)&nvram;

    while (d < ((char *)&nvram + sizeof nvram))
        *d++ = *s++;
    }
#endif
    /*
     * Assume that the boot server is also the name, log and ntp server!
     */
    rtems_bsdnet_config.name_server[0] =
    rtems_bsdnet_config.ntp_server[0]  =
      rtems_bsdnet_bootp_server_name   = addr(server, nvram.ServerIPAddress);
    rtems_bsdnet_bootp_server_address.s_addr = nvram.ServerIPAddress;
    /*
     * Nothing better to use as host name!
     */
    rtems_bsdnet_config.ifconfig->ip_address =
      rtems_bsdnet_config.hostname = addr(ip_address, nvram.ClientIPAddress);

    rtems_bsdnet_config.gateway = addr(gateway, nvram.GatewayIPAddress);
    rtems_bsdnet_config.ifconfig->ip_netmask = addr(ip_netmask, nvram.SubnetIPAddressMask);

    rtems_bsdnet_bootp_boot_file_name = nvram.BootFilenameString;
    rtems_bsdnet_bootp_cmdline = nvram.ArgumentFilenameString;
    splitRtemsBootpCmdline(rtems_bsdnet_bootp_cmdline);
    return 0;
#else
    /* TODO: Implement NVRAM boot configuration for libbsd if needed */
    return -1;
#endif
}

#elif defined(__mcf528x__)

static char *
env(const char *parm, const char *defaultValue)
{
    const char *cp = bsp_getbenv(parm);

    if (!cp) {
        if (!defaultValue)
            return NULL;
        cp = defaultValue;
        printf ("%s environment variable missing -- using %s.\n", parm, cp);
    }
    return epicsStrDup(cp);
}

int
setBootConfigFromNVRAM()
{
#ifdef RTEMS_LEGACY_STACK
    const char *cp1;

    if (rtems_bsdnet_config.bootp != NULL)
        return 0;
    rtems_bsdnet_config.gateway = env("GATEWAY", NULL);
    rtems_bsdnet_config.ifconfig->ip_netmask = env("NETMASK", "255.255.252.0");

    rtems_bsdnet_bootp_server_name = env("SERVER", "192.168.0.1");
    rtems_bsdnet_config.name_server[0] = env("NAMESERVER", rtems_bsdnet_bootp_server_name);
    rtems_bsdnet_config.ntp_server[0] = env("NTPSERVER", rtems_bsdnet_bootp_server_name);
    cp1 = env("DOMAIN", NULL);
    if (cp1 != NULL)
        rtems_bsdnet_config.domainname = cp1;
    rtems_bsdnet_config.hostname = env("HOSTNAME", "iocNobody");
    rtems_bsdnet_config.ifconfig->ip_address = env("IPADDR0", "192.168.0.2");
    rtems_bsdnet_bootp_boot_file_name = env("BOOTFILE", "uC5282App.boot");
    rtems_bsdnet_bootp_cmdline = env("CMDLINE", "epics/iocBoot/iocNobody/st.cmd");
    splitNfsMountPath(env("NFSMOUNT", NULL));
    if ((cp1 = env("TZ", NULL)) != NULL)
        epicsEnvSet("TZ", cp1);
    return 0;
#else
    /* TODO: Implement NVRAM boot configuration for libbsd if needed */
    return -1;
#endif
}

#elif defined(HAS_UBOOT)

int
setBootConfigFromNVRAM(void){
    char *cp;

    if ((nvram_dhcpena = bsp_uboot_getenv("dhcp_ena")) == NULL){
        nvram_dhcpena = "yes";
    }

    nvram_if = bsp_uboot_getenv("bootif");
    if ((nvram_if = bsp_uboot_getenv("bootif")) == NULL){
    	nvram_if = "tsec0";
    }

    nvram_ip = bsp_uboot_getenv("ipaddr");

    nvram_netmask = bsp_uboot_getenv("netmask");

    nvram_gateway = bsp_uboot_getenv("gatwayip");

    nvram_bootserver = bsp_uboot_getenv("bootserverip");

    nvram_ntpserver = bsp_uboot_getenv("epics-ntpserver");

    nvram_epics_script = bsp_uboot_getenv("epics-script");

    nvram_hostname = bsp_uboot_getenv("hostname");

    nvram_boot_file_name = bsp_uboot_getenv("bootfile");

    splitRtemsBootpCmdline(nvram_epics_script);
    splitNfsMountPath(bsp_uboot_getenv("epics-nfsmount"));
    if ((cp = bsp_uboot_getenv("epics-tz")) != NULL)
        epicsEnvSet("TZ", cp);

    return 0;
}

#else
/*
 * Placeholder for systems without NVRAM
 */
int
setBootConfigFromNVRAM()
{
    printf("SYSTEM HAS NO NON-VOLATILE RAM!\n");
    printf("YOU MUST USE SOME OTHER METHOD TO OBTAIN NETWORK CONFIGURATION\n");
    return -1;
}
#endif
