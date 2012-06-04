#include "http_client_pool.h"
#include <netinet/tcp.h>
#include <sys/types.h>
#include "sstr.h"

#define CLIENT_STACK_SIZE  65536

SSTRL(CRLFCRLF, "\r\n\r\n");
SSTRL(CRLF, "\r\n");
SSTRL(CONTENT_LENGTH, "\r\nContent-Length: ");
SSTRL(TRANSFER_ENCODING, "\r\nTransfer-Encoding: ");

SSTRL(CONNECTION, "\r\nConnection: ");
SSTRL(CONNECTION_CLOSE, "close");


int http_client_pool_init(struct http_client_pool *http_client_pool, size_t initial, size_t grow) {
    if (0 > ctx_pool_init(&http_client_pool->ctx_pool, initial, grow, CLIENT_STACK_SIZE, sizeof(struct http_client_context)))
        return -1;
    return 0;
}

#define CLIENT_ERROR()                          \
    do {                                        \
        printf("client error\n");               \
        close(fd);                              \
        return;                                 \
    } while (0)


void http_client_fiber_main(void) {
    struct http_client_context *ctx = http_client_get_context();
    int fd = current_ctx->fd;
    int persistent = 0;

    /*
     * write request
     */
    int res;
    for (; (res = vmbuf_write(&ctx->request, fd)) == 0; yield());
    if (0 > res)
        perror("write");
    /*
     * HTTP header
     */
    char *eoh;
    for (;;yield()) {
        vmbuf_read(&ctx->response, fd);
        *vmbuf_wloc(&ctx->response) = 0;
        if (NULL != (eoh = strstr(vmbuf_data(&ctx->response), CRLFCRLF)))
            break;
    }
    char *data = vmbuf_data(&ctx->response);
    char *p = strstr(data, CONNECTION);
    if (p != NULL) {
        p += SSTRLEN(CONNECTION);
        persistent = (0 == SSTRNCMPI(CONNECTION_CLOSE, p) ? 0 : 1);
    }
    SSTRL(HTTP, "HTTP/");
    if (0 != SSTRNCMP(HTTP, data))
        CLIENT_ERROR();

    p = strchrnul(data, ' ');
    int code = (*p ? atoi(p + 1) : 0);
    if (0 == code)
        CLIENT_ERROR();
    if (code == 204 || code == 304) /* No Content,  Not Modified */
        return;

    char *content_len = strstr(data, CONTENT_LENGTH);
    if (NULL != content_len) {
        content_len += SSTRLEN(CONTENT_LENGTH);
        size_t content_end = eoh - data + SSTRLEN(CRLFCRLF) + atoi(content_len);
        while (vmbuf_wlocpos(&ctx->response) < content_end) {
            if (0 > vmbuf_read(&ctx->response, fd))
                CLIENT_ERROR();
            yield();
        }
    } else {
        CLIENT_ERROR(); // TODO: chunked
    }
    *vmbuf_wloc(&ctx->response) = 0;
     (void)persistent;
    close(current_ctx->fd);
}

struct http_client_context *http_client_pool_create_client(struct http_client_pool *http_client_pool, struct in_addr addr, uint16_t port) {
    int cfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (0 > cfd)
        return perror("socket"), NULL;

    const int option = 1;
    if (0 > setsockopt(cfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)))
        return perror("setsockopt SO_REUSEADDR"), NULL;

    if (0 > setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option)))
        return perror("setsockopt TCP_NODELAY"), NULL;

    struct sockaddr_in saddr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = addr };
    connect(cfd, (struct sockaddr *)&saddr, sizeof(saddr));

    struct ribs_context *new_ctx = ctx_pool_get(&http_client_pool->ctx_pool);
    new_ctx->fd = cfd;
    new_ctx->data.ptr = http_client_pool;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = cfd;
    if (0 > epoll_ctl(ribs_epoll_fd, EPOLL_CTL_ADD, cfd, &ev))
        perror("epoll_ctl");
    epoll_worker_fd_map[cfd].ctx = new_ctx;
    ribs_makecontext(new_ctx, current_ctx, new_ctx, http_client_fiber_main, NULL);
    struct http_client_context *cctx = (struct http_client_context *)new_ctx->reserved;
    vmbuf_init(&cctx->request, 4096);
    vmbuf_init(&cctx->response, 4096);
    return cctx;
}