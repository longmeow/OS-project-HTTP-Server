#define _XOPEN_SOURCE 700
#define _BSD_SOURCE 1

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

/*
 * Global configuration variables.
 * Use these in implementation of handle_files_request.
 * Their values are set up in main() using the
 * command line arguments.
 */

wq_t work_queue;
int num_threads;
int server_port;
char *server_files_directory;

/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 */

/* Handle the request to take all the files in "FILES"*/
void handle_files_request(int fd)
{
  printf("The thread number is %ld\n", pthread_self());
  struct http_request *request = http_request_parse(fd);
  uint16_t resp_status_code;
  char *resp_mime_type;
  char *resp_length;
  char *resp_str;
  size_t response_size_bytes = 0;

  /* Absolute server root path */
  char *path_buf = NULL;
  if (!(path_buf = getcwd(NULL, 0)))
  {
    perror("getcwd");
    exit(EXIT_FAILURE);
  }

  /* Static files directory */

  /* cwd +1 for a "/", +1 for null termination */
  char *req_file_path = calloc(strlen(path_buf) + strlen(server_files_directory) + strlen(request->path) + 1 + 1, sizeof(char));
  strcat(req_file_path, path_buf);
  strcat(req_file_path, "/");
  strcat(req_file_path, server_files_directory);
  strcat(req_file_path, request->path);
  free(path_buf);

  struct stat req_file;
  if (stat(req_file_path, &req_file) == -1)
  {
    if (errno == ENOENT)
    {
      free(req_file_path);
      goto not_found;
    }

    perror("stat");
    exit(EXIT_FAILURE);
  }

  if (S_ISDIR(req_file.st_mode))
  {
    // Directory request
    // Check if there is index.html serve it if thats the case
    if (!(req_file_path = realloc(req_file_path,
                                  strlen(req_file_path) +
                                      strlen("index.html") * sizeof(char))))
    {
      perror("realloc req_file_path");
      exit(EXIT_FAILURE);
    }

    strcat(req_file_path, "index.html");
    int index_fd = open(req_file_path, O_RDONLY);
    if (index_fd == -1)
    {
      if (errno == ENOENT)
      {
        goto show_dir;
      }
      perror("open fd@index.html");
      exit(EXIT_FAILURE);
    }

    char *index_html_str = malloc(sizeof(char));
    size_t html_length = 1;
    size_t i = 0;
    char rbuf;
    ssize_t n;

    while ((n = read(index_fd, &rbuf, 1) > 0))
    {
      if (html_length <= i)
      {
        html_length <<= 1;
        index_html_str = realloc(index_html_str, html_length);
        if (!index_html_str)
        {
          perror("realloc@index_html_str");
          exit(EXIT_FAILURE);
        }
      }

      index_html_str[i++] = rbuf;
    }

    if (html_length <= i)
    {
      index_html_str = realloc(index_html_str, html_length + 1);
      if (!index_html_str)
      {
        perror("realloc@index_html_str");
        exit(EXIT_FAILURE);
      }
    }

    index_html_str[i++] = '\0';

    i = strlen(index_html_str);
    resp_str = index_html_str;
    size_t j = snprintf(NULL, 0, "%lu", i);
    resp_length = calloc(j + 1, 1);
    if (!resp_length)
    {
      perror("malloc@resp_length");
      exit(EXIT_FAILURE);
    }
    size_t k = snprintf(resp_length, j + 1, "%lu", i);
    if (j != k)
    {
      perror("setting content length");
      exit(EXIT_FAILURE);
    }
    resp_status_code = 200;
    resp_mime_type = http_get_mime_type(req_file_path);
    free(req_file_path);
    close(index_fd);
    goto respond;

  show_dir:;
    size_t nl = strlen(req_file_path) - strlen("index.html") * sizeof(char);
    req_file_path = realloc(req_file_path, nl);
    if (!req_file_path)
    {
      perror("realloc@req_file_path");
      exit(EXIT_FAILURE);
    }
    req_file_path[nl] = '\0';

    DIR *dir = opendir(req_file_path);
    if (!dir)
    {
      if (errno == ENOENT)
      {
        free(req_file_path);
        return;
      }

      perror("opendir@req_file_path");
      exit(EXIT_FAILURE);
    }

    struct dirent *dir_entry;
    char *html_str = calloc(1, 1);
    char *link = calloc(1, 1);
    char *link_template = "<a style=\"display: block;\" href=\"%s\">%s</a>";

    while ((dir_entry = readdir(dir)))
    {
      char *den = dir_entry->d_name;
      size_t link_length = strlen(link) + strlen(den) * 2 +
                           strlen(link_template) - 4;
      link = realloc(link, link_length);
      if (!link)
      {
        perror("realloc@link");
        exit(EXIT_FAILURE);
      }
      snprintf(link, link_length + 1, link_template, den, den);

      html_str = realloc(html_str, strlen(html_str) + strlen(link));
      if (!html_str)
      {
        perror("realloc@html_str");
        exit(EXIT_FAILURE);
      }

      strcat(html_str, link);
    }

    resp_status_code = 200;
    resp_mime_type = http_get_mime_type(".html");
    resp_str = html_str;
    size_t m = strlen(html_str);
    size_t l = snprintf(NULL, 0, "%lu", m);
    resp_length = calloc(l + 1, 1);
    if (!resp_length)
    {
      perror("malloc@resp_length");
      exit(EXIT_FAILURE);
    }
    size_t o = snprintf(resp_length, l + 1, "%lu", m);
    if (o != l)
    {
      perror("setting content length");
      exit(EXIT_FAILURE);
    }

    free(link);
    free(req_file_path);
    goto respond;
  }
  else if (S_ISREG(req_file.st_mode))
  {
    // regular file request
    int file_fd = open(req_file_path, O_RDONLY);
    if (file_fd == -1)
    {
      if (errno == ENOENT)
      {
        goto not_found;
      }
      perror("open fd@index.html");
      exit(EXIT_FAILURE);
    }

    char *file_str = malloc(sizeof(char));
    size_t file_str_length = 1;
    size_t i = 0;
    char rbuf;
    ssize_t n;

    while ((n = read(file_fd, &rbuf, 1) > 0))
    {
      if (file_str_length <= i)
      {
        file_str_length <<= 1;
        file_str = realloc(file_str, file_str_length);
        if (!file_str)
        {
          perror("realloc@file_str");
          exit(EXIT_FAILURE);
        }
      }

      file_str[i++] = rbuf;
    }

    response_size_bytes = i;
    resp_str = file_str;
    size_t j = snprintf(NULL, 0, "%lu", i);
    resp_length = calloc(j + 1, 1);
    if (!resp_length)
    {
      perror("malloc@resp_length");
      exit(EXIT_FAILURE);
    }
    size_t k = snprintf(resp_length, j + 1, "%lu", i);
    if (j != k)
    {
      perror("setting content length");
      exit(EXIT_FAILURE);
    }
    resp_status_code = 200;
    resp_mime_type = http_get_mime_type(req_file_path);
    free(req_file_path);
    close(file_fd);
    goto respond;
  }
  else
  {
    char *not_allowed = "<h1 style=\"text-align: center;\">405 Not Allowed</h1>";

    resp_status_code = 405;
    resp_mime_type = http_get_mime_type(".html");
    resp_str = not_allowed;

    size_t m = strlen(not_allowed);
    size_t l = snprintf(NULL, 0, "%lu", m);
    resp_length = calloc(l + 1, 1);
    if (!resp_length)
    {
      perror("malloc@resp_length");
      exit(EXIT_FAILURE);
    }
    size_t o = snprintf(resp_length, l + 1, "%lu", m);
    if (o != l)
    {
      perror("setting content length");
      exit(EXIT_FAILURE);
    }

    goto respond;
  }

not_found:;
  char *template = "<h1 style=\"text-align: center;\">404 Not Found</h1>";
  char *not_found = calloc(strlen(template), 1);
  strcpy(not_found, template);
  resp_status_code = 404;
  resp_mime_type = http_get_mime_type(".html");
  resp_str = not_found;

  size_t m = strlen(not_found);
  size_t l = snprintf(NULL, 0, "%lu", m);
  resp_length = calloc(l + 1, 1);
  if (!resp_length)
  {
    perror("malloc@resp_length");
    exit(EXIT_FAILURE);
  }
  size_t o = snprintf(resp_length, l + 1, "%lu", m);
  if (o != l)
  {
    perror("setting content length");
    exit(EXIT_FAILURE);
  }

respond:

  http_start_response(fd, resp_status_code);
  http_send_header(fd, "Content-Type", resp_mime_type);
  http_send_header(fd, "Content-Length", resp_length);
  http_end_headers(fd);
  if (response_size_bytes)
    http_send_data(fd, resp_str, response_size_bytes);
  else
    http_send_string(fd, resp_str);
  free(resp_str);
  free(resp_length);
  response_size_bytes = 0;
}

