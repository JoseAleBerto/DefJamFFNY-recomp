/* netplay_lobby.c -- see netplay_lobby.h.
 *
 * Wire format (TCP): frames of [u32 type][u32 length][payload], little-endian.
 *   client -> host  HELLO   content id (64 bytes), name (24 bytes), bundle
 *   host -> client  MERGED  bundle      or  REJECT  reason (text)
 * A bundle: "DJSB", u32 count, then per file: u16 path length, path ("folder/file"), u32 size, bytes.
 */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <direct.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET lsock;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <dirent.h>
#  include <unistd.h>
#  include <errno.h>
typedef int lsock;
#  define BAD_SOCK (-1)
#  define sock_close close
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "netplay_lobby.h"
#include "netplay_net.h"

#define T_HELLO   1u
#define T_MERGED  2u
#define T_REJECT  3u
#define MAX_BUNDLE (32u << 20)
#define MAX_FILES  512
#define PATH_MAX_LEN 200

static char s_error[256];

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof s_error, fmt, ap);
    va_end(ap);
}
const char *np_lobby_error(void) { return s_error; }

/* ---- talking to the player ------------------------------------------------------------ */

#if defined(_WIN32)
static HANDLE s_console = INVALID_HANDLE_VALUE;
static int s_console_tried;
#endif

void np_say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[NETPLAY] %s\n", line);
    fflush(stderr);
#if defined(_WIN32)
    /* The game is a windowed program; when a .bat started it, write to that window too. */
    if (!s_console_tried) {
        s_console_tried = 1;
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            s_console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
    }
    if (s_console != INVALID_HANDLE_VALUE) {
        DWORD n;
        WriteFile(s_console, line, (DWORD)strlen(line), &n, NULL);
        WriteFile(s_console, "\r\n", 2, &n, NULL);
    }
#endif
}

/* ---- files ------------------------------------------------------------------------- */

typedef struct { char path[PATH_MAX_LEN]; uint32_t size; uint8_t *data; } bfile;
typedef struct { int count; bfile files[MAX_FILES]; } bundle;

static void bundle_free(bundle *b)
{
    int i;
    for (i = 0; i < b->count; i++) free(b->files[i].data);
    b->count = 0;
}

static int read_file(const char *path, uint8_t **data, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > MAX_BUNDLE) { fclose(f); return -1; }
    *data = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (!*data || (n && fread(*data, 1, (size_t)n, f) != (size_t)n)) { fclose(f); free(*data); return -1; }
    fclose(f);
    *size = (uint32_t)n;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, uint32_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (size && fwrite(data, 1, size, f) != size) { fclose(f); return -1; }
    return fclose(f) ? -1 : 0;
}

static int is_dir(const char *path)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}

static int make_dir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0 || is_dir(path) ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || is_dir(path) ? 0 : -1;
#endif
}

int np_mkdirs(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            if (tmp[i - 1] == ':') continue;            /* "C:\" */
            tmp[i] = 0;
            make_dir(tmp);
            tmp[i] = c;
        }
    }
    return make_dir(tmp);
}

/* Calls fn(name, is_directory) for each entry of a directory (not . and ..). */
typedef void (*dir_fn)(const char *dir, const char *name, int directory, void *ctx);
static void list_dir(const char *dir, dir_fn fn, void *ctx)
{
#if defined(_WIN32)
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s/*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        fn(dir, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char full[1024];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        fn(dir, e->d_name, is_dir(full), ctx);
    }
    closedir(d);
#endif
}

typedef struct { bundle *b; int profiles_only; const char *folder; } collect_ctx;

static void add_file(bundle *b, const char *full, const char *rel)
{
    bfile *f;
    if (b->count >= MAX_FILES || strlen(rel) >= PATH_MAX_LEN) return;
    f = &b->files[b->count];
    if (read_file(full, &f->data, &f->size)) return;
    snprintf(f->path, sizeof f->path, "%s", rel);
    b->count++;
}

static void collect_in_folder(const char *dir, const char *name, int directory, void *vctx)
{
    collect_ctx *c = (collect_ctx *)vctx;
    char full[1024], rel[PATH_MAX_LEN + 8];
    if (directory) return;                              /* profiles hold files only */
    snprintf(full, sizeof full, "%s/%s", dir, name);
    snprintf(rel, sizeof rel, "%s/%s", c->folder, name);
    add_file(c->b, full, rel);
}

