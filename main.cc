#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

struct tunnel {
    std::string dest;
    uint16_t port;

    tunnel() : port(0) {}
};

struct clientinfo {
    sockaddr_in addr;
    int fd;

    tunnel dest;
    int destfd;

    pollfd evs[2];

    bool post;
    std::string host;
    std::string url;
    std::string resp4url;

    CURL* curl;
    curl_slist* curl_headers;

    clientinfo()
        : fd(-1),
          destfd(-1),
          post(false),
          curl(nullptr),
          curl_headers(nullptr) {
        bzero(&addr, sizeof(sockaddr_in));
        bzero(evs, sizeof(pollfd) * 2);
    }

    pollfd* events() {
        evs[0].fd = fd;
        evs[0].events = POLLIN;

        if (destfd != -1) {
            evs[1].fd = destfd;
            evs[1].events = POLLIN;
        }

        return evs;
    }

    int evlen() { return (destfd != -1) ? 2 : 1; }

    char* str(const pollfd& ev);
    char* str(const int fd);
    void close();
};

struct _opt {
    bool verbose;

    _opt() : verbose(false) {}
} g_opt;

char* clientinfo::str(const int evfd) {
    static char buff[256];

    if (evfd == fd)
        snprintf(buff, 256, "client(fd:%d)[%s:%d]", fd,
                 inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    else
        snprintf(buff, 256, "tunnel(fd:%d)[%s:%d]", destfd, dest.dest.c_str(),
                 dest.port);

    return buff;
}

void clientinfo::close() {
    ::close(fd);

    if (destfd != -1) ::close(destfd);
}

size_t curl_write_func(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t realsize = size * nmemb;

    clientinfo* client_ptr = (clientinfo*)userdata;
    client_ptr->resp4url.append(ptr, realsize);

    return realsize;
}

int process_http_request(clientinfo* cli, char* request, char** body) {
    /*
     * CONNECT youtube.com:443 HTTP/1.1
     * Host: youtube.com
     * Proxy-Connection: keep-alive
     * Connection: keep-alive
     *
     * GET
     * http://i2.hdslb.com/bfs/archive/be98ca239c143fb0d07f479e60483000745086c5.jpg@1440w_810h_1e.webp
     * HTTP/1.1 Host: i2.hdslb.com Proxy-Connection: keep-alive User-Agent:
     * bili-hd2/33400100 CFNetwork/1329 Darwin/21.3.0 os/ios model/iPad
     * Pro 12.9-Inch 5G mobi_app/ipad osVer/15.3 network/2 Accept-Encoding:
     * gzip, deflate
     */
    cli->url.clear();
    cli->resp4url.clear();

    if (cli->curl != nullptr) {
        curl_easy_cleanup(cli->curl);
        cli->curl = nullptr;
    }

    if (cli->curl_headers != nullptr) {
        curl_slist_free_all(cli->curl_headers);
        cli->curl_headers = nullptr;
    }

    char* offset = request;
    char* endofline = nullptr;
    while ((endofline = strstr(offset, "\r\n")) != nullptr &&
           endofline != offset) {
        endofline[0] = 0;

        if (strncasecmp(offset, "CONNECT ", 8) == 0) {
            int i = 8;
            while (offset[i] != ':' && offset[i] != ' ') {
                cli->dest.dest.push_back(offset[i]);
                ++i;
            }

            if (offset[i] == ' ')
                cli->dest.port = 80;
            else {
                ++i;
                char* s_port = &offset[i];
                while (offset[i] != ' ') ++i;

                offset[i] = 0;
                cli->dest.port = strtoul(s_port, NULL, 10);
            }
        } else if (strncasecmp(offset, "GET ", 4) == 0 ||
                   strncasecmp(offset, "POST ", 5) == 0) {
            int i = 4;
            cli->post = false;
            if (strncasecmp(offset, "POST ", 5) == 0) {
                cli->post = true;
                i = 5;
            }

            while (offset[i] != ' ') {
                cli->url.push_back(offset[i]);
                ++i;
            }

            cli->curl = curl_easy_init();
            if (cli->curl == nullptr) return -1;

            if (cli->post &&
                CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POST, 1L))
                return -1;

            if (CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_HEADER, 1L))
                return -1;

            if (CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_WRITEFUNCTION,
                                             curl_write_func))
                return -1;

            if (CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_WRITEDATA, cli))
                return -1;
        } else if (cli->curl != nullptr) {
            if (strncasecmp(offset, "Host: ", 6) == 0) cli->host = offset + 6;

            cli->curl_headers = curl_slist_append(cli->curl_headers, offset);
        }

        offset = endofline + 2;
    }

    if (endofline == nullptr) return -1;

    if (body != nullptr) *body = endofline + 2;

    if (cli->curl != nullptr &&
        CURLE_OK !=
            curl_easy_setopt(cli->curl, CURLOPT_HTTPHEADER, cli->curl_headers))
        return -1;

    return 0;
}

