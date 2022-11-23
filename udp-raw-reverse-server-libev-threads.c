#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <ev.h>

#define PORT         3333
#define BUFF_SIZE    4096
#define REVERSE_PORT 10002
#define MAX_LISTEN   7

int ip_hlen = sizeof(struct iphdr);
int udp_hlen = sizeof(struct udphdr);

char *in_iface = "lo";
char *out_iface = "lo";


// Get IP & port for sock id
void get_ip_port(int sid, char *s) {
    int r;
    struct sockaddr_in addr;
    socklen_t size;

    size = sizeof(struct sockaddr_in);
    r = getpeername(sid, (struct sockaddr *)&addr, &size);
    sprintf(s, "%s:%u", inet_ntoa(addr.sin_addr), addr.sin_port);
}

// send message to reverse server (backend) and receive answer
int tcp_client(char *msg, char *buff) {
    const char* hostname = "localhost";
    int port = REVERSE_PORT;
    struct sockaddr_in addr;
    int sock;
    int n = 0, len = 0, maxlen = 1024;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, hostname, &addr.sin_addr);
    addr.sin_port = htons(port);
    if( (sock = socket(PF_INET, SOCK_STREAM, 0)) < 0 ) 
        { printf("(Thread 1) could not create socket\n"); return 1; }
    if( connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 ) 
        { printf("(Thread 1) could not connect to server\n"); return 1; }
    send(sock, msg, strlen(msg), 0);
    while((n = recv(sock, buff, maxlen, 0)) > 0) {
        buff += n;
        maxlen -= n;
        len += n;
        buff[len] = '\0';
    }
    close(sock);
    return 0;
}

// Function for read callback (echo)
void read_cb_echo(struct ev_loop *loop, struct ev_io *watcher, int event) {
    char buff[BUFF_SIZE] = {0};
    char rev[BUFF_SIZE] = {0};
    char ip[32];
    int r = 0;
    int32_t n;

    if( event & EV_ERROR ) { printf("Forbidden event: %d\n", event); return; }
    n = read(watcher->fd, buff, sizeof(buff));
    if( n == -1 ) { if( EINTR != errno && EAGAIN != errno ) r = 1; } // Error read TCP
    else { if( n == 0 ) r = 2; } // TCP Close
    get_ip_port(watcher->fd, ip);
    if( r != 0) {
        printf("(Thread 2) TCP %s CLOSE.\n", ip);
        ev_io_stop(loop, watcher);
        free(watcher);
    }
    else {
        printf("(Thread 2) READ from client %s: %s\n", ip, buff);
        printf("(Thread 2) SEND to local main thread (for reverse).\n");
        tcp_client(buff, rev);
        printf("(Thread 2) SEND to client %s: %s\n", ip, rev);
        n = write(watcher->fd, rev, sizeof(rev)); // send reverse
    }
}

// Function for accept callback (echo)
void accept_cb_echo(struct ev_loop *loop, struct ev_io *watcher, int event) {
    struct sockaddr_in addr;
    socklen_t len;
    int sock;
    struct ev_io *wc; // Client Watcher

    if( event & EV_ERROR ) { printf("Forbidden  event %d\n", event); return; }
    // Accept connection
    len = sizeof(addr);
    sock = accept(watcher->fd, (struct sockaddr *)&addr, &len);
    if( sock == -1 ) return;
    if( fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) == -1 ) { close(sock); return; } // Non Blocked
    printf("(Thread 2) Connected with: %s:%u\n", inet_ntoa(addr.sin_addr), addr.sin_port);
    wc = (struct ev_io*) malloc(sizeof(struct ev_io));
    ev_io_init(wc, read_cb_echo, sock, EV_READ); // Init event watcher for read client
    ev_io_start(loop, wc);
}


/*  Run frontend UDP servers on separate threads  */

// Computing checksum RFC 1071
unsigned short csum(unsigned short *buf, int nwords) {
  unsigned long sum;

  for(sum=0; nwords>0; nwords--) sum += *buf++;
  sum = (sum >> 16) + (sum &0xffff);
  sum += (sum >> 16);
  return (unsigned short)(~sum);
}

