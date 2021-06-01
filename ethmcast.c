/*
 * DISCLAIMER
 * THE USE OF THIS CODE IS UNLIMITED PROVIDED THAT YOU DO NOT HOLD THE AUTHOR
 * OF THIS CODE OR ANY PART OF IT RESPONSIBLE FOR ANYTHING
 * I.E USE IT AS YOU WISH BUT DO OT BLAME FOR FOR ANYTHING

 * I got the code from
 * https://gist.github.com/lethean/5fb0f493a1968939f2f7
 *
 * And I made some modifications
 * ============================
 * - Objctive of the modifications
 *   Ethernet code to be able to send and receive ethernet frames using
 *   both unicast and multicast
 *   Also helps testing subscribe/unsubscribe from ethernet multicast as
 *   well as using socket and ioctl to filter on certain ethertype alues
 * - No longer set the interface to promiscous mode
 * - Add support to listen on multicast addresses
 *   In this case, the listener subscribes to the two multicast addresses
 *   stored in the variables "mcast_addr1" and "mcast_addr2"
 * - Added a singal hanlder to catch CTrL^C to cleanly unsbscribe from the
 *   multicast addresses
 *
 * How to build
 *   gcc -o ethmcast ethmcast.c -Wall
 * 
 * How to use with Multicast 
 * =============
 * - Make sure that the two machines are directly connected over ethernet
 * - Make sure that the interfaces on both machines or VMs are up using 
 *   ip link show <I/F>
 * - On the listener side, do
 *    sudo ./ethmcast -l -m -i enp0s8
 * - On the sender side
 *   - TO send to one of the multicast addresses
 *      sudo ./ethmcast -d 01:00:5e:00:00:10 hello
 *   - To send unicast,
 *     - Find out the MAC address of the destination machine using a command like 
 *       "ip link show <I/F>" or "ifconfig -a enp0s8" on the destination machine
 *     - Put the MAC address after the "-d" option
 *       Examples
 *        sudo ./ethmcast -i enp0s8 -d 01:00:5e:00:00:02
 *        sudo ./ethmcast -i enp0s8 -d 08:00:27:d4:12:02
 *        sudo ./ethmcast -i enp0s8 -d 08:00:27:00:56:ca
 *        sudo ./ethmcast -i enp0s8 -d ff:ff:ff:ff:ff:ff
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <unistd.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>

#define COLOR_NORMAL   "\x1B[0m"
#define COLOR_RED   "\x1B[31m"
#define COLOR_GREEN   "\x1B[32m"
#define COLOR_YELLOW   "\x1B[33m"
#define COLOR_BLUE   "\x1B[34m"
#define COLOR_MAGENTA   "\x1B[35m"
#define COLOR_CYAN   "\x1B[36m"
#define COLOR_WIHTE   "\x1B[37m"
#define COLOR_RESET "\x1B[0m"


#define ETHER_TYPE (0x80ab) /* custom type */

#define BUF_SIZE (ETH_FRAME_LEN)

