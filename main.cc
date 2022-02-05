#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <curl/curl.h>
#include <utility>
#include <string>
#include <list>
#include <memory>
#include <map>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <vector>

struct tunnel {
    std::string dest;
    uint16_t port;

    tunnel() : port(0)
    {}
};

struct clientinfo {
    sockaddr_in addr;
    int fd;

    tunnel dest;
    int destfd;

    bool post;
    std::string host;
    std::string url;
    std::string resp4url;

    CURL* curl;
    curl_slist* curl_headers;

    clientinfo() : 
        fd(-1),
        destfd(-1),
        post(false),
        curl(nullptr),
        curl_headers(nullptr)
    {
        bzero(&addr, sizeof(sockaddr_in));
    }

    char* str(const pollfd& ev);
    char* str(const int fd);
    void close();
};

struct _opt {
    bool verbose;
    std::unordered_map<int, std::shared_ptr<clientinfo>> fdmap;

    _opt() : 
        verbose(false)
    {}
} g_opt;

char* clientinfo::str(const int evfd)
{
    static char buff[256];

    if(evfd == fd)
        snprintf(buff, 256, "client(fd:%d)[%s:%d]", fd, 
                 inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    else
        snprintf(buff, 256, "tunnel(fd:%d)[%s:%d]", destfd,
                 dest.dest.c_str(), dest.port);

    return buff;
}

char* clientinfo::str(const pollfd& ev)
{
    return str(ev.fd);
}

void clientinfo::close()
{
    ::close(fd);

    if(destfd != -1)
    {
        ::close(destfd);
        g_opt.fdmap.erase(destfd);
    }

    g_opt.fdmap.erase(fd);
}

size_t curl_write_func(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t realsize = size * nmemb;

    clientinfo* client_ptr = (clientinfo*)userdata;
    client_ptr->resp4url.append(ptr, realsize);

    return realsize;
}

int process_http_request(std::shared_ptr<clientinfo> cli, char* request, char** body)
{
/*
 * CONNECT youtube.com:443 HTTP/1.1
 * Host: youtube.com
 * Proxy-Connection: keep-alive
 * Connection: keep-alive
 *
 * GET http://i2.hdslb.com/bfs/archive/be98ca239c143fb0d07f479e60483000745086c5.jpg@1440w_810h_1e.webp HTTP/1.1
 * Host: i2.hdslb.com
 * Proxy-Connection: keep-alive
 * User-Agent: bili-hd2/33400100 CFNetwork/1329 Darwin/21.3.0 os/ios model/iPad Pro 12.9-Inch 5G mobi_app/ipad osVer/15.3 network/2
 * Accept-Encoding: gzip, deflate
 */
    cli->url.clear();
    cli->resp4url.clear();

    if(cli->curl != nullptr)
    {
        curl_easy_cleanup(cli->curl);
        cli->curl = nullptr;
    }

    if(cli->curl_headers != nullptr)
    {
        curl_slist_free_all(cli->curl_headers);
        cli->curl_headers = nullptr;
    }

    char* offset = request;
    char* endofline = nullptr;
    while((endofline = strstr(offset, "\r\n")) != nullptr && endofline != offset)
    {
        endofline[0] = 0;

        if(strncasecmp(offset, "CONNECT ", 8) == 0)
        {
            int i = 8;
            while(offset[i] != ':' && offset[i] != ' ')
            {
                cli->dest.dest.push_back(offset[i]);
                ++i;
            }

            if(offset[i] == ' ')
                cli->dest.port = 80;
            else
            {
                ++i;
                char* s_port = &offset[i];
                while(offset[i] != ' ')
                    ++i;

                offset[i] = 0;
                cli->dest.port = strtoul(s_port, NULL, 10);
            }
        }
        else if(strncasecmp(offset, "GET ", 4) == 0 || strncasecmp(offset, "POST ", 5) == 0)
        {
            int i = 4;
            cli->post = false;
            if(strncasecmp(offset, "POST ", 5) == 0)
            {
                cli->post = true;
                i = 5;
            }

            while(offset[i] != ' ')
            {
                cli->url.push_back(offset[i]);
                ++i;
            }

            cli->curl = curl_easy_init();
            if(cli->curl == nullptr)
                return -1;

            if(cli->post && CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POST, 1L))
                return -1;

            if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_HEADER, 1L))
                return -1;

            if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_WRITEFUNCTION, curl_write_func))
                return -1;

            clientinfo* client_ptr = cli.get();
            if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_WRITEDATA, client_ptr))
                return -1;
        }
        else if(cli->curl != nullptr)
        {
            if(strncasecmp(offset, "Host: ", 6) == 0)
                cli->host = offset + 6;
            
            cli->curl_headers = curl_slist_append(cli->curl_headers, offset);
        }

        offset = endofline + 2;
    }

    if(endofline == nullptr)
        return -1;

    if(body != nullptr)
        *body = endofline + 2;

    if(cli->curl != nullptr && CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_HTTPHEADER, cli->curl_headers))
        return -1;

    return 0;
}