// Send raw UDP
int send_raw_udp(struct iphdr *ip_header, struct udphdr  *udp_header, char *message, int size, char *packet) {
    int sock;
    int sport, dport;
    struct in_addr in, out;
    struct sockaddr_in addr;
    char buffer[BUFF_SIZE];
    struct iphdr new_ip_header;
    struct udphdr new_udp_header;
    const int on = 1;

    // sock = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if( (sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0 ) 
        { perror ("socket failed "); exit (EXIT_FAILURE); }
    if( setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &on, sizeof (on)) < 0 ) 
        { perror ("setsockopt failed to set IP_HDRINCL "); exit (EXIT_FAILURE); }
    if( setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, out_iface, sizeof(out_iface) ) < 0) 
        { perror("bind to interface failed"); exit(EXIT_FAILURE); }

    ip_header->saddr = inet_addr("192.168.10.1");
    ip_header->daddr = inet_addr("192.168.10.12");
    ip_header->ihl = ip_hlen / sizeof(uint32_t);
    ip_header->version = 4;
    ip_header->tos = 0;
    //ip_header->ip_len = htons (ip_len + udp_len + datalen);
    ip_header->tot_len = htons (size);
    ip_header->id = htons (0);
    ip_header->frag_off = htons ((0 << 15) + (0 << 14) + (0 << 13) + 0);
    ip_header->ttl = 255;
    ip_header->protocol = IPPROTO_UDP;
    ip_header->check = csum((unsigned short *)ip_header, ip_hlen);

    // UDP header
    udp_header->source = htons(7777);
    udp_header->dest = htons(3333);
    udp_header->len = htons(size-ip_hlen);
    udp_header->check = csum((unsigned short *)message, size - ip_hlen - udp_hlen);

    memcpy((void *)buffer, (void *)ip_header, ip_hlen);
    memcpy((void *)(buffer + ip_hlen), (void *)udp_header, udp_hlen);
    memcpy((void *)(buffer + ip_hlen + udp_hlen), (void *)message, size - ip_hlen - udp_hlen);

    memcpy((void *)&new_ip_header, (void *)buffer, ip_hlen);
    memcpy((void *)&new_udp_header, (void *)(buffer + ip_hlen), udp_hlen);
    sport = htons(new_udp_header.source);
    dport = htons(new_udp_header.dest);
    in.s_addr = new_ip_header.saddr;
    out.s_addr = new_ip_header.daddr;

    printf("Send src=%s:%d dst=%s:%d ", inet_ntoa(in), sport, inet_ntoa(out), dport);
    printf("%d:%d byte ipcsum=%d udpcsum=%d TTL=%d\n", size, size-ip_hlen-udp_hlen, new_ip_header.check, new_udp_header.check, new_ip_header.ttl);

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
//    addr.sin_addr.s_addr = inet_addr("192.168.10.12");
    sendto(sock, buffer, size, 0, (struct sockaddr *)&addr, sizeof(addr));
//    sendto(sock, packet, size, 0, (struct sockaddr *)&addr, sizeof(addr));

    close(sock);
    return 0;
}

// Read callback for UDP
void read_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    char packet[BUFF_SIZE] = {0};
    char message[BUFF_SIZE] = {0};
    char rev[BUFF_SIZE] = {0};
    char ip[32];
    int len, n;
    struct sockaddr_in cliaddr;
    struct iphdr ip_header;
    struct udphdr udp_header;

    if (EV_ERROR & revents) { printf("read invalid event\n"); return; }
    memset(&cliaddr, 0, sizeof(cliaddr));
    len = sizeof(cliaddr);
    n = recvfrom(watcher->fd, (char *)packet, BUFF_SIZE, MSG_WAITALL, ( struct sockaddr *) &cliaddr, &len);
    packet[n] = '\0';

    memcpy((void *)&ip_header, (void *)packet, ip_hlen);
    memcpy((void *)&udp_header, (void *)(packet + ip_hlen), udp_hlen);
    memcpy((void *)message, (void *)(packet + ip_hlen + udp_hlen), n - ip_hlen - udp_hlen);

    int sport, dport;
    sport = htons(udp_header.source);
    dport = htons(udp_header.dest);
    struct in_addr in, out;
    in.s_addr = ip_header.saddr;
    out.s_addr = ip_header.saddr;
    printf("Receive src=%s:%d  dst=%s:%d ", inet_ntoa(in), sport, inet_ntoa(out), dport);
    printf("%d:%d byte ipcsum=%d udpcsum=%d\n", n, n-ip_hlen-udp_hlen, ip_header.check, udp_header.check);

    // Send message to main thread for reverse
    tcp_client(message, rev);

    // Send raw UPD to out interface
    if(ip_header.ttl > 0) send_raw_udp(&ip_header, &udp_header, rev, n, packet);
}


int udp_socket_init(int *sd, char *ipaddr, uint16_t port) {
    int sock;
    char buffer[BUFF_SIZE];
    struct sockaddr_in addr;

    if( (sock = socket(AF_INET, SOCK_RAW, IPPROTO_UDP)) < 0 ) 
        { perror("Socket creation failed"); exit(EXIT_FAILURE); }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    if( ipaddr == NULL ) { addr.sin_addr.s_addr = htonl(INADDR_ANY); } 
    else { addr.sin_addr.s_addr = inet_addr(ipaddr); }
    addr.sin_port = htons(port);

    // Bind socket
    if( bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ) { perror("bind failed"); exit(EXIT_FAILURE); }
    if( fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) == -1 ) { close(sock); exit(EXIT_FAILURE); } // Non Blocked
    // Bind socket to in_face
    if( setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, in_iface, sizeof(in_iface)) < 0 ) 
        { perror("Bind to interface failed");	exit(EXIT_FAILURE); }

    *sd = sock;
    return 0;
}

struct thargs { int port; };