int tunnel_connect(int evfd, clientinfo* cli, char* cachebuf, ssize_t rc,
                   char* body) {
    size_t headlen = body - cachebuf;
    size_t bodylen = rc - headlen;

    if (cli->dest.dest.empty()) {
        fprintf(stderr, "ERR | %s parse proxy request fail: \"%s\"\n",
                cli->str(evfd), cachebuf);
        cli->close();
        return 0;
    }

    g_opt.verbose&& fprintf(stderr, "INF | %s connect to [%s:%d] ...\n",
                            cli->str(evfd), cli->dest.dest.c_str(),
                            cli->dest.port);

    cli->destfd = socket(PF_INET, SOCK_STREAM, 0);
    if (cli->destfd == -1) {
        fprintf(stderr, "ERR | %s create dest fd fail, errno:%d -> %s\n",
                cli->str(evfd), errno, strerror(errno));
        cli->close();
        return 0;
    }

    hostent* host = gethostbyname(cli->dest.dest.c_str());
    if (NULL == host || host->h_addr_list[0] == nullptr) {
        fprintf(stderr, "ERR | %s get host(%s) fail, errno:%d -> %s\n",
                cli->str(evfd), cli->dest.dest.c_str(), errno, strerror(errno));
        cli->close();
        return 0;
    }

    sockaddr_in destaddr;
    destaddr.sin_family = PF_INET;
    destaddr.sin_port = htons(cli->dest.port);
    destaddr.sin_addr = *(in_addr*)host->h_addr_list[0];

    if (-1 ==
        connect(cli->destfd, (const sockaddr*)&destaddr, sizeof(sockaddr_in))) {
        fprintf(stderr, "ERR | %s connect to ", cli->str(evfd));
        fprintf(stderr, "%s fail, errno:%d -> %s\n", cli->str(cli->destfd),
                errno, strerror(errno));
        cli->close();
        return 0;
    }

    g_opt.verbose&& fprintf(stderr, "INF | %s connect to ", cli->str(evfd));
    g_opt.verbose&& fprintf(stderr, "%s success\n", cli->str(cli->destfd));

    if (bodylen > 0) {
        ssize_t sc = send(cli->destfd, body, bodylen, 0);
        if (-1 == sc) {
            fprintf(stderr, "ERR | %s send to ", cli->str(evfd));
            fprintf(stderr, "%s fail, errno:%d -> %s\n", cli->str(cli->destfd),
                    errno, strerror(errno));
            cli->close();
            return 0;
        }

        g_opt.verbose&& fprintf(
            stderr, "INF | %s send(rc:%ld, head:%ld, body:%ld, sc:%ld) to ",
            cli->str(evfd), rc, headlen, bodylen, sc);
        g_opt.verbose&& fprintf(stderr, "%s\n", cli->str(cli->destfd));
    }

    const char* resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
    ssize_t sc = send(cli->fd, resp, strlen(resp), 0);
    if (-1 == sc) {
        fprintf(
            stderr,
            "ERR | resp to %s connection established fail, errno:%d -> %s\n",
            cli->str(cli->fd), errno, strerror(errno));
        cli->close();
        return 0;
    }

    return 1;
}

