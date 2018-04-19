/*
 * tiny.c - a minimal HTTP server that serves static and
 *          dynamic content with the GET method. Neither
 *          robust, secure, nor modular. Use for instructional
 *          purposes only.
 *          Dave O'Hallaron, Carnegie Mellon
 *
 *          usage: tiny <port>
 */

// revised by: dongjy
// add POST support

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFSIZE 1024
#define MAXERR 16

extern char **environ;

void error(char *msg) {
  perror(msg);
  exit(1);
}

void cerror(FILE *stream, char *cause, char *errno, char *shortmsg,
            char *longmsg) {
  fprintf(stream, "HTTP/1.1 %s %s", errno, shortmsg);
  fprintf(stream, "Content-type: text/html\n");
  fprintf(stream, "\n");
  fprintf(stream, "<html><title>tiny Error</title>");
  fprintf(stream, "<body bgcolor="
                  "0080ff"
                  ">\n");
  fprintf(stream, "%s: %s\n", errno, shortmsg);
  fprintf(stream, "<p>%s: %s\n", longmsg, cause);
  fprintf(stream, "<hr><em>The tiny Web Server</em>\n");
}

int main(int argc, char **argv) {

  int parentfd;
  int childfd;
  int portno;
  socklen_t clientlen;
  struct hostent *hostp;
  char *hostaddrp;
  int optval;
  struct sockaddr_in serveraddr;
  struct sockaddr_in clientaddr;

  FILE *stream;
  char buf[BUFSIZE];
  char method[BUFSIZE];
  char uri[BUFSIZE];
  char version[BUFSIZE];
  char filename[BUFSIZE];
  char filetype[BUFSIZE];
  char cgiargs[BUFSIZE];
  char *p;
  int is_static;
  struct stat sbuf;
  int fd;
  int pid;
  int wait_status;
  char **exec_arg;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);

  // open socket descriptor
  parentfd = socket(AF_INET, SOCK_STREAM, 0);
  if (parentfd < 0)
    error("ERROR opening socket");

  // allows us to restart server immediately
  optval = 1;
  setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
             sizeof(int));

  // bind port to socket
  bzero((char *)&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);
  if (bind(parentfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
    error("ERROR on binding");

  if (listen(parentfd, 5) < 0)
          error("ERROR on listen");

  // main loop: wait for a connection request, parse HTTP,
  // serve requested content, close connection.
  clientlen = sizeof(clientaddr);
  while (1) {

    // wait for a connection request
    childfd = accept(parentfd, (struct sockaddr *)&clientaddr, &clientlen);
    if (childfd < 0)
      error("ERROR on accept");

    hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr,
                          sizeof(clientaddr.sin_addr.s_addr), AF_INET);

    if (hostp == NULL)
      error("ERROR on gethostbyaddr");
    hostaddrp = inet_ntoa(clientaddr.sin_addr);
    if (hostaddrp == NULL)
      error("ERROR on inet_ntoa\n");

    // open the child socket descriptor as a stream
    if ((stream = fdopen(childfd, "r+")) == NULL)
      error("ERROR on fdopen");

    // get the HTTP request line
    fgets(buf, BUFSIZE, stream);
    printf("%s", buf);
    sscanf(buf, "%s %s %s\n", method, uri, version);

    // tiny only supports the GET method
    if (strcasecmp(method, "GET")) {
      cerror(stream, method, "501", "Not Implemented",
             "Tiny does not implement this method");
      fclose(stream);
      close(childfd);
      continue;
    }

    // read (and ignore) the HTTP headers
    fgets(buf, BUFSIZE, stream);
    printf("%s", buf);
    while (strcmp(buf, "\r\n")) {
      fgets(buf, BUFSIZE, stream);
      printf("%s", buf);
    }

    // parse the uri [crufty]
    if (!strstr(uri, "cgi-bin")) { // static content
      is_static = 1;
      strcpy(cgiargs, "");
      strcpy(filename, ".");
      strcat(filename, uri);
      if (uri[strlen(uri) - 1] == '/')
        strcat(filename, "index.html");
    } else {
      is_static = 0;
      p = index(uri, '?');
      if (p) {
        strcpy(cgiargs, p + 1);
        *p = '\0';
      } else {
        strcpy(cgiargs, "");
      }
      strcpy(cgiargs, "");
      strcat(filename, uri);
    }

    // make sure the file exists
    if (stat(filename, &sbuf) < 0) {
      cerror(stream, filename, "404", "Not Found",
             "tiny couldn't find this file");
      fclose(stream);
      close(childfd);
      continue;
    }

    // serve static content
    if (is_static) {
      if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
      else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
      else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpg");
      else
        strcpy(filetype, "text/plain");

      // print response header
      fprintf(stream, "HTTP/1.1 200 OK\n");
      fprintf(stream, "Server: Tiny Web Server\n");
      fprintf(stream, "Content-length: %d\n", (int)sbuf.st_size);
      fprintf(stream, "Content-type: %s\n", filetype);
      fprintf(stream, "\r\n");
      fflush(stream);

      // Use mmap to return arbitrary-sized response body
      fd = open(filename, O_RDONLY);
      p = mmap(0, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      fwrite(p, 1, sbuf.st_size, stream);
      munmap(p, sbuf.st_size);
    }

    // serve dynamic content
    else {
      // make sure file is a regular file
      if (!(S_IFREG & sbuf.st_mode) || !(S_IXUSR & sbuf.st_mode)) {
        cerror(stream, filename, "403", "Forbidden",
               "You are not allowed to access this item");
        fclose(stream);
        close(childfd);
        continue;
      }
    }

    // a read server would set other CGI environ var as well
    setenv("QUERY_STRING", cgiargs, 1);

    // print first part of response header
    sprintf(buf, "HTTP/1.1 200 OK\n");
    write(childfd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Serverc\n");
    write(childfd, buf, strlen(buf));
    pid = fork();
    if (pid < 0) {
      perror("ERROR in fork");
      exit(1);
    } else if (pid > 0) { // parent process
      wait(&wait_status);
    } else {            // child process
      close(0);         // close stdin
      dup2(childfd, 1); // map socket to stdout
      dup2(childfd, 2); // map socket to stderr
      exec_arg[0] = filename;
      exec_arg[1] = NULL;
      if (execve(filename, exec_arg, environ) < 0) {
        perror("ERROR in execve");
      }
    }

    // clean up
    fclose(stream);
    close(childfd);
  }
}
