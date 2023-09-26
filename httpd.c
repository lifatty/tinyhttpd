#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <strings.h>


#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
#define STDIN   0
#define STDOUT  1
#define STDERR  2

void accept_request(void *);
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


int startup(u_short *port)
{
    int httpd = 0;
    int on = 1;
    struct sockaddr_in name;

    // 创建套接字(因特网地址族、流套接字和默认协议)
    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");
    // 初始化结构体
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    // INADDR_ANY是一个 IPV4通配地址的常量
    // 大多实现都将其定义成了 0.0.0.0 
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    // 允许重用本地地址和端口 
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0)  
    {  
        error_die("setsockopt failed");
    }
    // bind()用于绑定地址与 socket 
    // 如果传进去的sockaddr结构中的 sin_port 指定为0，这时系统会选择一个临时的端口号
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");
    // 如果调用 bind 后端口号仍然是0，则手动调用 getsockname() 获取端口号
    if (*port == 0)
    {
        socklen_t namelen = sizeof(name);
        // 调用 getsockname()获取系统给 httpd 随机分配的端口号 
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        *port = ntohs(name.sin_port);
    }
    // 让 httpd 监听 request 
    if (listen(httpd, 5) < 0)
        error_die("listen");
    return httpd;
}



//请求导致对服务器端口上的accept0的调用返回,适当处理请求
void accept_request(void *arg)
{
    int client = (intptr_t)arg;
    char buf[1024];
    size_t numchars;
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    // 包含文件信息的数量
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                       * program */
    char *query_string = NULL;

    // 读 http 请求的第一行数据（request line），把请求方法存进 method 中
    numchars = get_line(client, buf, sizeof(buf));
    i = 0; j = 0;
    while (!ISspace(buf[i]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[i];
        i++;
    }
    j=i;
    method[i] = '\0';

    // 如果既不是 GET 也不是 POST，直接发送 response 告诉客户端没实现该方法
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return;
    }

    // 如果是 POST 方法就，开启 cgi
    if (strcasecmp(method, "POST") == 0)
        cgi = 1;

    i = 0;
    // 跳过空白字符 
    while (ISspace(buf[j]) && (j < numchars))
        j++;
    // 读取 url
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars))
    {
        url[i] = buf[j];
        i++; j++;
    }
    url[i] = '\0';

    // 如果是 GET 请求，
    if (strcasecmp(method, "GET") == 0)
    {
        query_string = url;
        // 检查 url 中是否存在 ？
        while ((*query_string != '?') && (*query_string != '\0'))
            query_string++;
        // GET请求中，?后面为参数
        if (*query_string == '?')
        {
            // 开启 cgi
            cgi = 1;
            *query_string = '\0';
            query_string++;
        }
    }

    sprintf(path, "htdocs%s", url);
    // 如果以 / 结尾，在后面加上 index.html
    if (path[strlen(path) - 1] == '/')
        strcat(path, "index.html");
    // 根据路径找到对应文件 
    if (stat(path, &st) == -1) {
        while ((numchars > 0) && strcmp("\n", buf)) 
            numchars = get_line(client, buf, sizeof(buf));
        // 404
        not_found(client);
    }
    else
    {
        // 如果是个目录，则默认使用该目录下 index.html 文件 
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html");
        // 如果文件有可执行权限，开启 cgi 
        if ((st.st_mode & S_IXUSR) ||
                (st.st_mode & S_IXGRP) ||
                (st.st_mode & S_IXOTH))
            cgi = 1;
        // 不是 cgi,直接把服务器文件返回，否则执行 cgi
        if (!cgi)
            serve_file(client, path);
        else
            execute_cgi(client, path, method, query_string);
    }

    // 断开与客户端的连接
    close(client);
}