static char broadcast_addr[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static char mcast_addr1[ETH_ALEN] = { 0x1, 0x0, 0x5e, 0x0, 0x0, 0x10 };
static char mcast_addr2[ETH_ALEN] = { 0x1, 0x0, 0x5e, 0x0, 0x0, 0x20 };

/* Need these to be public so that the signal handler can use them */
static int if_index = -1;
static  char *if_name;
static  int sock = -1;
static  int is_listen;
static  bool is_listen_multicast = false;


/* Macro to print error */
#define PRINT_ERR(str, param...)                                        \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);
#define PRINT_INFO(str, param...)                                       \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);
#define PRINT_DEBUG(str, param...)                                      \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);


/* Print error in red color */
__attribute__ ((format (printf, 1, 2), unused))
static void print_error(char *string, ...) 
{
  va_list args;
  va_start(args, string);
  fprintf(stderr, COLOR_RED);
  vfprintf(stderr, string, args);
  fprintf(stderr, COLOR_RESET);
  va_end(args);
}

/* Print error in info or debug in normal color */
__attribute__ ((format (printf, 1, 2), unused))
static void print_debug(char *string, ...)
{
  va_list args;
  va_start(args, string);
  fprintf(stdout, COLOR_RESET);
  vfprintf(stdout, string, args);
  va_end(args);
}

/* return a printable string corresponding to the MAC address */
#define PRINT_BUF_SIZE (256)
#define NUM_PRINT_BUFS 32
static char *
print_mac(char *mac)
{
  static __thread char buflist[NUM_PRINT_BUFS][PRINT_BUF_SIZE + 1];
  static uint32_t counter;
  char *buf = &buflist[counter++][0];
  snprintf(buf, PRINT_BUF_SIZE,"%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
  return (buf);
}


static void
print_usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s [-l] [-m] [-e <ethertype>] [-i <device>] [-d <dest-addr>] msg\n"
           "\t-l Listen mode (Wait for frames to be received)\n"
           "\t-e <ethertype>: Set ethertype to <ethertype>, default %04x\n" 
           "\t-m Subscribe to Mcast addresses: "
           "%02x:%02x:%02x:%02x:%02x:%02x and %02x:%02x:%02x:%02x:%02x:%02x\n"

           "\t-i <device>  Send/Receive to/from interface '<device>'\n"
           "\t-d dest-addr sent ethernet destiantion addr to <dest-addr>\n",
           progname,
           ETHER_TYPE,
           mcast_addr1[0],
           mcast_addr1[1],
           mcast_addr1[2],
           mcast_addr1[3],
           mcast_addr1[4],
           mcast_addr1[5],
           mcast_addr2[0],
           mcast_addr2[1],
           mcast_addr2[2],
           mcast_addr2[3],
           mcast_addr2[4],
           mcast_addr2[5]);
}


/* CTRL^C handler */
static void
signal_handler(int s)
{
  struct packet_mreq mreq;
  PRINT_DEBUG("Caught signal %d\n",s);
  if (!is_listen || !is_listen_multicast) {
    close (sock);
    exit(EXIT_SUCCESS); 
  }

  PRINT_DEBUG( "Going to UNsubscribe from %s and %s on %s(%d)\n",
               print_mac(mcast_addr1), print_mac(mcast_addr2), if_name, if_index);
  /* Setup the structure for setsockopt to subscribe to mcast address */
  memset(&mreq, 0, sizeof(struct packet_mreq));
  mreq.mr_ifindex = if_index;
  mreq.mr_type = PACKET_MR_MULTICAST;
  mreq.mr_alen = ETH_ALEN;
  /* UNsubscribe to the first mcast address */
  memcpy(mreq.mr_address, mcast_addr1, ETH_ALEN);
  if (setsockopt (sock, SOL_PACKET, PACKET_DROP_MEMBERSHIP,
                  &mreq, sizeof(mreq)) < 0) {
    PRINT_ERR("\nCAnnot UNsubscribe from mcast address %s on %s(%d): %d %s\n",
              print_mac(mcast_addr1), if_name, if_index, errno, strerror(errno));
    close(sock);
    exit (EXIT_FAILURE);
  }
  /* UNsubscribe to the second mcast address */
  memcpy(mreq.mr_address, mcast_addr2, ETH_ALEN);
  if (setsockopt (sock, SOL_PACKET, PACKET_DROP_MEMBERSHIP,
                  &mreq, sizeof(mreq)) < 0) {
    PRINT_ERR("\nCannot UNsubscribe from  mcast address %s on %s(%d): %d %s\n",
              print_mac(mcast_addr2), if_name, if_index, errno, strerror(errno));
    close (sock);
    exit (EXIT_FAILURE);
  }
  close (sock);

  fprintf(stdout,"Successfully unsubscribed from %s and %s on %s(%d)\n",
    print_mac(mcast_addr1), print_mac(mcast_addr2), if_name, if_index);
  exit(EXIT_SUCCESS); 
}