static int folder_is_options(const char *dir)
{
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s/Options", dir);
    f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void collect_top(const char *dir, const char *name, int directory, void *vctx)
{
    collect_ctx *c = (collect_ctx *)vctx;
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (!directory) {
        if (!c->profiles_only) add_file(c->b, full, name);          /* TitleMeta.xbx, TitleImage.xbx */
        return;
    }
    if (c->profiles_only && folder_is_options(full)) return;       /* the host's options win */
    {
        collect_ctx inner = *c;
        inner.folder = name;
        list_dir(full, collect_in_folder, &inner);
    }
}

static void collect(const char *userdata, int profiles_only, bundle *out)
{
    collect_ctx c;
    out->count = 0;
    c.b = out; c.profiles_only = profiles_only; c.folder = "";
    if (is_dir(userdata)) list_dir(userdata, collect_top, &c);
}

static int folder_of(const char *path, char *out, size_t n)
{
    const char *slash = strchr(path, '/');
    if (!slash) { out[0] = 0; return 0; }
    if ((size_t)(slash - path) >= n) return 0;
    memcpy(out, path, (size_t)(slash - path));
    out[slash - path] = 0;
    return 1;
}

static int bundle_has_folder(const bundle *b, int upto, const char *folder)
{
    int i;
    char f[PATH_MAX_LEN];
    for (i = 0; i < upto; i++)
        if (folder_of(b->files[i].path, f, sizeof f) && !strcmp(f, folder)) return 1;
    return 0;
}

/* Moves src's profile folders that dst lacks into dst; returns how many folders were added.
 * A bundle lists each folder's files together. */
static int merge(bundle *dst, bundle *src, const char *who)
{
    int i, added = 0, skip = 0, before = dst->count;
    char cur[PATH_MAX_LEN] = "", f[PATH_MAX_LEN];
    for (i = 0; i < src->count; i++) {
        if (!folder_of(src->files[i].path, f, sizeof f)) continue;     /* only profile folders */
        if (strcmp(f, cur)) {
            snprintf(cur, sizeof cur, "%s", f);
            skip = bundle_has_folder(dst, before, f);
            if (skip) np_say("%s: ya hay un perfil con ese mismo nombre en la partida; se queda el primero", who);
            else added++;
        }
        if (skip || dst->count >= MAX_FILES) continue;
        dst->files[dst->count++] = src->files[i];
        src->files[i].data = NULL;
    }
    return added;
}

static int safe_rel(const char *p)
{
    if (!*p || p[0] == '/' || p[0] == '\\' || strchr(p, ':') || strstr(p, "..") || strchr(p, '\\')) return 0;
    return strchr(p, '/') == strrchr(p, '/');           /* at most "folder/file" */
}

static void remove_tree2(const char *dir, const char *name, int directory, void *ctx)
{
    char full[1024];
    (void)ctx;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (directory) {
        list_dir(full, remove_tree2, NULL);
#if defined(_WIN32)
        _rmdir(full);
#else
        rmdir(full);
#endif
    } else remove(full);
}

static int write_bundle(const bundle *b, const char *userdata)
{
    int i;
    if (is_dir(userdata)) list_dir(userdata, remove_tree2, NULL);   /* the previous session's copy */
    if (np_mkdirs(userdata)) { fail("cannot create %s", userdata); return -1; }
    for (i = 0; i < b->count; i++) {
        char full[1024], f[PATH_MAX_LEN];
        if (!safe_rel(b->files[i].path)) continue;
        if (folder_of(b->files[i].path, f, sizeof f)) {
            char d[1024];
            snprintf(d, sizeof d, "%s/%s", userdata, f);
            make_dir(d);
        }
        snprintf(full, sizeof full, "%s/%s", userdata, b->files[i].path);
        if (write_file(full, b->files[i].data, b->files[i].size)) { fail("cannot write %s", full); return -1; }
    }
    return 0;
}

static int count_profiles(const bundle *b)
{
    int i, n = 0;
    char last[PATH_MAX_LEN] = "", f[PATH_MAX_LEN];
    for (i = 0; i < b->count; i++)
        if (folder_of(b->files[i].path, f, sizeof f) && strcmp(f, last)) {
            snprintf(last, sizeof last, "%s", f);
            if (strstr(b->files[i].path, "/Options")) continue;
            n++;
        }
    return n;
}

/* ---- serialisation ---------------------------------------------------------------------- */

static uint8_t *serialize(const bundle *b, uint32_t *len)
{
    uint32_t n = 8, i;
    uint8_t *buf, *p;
    for (i = 0; i < (uint32_t)b->count; i++) n += 2 + (uint32_t)strlen(b->files[i].path) + 4 + b->files[i].size;
    buf = (uint8_t *)malloc(n);
    if (!buf) return NULL;
    p = buf;
    memcpy(p, "DJSB", 4); p += 4;
    p[0] = (uint8_t)b->count; p[1] = (uint8_t)(b->count >> 8); p[2] = p[3] = 0; p += 4;
    for (i = 0; i < (uint32_t)b->count; i++) {
        uint32_t pl = (uint32_t)strlen(b->files[i].path), s = b->files[i].size;
        p[0] = (uint8_t)pl; p[1] = (uint8_t)(pl >> 8); p += 2;
        memcpy(p, b->files[i].path, pl); p += pl;
        p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)(s >> 16); p[3] = (uint8_t)(s >> 24); p += 4;
        if (s) memcpy(p, b->files[i].data, s);
        p += s;
    }
    *len = n;
    return buf;
}