int proxy_url(int evfd, clientinfo* cli, char* cachebuf, ssize_t rc,
              char* body) {
    if (cli->post) {
        cli->curl_headers = curl_slist_append(cli->curl_headers, "Expect:");

        if (CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POSTFIELDSIZE,
                                         rc - (body - cachebuf))) {
            fprintf(stderr, "ERR | %s set CURLOPT_POSTFIELDSIZE fail <- %s\n",
                    cli->str(evfd), cli->url.c_str());
            cli->close();
            return 0;
        }

        if (CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POSTFIELDS, body)) {
            fprintf(stderr, "ERR | %s set CURLOPT_POSTFIELDS fail <- %s\n",
                    cli->str(evfd), cli->url.c_str());
            cli->close();
            return 0;
        }
    }

    if (strncasecmp(cli->url.c_str(), "http://", 7) != 0 &&
        strncasecmp(cli->url.c_str(), "https://", 8) != 0) {
        std::string uri = "http://";
        uri.append(cli->host);
        uri.append(cli->url);

        cli->url = uri;
    }

    if (CURLE_OK !=
        curl_easy_setopt(cli->curl, CURLOPT_URL, cli->url.c_str())) {
        fprintf(stderr, "ERR | %s set CURLOPT_URL fail <- %s\n", cli->str(evfd),
                cli->url.c_str());
        cli->close();
        return 0;
    }

    g_opt.verbose&& fprintf(stderr, "INF | %s %s -> %s ...\n",
                            cli->post ? "POST" : "GET", cli->str(evfd),
                            cli->url.c_str());

    CURLcode retcode = curl_easy_perform(cli->curl);
    if (CURLE_OK != retcode) {
        fprintf(stderr, "ERR | %s download fail <- %s\n", cli->str(evfd),
                cli->url.c_str());
        cli->close();
        return 0;
    }

    long http_code = 0;
    retcode = curl_easy_getinfo(cli->curl, CURLINFO_RESPONSE_CODE, &http_code);

    g_opt.verbose&& fprintf(stderr, "INF | %s http_code:%ld <- %s ...\n",
                            cli->str(evfd), http_code, cli->url.c_str());

    ssize_t sc = send(cli->fd, cli->resp4url.c_str(), cli->resp4url.size(), 0);
    if (-1 == sc) {
        fprintf(stderr, "ERR | %s resp to %s fail, errno:%d -> %s\n",
                cli->url.c_str(), cli->str(evfd), errno, strerror(errno));
        cli->close();
        return 0;
    }

    return 1;
}

