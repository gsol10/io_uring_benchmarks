#include <stdint.h>
#include <sys/types.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <string.h>
#include <bits/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ETHTOOL_BUSINFO_LEN     32

struct ethtool_drvinfo
{
  uint32_t cmd;
  char driver[32];                                /* driver short name, "tulip", "eepro100" */
  char version[32];                               /* driver version string */
  char fw_version[32];                            /* firmware version string, if applicable */
  char bus_info[ETHTOOL_BUSINFO_LEN];             /* Bus info for this IF. */
/*
 * For PCI devices, use pci_dev->slot_name.
 */
  char reserved1[32];
  char reserved2[16];
  uint32_t n_stats;                                    /* number of u64's from ETHTOOL_GSTATS */
  uint32_t testinfo_len;
  uint32_t eedump_len;                                 /* Size of data from ETHTOOL_GEEPROM (bytes) */
  uint32_t regdump_len;                                /* Size of data from ETHTOOL_GREGS (bytes) */
};

typedef char *caddr_t;

#define ETHTOOL_GDRVINFO        0x00000003 

#ifndef SIOCETHTOOL
#define SIOCETHTOOL     0x8946
#endif

void get_businfo(char *interface_name, char *bus_info) {
    struct ethtool_drvinfo drvinfo;
    struct ifreq ifr;
    int fd = socket(PF_INET, SOCK_DGRAM, 0);

    drvinfo.cmd = ETHTOOL_GDRVINFO;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, interface_name);
    ifr.ifr_data = (caddr_t) & drvinfo;
    ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    memcpy(bus_info, drvinfo.bus_info, strlen(drvinfo.bus_info));
}

int get_ifindex(char *interface_name) {
  int fd = socket(PF_INET, SOCK_DGRAM, 0);
  
  struct ifreq ifr;
  
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, interface_name);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) == 0) {
    close(fd);
    return ifr.ifr_ifindex;
  }
  close(fd);
  return -1;
}
char ** get_devices(int *nret) {
    FILE *fp = fopen("/proc/net/dev", "r");
    char * line = NULL;
    char ifname[20];
    size_t len;
    int read;
    char **ifnames;
    int nb_names;
    unsigned long int r_bytes, t_bytes, r_packets, t_packets;

    // skip first two lines
    getline(&line, &len, fp);
    getline(&line, &len, fp);

    while ((read = getline(&line, &len, fp)) != -1) {
        ifnames = realloc(ifnames, ++nb_names * sizeof(char *));
        char *start=line;
        while (*start == ' ') {
          start++;
        }
        int n = 0;
        while (start[n] != ':') {
          n++;
        }
        char * name = malloc(n + 1);
        strncpy(name, start, n);
        name[n] = '\x00';
        ifnames[nb_names - 1] = name;
        //printf("Retrieved line of length %d:\n", read);
        //printf("Name: %s\n", name);
    }
    if (line != NULL) {
      free(line);
    }
    

    *nret = nb_names;

    fclose(fp);
    return ifnames;
}

//This function returns the ifindex of given pci, or -1 if it fails.
int get_ifindex_of_pic(char *pci) {
  int n;
  char **devices = get_devices(&n);

  char bus_info[32];
  for (int i = 0; i < n; i ++) {
    memset(bus_info, '\x00', 32);
    get_businfo(devices[i], bus_info);
    if (strlen(bus_info) > 5) {
      if (strcmp(bus_info + 5, pci) == 0) { // + 5 avoid the leading "0000:"
        return get_ifindex(devices[i]);
      }
    }
  }
  return -1;
}

// int main() {
//   int n;
//   char **devices = get_devices(&n);

//   char bus_info[32];
//   for (int i = 0; i < n; i ++) {
//     memset(bus_info, '\x00', 32);
//     get_businfo(devices[i], bus_info);
//     printf("Device %d is %s, index is %d, bus is %s\n", i, devices[i], get_ifindex(devices[i]), bus_info);
//   }

  
  
//   // printf("Eth bus info : %s\n", bus_info);

//   // printf("Ifindex = %d\n", get_ifindex("enp9s0"));
// }