#include <stdio.h>
#include "csapp.h"

void cache_init();
void *thread_routine(void *connfdp);
void doit(int connfd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio);

// main function
// 프록시 서버 과제의 sequantial 및 concurrent와 관련된 주석은 생략함.
int main(int argc, char **argv)
{
    int listenfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    cache_init(); 
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(clientaddr);
        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread_routine, connfdp);
    }
    return 0;
}

void *thread_routine(void *connfdp)
{
    int connfd = *((int *)connfdp);
    Pthread_detach(pthread_self());
    Free(connfdp);
    doit(connfd);
    Close(connfd);
}

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
// 오브젝트 최대갯수는 캐시용량과 오브젝트 용량으로 결정됨
#define MAX_OBJECT_NUM ((int)(MAX_CACHE_SIZE / MAX_OBJECT_SIZE))

typedef struct 
{
    char cache_obj[MAX_OBJECT_SIZE];
    char cache_uri[MAXLINE];
    int order; // LRU order
    int alloc, read;
    // read 및 write 읽기 및 쓰기 권한 관련 세마포어 선언
    sem_t ws, rs;
} cache_block;

typedef struct
{
    cache_block cacheOBJ[MAX_OBJECT_NUM];

} Cache;

// cache 구조체 선언
Cache cache;

// 캐시 블록 별 인자들 초기화
void cache_init()
{
    int index = 0;
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        cache.cacheOBJ[index].order = 0; // 캐시에 새로운 내용을 덮어씌울 때 사용한지 가장 오래된 index를 찾기 위한 인자
        cache.cacheOBJ[index].alloc = 0; // 해당 블록의 할당 여부를 판단하기 위한 인자
        Sem_init(&cache.cacheOBJ[index].ws, 0, 1); // 해당 블록의 쓰기 권한 관련 세마포어
        Sem_init(&cache.cacheOBJ[index].rs, 0, 1); // 해당 블록의 읽기 권한 관련 세마포어
        cache.cacheOBJ[index].read = 0; // 현재 블록을 읽고 있는 쓰레드의 숫자
    }
}

// 캐시를 읽기 전 세마포어를 확인하여 타 스레드로부터 보호함
void readstart(int index)
{   
    // 쓰기 권한을 확인하는 과정에서 타 쓰레드에 의해 read가 변동되는 것을 방지하기 위해 읽기 권한을 제한함
    P(&cache.cacheOBJ[index].rs); 
    cache.cacheOBJ[index].read += 1;
    // +1한 값이 1이라면 현재 해당 캐시블록을 읽고 있는 쓰레드가 없어 타 쓰레드가 write를 위해 접근할 수 있음.
    // 따라서, 해당 블록의 쓰기 권한을 제한함
    if (cache.cacheOBJ[index].read == 1)
        P(&cache.cacheOBJ[index].ws);
    V(&cache.cacheOBJ[index].rs); // 쓰기 권한 부여
}

// readstart의 역연산
void readend(int index)
{
    P(&cache.cacheOBJ[index].rs);
    cache.cacheOBJ[index].read -= 1;
    // 현재 read 값에서 1을 뺀 값이 0인 경우, 현재 이 블록을 읽고 있는 쓰레드가 자신 밖에 없으므로
    // 해당 블록의 쓰기 권한을 다시 부여해줌
    if (cache.cacheOBJ[index].read == 0)
        V(&cache.cacheOBJ[index].ws);
    V(&cache.cacheOBJ[index].rs);
}

// 필요한 정보를 담은 캐시가 존재하는지 확인하고 있다면 인덱스를 리턴함.
int cache_find(char *uri)
{
    int index = 0;
    // 전체 캐시 블록에 대해 탐색함
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        // 탐색 전 해당 인덱스의 쓰기 권한을 결정하기 위한 함수
        readstart(index);
        // 해당 블록이 할당 상태이고, 목표하는 정보를 갖고 있을 경우 캐시 히트이므로 break
        if (cache.cacheOBJ[index].alloc && (strcmp(uri, cache.cacheOBJ[index].cache_uri) == 0))
            break;
        readend(index);
    }
    // 캐시 미스일 경우 -1 리턴
    if (index == MAX_OBJECT_NUM)
        return -1;
    // 캐시 히트일 경우 해당 인덱스 리턴
    return index;
}

// 빈 캐시, 혹은 사용한지 가장 오래된 캐시 차출
int cache_eviction()
{
    //minorder는 MAX 값에서 자신보다 작은 값으로 계속 갱신됨
    int minorder = MAX_OBJECT_NUM + 1;
    int minindex = 0;
    int index = 0;
    // 모든 index를 탐색하며 비교
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        readstart(index);
        // 할당되지 않은 블록을 발견하면 탐색을 중단하고 index를 return
        if (!cache.cacheOBJ[index].alloc)
        {
            readend(index);
            return index;
        }
        // 빈 캐시를 발견하지 못하는 동안 minorder보다 작을 경우 minorder 및 해당 minindex 갱신.
        if (cache.cacheOBJ[index].order < minorder)
        {
            minindex = index;
            minorder = cache.cacheOBJ[index].order;
        }
    readend(index);
    }
    // 빈 캐시가 존재하지 않을 경우 order가 가장 낮은 값의 index를 리턴함
    return minindex;
}