void *thread_routine(void *arg)
{
  void (*request_handler)(int) = arg;
  int sock;

  while (1)
  {
    sock = wq_pop(&work_queue);
    request_handler(sock);
    close(sock);
  }

  return NULL;
}

/* Create thread pool  */
void init_thread_pool(int num_threads, void (*request_handler)(int))
{
  wq_init(&work_queue); // Initialize work queue
  pthread_t tpool[num_threads];
  size_t i;
  for (i = 0; i < num_threads; i++)
  {
    pthread_create(&(tpool[i]), NULL, thread_routine, request_handler);
  }
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int))
{

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  /* Create server socket */
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1)
  {
    perror("Failed to create a new socket");
    exit(errno);
  }

  /* Setting some mysterious settings for server socket. See "man setsockopt" for
     more info. */
  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                 sizeof(socket_option)) == -1)
  {
    perror("Failed to set socket options");
    exit(errno);
  }

  /* Initialize socket structure */
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /* Bind the address and the port to the socket */
  if (bind(*socket_number, (struct sockaddr *)&server_address,
           sizeof(server_address)) == -1)
  {
    perror("Failed to bind on socket");
    exit(errno);
  }

  /* Listen for new connections */
  if (listen(*socket_number, 1024) == -1)
  {
    perror("Failed to listen on socket");
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

  /*Initialize thread pool*/
  init_thread_pool(num_threads, request_handler);

  /* Serve forever */
  while (1)
  {
    /* Accept new client connection */
    client_socket_number = accept(*socket_number,
                                  (struct sockaddr *)&client_address,
                                  (socklen_t *)&client_address_length);
    if (client_socket_number < 0)
    {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
           inet_ntoa(client_address.sin_addr),
           client_address.sin_port);

    /* Push client socket into work_queue */
    wq_push(&work_queue, client_socket_number);
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum)
{
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0)
    perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
    "Usage: ./httpserver --files www_directory/ --port 8000 [--num-threads 5]\n";

void exit_with_usage()
{
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
  /* Registering signal handler. When user enteres Ctrl-C, 
     signal_callback_handler will be invoked. */
  signal(SIGINT, signal_callback_handler);

  /* Default settings */
  void (*request_handler)(int) = NULL;
  size_t i;

  for (i = 1; i < argc; i++)
  {
    if (strcmp("--files", argv[i]) == 0)
    {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory)
      {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    }
    else if (strcmp("--port", argv[i]) == 0)
    {
      char *server_port_string = argv[++i];
      if (!server_port_string)
      {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    }
    else if (strcmp("--num-threads", argv[i]) == 0)
    {
      char *num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1)
      {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    }
    else if (strcmp("--help", argv[i]) == 0)
    {
      exit_with_usage();
    }
    else
    {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL)
  {
    fprintf(stderr, "Please specify \"--files [DIRECTORY]\" or \n");
    exit_with_usage();
  }

  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