int tunnel_connect(const pollfd& ev, ::std::shared_ptr<clientinfo> cli,
                   char* cachebuf, ssize_t rc, char* body)
{
    size_t headlen = body - cachebuf;
    size_t bodylen = rc - headlen;

    if(cli->dest.dest.empty())
    {
        fprintf(stderr, "ERR | %s parse proxy request fail: %s\n", cli->str(ev), cachebuf);
        cli->close();
        return -1;
    }

    g_opt.verbose && fprintf(stderr, "INF | %s connect to [%s:%d] ...\n", 
                             cli->str(ev), cli->dest.dest.c_str(), cli->dest.port);

    cli->destfd = socket(PF_INET, SOCK_STREAM, 0);
    if(cli->destfd == -1)
    {
        fprintf(stderr, "ERR | %s create dest fd fail, errno:%d -> %s\n", 
                cli->str(ev), errno, strerror(errno));
        cli->close();
        return -1;
    }

    g_opt.fdmap[cli->destfd] = cli;

    hostent* host = gethostbyname(cli->dest.dest.c_str());
    if(NULL == host || host->h_addr_list[0] == nullptr)
    {
        fprintf(stderr, "ERR | %s get host(%s) fail, errno:%d -> %s\n", 
                cli->str(ev), cli->dest.dest.c_str(), errno, strerror(errno));
        cli->close();
        return -1;
    }

    sockaddr_in destaddr;
    destaddr.sin_family = PF_INET;
    destaddr.sin_port = htons(cli->dest.port);
    destaddr.sin_addr = *(in_addr*)host->h_addr_list[0];

    if(-1 == connect(cli->destfd, (const sockaddr*)&destaddr, sizeof(sockaddr_in)))
    {
        fprintf(stderr, "ERR | %s connect to ", cli->str(ev));
        fprintf(stderr, "%s fail, errno:%d -> %s\n", 
                cli->str(cli->destfd), errno, strerror(errno));
        cli->close();
        return -1;
    }

    g_opt.verbose && fprintf(stderr, "INF | %s connect to ", cli->str(ev));
    g_opt.verbose && fprintf(stderr, "%s success\n", cli->str(cli->destfd));

    if(bodylen > 0)
    {
        ssize_t sc = send(cli->destfd, body, bodylen, 0);
        if(-1 == sc)
        {
            fprintf(stderr, "ERR | %s send to ", cli->str(ev));
            fprintf(stderr, "%s fail, errno:%d -> %s\n", 
                    cli->str(cli->destfd), errno, strerror(errno));
            cli->close();
            return -1;
        }

        g_opt.verbose && fprintf(stderr, "INF | %s send(rc:%ld, head:%ld, body:%ld, sc:%ld) to ", 
                                 cli->str(ev), rc, headlen, bodylen, sc);
        g_opt.verbose && fprintf(stderr, "%s\n", cli->str(cli->destfd));
    }

    const char* resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
    ssize_t sc = send(cli->fd, resp, strlen(resp), 0);
    if(-1 == sc)
    {
        fprintf(stderr, "ERR | resp to %s connection established fail, errno:%d -> %s\n", 
                cli->str(cli->fd), errno, strerror(errno));
        cli->close();
        return -1;
    }

    return 0;
}

