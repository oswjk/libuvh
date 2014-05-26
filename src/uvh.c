#include "uvh.h"
#include "sds.h"
#include "http_parser.h"

#ifndef offsetof
#ifdef __GNUC__
#define offsetof(type, member)  __builtin_offsetof (type, member)
#endif
#endif

#ifndef container_of
#ifdef __GNUC__
#define member_type(type, member) __typeof__ (((type *)0)->member)
#else
#define member_type(type, member) const void
#endif

#define container_of(ptr, type, member) ((type *)( \
    (char *)(member_type(type, member) *){ ptr } - offsetof(type, member)))
#endif

#define LOG(LEVEL, FMT, args...) \
    fprintf(stderr, FMT "\n" , ##args)

#define LOG_DEBUG(FMT, args...)   LOG(0, "DEBUG   " FMT , ##args)
#define LOG_WARNING(FMT, args...) LOG(0, "WARNING " FMT , ##args)
#define LOG_ERROR(FMT, args...)   LOG(0, "ERROR   " FMT , ##args)
#define LOG_INFO(FMT, args...)    LOG(0, "INFO    " FMT , ##args)

struct http_status_code_def
{
    int code;
    const char *str;
};

static struct http_status_code_def http_status_code_defs[] =
{
#define XX(CODE, NAME, STR) { CODE, STR },
    HTTP_STATUS_CODE_MAP(XX)
#undef XX
    { -1, NULL }
};

static void on_connection(uv_stream_t *stream, int status);
static uv_buf_t alloc_cb(uv_handle_t *, size_t size);
static void read_cb(uv_stream_t *stream, ssize_t nread, uv_buf_t buf);
static void close_cb(uv_handle_t *handle);

static int on_message_begin(http_parser *parser);
static int on_url(http_parser *parser, const char *at, size_t len);
// static int on_status(http_parser *parser, const char *at, size_t len);
static int on_header_field(http_parser *parser, const char *at,
    size_t len);
static int on_header_value(http_parser *parser, const char *at,
    size_t len);
static int on_headers_complete(http_parser *parser);
static int on_body(http_parser *parser, const char *at, size_t len);
static int on_message_complete(http_parser *parser);

static void uvh_request_write_chunk(struct uvh_request *req, sds chunk);

struct uvh_server_private
{
    struct uvh_server server;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uv_loop_t *loop;
    struct http_parser_settings http_parser_settings;
    uv_tcp_t stream;
    char stop;
};

struct uvh_request_private
{
    struct uvh_request req;
    struct http_parser parser;
    uv_tcp_t stream;
    sds header_name;
    sds header_value;
    int header_state;
    char keepalive;
    int send_status;
    sds send_headers;
    sds send_body;
    int streaming;
    uvh_stream_cb stream_cb;
    void *stream_userdata;
};

UVH_EXTERN struct uvh_server *uvh_server_init(uv_loop_t *loop, void *data,
    uvh_request_handler_cb request_handler)
{
    struct uvh_server_private *server;
    int rc;

    server = calloc(1, sizeof(*server));

    if (!server)
        goto error;

    server->loop = loop;
    server->server.data = data;
    server->server.request_handler = request_handler;

    rc = uv_tcp_init(loop, &server->stream);

    if (rc < 0)
        goto error;

    server->stream.data = server;

    server->http_parser_settings.on_message_begin = on_message_begin;
    server->http_parser_settings.on_url = on_url;
    // server->http_parser_settings.on_status = on_status;
    server->http_parser_settings.on_header_field = on_header_field;
    server->http_parser_settings.on_header_value = on_header_value;
    server->http_parser_settings.on_headers_complete = on_headers_complete;
    server->http_parser_settings.on_body = on_body;
    server->http_parser_settings.on_message_complete = on_message_complete;

    return &server->server;

error:

    if (server)
    {
        free(server);
    }

    return NULL;
}

UVH_EXTERN void uvh_server_free(struct uvh_server *server)
{
    struct uvh_server_private *p = container_of(server,
        struct uvh_server_private, server);
    free(p);
}

UVH_EXTERN int uvh_server_listen(struct uvh_server *server, const char *address,
    short port)
{
    struct uvh_server_private *serverp = container_of(server,
        struct uvh_server_private, server);
    struct sockaddr_in addr = uv_ip4_addr(address, port);

    memcpy(&serverp->addr, &addr, sizeof(addr));
    serverp->addr_len = sizeof(addr);

    uv_tcp_bind(&serverp->stream, addr);

    int r = uv_listen((uv_stream_t *) &serverp->stream, 128,
        on_connection);

    if (r)
        return r;

    return 0;
}

static void on_server_close(uv_handle_t *handle)
{
    LOG_DEBUG("%s", __FUNCTION__);
}

UVH_EXTERN void uvh_server_stop(struct uvh_server *server)
{
    struct uvh_server_private *p;

    p = container_of(server, struct uvh_server_private, server);

    p->stop = 1;

    uv_close((uv_handle_t *) &p->stream, &on_server_close);
}

void request_init(struct uvh_request_private *req, struct uvh_server *server)
{
    if (req->req.header_count > 0)
    {
        int i;
        for (i = 0; i < req->req.header_count; ++i)
        {
            sdsfree((sds) req->req.headers[i].name);
            sdsfree((sds) req->req.headers[i].value);
        }

        if (req->req.method)
            sdsfree((sds) req->req.method);

        if (req->req.version)
            sdsfree((sds) req->req.version);

        if (req->req.url.full)
            sdsfree((sds) req->req.url.full);

        if (req->req.url.schema)
            sdsfree((sds) req->req.url.schema);

        if (req->req.url.host)
            sdsfree((sds) req->req.url.host);

        if (req->req.url.port)
            sdsfree((sds) req->req.url.port);

        if (req->req.url.path)
            sdsfree((sds) req->req.url.path);

        if (req->req.url.query)
            sdsfree((sds) req->req.url.query);

        if (req->req.url.fragment)
            sdsfree((sds) req->req.url.fragment);

        if (req->req.url.userinfo)
            sdsfree((sds) req->req.url.userinfo);

        if (req->req.content)
            sdsfree((sds) req->req.content);
    }

    memset(&req->req, 0, sizeof(req->req));

    req->req.server = server;

    req->header_state = 0;

    req->send_body = sdsempty();
    req->send_headers = sdsempty();
    req->send_status = HTTP_OK;

    http_parser_init(&req->parser, HTTP_REQUEST);
    req->parser.data = req;

    req->streaming = 0;
    req->stream_cb = NULL;
    req->stream_userdata = NULL;
}

static void on_connection(uv_stream_t *stream, int status)
{
    struct uvh_server_private *priv = container_of((uv_tcp_t *) stream,
        struct uvh_server_private, stream);

    LOG_DEBUG("%s", __FUNCTION__);

    if (status == -1)
    {
        LOG_WARNING("on_connection: status = -1");
        return;
    }

    if (priv->stop)
    {
        LOG_WARNING("on_connection: stop bit set");
        uv_tcp_t *client = calloc(1, sizeof(*client));
        uv_tcp_init(priv->loop, client);
        uv_accept(stream, (uv_stream_t *) client);
        uv_close((uv_handle_t *) client, NULL);
        return;
    }

    struct uvh_request_private *req = calloc(1, sizeof(*req));
    request_init(req, &priv->server);

    if (uv_tcp_init(priv->loop, &req->stream))
    {
        LOG_WARNING("failed to initialize uv_tcp_t");
        goto error;
    }

    req->stream.data = req;

    if (uv_accept(stream, (uv_stream_t *) &req->stream) == 0)
    {
        uv_read_start((uv_stream_t *) &req->stream, alloc_cb,
            read_cb);
        return;
    }
    else
    {
        uv_close((uv_handle_t *) &req->stream, NULL);
        LOG_WARNING("failed to accept");
    }

error:

    if (req)
    {
        sdsfree(req->send_body);
        sdsfree(req->send_headers);
        free(req);
    }
}

static uv_buf_t alloc_cb(uv_handle_t *handle, size_t size)
{
    (void) handle;
    return uv_buf_init(calloc(1, size), size);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, uv_buf_t buf)
{
    struct uvh_request_private *req;
    struct uvh_server_private *serverp;

    req = (struct uvh_request_private *) stream->data;
    serverp = container_of(req->req.server, struct uvh_server_private,
        server);

    LOG_DEBUG("read_cb: nread: %d, buf.len: %d", (int)nread, (int)buf.len);

    if (nread < 0)
    {
        uv_err_t err = uv_last_error(stream->loop);

        if (buf.base)
            free(buf.base);

        if (err.code == UV_EOF)
        {
            LOG_DEBUG("EOF");
            http_parser_execute(&req->parser,
                &serverp->http_parser_settings, NULL, 0);
        }

        uv_close((uv_handle_t *) stream, &close_cb);

        return;
    }

    if (nread == 0)
    {
        free(buf.base);
        return;
    }

    int nparsed = http_parser_execute(&req->parser,
        &serverp->http_parser_settings,
        buf.base, nread);

    LOG_DEBUG("nparsed:%d", nparsed);

    if (nparsed != nread)
    {
        LOG_ERROR("http parse error, closing connection");
        uv_close((uv_handle_t *) stream, &close_cb);
    }

    free(buf.base);
}

static void close_cb(uv_handle_t *handle)
{
    LOG_DEBUG("%s", __FUNCTION__);

    struct uvh_request_private *p;

    p = (struct uvh_request_private *) handle->data;

    sdsfree((sds) p->req.method);
    sdsfree((sds) p->req.version);
    sdsfree((sds) p->req.url.full);
    sdsfree((sds) p->req.url.schema);
    sdsfree((sds) p->req.url.host);
    sdsfree((sds) p->req.url.port);
    sdsfree((sds) p->req.url.path);
    sdsfree((sds) p->req.url.query);
    sdsfree((sds) p->req.url.fragment);
    sdsfree((sds) p->req.url.userinfo);
    sdsfree((sds) p->req.content);
    sdsfree((sds) p->send_body);
    sdsfree((sds) p->send_headers);

    free(p);
}

static int on_message_begin(http_parser *parser)
{
    struct uvh_request_private *priv;
    priv = (struct uvh_request_private *) parser->data;
    priv->req.content = (const char *) sdsempty();
    return 0;
}

static int on_url(http_parser *parser, const char *at, size_t len)
{
    struct uvh_request_private *priv;
    priv = (struct uvh_request_private *) parser->data;

    LOG_DEBUG("on_url: <%.*s>", (int) len, at);

    if (!priv->req.url.full)
    {
        priv->req.url.full = sdsnewlen(at, len);
    }
    else
    {
        priv->req.url.full = sdscatlen((sds) priv->req.url.full,
            at, len);
    }

    return 0;
}

// static int on_status(http_parser *parser, const char *at, size_t len)
// {
//     LOG_DEBUG("on_status: <%.*s>", (int) len, at);
//     return 0;
// }

static int on_header_field(http_parser *parser, const char *at,
    size_t len)
{
    struct uvh_request_private *priv;
    struct uvh_request *req;

    priv = (struct uvh_request_private *) parser->data;
    req = &priv->req;

    if (priv->header_state == 0)
    {
        priv->header_name = sdsnewlen(at, len);
    }
    else if (priv->header_state == 1)
    {
        priv->header_name = sdscatlen(priv->header_name, at, len);
    }
    else if (priv->header_state == 2)
    {
        req->headers[req->header_count].name = priv->header_name;
        req->headers[req->header_count].value = priv->header_value;
        req->header_count += 1;

        priv->header_name = sdsnewlen(at, len);
        priv->header_value = NULL;
    }

    priv->header_state = 1;

    return 0;
}

static int on_header_value(http_parser *parser, const char *at,
    size_t len)
{
    struct uvh_request_private *priv;

    priv = (struct uvh_request_private *) parser->data;

    if (priv->header_state == 1)
    {
        priv->header_value = sdsnewlen(at, len);
    }
    else if (priv->header_state == 2)
    {
        priv->header_value = sdscatlen(priv->header_value, at, len);
    }

    priv->header_state = 2;

    return 0;
}

static int on_headers_complete(http_parser *parser)
{
    struct uvh_request_private *priv;
    struct uvh_request *req;
    struct http_parser_url url;
    const char *full;

    priv = (struct uvh_request_private *) parser->data;
    req = &priv->req;
    full = req->url.full;

    if (priv->header_state == 2)
    {
        req->headers[req->header_count].name = priv->header_name;
        req->headers[req->header_count].value = priv->header_value;
        req->header_count += 1;
    }

    LOG_DEBUG("on_headers_complete");

    http_parser_parse_url(req->url.full, sdslen((sds) req->url.full),
        1, &url);

#define UF_OFFSET(X) url.field_data[X].off
#define UF_LEN(X) url.field_data[X].len
#define UF_SET(X) (url.field_set & (1 << (X)))
#define UF_CHECK_AND_SET(X, DST) \
    if (UF_SET(X)) \
        (DST) = sdsnewlen(full + UF_OFFSET(X), UF_LEN(X))

    UF_CHECK_AND_SET(UF_SCHEMA, req->url.schema);
    UF_CHECK_AND_SET(UF_HOST, req->url.host);
    UF_CHECK_AND_SET(UF_PORT, req->url.port);
    UF_CHECK_AND_SET(UF_PATH, req->url.path);
    UF_CHECK_AND_SET(UF_QUERY, req->url.query);
    UF_CHECK_AND_SET(UF_FRAGMENT, req->url.fragment);
    UF_CHECK_AND_SET(UF_USERINFO, req->url.userinfo);

#undef UF_CHECK_AND_SET
#undef UF_SET
#undef UF_LEN
#undef UF_OFFSET

    return 0;
}

static int on_body(http_parser *parser, const char *at, size_t len)
{
    struct uvh_request_private *priv;
    priv = (struct uvh_request_private *) parser->data;
    priv->req.content = (const char *) sdscatlen((sds) priv->req.content,
        at, len);
    return 0;
}

static int on_message_complete(http_parser *parser)
{
    struct uvh_request_private *priv;

    priv = (struct uvh_request_private *) parser->data;

    LOG_DEBUG("on_message_complete");

    priv->keepalive = http_should_keep_alive(parser);

    if (priv->req.content)
        priv->req.content_length = sdslen((sds) priv->req.content);
    else
        priv->req.content_length = 0;

    priv->req.method = sdsnew(http_method_str(parser->method));

    priv->req.version = sdsempty();
    priv->req.version = sdscatprintf((sds) priv->req.version,
        "HTTP/%d.%d", parser->http_major, parser->http_minor);

    if (priv->req.server->request_handler)
        priv->req.server->request_handler(&priv->req);

    return 0;
}

struct uvh_write_request
{
    uv_buf_t buf;
    uv_write_t wreq;
    struct uvh_request_private *req;
};

static void uvh_write_request_free(struct uvh_write_request *req)
{
    sdsfree((sds) req->buf.base);
    free(req);
}

static void after_request_write(uv_write_t *req, int status)
{
    LOG_DEBUG("%s", __FUNCTION__);
    struct uvh_write_request *wreq = container_of(req, struct uvh_write_request,
        wreq);
    (void) status;
    uvh_write_request_free(wreq);
}

static void uvh_request_write_sds(struct uvh_request *req, sds data,
    uv_write_cb cb)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    struct uvh_write_request *wreq = calloc(1, sizeof(*wreq));

    wreq->buf.base = (char *) data;
    wreq->buf.len = sdslen(data);

    wreq->req = p;

    uv_write(&wreq->wreq, (uv_stream_t *) &p->stream, &wreq->buf, 1, cb);
}