// LRU order를 재정렬하는 함수
void cache_reorder(int target)
{
    // 방금 쓴 target index의 order를 최대값으로 초기화
    cache.cacheOBJ[target].order = MAX_OBJECT_NUM + 1;
    int index = 0;
    // 타겟값을 제외하고 모든 index의 order에서 1을 빼줌
    for (; index < MAX_OBJECT_NUM; index += 1)
    {
        if (index != target) // index가 target이 아닌 경우
        {
            P(&cache.cacheOBJ[index].ws);
            cache.cacheOBJ[index].order -= 1;
            V(&cache.cacheOBJ[index].ws);
        }
    }
}

// cache_eviction으로 차출된 캐시에 uri와 buf를 저장
void cache_uri(char *uri, char *buf)
{
    // 받아온 인자를 캐시에 저장하기 위해 할당되지 않은 블록 혹은 사용한지 가장 오래된 블록을 차출함
    int index = cache_eviction();
    // 해당 캐시 블록에 인자값 저장하기 전에 타 쓰레드의 쓰기 권한 제한
    P(&cache.cacheOBJ[index].ws);
    // buf, uri 값 copy
    strcpy(cache.cacheOBJ[index].cache_obj, buf);
    strcpy(cache.cacheOBJ[index].cache_uri, uri);
    cache.cacheOBJ[index].alloc = 1; // 할당된 상태로 수정
    // LRU order 재정렬
    cache_reorder(index);
    V(&cache.cacheOBJ[index].ws);
}

void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char HTTPheader[MAXLINE], hostname[MAXLINE], path[MAXLINE];
    int EndServerfd;
    rio_t rio, serv_rio;
    
    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement this method\n");
        return;
    }
    char uri_store[MAX_OBJECT_SIZE];
    strcpy(uri_store, uri);
    int cache_index;

    // 캐시에 해당 url이 존재하는지 확인
    if ((cache_index = cache_find(uri_store)) != -1)
    {
        // 캐시 적중 시 클라이언트한테 보내고 doit 종료
        readstart(cache_index);
        Rio_writen(connfd, cache.cacheOBJ[cache_index].cache_obj, strlen(cache.cacheOBJ[cache_index].cache_obj));
        readend(cache_index);
        return;
    }
    int port;

    // 캐시 미스일 경우, 기존의 프록시 서버와 동일하게 진행함.
    parse_uri(uri, hostname, path, &port);
    makeHTTPheader(HTTPheader, hostname, path, port, &rio);

    char portch[10];
    sprintf(portch, "%d", port);
    
    EndServerfd = Open_clientfd(hostname, portch);
    if(EndServerfd < 0)
    {
        printf("connection failed\n");
        return;
    }
    Rio_readinitb(&serv_rio, EndServerfd);
    Rio_writen(EndServerfd, HTTPheader, strlen(HTTPheader));
    
    char cachebuf[MAX_OBJECT_SIZE];
    size_t sizerecvd, sizebuf = 0;

    // 서버에서 받은 응답을 한줄씩 확인하여 캐시 블록 크기 내 범위에서 캐시에 저장함.
    while((sizerecvd = Rio_readlineb(&serv_rio, buf, MAXLINE)) != 0)
    {
        // 누적된 sizebuf가 MAX_OBJECT_SIZE보다 작으면 cachebuf에 이어 붙임
        sizebuf = sizebuf + sizerecvd;
        if (sizebuf < MAX_OBJECT_SIZE)
        {
            strcat(cachebuf, buf);
        }
        printf("proxy received %d bytes, then send\n", sizerecvd);
        // cache 크기와 관계없이 서버로부터 받은 응답은 모두 클라이언트에게 전송
        Rio_writen(connfd, buf, sizerecvd);
    }
    Close(EndServerfd);
    if (sizebuf < MAX_OBJECT_SIZE)
    {
        // sizebuf가 MAX_OBJECT_SIZE보다 작을 경우만 캐시에 저장함
        cache_uri(uri_store, cachebuf);
    }
}


int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    *port = 8000;
    char *hostnameP = strstr(uri, "//");
    if (hostnameP != NULL)
    {
        hostnameP = hostnameP + 2;
    }
    else
    {
        hostnameP = uri;
    }
    char *pathP = strstr(hostnameP, ":");
    if(pathP != NULL)
    {
        *pathP = '\0';
        sscanf(hostnameP, "%s", hostname);
        sscanf(pathP + 1, "%d%s", port, path);
    }
    else
    {
        pathP = strstr(hostnameP, "/");
        if(pathP != NULL)
        {
            *pathP = '\0';
            sscanf(hostnameP, "%s", hostname);
            *pathP = '/';
            sscanf(pathP, "%s", path);
        }
        else
        {
            sscanf(hostnameP, "%s", hostname);
        }
    }
    return 0;
}

static const char *user_agent_header = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_header = "Connection: close\r\n";
static const char *prox_header = "Proxy-Connection: close\r\n";
static const char *host_header_format = "Host: %s\r\n";
static const char *requestlint_header_format = "GET %s HTTP/1.0\r\n";
static const char *endof_header = "\r\n";
static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_header[MAXLINE], other_header[MAXLINE], host_header[MAXLINE];
    sprintf(request_header, requestlint_header_format, path);
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        if(strcmp(buf, endof_header) == 0)
        {
            break;
        }
        if(!strncasecmp(buf, host_key, strlen(host_key)))
        {
            strcpy(host_header, buf);
            continue;
        }
        if(!strncasecmp(buf, connection_key, strlen(connection_key))
                &&!strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
                &&!strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_header, buf);
        }
    }
    if(strlen(host_header) == 0)
    {
        sprintf(host_header, host_header_format, hostname);
    }
    sprintf(HTTPheader, "%s%s%s%s%s%s%s", request_header, host_header, conn_header, prox_header, user_agent_header, other_header, endof_header);
}