int proxy_url(const pollfd& ev, std::shared_ptr<clientinfo> cli,
              char* cachebuf, ssize_t rc, char* body)
{
    if(cli->post)
    {
        cli->curl_headers = curl_slist_append(cli->curl_headers, "Expect:");

        if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POSTFIELDSIZE, rc - (body - cachebuf)))
        {
            fprintf(stderr, "ERR | %s set CURLOPT_POSTFIELDSIZE fail <- %s\n", 
                    cli->str(ev), cli->url.c_str());
            return -1;
        }

        if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_POSTFIELDS, body))
        {
            fprintf(stderr, "ERR | %s set CURLOPT_POSTFIELDS fail <- %s\n", 
                    cli->str(ev), cli->url.c_str());
            return -1;
        }
    }

    if(strncasecmp(cli->url.c_str(), "http://", 7) != 0 &&
       strncasecmp(cli->url.c_str(), "https://", 8) != 0)
    {
        std::string uri = "http://";
        uri.append(cli->host);
        uri.append(cli->url);

        cli->url = uri;
    }

    if(CURLE_OK != curl_easy_setopt(cli->curl, CURLOPT_URL, cli->url.c_str()))
    {
        fprintf(stderr, "ERR | %s set CURLOPT_URL fail <- %s\n", 
                cli->str(ev), cli->url.c_str());
        return -1;
    }

    g_opt.verbose && fprintf(stderr, "INF | %s %s -> %s ...\n", 
                             cli->post?"POST":"GET",
                             cli->str(ev), cli->url.c_str());

    CURLcode retcode = curl_easy_perform(cli->curl);
    if(CURLE_OK != retcode) 
    {
        fprintf(stderr, "ERR | %s download fail <- %s\n", cli->str(ev), cli->url.c_str());
        return -1;
    }

    long http_code = 0;
    retcode = curl_easy_getinfo(cli->curl, CURLINFO_RESPONSE_CODE, &http_code);

    g_opt.verbose && fprintf(stderr, "INF | %s http_code:%ld <- %s ...\n", 
                             cli->str(ev), http_code, cli->url.c_str());

    ssize_t sc = send(cli->fd, cli->resp4url.c_str(), cli->resp4url.size(), 0);
    if(-1 == sc)
    {
        fprintf(stderr, "ERR | %s resp to %s fail, errno:%d -> %s\n", 
                cli->url.c_str(), cli->str(cli->fd), errno, strerror(errno));
        cli->close();
        return -1;
    }

    return 0;
}

int recv_client(const pollfd& ev)
{
    auto iter = g_opt.fdmap.find(ev.fd);
    if(iter == g_opt.fdmap.end())
    {
        fprintf(stderr, "ERR | not found client fd(%d)\n", ev.fd);
        close(ev.fd);
        return -1;
    }

    ::std::shared_ptr<clientinfo> cli = iter->second;

    static std::string cachebuf(4096, 0);
    ssize_t rc = recv(ev.fd, (char*)cachebuf.data(), 4096, 0);
    if(-1 == rc)
    {
        if(errno != ECONNRESET)
        {
            fprintf(stderr, "ERR | %s recv fail, errno:%d -> %s\n", 
                    cli->str(ev), errno, strerror(errno));
        }

        cli->close();
        return -1;
    }
    else if(0 == rc)
    {
        g_opt.verbose && fprintf(stderr, "INF | %s disconnected\n", cli->str(ev));
        cli->close();
        return 0;
    }

    g_opt.verbose && fprintf(stderr, "INF | %s recv(rc:%ld)\n", cli->str(ev), rc);

    if(cli->destfd > 0)
    {
        int fd = cli->destfd;
        if(ev.fd == cli->destfd)
            fd = cli->fd;

        ssize_t sc = send(fd, cachebuf.data(), rc, 0);
        if(-1 == sc)
        {
            fprintf(stderr, "ERR | %s send to ", cli->str(ev));
            fprintf(stderr, "%s fail, errno:%d -> %s\n", cli->str(fd),
                    errno, strerror(errno));
        }
        
        g_opt.verbose && fprintf(stderr, "INF | %s send(rc:", cli->str(ev));
        g_opt.verbose && fprintf(stderr, "%ld, sc:%ld) to %s\n", rc, sc, cli->str(fd));

        return 0;
    }

    // new connect request
    char* body = nullptr;
    if(-1 == process_http_request(cli, (char*)cachebuf.data(), &body))
    {
        fprintf(stderr, "ERR | %s parse http request fail\n", cli->str(ev));
        cli->close();
        return -1;
    }

    if(cli->url.empty())
        return tunnel_connect(ev, cli, (char*)cachebuf.data(), rc, body);

    return proxy_url(ev, cli, (char*)cachebuf.data(), rc, body);
}

