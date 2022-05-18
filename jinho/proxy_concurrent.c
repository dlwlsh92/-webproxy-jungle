#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
void *thread_routine(void *connfdp);

// 프록시 서버도 main의 알고리즘, doit의 상단부는 tiny와 같다
int main(int argc, char **argv)
{
    int listenfd, connfd; // 클라이언트의 연결을 들을 listen socket 및 연결된 후 connect socket
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(clientaddr);
        // 각 스레드는 모두 각각의 connfd를 가짐.
        // connfd 값을 저장할 메모리를 할당함
        // 단순히 accept 후 connfd를 피어쓰레드에게 넘겨줄 경우(malloc 없이), 피어쓰레드 할당이 다음 accept 후에 완료된다면 피어 쓰레드의 지역 connfd 변수는 다음 연결의 식별자 번호를 가짐.
        // 결과적으로 두 개의 쓰레드가 동일한 식별자에서 입력과 출력을 수행하게 됨.
        // 따라서, accept에서 리턴되는 각각의 연결 식별자를 자신만의 동적으로 할당된 메모리 블록에 할당해야 함.
        // 또다른 이슈는 쓰레드 루틴에서 메모리
        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        // thread_routine을 실행하는 thread 생성
        // 연결마다 고유한 connfdp를 thread_routine의 인자로 가져감.
        Pthread_create(&tid, NULL, thread_routine, connfdp);
    }
    return 0;
}

void *thread_routine(void *connfdp)
{
    // 각 스레드별 connfd는 입력으로 가져온 connfdp가 가리키던 할당된 위치의 fd값
    int connfd = *((int *)connfdp);
    // 각 스레드가 다른 스레드들의 종료를 기다리지 않도록 분리시켜줌(= 각 스레드는 자신의 일을 끝내면 종료됨)
    Pthread_detach(pthread_self());
    // connfdp도 이미 connfd를 얻어 역할을 다했으니 반납한다
    Free(connfdp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char HTTPheader[MAXLINE], hostname[MAXLINE], path[MAXLINE];
    rio_t rio;
    int EndServerfd;
    rio_t serv_rio;

    // rio와 connfd를 연결
    Rio_readinitb(&rio, connfd);
    // rio 내부 버퍼에 있는 client request를 읽어 userbuf에 저장
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    // buf에서 각각 method(=GET), uri(=54.180.144.225/), version(HTTP/1.1) 변수에 문자열 저장
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET")) // 대소문자를 구분하지 않고 비교하고, 같으면 0을 retuen함.
    {
        printf("Proxy does not implement this method\n");
        return;
    }
    int port;

    // 현재 프록시 서버의 목적에 맞게 uri에서 hostname과 path를 추출하고, port를 결정하기 위함.
    parse_uri(uri, hostname, path, &port);

    // 결정된 hostname, path, port에 따라 HTTP header를 만든다
    makeHTTPheader(HTTPheader, hostname, path, port, &rio);

    char portch[20];
    sprintf(portch, "%d", port);

    // endserver과 연결
    EndServerfd = Open_clientfd(hostname, portch);
    if(EndServerfd < 0)
    {
        printf("connection failed\n");
        return;
    }
    // 서버의 내부 버퍼를 초기화하고, EndServerfd와 연결함.
    Rio_readinitb(&serv_rio, EndServerfd);
    // 서버에게 전달할 HTTPheader를 EndServerfd에 작성함.
    Rio_writen(EndServerfd, HTTPheader, strlen(HTTPheader));
    
    // server가 작성란 respon
    size_t n; // buf에 작성한 내용의 크기
    while((n = Rio_readlineb(&serv_rio, buf, MAXLINE)) != 0)
    {
        printf("proxy received %d bytes, then send\n", n);
        Rio_writen(connfd, buf, n); // client와 연결된 connfd에 server로부터 읽은 buf를 작성함.
    }
    Close(EndServerfd); // clientfd close
}

int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    
    *port = 8000;

    // http://를 배제하기 위해 '//'의 위치를 hostnameP에 저장함
    char *hostnameP = strstr(uri, "//");
    // uri에 '//'가 존재(= http://)
    if (hostnameP != NULL)
    {
        // hostname은 '//' 이후부터 시작하므로 hostnameP의 위치를 hostname이 시작하는 위치로 갱신해줌
        hostnameP = hostnameP + 2;
    }
    // '//'이 없는 경우, hostname이 uri에 바로 나타난다는 의미이므로 hostnameP = uri
    else
    {
        hostnameP = uri;
    }
    // 포트 번호 유무를 찾기 위한 pathP 초기화
    char *pathP = strstr(hostnameP, ":");

    // pathP가 존재함(=port 번호가 존재함) ex) localhost:8000/home.html
    if(pathP != NULL)
    {
        // ':'가 있다면 해당 위치에 '\0'(=종료 문자)를 넣어 hostname과 분리함. 
        *pathP = '\0';
        // hostname은 hostnameP의 앞부분이므로 hostname에 저장함
        sscanf(hostnameP, "%s", hostname); // ex) localhost
        // pathP + 1부터 port와 path가 존재함. 따라서, 숫자는 port에 문자는 path에 저장함. ex) port: 8000, path: /home.html
        sscanf(pathP + 1, "%d%s", port, path);
    }
    else
    {
        // ':'가 없다면 포트가 존재하지 않다는 의미이므로 path를 찾기 위해 '/' 위치를 찾음
        pathP = strstr(hostnameP, "/");
        if(pathP != NULL) // path가 존재하는 경우
        {
            // 위와 동일하게 '/'를 '\0'(=종료 문자)로 바꿔서 hostname과 path를 분리함.
            // 포트 번호는 입력되지 않았으므로 디폴트 port 값을 사용함. 여기선 서버의 포트를 8000으로 열었으므로 8000 값 사용함.
            *pathP = '\0';
            sscanf(hostnameP, "%s", hostname);
            *pathP = '/';
            sscanf(pathP, "%s", path);
        }
        else
        {
            // ':'도 '/'도 없다면 hostname만 추출함.
            sscanf(hostnameP, "%s", hostname);
        }
    }
    return 0;
}