UVH_EXTERN void uvh_request_write(struct uvh_request *req,
    const char *data, size_t len)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    if (p->streaming)
    {
        uvh_request_write_chunk(req, sdsnewlen(data, len));
    }
    else
        p->send_body = sdscatlen(p->send_body, data, len);
}

UVH_EXTERN void uvh_request_writef(struct uvh_request *req, const char *fmt,
    ...)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    va_list ap;
    sds result;

    va_start(ap, fmt);
    result = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    if (p->streaming)
    {
        uvh_request_write_chunk(req, result);
    }
    else
        p->send_body = sdscatsds(p->send_body, result);

    sdsfree(result);
}

UVH_EXTERN void uvh_request_write_status(struct uvh_request *req, int status)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    p->send_status = status;
}

UVH_EXTERN void uvh_request_write_header(struct uvh_request *req,
    const char *name, const char *value)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    if (p->streaming)
        return;

    p->send_headers = sdscatprintf(p->send_headers, "%s: %s\r\n", name, value);
}

UVH_EXTERN const char *http_status_code_str(int code)
{
    struct http_status_code_def *def = http_status_code_defs;
    while (def->code != -1)
    {
        if (def->code == code)
        {
            return def->str;
        }
        ++def;
    }

    return NULL;
}

UVH_EXTERN const char *uvh_request_get_header(struct uvh_request *req,
    const char *name)
{
    int i;

    for (i = 0; i < req->header_count; ++i)
    {
        if (strcasecmp(name, req->headers[i].name) == 0)
            return req->headers[i].value;
    }

    return NULL;
}