int accept_client(int listenfd)
{
    std::shared_ptr<clientinfo> cli(new clientinfo);

    socklen_t cliaddrlen;
    cli->fd = accept(listenfd, (sockaddr*)&cli->addr, &cliaddrlen);
    if(-1 == cli->fd)
    {
        fprintf(stderr, "ERR | accept client fail, errno:%d -> %s\n", errno, strerror(errno));
        return -1;
    }

    g_opt.verbose && fprintf(stderr, "INF | client(fd:%d)[%s:%d] connected\n", 
                             cli->fd, inet_ntoa(cli->addr.sin_addr), 
                             ntohs(cli->addr.sin_port));

    g_opt.fdmap[cli->fd] = cli;
    return 0;
}

int main(int argc, char* argv[])
{
    for(int i=1; i<argc; ++i)
    {
        if(strcmp(argv[i], "-v") == 0)
            g_opt.verbose = true;
        else if(strcmp(argv[i], "-h") == 0)
        {
            fprintf(stderr, "usage: proxy [OPTIONS]\n"
                            "Options:\n"
                            "   -v          show detail log\n"
                            "   -h          help information\n");
            return 0;
        }
    }

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if(listenfd == -1)
    {
        fprintf(stderr, "ERR | create listen socket fail, errno:%d -> %s\n", errno, strerror(errno));
        return -1;
    }

    int val = 1;
    if(-1 == setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int)))
    {
        fprintf(stderr, "ERR | set listen socket reuse addr fail, errno:%d -> %s\n", errno, strerror(errno));
        return -1;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(8080);

    if(-1 == bind(listenfd, (const sockaddr*)&addr, sizeof(sockaddr_in)))
    {
        fprintf(stderr, "ERR | bind listen socket fail, errno:%d -> %s\n", errno, strerror(errno));
        return -1;
    }

    if(-1 == listen(listenfd, 1024))
    {
        fprintf(stderr, "ERR | listen socket fail, errno:%d -> %s\n", errno, strerror(errno));
        return -1;
    }

    g_opt.verbose && fprintf(stderr, "INF | [0.0.0.0:8080] listening ...\n");

    std::vector<pollfd> pollvec;
    pollvec.reserve(4096);
    pollvec.push_back({listenfd, POLLIN, 0});

    uint64_t seq = 0;
    int ready = 0;
    while((ready = poll(pollvec.data(), pollvec.size(), -1)) > 0)
    {
        ++seq;

        g_opt.verbose && fprintf(stderr, "INF | ------------------------ seq[%llu][ready:%d][size:%lu] ---------------------------\n", 
                                 seq, ready, pollvec.size());

        for(int i=0; i<pollvec.size(); ++i)
        {
            pollfd& ev = pollvec.at(i);
            if((ev.revents & POLLIN) != POLLIN) 
                continue;

            if(ev.fd == listenfd)
                accept_client(listenfd);
            else
                recv_client(ev);
        }

        pollvec.clear();

        pollvec.push_back({listenfd, POLLIN, 0});
        for(auto iter=g_opt.fdmap.begin(); iter!=g_opt.fdmap.end(); ++iter)
            pollvec.push_back({iter->first, POLLIN, 0});
    }

    close(listenfd);

    return 0;
}


