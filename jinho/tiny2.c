/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

int main(int argc, char **argv) { // argc: 인자 개수, argv: 인자들이 들어간 배열
  int listenfd, connfd; // 듣기 소켓 식별자 및 연결 소켓 식별자
  char hostname[MAXLINE], port[MAXLINE]; // 호스트 및 포트
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) { // 포트 번호가 입력되지 않았을 경우 argv는 실행파일의 경로밖에 없으므로 argc가 1이 됨
    fprintf(stderr, "usage: %s <port>\n", argv[0]); // 경로와 포트 번호를 함께 입력하라는 에러 메세지 출력
    exit(1);
  }

  // addrinfo 구조체는 네트워크 주소정보(인터넷 주소)와 호스트 이름을 표현하는데 사용되며,
  // 이 정보는 bind(), connect() 호출 시 입력 파라미터로 사용될 수 있고, 
  // getaddrinfo() 함수 호출 시, hint 정보를 알리는 입력 파라미터로 사용할 수 있으며,
  // getaddrinfo() 함수의 결과값을 전달하는 출력 파라미터로도 사용됨
  // (함수의 네번째 인자값에 addrinfo 구조체의 linked list로 돌려줌)

  listenfd = Open_listenfd(argv[1]); // 포트 번호를 이용하여 듣기 식별자를 오픈하고 리턴하는 함수
  while (1) { // 무한 서버 루프를 실행하고, 반복적으로 연결요청을 접수함.
    clientlen = sizeof(clientaddr);
    // client가 connect 요청 시 주소 정보를 전달함.
    // SA의 자료형인 sockaddr에는 protocal family(IP주소의 크기 정보), port, address 정보가 담겨 있음.
    // 연결이 완료되면 clientaddr(socket addr 구조체=ip 및 포트 정보 담김)에는 클라이언트의 주소 정보가 채워지고, connection 식별자를 리턴함.
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen);  // line:netp:tiny:accept

    // sockaddr 구조체를 통해 address 정보를 주면 host와 service에 대한 정보를 얻을 수 있음.
    // ex) 143.248.220.177, 49191
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);   // line:netp:tiny:doit
    Close(connfd);  // line:netp:tiny:close
  }
}
// 한 개의 HTTP 트랜잭션을 처리함.
void doit(int fd) // connfd를 인자로 받음.
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;
  
  //  rio struct
  //  int rio_fd  /* descriptor for this internal buf
  //  int rio_cnt /* Unread bytes in internal buf
  //  char *rio_bufptr  /* Next unread byte in internal buf
  //  char rio_buf[RIO_BUFSIZE] /* Internal buffer
  // rio_readinitb: 식별자 fd를 주소 &rio에 위치한 rio_t 타입의 읽기 버퍼와 연결함.
  // rio 구조체 초기화
  Rio_readinitb(&rio, fd);
  // rio_readlineb 함수는 다음 텍스트 줄을 파일 rp(newline 문자를 포함하여)에서 읽고, usrbuf로 복사함.
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers:\n");
  printf("%s", buf);
  // ex) GET /cgi-bin/adder?11&11 HTTP/1.1
  // method, uri, version에 해당 문자열 저장
  sscanf(buf, "%s %s %s", method, uri, version);
  if (strcasecmp(method, "GET") && (strcasecmp(method, "HEAD"))){
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }

  read_requesthdrs(&rio); // 헤더 부분 다 읽고, data 시작 위치로 포인터 이동

  is_static = parse_uri(uri, filename, cgiargs); // 정적 요청인지 동적 요청인지 판단함. 정적일 경우 1, 동적일 경우 0 리턴

  if (stat(filename, &sbuf) < 0){ 
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  if (is_static){ // 1일 경우
    // 해당 파일이 읽기 권한이 있는지 여부 확인
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)){
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read this file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size, method);
  } else { // 0일 경우
    // 해당 파일이 실행 가능한지 여부 확인
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)){
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs, method);
  }
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);

  while (strcmp(buf, "\r\n")){
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  if (!strstr(uri, "cgi-bin")) {
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri); // ./
    if (uri[strlen(uri)-1] == '/') // 마지막 문자열이 /일 경우
      strcat(filename, "home.html"); // ./home.html
    return 1;
  }
  else {
    ptr = index(uri, '?'); // ?arg1&arg2
    if (ptr) {
      strcpy(cgiargs, ptr+1); // ?이후 문자열을 cgiargs에 저장
      *ptr = '\0'; // ?를 종료 문자로 바꿔서 그 뒤로는 읽을 수 없게 설정
    }
    else
      strcpy(cgiargs, ""); // 인자가 없는 경우
    strcpy(filename, "."); 
    strcat(filename, uri); // ./cgi-bin/adder?11&11 실행 할 때 경로로 만들어주려고 하는듯
    return 0;
  }
}
// 정적 컨텐츠를 클라이언트에게 제공함
void serve_static(int fd, char *filename, int filesize, char *method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  get_filetype(filename, filetype); // tiny에서 제공하는 5개의 정적 컨텐츠 중 어떤 형식인지 검사해서 filetype을 결정함.
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf)); // buf에서 strlen(buf) 크기만큼 fd(=connfd)에 복사
  printf("Response headers:\n");
  printf("%s", buf);

  // filename 경로에 읽기 전용으로 파일을 생성하고 디스크립터를 반환
  srcfd = Open(filename, O_RDONLY, 0); 
  // Mmap: 메모리의 내용을 파일이나 디바이스에 맵핑하기 위해 사용하는 시스템 호출
  // 원본 파일이 맵핑하고 있는 메모리 영역을 srcfd 파일도 맵핑하는 의미인듯
  // 파일은 디스크에 존재하고 있는데 읽거나 쓸때마다 디스크에 접근하면 속도가 느리니까 메모리에다 맵핑하는듯??
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  srcp = (char*)malloc(filesize);
  Rio_readn(srcfd, srcp, filesize);
  Close(srcfd);
  if (strcasecmp(method, "HEAD")){
    Rio_writen(fd, srcp, filesize);
  }
  free(srcp);
}


void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs, char *method)
{
  char buf[MAXLINE], *emptylist[] = {NULL};

  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0){ // Fork: 자신과 동일한 자식 프로세스를 생성함
    // 부모 프로세스와 독립적으로 진행함
    setenv("QUERY_STRING", cgiargs, 1); // 인자값을 환경변수 QUERY_STRING에 저장, 1이면 기존 값에 덮어씌울 수 있음.

    setenv("REQUEST_METHOD", method, 1);

    Dup2(fd, STDOUT_FILENO);
    Execve(filename, emptylist, environ);
  }

  Wait(NULL);
}