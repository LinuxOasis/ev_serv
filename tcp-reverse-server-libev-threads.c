#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>

#define BUFF_SIZE 4096
#define REVERSE_PORT 10002

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
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) { printf("(Thread 1) could not create socket\n"); return 1; }
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { printf("(Thread 1) could not connect to server\n"); return 1; }
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

// Get IP & port for sock id
void get_ip_port(int sid, char *s) {
    int r;
    struct sockaddr_in addr;
    socklen_t size;

    size = sizeof(struct sockaddr_in);
    r = getpeername(sid, (struct sockaddr *)&addr, &size);
    sprintf(s, "%s:%u", inet_ntoa(addr.sin_addr), addr.sin_port);
}

// Init socket
int socket_init(int *sid, char *ip, uint16_t port) {
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

// Function for accept callback (echo)
void accept_cb_echo(struct ev_loop *loop, struct ev_io *watcher, int event)
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
    printf("(Thread 2) Connected with: %s:%u\n", inet_ntoa(addr.sin_addr), addr.sin_port);
    wc = (struct ev_io*) malloc(sizeof(struct ev_io));
    ev_io_init(wc, read_cb_echo, sock, EV_READ); // Init event watcher for read client
    ev_io_start(loop, wc);
}

// Function for accept callback (reverse)
void accept_cb_reverse(struct ev_loop *loop, struct ev_io *watcher, int event)
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
    printf("(Thread main) Connected with: %s:%u\n", inet_ntoa(addr.sin_addr), addr.sin_port);
    wc = (struct ev_io*) malloc(sizeof(struct ev_io));
    ev_io_init(wc, read_cb_reverse, sock, EV_READ); // Init event watcher for read client
    ev_io_start(loop, wc);
}


// run frontend TCP servers on separate threads

struct thargs { int port; };

void* fth2(void* p) {
    struct thargs* a = (struct thargs*) p;

    struct ev_io   accept_io;
    struct ev_loop *loop;
    int sock ;
    int port = a->port;

    printf("(Thread 2) Start echo frontend server on port %d\n", port);
    if( (loop = ev_loop_new(EVBACKEND_EPOLL)) == NULL ) // Create ev_loop
        { printf("(Thread 2) Fail of create loop!\n"); return NULL; }
    if( socket_init(&sock, NULL, port) < 0 ) // Init socket
        { printf("(Thread 2) Fail of init socket!\n"); return NULL; }
    ev_io_init(&accept_io, accept_cb_echo, sock, EV_READ); // Init io ev_loop (echo)
    ev_io_start(loop, &accept_io); // Start io ev_loop
    ev_run(loop, 0); // Run ev_loop
    return NULL;
}

// Run TCP servers for reverse messages with libev & thread library
int main(int argc, char *argv[]) {
    struct ev_io   accept_io;
    struct ev_loop *loop;
    int sock;
    int port = 10001; // default port for frontend
    pthread_t th2;
    struct thargs arg2;

    // Set num port on frontend
    if( argc>1 ) { port = atoi(argv[1]); printf("Set port %d\n", port); } 
    else { printf("Default port %d\n", port); }

    // Run ev_loop for reverse server on main thread
    printf("(Thread main) Start reverse server on port %d\n", REVERSE_PORT);
    if( (loop = ev_loop_new(EVBACKEND_EPOLL)) == NULL ) // Create ev_loop
        { printf("(Thread main) Fail of create loop!\n"); return -1; }
    if( socket_init(&sock, NULL, REVERSE_PORT) < 0 ) // Init socket
        { printf("(Thread main) Fail of init socket!\n"); return -1; }
    ev_io_init(&accept_io, accept_cb_reverse, sock, EV_READ); // Init io ev_loop (reverse)
    ev_io_start(loop, &accept_io); // Start io ev_loop

    // Run frontend in separate thread
    arg2.port = port;
    pthread_create(&th2, NULL, &fth2, &arg2); // thread 2 for echo server
    // pthread_join(th2, NULL);

    // Run ev_loop for main thread
    ev_run(loop, 0); 

    return 0;
}