int
main (int    argc,
      char **argv)
{
  char *msg;
  uint8_t if_addr[ETH_ALEN];
  uint8_t dest_addr[ETH_ALEN];
  size_t send_len;
  char buf[BUF_SIZE];
  int i;
  uint32_t ether_type = ETHER_TYPE;

  if_name = "eth0";
  memcpy (dest_addr, broadcast_addr, ETH_ALEN);
  is_listen = 0;
  msg = "Hello";
  
  int opt;
  
  while ((opt = getopt (argc, argv, "mli:d:e:")) != -1) {
    switch (opt)
      {
      case 'l':
        is_listen = 1;
        break;
      case 'i':
        if_name = optarg;
        break;
      case 'm':
        is_listen_multicast = true;
        break;
      case 'e':
        if (1 != sscanf(optarg, "%02x", &ether_type)) {
          PRINT_ERR("\nCannot read ethertype %s. "
                    "Must be in the form 0xnnnn \n", optarg);
          exit (EXIT_FAILURE);
        }
        if (ether_type > 0xffff || ether_type < 0x0600) {
          PRINT_ERR("\nInvalid ethertype %s. "
                    "Must be between 0x600 and 0xffff\n", optarg);
          exit (EXIT_FAILURE);
        }
      case 'd':
        {
          int mac[ETH_ALEN];
          
          if (ETH_ALEN != sscanf (optarg,
                                  "%02x:%02x:%02x:%02x:%02x:%02x",
                                  &mac[0],
                                  &mac[1],
                                  &mac[2],
                                  &mac[3],
                                  &mac[4],
                                  &mac[5]))
            {
              print_usage (argv[0]);
              return EXIT_FAILURE;
            }
          for (i = 0; i < ETH_ALEN; i++) {
              dest_addr[i] = mac[i];
          }
        }
        break;
      default: /* '?' */
        print_usage (argv[0]);
        return EXIT_FAILURE;
      }
  }
  
  if (optind < argc)
    msg = argv[optind];


  /* Setup handler for CTRL^C */
  struct sigaction sigIntHandler;
  
  sigIntHandler.sa_handler = signal_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;  
  if (sigaction(SIGINT, &sigIntHandler, NULL) != 0) {
    PRINT_ERR("Cannot setup signal handler: %d %s\n",
              errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
              

  

  /* Create the AF_PACKET socket. */
  sock = socket (AF_PACKET, SOCK_RAW, htons(ether_type));
  if (sock < 0) {
    perror ("socket()");
    exit(EXIT_FAILURE);
  }
  
  /* Get the index number and MAC address of ethernet interface. */
  
  struct ifreq ifr;

  memset (&ifr, 0, sizeof (ifr));
  strncpy (ifr.ifr_name, if_name, IFNAMSIZ - 1);

  if (ioctl (sock, SIOCGIFINDEX, &ifr) < 0) {
    PRINT_ERR("SIOCGIFINDEX %s %s\n",
                ifr.ifr_name, strerror(errno));
    perror("SIOCGIFINDEX");
    exit(EXIT_FAILURE);
  }
  if_index = ifr.ifr_ifindex;

  if (ioctl (sock, SIOCGIFHWADDR, &ifr) < 0) {
    PRINT_ERR("SIOCGIFHWADDR on %s (%d): %s", if_name, if_index, strerror(errno));
    exit(EXIT_FAILURE);
  }
  memcpy (if_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
  
  /************* Section to handle the case where We are receive **************/
  if (is_listen)  {
    struct ifreq ifr;
    int s;
    
    memset (&ifr, 0, sizeof (ifr));
    strncpy (ifr.ifr_name, if_name, IFNAMSIZ - 1);

    /* Set interface to promiscuous mode. */
    /*if (ioctl (sock, SIOCGIFFLAGS, &ifr) < 0) {
      perror ("SIOCGIFFLAGS");
      exit(EXIT_FAILURE);
    }
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl (sock, SIOCSIFFLAGS, &ifr) < 0) {
      perror ("SIOCSIFFLAGS");
      exit(EXIT_FAILURE);
      }*/

    /* Allow the socket to be reused. */
    s = 1;
    if (setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &s, sizeof (s)) < 0)  {
      perror ("SO_REUSEADDR");
      close (sock);
      return EXIT_FAILURE;
    }

    /* Bind to device. */
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, if_name, IFNAMSIZ - 1)
        < 0) {
      perror ("SO_BINDTODEVICE");
      close (sock);
      return EXIT_FAILURE;
    }

    /* Bypass the qdisc layer in kernel to maximize transmission performance
     * Remember that the "lebvel" argument MUST be SOL_PACKET for this option
     * see "man -s7 packet"
     */ 
    s = 1;
    if (setsockopt (sock, SOL_PACKET, PACKET_QDISC_BYPASS, &s, sizeof (s)) < 0) {
      PRINT_ERR("CANNOT PACKET_QDISC_BYPASS in %s(%d) %d %s\n",
                if_name, if_index, errno, strerror(errno));
      perror ("PACKET_QDISC_BYPASS");
      close (sock);
      return EXIT_FAILURE;
    }
    
    
    /* If multicast is enabled, subscribe to both groups */
    if (is_listen_multicast) {
      struct packet_mreq mreq;
      PRINT_DEBUG( "Going to subscribe to %s and %s\n",
                   print_mac(mcast_addr1), print_mac(mcast_addr2));
      /* Setup the structure for setsockopt to subscribe to mcast address */
      memset(&mreq, 0, sizeof(struct packet_mreq));
      mreq.mr_ifindex = if_index;
      mreq.mr_type = PACKET_MR_MULTICAST;
      mreq.mr_alen = ETH_ALEN;
      /* subscribe to the first mcast address */
      memcpy(mreq.mr_address, mcast_addr1, ETH_ALEN);
      if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                      &mreq, sizeof(mreq)) < 0) {
        PRINT_ERR("\nCAnnot subscribe to mcast address %s: %d %s\n",
                  print_mac(mcast_addr1), errno, strerror(errno));
        close (sock);
        return EXIT_FAILURE;
      }
      /* subscribe to the second mcast address */
      memcpy(mreq.mr_address, mcast_addr2, ETH_ALEN);
      if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                      &mreq, sizeof(mreq)) < 0) {
        PRINT_ERR("\nCAnnot subscribe to mcast address %s: %d %s\n",
                  print_mac(mcast_addr2), errno, strerror(errno));
        close (sock);
        return EXIT_FAILURE;
      }
    }
    
    while (1) {
      struct ether_header *eh = (struct ether_header *) buf;
      ssize_t received;
      char *p;
      
      received = recvfrom (sock, buf, BUF_SIZE, 0, NULL, NULL);
      if (received <= 0)
        break;
      
      /* Receive only destination address is broadcast or me. */
      if (memcmp(eh->ether_dhost, if_addr, ETH_ALEN) != 0 &&
          memcmp(eh->ether_dhost, broadcast_addr, ETH_ALEN) != 0 &&
          memcmp(eh->ether_dhost, mcast_addr1, ETH_ALEN) != 0 &&
          memcmp(eh->ether_dhost, mcast_addr2, ETH_ALEN) != 0) {
        fprintf (stdout,
                 "UNEXPECTED %s -> %s on %s\n",
                 print_mac((char *)eh->ether_shost),
                 print_mac((char *)eh->ether_dhost),
                 if_name);
        continue;                 
      }

      
      fprintf (stdout,
               "%02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x ",
               eh->ether_shost[0],
                   eh->ether_shost[1],
               eh->ether_shost[2],
               eh->ether_shost[3],
               eh->ether_shost[4],
               eh->ether_shost[5],
               eh->ether_dhost[0],
               eh->ether_dhost[1],
               eh->ether_dhost[2],
               eh->ether_dhost[3],
               eh->ether_dhost[4],
               eh->ether_dhost[5]);
      
      received -= sizeof (*eh);
      p = buf + sizeof (*eh);
      for (i = 0; i < received; i++)
        fputc (p[i], stdout);
      
      fputc ('\n', stdout);
    }
    
    close (sock);
    
    return 0;
  } /* if (is_listen)  { */


  /***************Section to handle the case where We are sender **************/

  
  memset (buf, 0, BUF_SIZE);
  
  /* Construct ehternet header. */
  struct ether_header *eh;
  
  /* Ethernet header */
  eh = (struct ether_header *) buf;
  memcpy (eh->ether_shost, if_addr, ETH_ALEN);
  memcpy (eh->ether_dhost, dest_addr, ETH_ALEN);
  eh->ether_type = htons (ether_type);
  
  send_len = sizeof (*eh);
  
  
  /* Fill the packet data. */
  for (i = 0; msg[i] != '\0'; i++) {
    buf[send_len++] = msg[i];
  }
  
  /* Fill the destination address and send it. */
  
  struct sockaddr_ll sock_addr;

  sock_addr.sll_ifindex = if_index;
  sock_addr.sll_halen = ETH_ALEN;
  memcpy (sock_addr.sll_addr, dest_addr, ETH_ALEN);

  if (sendto (sock, buf, send_len, 0,
              (struct sockaddr *) &sock_addr, sizeof (sock_addr)) < 0) {
    perror ("sendto()");
  }
  
  close (sock);
  
  return 0;
}


 

            