void execute_cgi(int client, const char *path, const char *method, const char *query_string)
{
	char buf[1024];
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;
	//读取到的字符数
	int numchars = 1;
	//http 的 content_length
	int content_length = -1;
	
	buf[0] = 'A'; buf[1] = '\0';
	if(strcasecmp(method, "GET") == 0)
		//读取并丢弃 http header
		while((numchars > 0) && strcmp("\n", buf))
			numchars = get_line(client, buf, sizeof(buf));
	else if(strcasecmp(method, "POST") == 0)
	{
		numchars = get_line(client, buf, sizeof(buf));
		while((numchars > 0) && strcmp("\n", buf))
		{
			//如果是POST请求， 就需要得到Content_length
			//Content_length字符长度为15， 从17位开始是具体的长度信息
			buf[15] = '\0';
			if(strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&(buf[16]));
			numchars = get_line(client, buf, sizeof(buf));
		}
		if(content_length == -1)
		{
			bad_request(client);
			return;
		}
	}
	//pipe()建立output管道
	if(pipe(cgi_output) < 0)
	{
		cannot_execute(client);
		return;
	}
	if((pid = fork()) < 0)
	{
		cannot_execute(client);
		return;
	}
	sprintf(buf, "HHTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);

	//子进程用于执行cgi
	if(pid == 0)
	{
		char meth_env[255];
		char query_env[255];
		char length_env[255];
		//将子进程的stdout重定向到cgi_output的管道写端上
		//将stdin重定向到cgi_input管道的读端上，并关闭管道的其他端口
		//dup2()
		dup2(cgi_output[1], STDOUT);
		dup2(cgi_input[0], STDIN);
		close(cgi_output[0]);
		close(cgi_input[1]);
		//设置cgi环境变量putenv()
		sprintf(meth_env, "REQUEST_METHOD=%s", method);
		putenv(query_env);
		if(strcasecmp(method, "GET") == 0)
		{
			sprintf(query_env, "QUERY_STRING =%s", query_string);
			putenv(query_env);
		}
		else 	//POST
		{
			sprintf(query_env, "CONTENT_LENGTH=%d", content_length);
			putenv(query_env);

		}
		//将子进程替换成另一个进程并执行cgi脚本
		//excel()包含于<unistd.h>
		execl(path,path, NULL);
		exit(0);
	}
	else
	{
		//父进程关闭cgi_output管道的写端和cgi_input的读端
		close(cgi_output[1]);
		close(cgi_input[0]);
		if(strcasecmp(method, "POST") == 0)
			//根据content-length读取客户端的消息
			//并提供cgi_input传入子进程的标准输入
			for(i = 0; i < content_length; i++)
			{
				recv(client, &c, 1, 0);
				write(cgi_input[1], &c, 1);
			}
		//通过cgi_output,获取子进程的标准输出，并将其写入到客户端
		while(read(cgi_output[0], &c, 1) > 0)
			send(client, &c, 1, 0);
		//关闭管道端口，等待子进程结束，退出程序
		close(cgi_output[0]);
		close(cgi_input[1]);
		waitpid(pid, &status, 0);
	}
		


}

//向客户发送常规文件。使用标头，并在发生错误时向客户端报告错误
void serve_file(int client, const char *filename)
{
	FILE *resource =NULL;
	int numchars = 1;
	char buf[1024];

	buf[0] = 'A'; buf[1] = '\0';
	while((numchars > 0)&& strcmp("\n", buf)) //读取并丢弃头部
		numchars = get_line(client, buf, sizeof(buf));

	resource = fopen(filename, "r");
	if(resource == NULL)
		not_found(client);
	else
	{
		headers(client, filename);
		cat(client, resource);
	}
	fclose(resource);

}




//通知客户端所请求的Web方法尚未实现
void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
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


//向客户端发送 404 未找到状态消息
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


//从套接字获取一行，无论该行以换行符、回车符还是 CRLF 组合结尾
//以空字符终止读取的字符串如果在缓冲区末尾之前没有找到换行符，则字符串以 null 终止。
//如果读取以上三个行终止符中的任何一个，则字符串的最后一个字符将是换行符，并且字符串将以空字符终止
int get_line(int sock, char *buf, int size)
{
	int i = 0;
	char c = '\0';
	int n;

	while((i < size -1) && (c != '\n'))
	{
		n = recv(sock, &c, 1, 0);
		if(n > 0)
		{
			if(c == '\r')
			{
				n =recv(sock, &c, 1, MSG_PEEK);
				if((n > 0) && (c == '\n'))
					recv(sock, &c, 1, 0);
				else
					c = '\n';
			}
			buf[i] = c;
			i++;
		}
		else
			c= '\n';
	}
	buf[i] = '\0';
	return i;
}



//返回有关文件的信息性 HTTP 标头
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


//使用 perror()打印出错误消息 (对于系统错误;基于errno的值，这表明系统调用错误)并退出表明错误的程序
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

//通知客户端cgi脚本无法执行
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

//将文件的全部内容放在套接字上。该函数以UNIX“cat”命令命名，因为仅执行诸如 pipe fork 和 exec（"cat")
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

//通知客户端的请求有问题
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

int main(void)
{
	int server_sock = -1;
	u_short port = 4000;
	int client_sock = -1;
	//sockaddr_in 是IPV4的套接字地址结构
	struct sockaddr_in client_name;
	socklen_t client_name_len = sizeof(client_name);

	server_sock = startup(&port);
	printf("httpd running on port %d\n", port);

	while(1)
	{
		//阻塞等待客户端的连接
		client_sock = accept(server_sock, (struct sockaddr *)&client_name, &client_name_len);
		if(client_sock == -1)
			error_die("accept");

		accept_request(&client_sock);
	}
	close(server_sock);
	return 0;
}
