
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

void* accept_request(void *);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

//accept 响应请求
void* accept_request(void* pclient)
{
 char buf[1024];
 int numchars;
 char method[255];
 char url[255];
 char path[512];
 size_t i, j;
 struct stat st;
 int cgi = 0;      /* becomes true if server decides this is a CGI
                    * program */
 char *query_string = NULL;

 int client = *(int*)pclient;
 //获取一行HTTP请求报文
 numchars = get_line(client, buf, sizeof(buf));
 
 i = 0; j = 0;
 //提取其中的方法post或get到method
 while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
 {
  method[i] = buf[j];
  i++; j++;
 }
 method[i] = '\0';

 //tinyhttpd只实现了get post 方法
 if (strcasecmp(method, "GET")&& strcasecmp(method, "POST"))
 {
  unimplemented(client);
  return NULL;
 }

 //cgi为标志位，1表示开启CGI解析(POST方法)
 if (strcasecmp(method, "POST") == 0)
  cgi = 1;

 i = 0;
 //跳过method后面的空白字符
 while (ISspace(buf[j]) && (j < sizeof(buf)))
  j++;
//获取url
 while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
 {
  url[i] = buf[j];
  i++; j++;
 }
 url[i] = '\0';

 //如果是get方法，url可能带？参数
 if (strcasecmp(method, "GET") == 0)
 {
  query_string = url;
  while ((*query_string != '?') && (*query_string != '\0'))
   query_string++;
  if (*query_string == '?')
  {
	  //带参数需要执行cgi，解析参数
   cgi = 1;
   *query_string = '\0';
   query_string++;
  }
 }
//以上 将起始行 解析完毕
 
 
 sprintf(path, "htdocs%s", url);
 //如果path是一个目录，默认设置首页为index.html 
 if (path[strlen(path) - 1] == '/')
  strcat(path, "index.html");

//函数定义:    int stat(const char *file_name, struct stat *buf);
 //函数说明:    通过文件名filename获取文件信息，并保存在buf所指的结构体stat中
 //返回值:     执行成功则返回0，失败返回-1，错误代码存于errno（需要include <errno.h>）
 if (stat(path, &st) == -1) {
	 //访问的网页不存在，则不断的读取剩余的请求头部信息，并丢弃
  while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
   numchars = get_line(client, buf, sizeof(buf));
  not_found(client);
 }
 else
 {
	//访问你的网页存在则进行处理
	//S_IFDIR 判断是否为目录
  if ((st.st_mode & S_IFMT) == S_IFDIR)
   strcat(path, "/index.html");
  
  //S_IXUSR：文件所有者具有可执行权限，
  //S_IXGRP：用户组具有可执行权限
  if ((st.st_mode & S_IXUSR) ||
      (st.st_mode & S_IXGRP) ||
      (st.st_mode & S_IXOTH)    )
   cgi = 1;
   
  if (!cgi)
	//将静态文件返回
   serve_file(client, path);
  else
   execute_cgi(client, path, method, query_string);
 }
//THHP协议是面向无连接的，所以要关闭
 close(client);
 return NULL;
}

//告知客户端该请求有错误400
void bad_request(int client)
{
 char buf[1024];

 sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
 send(client, buf, sizeof(buf), 0);
 sprintf(buf, "Content-type: text/html\r\n");
 send(client, buf, sizeof(buf), 0);
 sprintf(buf, "\r\n");
 send(client, buf, sizeof(buf), 0);
 sprintf(buf, "<P>Your browser sent a bad request, ");
 send(client, buf, sizeof(buf), 0);
 sprintf(buf, "such as a POST without a Content-Length.\r\n");
 send(client, buf, sizeof(buf), 0);
}

//把文件resource中的数据读取到client中
void cat(int client, FILE *resource)
{
 char buf[1024];

 fgets(buf, sizeof(buf), resource);
 while (!feof(resource))
 {
  send(client, buf, strlen(buf), 0);
  fgets(buf, sizeof(buf), resource);
 }
}

//告知客户端CGI脚本不能被执行，错误代码500
void cannot_execute(int client)
{
 char buf[1024];

 sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "Content-type: text/html\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
 send(client, buf, strlen(buf), 0);
}

//打印出错误信息并结束程序。
void error_die(const char *sc)
{
 perror(sc);
 exit(1);
}