static const char *user_agent_header = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_header = "Connection: close\r\n";
static const char *prox_header = "Proxy-Connection: close\r\n";
// static const char *host_header_format = "Host: %s\r\n";
// static const char *requestlint_header_format = "GET %s HTTP/1.0\r\n";
static const char *endof_header = "\r\n";
static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";
// 조건대로 헤더를 만듦
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_header[MAXLINE], other_header[MAXLINE], host_header[MAXLINE];

    // server에게 request할 문구에 parse_uri에서 추출한 path를 추가하여 request_header에 저장함.
    sprintf(request_header, "GET %s HTTP/1.0\r\n", path);
    
    // client_rio에서 request header를 읽어 필요에 따라 수정하거나, 저장함.
    // request header가 존재하는 동안 계속 한 줄씩 buf에 저장해서 확인함.
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        // request header 입력이 완료되어 buf에 endof_header가 들어오면 더 이상 request header가 존재하지 않으므로 break
        if(strcmp(buf, endof_header) == 0) // endof_header: '\r\n'
        {
            break;
        }
        // request에 host_key가 존재할 경우, buf의 내용을 host_header에 copy
        if(!strncasecmp(buf, host_key, strlen(host_key))) // host_key = HOST:
        {
            strcpy(host_header, buf);
            continue;
        }
        // client의 request와 관계없이 connection_key, proxy_connection_key, user_agent_key는 상수로 고정되어 있음.
        // 따라서, 해당 문구가 들어간 request는 무시하기 위한 조건문이고, 
        // 세 키워드를 포함하지 않은 요청은 서버에게 전해줘야 하므로 other_header에 buf를 이어 붙인다.
        // oter_header가 여러줄 존재할 수 있으니 strcat를 사용함.
        if(strncasecmp(buf, connection_key, strlen(connection_key)) 
          && strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key)) 
          && strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_header, buf);
        }
    }
    // host header를 입력하지 않은 경우
    if(strlen(host_header) == 0) 
    {
        // host header form에 hostname을 넣어주고, host_header에 저장함
        sprintf(host_header, "Host: %s\r\n", hostname);
    }
    // 최종적으로 서버에 전달할 request header 구성 요소들을 http_header에 저장함.
    sprintf(http_header, "%s%s%s%s%s%s%s", request_header, host_header, conn_header, prox_header, user_agent_header, other_header, endof_header);
    return ;
}