static int deserialize(const uint8_t *buf, uint32_t len, bundle *b)
{
    const uint8_t *p = buf, *end = buf + len;
    uint32_t count, i;
    b->count = 0;
    if (len < 8 || memcmp(p, "DJSB", 4)) return -1;
    count = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
    p += 8;
    for (i = 0; i < count && b->count < MAX_FILES; i++) {
        uint32_t pl, s;
        bfile *f = &b->files[b->count];
        if (p + 2 > end) return -1;
        pl = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;
        if (pl >= PATH_MAX_LEN || p + pl + 4 > end) return -1;
        memcpy(f->path, p, pl); f->path[pl] = 0; p += pl;
        s = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); p += 4;
        if (p + s > end) return -1;
        f->data = (uint8_t *)malloc(s ? s : 1);
        if (!f->data) return -1;
        if (s) memcpy(f->data, p, s);
        f->size = s;
        p += s;
        b->count++;
    }
    return 0;
}

/* ---- sockets --------------------------------------------------------------------------- */

static void net_init(void)
{
#if defined(_WIN32)
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

static void set_timeout(lsock s, int ms)
{
#if defined(_WIN32)
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval t;
    t.tv_sec = ms / 1000; t.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
}

static int send_all(lsock s, const void *data, uint32_t n)
{
    const char *p = (const char *)data;
    while (n) {
        int k = (int)send(s, p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (k <= 0) return -1;
        p += k; n -= (uint32_t)k;
    }
    return 0;
}

static int recv_all(lsock s, void *data, uint32_t n)
{
    char *p = (char *)data;
    while (n) {
        int k = (int)recv(s, p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (k <= 0) return -1;
        p += k; n -= (uint32_t)k;
    }
    return 0;
}

static int send_frame(lsock s, uint32_t type, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
    uint8_t h[8];
    uint32_t len = alen + blen;
    h[0] = (uint8_t)type; h[1] = (uint8_t)(type >> 8); h[2] = (uint8_t)(type >> 16); h[3] = (uint8_t)(type >> 24);
    h[4] = (uint8_t)len; h[5] = (uint8_t)(len >> 8); h[6] = (uint8_t)(len >> 16); h[7] = (uint8_t)(len >> 24);
    if (send_all(s, h, 8)) return -1;
    if (alen && send_all(s, a, alen)) return -1;
    if (blen && send_all(s, b, blen)) return -1;
    return 0;
}

static uint8_t *recv_frame(lsock s, uint32_t *type, uint32_t *len)
{
    uint8_t h[8], *buf;
    if (recv_all(s, h, 8)) return NULL;
    *type = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    *len = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    if (*len > MAX_BUNDLE + 256) return NULL;
    buf = (uint8_t *)malloc(*len ? *len : 1);
    if (!buf) return NULL;
    if (*len && recv_all(s, buf, *len)) { free(buf); return NULL; }
    return buf;
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* ---- host / client ------------------------------------------------------------------------ */

static bundle s_mine, s_merged, s_theirs;

static int run_host(int port, int players, const char *content_id, const char *session_userdata)
{
    lsock listener, clients[NP_MAX_PLAYERS];
    struct sockaddr_in a;
    int joined = 0, i, yes = 1;
    uint8_t *payload;
    uint32_t plen;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == BAD_SOCK) { fail("cannot open a TCP socket"); return -1; }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr *)&a, sizeof a) || listen(listener, 8)) {
        fail("cannot use TCP port %d (another program has it?)", port);
        sock_close(listener);
        return -1;
    }
    np_say("Esperando a %d jugador(es) en el puerto %d...", players - 1, port);
    while (joined < players - 1) {
        lsock c = accept(listener, NULL, NULL);
        uint32_t type, len;
        uint8_t *msg;
        char id[NP_ID_MAX + 1], name[NP_NAME_MAX + 1];
        if (c == BAD_SOCK) { sleep_ms(50); continue; }
        set_timeout(c, 30000);
        msg = recv_frame(c, &type, &len);
        if (!msg || type != T_HELLO || len < NP_ID_MAX + NP_NAME_MAX) { free(msg); sock_close(c); continue; }
        memcpy(id, msg, NP_ID_MAX); id[NP_ID_MAX] = 0;
        memcpy(name, msg + NP_ID_MAX, NP_NAME_MAX); name[NP_NAME_MAX] = 0;
        if (strcmp(id, content_id)) {
            const char *why = "different game build (both need the same version)";
            send_frame(c, T_REJECT, why, (uint32_t)strlen(why), NULL, 0);
            np_say("%s tried to join with a different version of the game", name);
            free(msg); sock_close(c);
            continue;
        }
        if (deserialize(msg + NP_ID_MAX + NP_NAME_MAX, len - NP_ID_MAX - NP_NAME_MAX, &s_theirs)) {
            np_say("%s sent damaged profiles; joining without them", name);
            bundle_free(&s_theirs);
        }
        {
            int added = merge(&s_merged, &s_theirs, name);
            np_say("%s conectado (%d de %d), trae %d perfil(es)", name, joined + 2, players, added);
        }
        bundle_free(&s_theirs);
        free(msg);
        clients[joined++] = c;
    }
    sock_close(listener);
    payload = serialize(&s_merged, &plen);
    if (!payload) { fail("out of memory"); return -1; }
    for (i = 0; i < joined; i++) {
        if (send_frame(clients[i], T_MERGED, payload, plen, NULL, 0))
            np_say("could not send the profiles to a player");
#if defined(_WIN32)
        shutdown(clients[i], SD_SEND);
#else
        shutdown(clients[i], SHUT_WR);
#endif
    }
    sleep_ms(300);
    for (i = 0; i < joined; i++) sock_close(clients[i]);
    free(payload);
    return write_bundle(&s_merged, session_userdata);
}