/*execute_cgi函数负责将请求传递给cgi程序处理，
服务器与cgi之间通过管道pipe通信，首先初始化两个管道，并创建子进程去执行cgi函数
子进程执行cgi程序，获取cgi的标准输出通过管道传给父进程，由父进程发送给客户端
*/
//参数path指向执行的CGI脚本路径 ，method指向http的请求方法
void execute_cgi(int client, const char *path,
                 const char *method, const char *query_string)
{
 char buf[1024];
 int cgi_output[2];
 int cgi_input[2];
 pid_t pid;
 int status;
 int i;
 char c;
 int numchars = 1;
 int content_length = -1;

 buf[0] = 'A'; buf[1] = '\0';
 //如果是get请求，读取并丢弃头部
 if (strcasecmp(method, "GET") == 0)//
  while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
   numchars = get_line(client, buf, sizeof(buf));
 else    /* POST */
 {
	 //如果是post请求
  numchars = get_line(client, buf, sizeof(buf));
  while ((numchars > 0) && strcmp("\n", buf))
  {
   buf[15] = '\0';
   //读取头信息找到Content-Length字段的值
   if (strcasecmp(buf, "Content-Length:") == 0)
	   //Content-Length:15
    content_length = atoi(&(buf[16]));
   numchars = get_line(client, buf, sizeof(buf));
  }
  if (content_length == -1) {
   bad_request(client);
   return;
  }
 }

 //正确返回200
 sprintf(buf, "HTTP/1.0 200 OK\r\n");
 send(client, buf, strlen(buf), 0);

 //pipe(int filep[2]) 管道函数，f[0]读，f[1]写
 //必须在fork()中调用pipe()，否则子进程不会继承文件描述符。
//两个进程必须有血缘关系才能使用pipe。但是可以使用命名管道。
 if (pipe(cgi_output) < 0) {
  cannot_execute(client);
  return;
 }
 if (pipe(cgi_input) < 0) {
  cannot_execute(client);
  return;
 }

 if ( (pid = fork()) < 0 ) {
  cannot_execute(client);
  return;
 }
 
 /*子进程继承了父进程的pipe，然后通过关闭子进程output管道的输出端，input管道的写入端； 
    关闭父进程output管道的写入端，input管道的输出端*/  

 if (pid == 0)  /* child: CGI script */
 {
  char meth_env[255];
  char query_env[255];
  char length_env[255];

  //把stdout 重定向到cgi_output[1]
  dup2(cgi_output[1], 1);
  dup2(cgi_input[0], 0);
  close(cgi_output[0]);//关闭cgi_output读端
  close(cgi_input[1]); //关闭cgi_input 写端
  
  sprintf(meth_env, "REQUEST_METHOD=%s", method);
  putenv(meth_env);
  if (strcasecmp(method, "GET") == 0) {
	  /*设置 query_string 的环境变量*/  
   sprintf(query_env, "QUERY_STRING=%s", query_string);
   putenv(query_env);
  }
  else {   /* POST */
   /*设置 content_length 的环境变量*/  
   sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
   putenv(length_env);
  }
  //exec函数簇，执行CGI脚本，获取cgi的标准输出作为相应内容发送给客户端  
  execl(path, path, NULL);
  exit(0);
 } 
 else
 {    /* parent */
  close(cgi_output[1]);
  close(cgi_input[0]);
  /*通过关闭对应管道的通道，然后重定向子进程的管道某端，这样就在父子进程之间构建一条单双工通道 
   如果不重定向，将是一条典型的全双工管道通信机制 
   */  
  if (strcasecmp(method, "POST") == 0)
   for (i = 0; i < content_length; i++) {
    recv(client, &c, 1, 0);//从客户端接收单个字符  
    write(cgi_input[1], &c, 1); 
	//数据传送过程：input[1](父进程) ——> input[0](子进程)[执行cgi函数] ——> STDIN ——> STDOUT   
     // ——> output[1](子进程) ——> output[0](父进程)[将结果发送给客户端]  
  
   }
  while (read(cgi_output[0], &c, 1) > 0)//读取output的管道输出到客户端，output输出端为cgi脚本执行后的内容
   send(client, &c, 1, 0);//即将cgi执行结果发送给客户端，即send到浏览器，如果不是POST则只有这一处理  

  close(cgi_output[0]);//关闭剩下的管道端，子进程在执行dup2之后，就已经关闭了管道一端通道  
  close(cgi_input[1]);
  waitpid(pid, &status, 0);//等待子进程终止 
 }
}


