#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>

#define BUFF_SIZE 4096

// Get IP & port for sock id
void get_ip_port(int sid, char *s)
{
    int r;
    struct sockaddr_in addr;
    socklen_t size;

    size = sizeof(struct sockaddr_in);
    r = getpeername(sid, (struct sockaddr *)&addr, &size);
    sprintf(s, "%s:%u", inet_ntoa(addr.sin_addr), addr.sin_port);
}

// Init socket
int socket_init(int *sid, char *ip, uint16_t port)
{
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

// Function for read callback
void read_cb(struct ev_loop *loop, struct ev_io *watcher, int event)
{
    char buff[BUFF_SIZE] = {0};
    char ip[32];
    int r = 0;
    int32_t nbyte;

    if( event & EV_ERROR ) { printf("Forbidden event: %d\n", event); return; }
    nbyte = read(watcher->fd, buff, sizeof(buff));
    if( nbyte == -1 ) { if( EINTR != errno && EAGAIN != errno ) r = 1; } // Error read TCP
    else { if( nbyte == 0 ) r = 2; } // TCP Close
    get_ip_port(watcher->fd, ip);
    if( r != 0) {
        printf("TCP %s CLOSE.\n", ip);
        ev_io_stop(loop, watcher);
        free(watcher);
        }
    else {
        printf("READ from %s: %s\n", ip, buff);
        nbyte = write(watcher->fd, buff, sizeof(buff)); // Send echo
        }
}

// Function for accept callback
void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int event)
{
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
    printf("Connected with: %s:%u\n", inet_ntoa(addr.sin_addr), addr.sin_port);
    wc = (struct ev_io*) malloc(sizeof(struct ev_io));
    ev_io_init(wc, read_cb, sock, EV_READ); // Init event watcher for read client
    ev_io_start(loop, wc);
}

int main(int argc, char *argv[])
{
    struct ev_io   accept_io;
    struct ev_loop *loop;
    int sock;
    int port=10001;

    // Set num port
    if( argc>1 ) { port = atoi(argv[1]); printf("Set port %d\n", port); } 
    else { printf("Default port %d\n", port); }

    if( (loop = ev_loop_new(EVBACKEND_EPOLL)) == NULL ) // Create ev_loop
        { printf("Fail of create loop!\n"); return -1; }

    if( socket_init(&sock, NULL, port) < 0 ) // Init socket
        { printf("Fail of init socket!\n"); return -1; }

    ev_io_init(&accept_io, accept_cb, sock, EV_READ); // Init io ev_loop
    ev_io_start(loop, &accept_io); // Start io ev_loop
    ev_run(loop, 0); // Run ev_loop

    return 0;
}