UVH_EXTERN void uvh_request_end(struct uvh_request *req)
{
    LOG_DEBUG("%s", __FUNCTION__);

    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    uvh_request_write_sds(req, sdscatprintf(sdsempty(),
        "%s %d %s\r\n", p->req.version, p->send_status,
        http_status_code_str(p->send_status)), &after_request_write);

    if (!p->streaming)
    {
        sds content_len = sdscatprintf(sdsempty(), "%d", (int)sdslen(p->send_body));
        uvh_request_write_header(req, "Content-Length", content_len);
        sdsfree(content_len);
    }

    LOG_DEBUG("keepalive: %d", p->keepalive);

    if (!p->keepalive)
    {
        uvh_request_write_header(req, "Connection", "close");
        // uv_close at some point?
    }

    uvh_request_write_sds(req, p->send_headers, &after_request_write);
    uvh_request_write_sds(req, sdsnew("\r\n"), &after_request_write);

    if (!p->streaming)
        uvh_request_write_sds(req, p->send_body, &after_request_write);
    else
        sdsfree(p->send_body);

    if (p->keepalive && !p->streaming)
    {
        request_init(p, req->server);
    }
}

static void after_last_chunk_write(uv_write_t *req, int status)
{
    LOG_DEBUG("%s", __FUNCTION__);

    struct uvh_write_request *wreq = container_of(req, struct uvh_write_request,
        wreq);

    (void)status;

    request_init(wreq->req, wreq->req->req.server);

    uvh_write_request_free(wreq);
}