//读取一行http报文，以\r 或\r\n为行结束符
//注：只是把\r\n之前的内容读取到buf中，最后再加一个\n\0
int get_line(int sock, char *buf, int size)
{
 int i = 0;
 char c = '\0';
 int n;

 //读取到的字符个数大于size或者读到\n结束循环
 while ((i < size - 1) && (c != '\n'))
 {
  n = recv(sock, &c, 1, 0); //接收一个字符
  /* DEBUG printf("%02X\n", c); */
  if (n > 0)
  {
   if (c == '\r')//回车字符
   {
	/*使用 MSG_PEEK 标志使下一次读取依然可以得到这次读取的内容，可认为接收窗口不滑动*/  
    n = recv(sock, &c, 1, MSG_PEEK);
    /* DEBUG printf("%02X\n", c); */
	
	//读取到回车换行
    if ((n > 0) && (c == '\n'))
     recv(sock, &c, 1, 0);//还需要读取，因为之前一次的读取，相当于没有读取
    else
     c = '\n';//如果只读取到\r，也要终止读取
   }
   //没有读取到\r,则把读取到内容放在buf中
   buf[i] = c;
   i++;
  }
  else
   c = '\n';
 }
 
 buf[i] = '\0';
 
 return i; 
 //返回读取到的字符个数,不包括\0
}


//服务器向client发送响应头部信息
void headers(int client, const char *filename)
{
 char buf[1024];
 (void)filename;  /* could use filename to determine file type */

 strcpy(buf, "HTTP/1.0 200 OK\r\n");
 send(client, buf, strlen(buf), 0);
 strcpy(buf, SERVER_STRING);
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "Content-Type: text/html\r\n");
 send(client, buf, strlen(buf), 0);
 strcpy(buf, "\r\n");
 send(client, buf, strlen(buf), 0);
}


//告知客户端404错误(没有找到)
void not_found(int client)
{
 char buf[1024];

 sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, SERVER_STRING);
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "Content-Type: text/html\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "your request because the resource specified\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "is unavailable or nonexistent.\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "</BODY></HTML>\r\n");
 send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
//调用 cat 把服务器文件内容返回给浏览器
void serve_file(int client, const char *filename)
{
 FILE *resource = NULL;
 int numchars = 1;
 char buf[1024];

 //读取，丢弃头部
 buf[0] = 'A'; buf[1] = '\0';
 while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
  numchars = get_line(client, buf, sizeof(buf));

 resource = fopen(filename, "r");
 if (resource == NULL)
  not_found(client);//文件不存在，返回404错误
 else
 {
  headers(client, filename);//服务器向client发送响应头部信息，200
  cat(client, resource);//将文件中的信息发送到client
 }
 fclose(resource);
}



//socket -->bind----> listen 
int startup(u_short *port)
{
 int httpd = 0;
 struct sockaddr_in name;

 httpd = socket(PF_INET, SOCK_STREAM, 0);
 if (httpd == -1)
  error_die("socket");
 memset(&name, 0, sizeof(name));
 name.sin_family = AF_INET;
 name.sin_port = htons(*port);
 name.sin_addr.s_addr = htonl(INADDR_ANY);
 if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
  error_die("bind");
 if (*port == 0)  /* if dynamically allocating a port */
 {
  socklen_t namelen = sizeof(name);
  if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
   error_die("getsockname");
  *port = ntohs(name.sin_port);
 }
 if (listen(httpd, 5) < 0)
  error_die("listen");
 return(httpd);
}

//返回给浏览器表明收到的 HTTP 请求所用的 method 不被支持。501
void unimplemented(int client)
{
 char buf[1024];

 sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, SERVER_STRING);//Server: jdbhttpd/0.1.0\r\n
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "Content-Type: text/html\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "</TITLE></HEAD>\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
 send(client, buf, strlen(buf), 0);
 sprintf(buf, "</BODY></HTML>\r\n");
 send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void)
{
 int server_sock = -1;
 u_short port = 0; //监听端口号
 int client_sock = -1;
 struct sockaddr_in client_name;
 socklen_t client_name_len = sizeof(client_name);
 pthread_t newthread;

 server_sock = startup(&port);//服务器端监听套接字设置  
 printf("httpd running on port %d\n", port);

 while (1)
 {
  client_sock = accept(server_sock,
                       (struct sockaddr *)&client_name,
                       &client_name_len);
  if (client_sock == -1)
   error_die("accept");
 /* accept_request(client_sock); */
 if (pthread_create(&newthread , NULL, accept_request,(void *)&client_sock) != 0)
   perror("pthread_create");
 }

 close(server_sock);

 return(0);
}