static int run_client(const char *address, int port, const char *content_id, const char *name,
                      const char *session_userdata)
{
    struct addrinfo hints, *res = NULL;
    char service[16];
    uint8_t hello[NP_ID_MAX + NP_NAME_MAX], *payload, *msg;
    uint32_t plen, type, len;
    lsock s = BAD_SOCK;
    time_t start = time(NULL);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(address, service, &hints, &res) || !res) { fail("cannot find the host '%s'", address); return -1; }
    np_say("Conectando con %s:%d...", address, port);
    while (s == BAD_SOCK) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != BAD_SOCK && connect(s, res->ai_addr, (int)res->ai_addrlen)) { sock_close(s); s = BAD_SOCK; }
        if (s == BAD_SOCK) {
            if (time(NULL) - start > 180) { freeaddrinfo(res); fail("the host did not answer (is it hosting? is Radmin connected?)"); return -1; }
            sleep_ms(1000);
        }
    }
    freeaddrinfo(res);
    set_timeout(s, 600000);         /* the host may wait a while for the others */
    memset(hello, 0, sizeof hello);
    snprintf((char *)hello, NP_ID_MAX, "%s", content_id);
    snprintf((char *)hello + NP_ID_MAX, NP_NAME_MAX, "%s", name);
    payload = serialize(&s_mine, &plen);
    if (!payload || send_frame(s, T_HELLO, hello, sizeof hello, payload, plen)) {
        free(payload); sock_close(s); fail("lost the connection to the host"); return -1;
    }
    free(payload);
    np_say("Conectado. Esperando a que se unan los demas...");
    msg = recv_frame(s, &type, &len);
    sock_close(s);
    if (!msg) { fail("lost the connection to the host"); return -1; }
    if (type == T_REJECT) {
        char why[200];
        uint32_t n = len < sizeof why - 1 ? len : (uint32_t)sizeof why - 1;
        memcpy(why, msg, n); why[n] = 0;
        free(msg);
        fail("the host refused: %s", why);
        return -1;
    }
    if (type != T_MERGED || deserialize(msg, len, &s_merged)) { free(msg); fail("damaged profiles from the host"); return -1; }
    free(msg);
    return write_bundle(&s_merged, session_userdata);
}

int np_lobby_run(int host, const char *address, int port, int players, const char *content_id,
                 const char *name, const char *my_userdata, const char *session_userdata)
{
    int r;
    net_init();
    s_error[0] = 0;
    if (host) {
        collect(my_userdata, 0, &s_merged);         /* the host's whole save area, options included */
        np_say("Tus perfiles: %d", count_profiles(&s_merged));
        r = run_host(port, players, content_id, session_userdata);
    } else {
        collect(my_userdata, 1, &s_mine);           /* only profiles */
        np_say("Perfiles que llevas: %d", count_profiles(&s_mine));
        r = run_client(address, port, content_id, name, session_userdata);
        bundle_free(&s_mine);
    }
    if (!r) np_say("Perfiles en la partida: %d", count_profiles(&s_merged));
    bundle_free(&s_merged);
    return r;
}