static void after_chunk_write(uv_write_t *req, int status)
{
    LOG_DEBUG("%s", __FUNCTION__);

    (void)status;

    struct uvh_write_request *wreq = container_of(req, struct uvh_write_request,
        wreq);

    struct uvh_request_private *p = wreq->req;

    uvh_write_request_free(wreq);

    if (p->stream_cb)
    {
        char *chunk;

        int chunklen = p->stream_cb(&chunk, p->stream_userdata);

        if (chunklen == 0)
        {
            uvh_request_write_chunk(&p->req, NULL);
        }
        else
        {
            uvh_request_write_chunk(&p->req, sdsnewlen(chunk, chunklen));
            free(chunk);
        }
    }
}

static void uvh_request_write_chunk(struct uvh_request *req, sds chunk)
{
    unsigned int len = chunk != NULL ? sdslen(chunk) : 0;

    LOG_DEBUG("%s len:%u", __FUNCTION__, len);

    sds chunklen = sdscatprintf(sdsempty(), "%X\r\n", len);

    uvh_request_write_sds(req, chunklen, &after_request_write);

    uv_write_cb callback;

    if (len > 0)
    {
        uvh_request_write_sds(req, chunk, &after_request_write);
        callback = &after_chunk_write;
    }
    else
    {
        if (chunk)
            sdsfree(chunk);

        callback = &after_last_chunk_write;
    }

    uvh_request_write_sds(req, sdsnew("\r\n"), callback);
}

UVH_EXTERN void uvh_request_stream(struct uvh_request *req, uvh_stream_cb cb,
    void *data)
{
    struct uvh_request_private *p = container_of(req,
        struct uvh_request_private, req);

    uvh_request_write_header(req, "Transfer-Encoding", "chunked");

    p->streaming = 1;
    p->stream_cb = cb;
    p->stream_userdata = data;

    uvh_request_end(req);

    if (cb)
    {
        char *chunk;
        int chunklen = cb(&chunk, data);
        uvh_request_write_chunk(req, sdsnewlen(chunk, chunklen));
        free(chunk);
    }
}