int recv_client(int evfd, clientinfo* cli) {
    static std::string cachebuf(4096, 0);

    ssize_t rc = recv(evfd, (char*)cachebuf.data(), 4096, 0);
    if (-1 == rc) {
        if (errno != ECONNRESET) {
            fprintf(stderr, "ERR | %s recv fail, errno:%d -> %s\n",
                    cli->str(evfd), errno, strerror(errno));
        }
        cli->close();
        return 0;
    } else if (0 == rc) {
        g_opt.verbose&& fprintf(stderr, "INF | %s disconnected\n",
                                cli->str(evfd));
        cli->close();
        return 0;
    }

    g_opt.verbose&& fprintf(stderr, "INF | %s recv(rc:%ld)\n", cli->str(evfd),
                            rc);

    if (cli->destfd > 0) {
        int tofd = (evfd == cli->destfd) ? cli->fd : cli->destfd;
        ssize_t sc = send(tofd, cachebuf.data(), rc, 0);
        if (-1 == sc) {
            fprintf(stderr, "ERR | %s send to ", cli->str(evfd));
            fprintf(stderr, "%s fail, errno:%d -> %s\n", cli->str(tofd), errno,
                    strerror(errno));
        }

        g_opt.verbose&& fprintf(stderr, "INF | %s send(rc:", cli->str(evfd));
        g_opt.verbose&& fprintf(stderr, "%ld, sc:%ld) to %s\n", rc, sc,
                                cli->str(tofd));
        return 1;
    }

    // new connect request
    char* body = nullptr;
    if (-1 == process_http_request(cli, (char*)cachebuf.data(), &body)) {
        fprintf(stderr, "ERR | %s parse http request fail\n", cli->str(evfd));
        cli->close();
        return 0;
    }

    if (cli->url.empty())
        return tunnel_connect(evfd, cli, (char*)cachebuf.data(), rc, body);

    return proxy_url(evfd, cli, (char*)cachebuf.data(), rc, body);
}

int accept_client(int listenfd) {
    clientinfo cli;

    socklen_t cliaddrlen;
    cli.fd = accept(listenfd, (sockaddr*)&cli.addr, &cliaddrlen);
    if (-1 == cli.fd) {
        fprintf(stderr, "ERR | accept client fail, errno:%d -> %s\n", errno,
                strerror(errno));
        return -1;
    }

    g_opt.verbose&& fprintf(stderr, "INF | client(fd:%d)[%s:%d] connected\n",
                            cli.fd, inet_ntoa(cli.addr.sin_addr),
                            ntohs(cli.addr.sin_port));

    pid_t pid = fork();
    if (-1 == pid) {
        fprintf(stderr, "ERR | create child process fail, errno:%d -> %s\n",
                errno, strerror(errno));
        return -1;
    } else if (pid > 0) {
        // parent process;
        close(cli.fd);
        return 0;
    }

    // child process;
    while (poll(cli.events(), cli.evlen(), -1) > 0) {
        for (int i = 0; i < cli.evlen(); ++i) {
            if ((cli.evs[i].revents & POLLIN) != POLLIN) continue;

            g_opt.verbose&& fprintf(stderr,
                                    "INF | event fd:%d found event:%x\n",
                                    cli.evs[i].fd, cli.evs[i].revents);

            if (0 == recv_client(cli.evs[i].fd, &cli)) exit(0);
        }
    }

    exit(0);
    return 0;
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0)
            g_opt.verbose = true;
        else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "usage: proxy [OPTIONS]\n"
                    "Options:\n"
                    "   -v          show detail log\n"
                    "   -h          help information\n");
            return 0;
        }
    }

    signal(SIGCHLD, SIG_IGN);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        fprintf(stderr, "ERR | create listen socket fail, errno:%d -> %s\n",
                errno, strerror(errno));
        return -1;
    }

    int val = 1;
    if (-1 ==
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int))) {
        fprintf(stderr,
                "ERR | set listen socket reuse addr fail, errno:%d -> %s\n",
                errno, strerror(errno));
        return -1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(8080);

    if (-1 == bind(listenfd, (const sockaddr*)&addr, sizeof(sockaddr_in))) {
        fprintf(stderr, "ERR | bind listen socket fail, errno:%d -> %s\n",
                errno, strerror(errno));
        return -1;
    }

    if (-1 == listen(listenfd, 1024)) {
        fprintf(stderr, "ERR | listen socket fail, errno:%d -> %s\n", errno,
                strerror(errno));
        return -1;
    }

    g_opt.verbose&& fprintf(stderr, "INF | [0.0.0.0:8080] listening ...\n");

    pollfd ev;
    ev.fd = listenfd;
    ev.events = POLLIN;

    while (poll(&ev, 1, -1) > 0) accept_client(listenfd);

    close(listenfd);
    return 0;
}