void* fth2(void* p) {
    struct thargs* a = (struct thargs*) p;
    struct ev_io   accept_io;
    struct ev_loop *loop;
    int sd;

    printf("(Thread 2) Start UDP frontend server on interface %s\n", in_iface);
    if( (loop = ev_loop_new(EVBACKEND_EPOLL)) == NULL ) { printf("(Thread 2) Fail of create loop!\n"); return NULL; }
    if( udp_socket_init(&sd, NULL, PORT) < 0) {  printf("UDP server init failed\r\n"); return NULL; }

    ev_io_init(&accept_io, read_cb, sd, EV_READ);
    ev_io_start(loop, &accept_io);
    ev_run(loop, 0);

    return NULL;
}


/* TCP Reverser server */

// Fuction for reverse buffer
void reverse_buff(char *buff, int size) {
   int i, len;
   char c;

   for(i=0; i<size/2; i++) {
      c = buff[i];
      buff[i] = buff[size - i - 1];
      buff[size - i - 1] = c;
   }
}

// Init TCP socket
int tcp_socket_init(int *sid, char *ip, uint16_t port) {
    int max_con = 7;
    int sock;
    int r = 1;
    struct sockaddr_in addr;

    if( (sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1; // Create socket
    if( setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r)) == -1 ) { close(sock); return -1; } // Reuse port
    if( fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) == -1 ) { close(sock); return -1; } // Set NON-BLOCK
    memset(&addr, 0 , sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if( ip == NULL ) { addr.sin_addr.s_addr = htonl(INADDR_ANY); } else { addr.sin_addr.s_addr = inet_addr(ip); }
    if( bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1 ) { close(sock); return -1; }
    if( listen(sock, max_con) == -1 ) { close(sock); return -1; } // Listen port
    *sid = sock;
    return 0;
}

// Function for read callback (reverse)
void read_cb_reverse(struct ev_loop *loop, struct ev_io *watcher, int event) {
    char buff[BUFF_SIZE] = {0};
    char ip[32];
    int r = 0;
    int32_t n;

    if( event & EV_ERROR ) { printf("Forbidden event: %d\n", event); return; }
    n = read(watcher->fd, buff, sizeof(buff));
    if( n == -1 ) { if( EINTR != errno && EAGAIN != errno ) r = 1; } // Error read TCP
    else { if( n == 0 ) r = 2; } // TCP Close
    get_ip_port(watcher->fd, ip);
    if( r != 0) {
        printf("(Thread main) TCP %s CLOSE.\n", ip);
        ev_io_stop(loop, watcher);
        free(watcher);
    }
    else {
        printf("(Thread main) READ from %s: %s\n", ip, buff);
        reverse_buff(buff, strlen(buff));
//        reverse_buff(buff, sizeof(buff));
        printf("(Thread main) SEND REVERSE to %s\n", ip);
        n = write(watcher->fd, buff, sizeof(buff)); // send reverse
    }
}

// Function for accept callback (reverse)
void accept_cb_reverse(struct ev_loop *loop, struct ev_io *watcher, int event) {
    struct sockaddr_in addr;
    socklen_t len;
    int sock;
    struct ev_io *wc; // Client Watcher

    if( event & EV_ERROR ) { printf("Forbidden  event %d\n", event); return; }
    len = sizeof(addr);
    sock = accept(watcher->fd, (struct sockaddr *)&addr, &len);
    if( sock == -1 ) return;
    if( fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) == -1 ) { close(sock); return; } // Non Blocked
    printf("(Thread main) Connected with: %s:%u\n", inet_ntoa(addr.sin_addr), addr.sin_port);
    wc = (struct ev_io*) malloc(sizeof(struct ev_io));
    ev_io_init(wc, read_cb_reverse, sock, EV_READ); // Init event watcher for read client
    ev_io_start(loop, wc);
}


// Run TCP servers for reverse messages with libev & thread library
int main(int argc, char *argv[]) {
    struct ev_io   accept_io;
    struct ev_loop *loop;
    int sock;
    int port = 10001; // default port for frontend
    pthread_t th2;
    struct thargs arg2;

    // Set in & out interfaces
    if( argc>1 ) in_iface = strdup(argv[1]);
    if( argc>2 ) out_iface = strdup(argv[2]);
    printf("in_iface=%s  out_iface=%s\n", in_iface, out_iface);

    // Run ev_loop for reverse server on main thread
    printf("(Thread main) Start reverse server on port %d\n", REVERSE_PORT);
    if( (loop = ev_loop_new(EVBACKEND_EPOLL)) == NULL ) // Create ev_loop
        { printf("(Thread main) Fail of create loop!\n"); return -1; }
    if( tcp_socket_init(&sock, NULL, REVERSE_PORT) < 0 ) // Init socket
        { printf("(Thread main) Fail of init socket!\n"); return -1; }
    ev_io_init(&accept_io, accept_cb_reverse, sock, EV_READ); // Init io ev_loop (reverse)
    ev_io_start(loop, &accept_io); // Start io ev_loop

    // Run frontend UDP server in separate thread
    arg2.port = port;
    pthread_create(&th2, NULL, &fth2, &arg2); // thread 2 for UDP server
    // pthread_join(th2, NULL);

    // Run ev_loop for main thread
    ev_run(loop, 0); 

    return 0;
}
