// http://localhost:8080

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <dirent.h>

#define SERVER_PORT "8080"
#define MAX_CONNECTION 1000
#define MAX_THREADS 10
#define HTTP_HEADER "HTTP/1.1 200 Ok\r\n"

int thread_count = 0; // Keeps the number of the threads working simultaneously.
pthread_mutex_t mutex; // To control thread_counter.

// SEMAPHORES
sem_t file_sem;
sem_t dir_sem;

int create_socket(const char *);

void *get_client_addr(struct sockaddr *);

void *recv_HttpRequest(void *);

void send_HttpResponse(const char *, int);

void parse_http_request(char *, char **, char **, char **, char **, char **, char **);

char *find_token(const char [], const char [], const char []);

int send_file(int, char [], char []);

void send_404(int);

void directory_listing(int, char *);

void sortFilenameList(char **, int);

int compareFilenames(const char *, const char *);


int main() {
    sem_init(&dir_sem, 0, 1); // Inıtialize the mutex from 1.
    sem_init(&file_sem, 0, 1);

    int sock = create_socket(SERVER_PORT);
    pthread_mutex_init(&mutex, NULL); // Inıtialize the mutex
    if (sock < 0) {
        fprintf(stderr, "error create socket\n");
        return -1;
    }
    printf("Server started %shttp://127.0.0.1:%s%s\n", "\033[92m", SERVER_PORT, "\033[0m");

    // Ignore SIGCHLD to avoid zombie threads
    signal(SIGCHLD, SIG_IGN);

    struct sockaddr_storage client_addr; // info of new connection
    int client_d; // socket client descriptor (client address)
    while (1) {
        // in this line waiting incoming connection
        socklen_t s_size = sizeof(client_addr);
        client_d = accept(sock, (struct sockaddr *) &client_addr, &s_size);
        if (client_d == -1) {
            fprintf(stderr, "error accept\n");
            break;
        }

        // get client ip
        char ip[INET6_ADDRSTRLEN];
        inet_ntop(client_addr.ss_family, get_client_addr((struct sockaddr *) &client_addr), ip, sizeof(ip));
        printf("\nServer: got connection from %s\n", ip);

        pthread_t sniffer_thread;
        int *new_sock = (int *) malloc(sizeof(int) * 1);
        *new_sock = client_d;

        // Create a thread for each request.
        if (pthread_create(&sniffer_thread, NULL, recv_HttpRequest, (void *) new_sock) != 0) {
            puts("Could not create thread");
            break;
        }
        pthread_detach(sniffer_thread);
    }

    pthread_mutex_destroy(&mutex);
    sem_close(&dir_sem);
    sem_close(&file_sem);
    close(sock);
    return -1;
}

void *get_client_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *) sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *) sa)->sin6_addr);
}

int create_socket(const char *Port) {
    // addrinfo - save all info for socket
    struct addrinfo hints;
    struct addrinfo *servinfo; // list

    memset(&hints, 0, sizeof(hints));

    // protocol family; type of protocol (IPv4 or IPv6)
    hints.ai_family = AF_UNSPEC;
    // type - TCP/IP protocol stream socket
    hints.ai_socktype = SOCK_STREAM;
    // automatically fill ip address
    hints.ai_flags = AI_PASSIVE;

    int result = getaddrinfo(NULL, Port, &hints, &servinfo);
    if (result != 0) {
        fprintf(stderr, "error getaddrinfo()\n");
        return -1;
    }

    int sock = -1;
    int yes = 1;

    // choose right socket, that corresponds to all parameters
    for (struct addrinfo *p = servinfo; p != NULL; p = p->ai_next) {

        // Socket setup: creates an endpoint for communication, returns a descriptor
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == -1) continue;

        // for reuse socket (need when restart server)
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            fprintf(stderr, "error setsockopt\n");
            close(sock);
            freeaddrinfo(servinfo);
            return -2;
        }

        // binding to local address and port (server processes must point port for communication with service functions)
        if (bind(sock, p->ai_addr, p->ai_addrlen) == -1) {
            close(sock);
            continue;
        }

        freeaddrinfo(servinfo);

        if (p == NULL) {
            fprintf(stderr, "failed to find address\n");
            return -3;
        }

        // sock - blocking socket (program stop on 1 function and be wait incoming connection)
        // prepare socket for incoming connection
        // creating a queue for incoming connections
        if (listen(sock, MAX_CONNECTION) == -1) {
            fprintf(stderr, "error listen\n");
            return -4;
        }

        break;
    }

    return sock;
}

void *recv_HttpRequest(void *socket_desc) {
    time_t t = time(NULL);
    struct tm *tmr = localtime(&t);
    printf("%s%s%s\n", "\033[94m", asctime(tmr), "\033[0m");  // get current time

    // Get the socket descriptor.
    int sock = *((int *) socket_desc);

    const int request_buffer_size = 65536;
    char request[request_buffer_size];

    ssize_t bytes_recvd = recv(sock, request, request_buffer_size - 1, 0);

    struct timespec begin, end; // for measure time execution
    clock_gettime(CLOCK_REALTIME, &begin);

    if (bytes_recvd < 0) {
        fprintf(stderr, "error recv\n");
    } else if (bytes_recvd == 0) // receive socket closed. Client disconnected upexpectedly.
    {
        fprintf(stderr, "%sClient disconnected unexpectedly.%s\n", "\033[91m", "\033[0m");
    } else { // Message received.
        pthread_mutex_lock(&mutex);
        thread_count++;
        pthread_mutex_unlock(&mutex);
        if (thread_count >
            MAX_THREADS) // If there is MAX_THREADS request at the same time, other request will be refused.
        {
            char *message = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n<!doctype html><html><body>System is busy right now.</body></html>";
            fprintf(stderr, "%sSystem is busy right now!%s\n", "\033[91m", "\033[0m");
            if (write(sock, message, strlen(message)) == -1) perror("write");
        } else {
            request[bytes_recvd] = '\0';
            send_HttpResponse(request, sock);

            // sleep(5); // If you want to see just 10 thread is working simultaneously, you can sleep here.
            // After send 10 request, 11th request will be responded as "System is busy right now".

            clock_gettime(CLOCK_REALTIME, &end);
            double diff = (double) (end.tv_nsec - begin.tv_nsec) / 1000000000.0 +
                          (double) (end.tv_sec - begin.tv_sec);
            printf("%sRequest processing time = %f seconds%s\n",
                   "\033[94m", diff, "\033[0m");
        }

        pthread_mutex_lock(&mutex);
        thread_count--;
        pthread_mutex_unlock(&mutex);

    }

    free(socket_desc);
    shutdown(sock, SHUT_RDWR);
    close(sock);

    pthread_exit(NULL);
}

void send_HttpResponse(const char *request, int sock) {
    char *req_method,    // "GET" or "POST"
    *request_file,       // "/index.html" things before '?'
    *args,        // "a=1&b=2"     things after  '?'
    *http_version, // "HTTP/1.1"
    *file_ext;

    char *copy = (char *) malloc(strlen(request) + 1);
    strcpy(copy, request);
    char *copy_request_file;

    parse_http_request(copy, &copy_request_file, &req_method, &request_file, &args, &http_version, &file_ext);

    if (strncmp(http_version, "HTTP/1.0", 8) != 0 &&
        strncmp(http_version, "HTTP/1.1", 8) != 0) // Bad request if not HTTP 1.0 or 1.1
    {
        send_404(sock);
        return;
    }

    char *copy_head = (char *) malloc(strlen(HTTP_HEADER) + 200);
    strcpy(copy_head, HTTP_HEADER);

    if (!strcmp(req_method, "GET")) {
        char file_path[500] = ".";
        if (strlen(request_file) <= 1) {
            //case that the request_file = "/"  --> Send index.html file
            strcat(file_path, "/index.html");
            strcat(copy_head, "Content-Type: text/html\r\n\r\n");
            if (send_file(sock, file_path, copy_head) == -1)
                fprintf(stderr, "%sAdd index.html file!%s\n", "\033[91m", "\033[0m");
        } else if (!strcmp(request_file, "/home")) {
            directory_listing(sock, ".");
        } else if (request_file[strlen(request_file) - 1] == '/') {
            directory_listing(sock, request_file + 1); // +1 - skip '/' in start of request_file
        } else if (!strcmp(file_ext, "html")) {
            //send html to client
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: text/html\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else if (strcmp(file_ext, "jpg") == 0 || strcmp(file_ext, "JPG") == 0) {
            //send image to client
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: image/jpeg\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else if (!strcmp(file_ext, "ico")) {
            strcat(file_path, "/favicon.ico");
            strcat(copy_head, "Content-Type: image/vnd.microsoft.icon\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else if (!strcmp(file_ext, "ttf")) {
            //font type, to display icon from FontAwesome
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: font/ttf\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else if (file_ext[strlen(file_ext) - 2] == 'j' && file_ext[strlen(file_ext) - 1] == 's') {
            //javascript
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: text/javascript\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else if (file_ext[strlen(file_ext) - 3] == 'c' && file_ext[strlen(file_ext) - 2] == 's' &&
                   file_ext[strlen(file_ext) - 1] == 's') {
            //css
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: text/css\r\n\r\n");
            send_file(sock, file_path, copy_head);
        } else {
            //send other file
            strcat(file_path, request_file);
            strcat(copy_head, "Content-Type: text/plain\r\n\r\n");
            send_file(sock, file_path, copy_head);
            printf("Else: %s \n", request_file);
        }
    } else if (!strcmp(req_method, "POST")) {
        char *find_string = find_token(request, "\r\n", "action");
        strcat(copy_head, "Content-Type: text/plain \r\n\r\n");
        strcat(copy_head, "User Action: ");
        strcat(copy_head, find_string);
        if (write(sock, copy_head, strlen(copy_head)) == -1) perror("write");

        printf("find string: %s \n", find_string);
    } else {
        send_404(sock);
    }
    free(copy_head);
    free(copy_request_file);
    free(copy);
}

void parse_http_request(char *copy_request, char **copy_request_file,
                        char **req_method, char **request_file,
                        char **args, char **http_version, char **file_ext) {
    *req_method = strtok(copy_request, " \t\r\n");
    *request_file = strtok(NULL, " \t");
    *http_version = strtok(NULL, " \t\r\n");
    if ((*args = strchr(*request_file, '?'))) {
        **args++ = '\0'; //split URI
    } else {
        *args = *request_file - 1; //use an empty string
    }

    //Pars file extension (such as JPG, jpg)
    *copy_request_file = (char *) malloc(strlen(*request_file) + 1);
    strcpy(*copy_request_file, *request_file);
    strtok(*copy_request_file, ".");
    *file_ext = strtok(NULL, " ");
    if (*file_ext == NULL) *file_ext = "";

    fprintf(stdout, "\x1b[32m+ [%s] %s %s %s\x1b[0m\n", *req_method, *request_file, *http_version, *args);
}

char *find_token(const char line[], const char symbol[], const char match[]) {
    char *copy = (char *) malloc(strlen(line) + 1);
    strcpy(copy, line);

    char *message;
    char *token = strtok(copy, symbol);

    while (token != NULL) {

        printf("--Token: %s \n", token);

        if (strlen(match) <= strlen(token)) {
            size_t match_char = 0;
            for (size_t i = 0; i < strlen(match); i++) {
                if (token[i] == match[i]) {
                    match_char++;
                }
            }
            if (match_char == strlen(match)) {
                message = token;
                return message;
            }
        }
        token = strtok(NULL, symbol);
    }
    message = "";
    return message;
}

int send_file(int sock, char file_path[], char head[]) {
    sem_wait(&file_sem); // Prevent two or more thread do some IO operation same time.

    struct stat stat_buf;  /* hold information about input file */
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        printf("Cannot Open file path : %s with error %d\n", file_path, fd);
        send_404(sock);
        return -1;
    }

    // return information about file
    fstat(fd, &stat_buf);
    long file_total_size = stat_buf.st_size;
    long block_size = stat_buf.st_blksize; // optimal block size

    send(sock, head, strlen(head), 0);
    // send messages in chunks
    while (file_total_size > 0) {
        long send_bytes = ((file_total_size < block_size) ? file_total_size : block_size);
        long done_bytes = sendfile(sock, fd, NULL, send_bytes);
        file_total_size = file_total_size - done_bytes;
    }
    printf("send file: %s \n", file_path);
    close(fd);

    sem_post(&file_sem);
    return 0;
}

void send_404(int sock) {
    const char *buffer = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n<!doctype html><html><body>400 Bad Request</body></html>";
    send(sock, buffer, strlen(buffer), 0);
}

void directory_listing(int sock, char *directory_path) {
    char *buffer = (char *) malloc(5000 * sizeof(char));
    char *dirNameBuffer = (char *) calloc(2000, sizeof(char));
    char *fileNameBuffer = (char *) calloc(2000, sizeof(char));

    strcpy(buffer, "<html><body><h1>Directory listing for: ");
    strcat(buffer, directory_path);
    strcat(buffer, "</h1><ul>\n");

    sem_wait(&dir_sem); // Prevent two or more thread do some IO operation same time.
    DIR *dir = opendir(directory_path);
    if (!dir) {
        perror("Failed to open directory");
        send_404(sock);
        return;
    }

    struct dirent *entry;
    int dirCount = 0;
    int fileCount = 0;

    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        // Separate directory and file listings. Names of each
        // are kept in a single buffer, separated by null characters.
        if (entry->d_type == DT_DIR) {
            strcat(dirNameBuffer, entry->d_name);
            strcat(dirNameBuffer, "\n");
            ++dirCount;
        } else if (entry->d_type == DT_REG) {
            strcat(fileNameBuffer, entry->d_name);
            strcat(fileNameBuffer, "\n");
            ++fileCount;
        }
    }
    sem_post(&dir_sem);

    // Here we set up pointers to the begine of each name
    // two buffers of file and directory names. We'll
    // sort pointers to arrange the listing alphabetically.
    char **directoryNames = (char **) malloc(sizeof(char *) * dirCount);
    for (int i = 0; i < dirCount; ++i) directoryNames[i] = (char *) malloc(sizeof(char) * 100);
    char **fileNames = (char **) malloc(sizeof(char *) * fileCount);
    for (int i = 0; i < fileCount; ++i) fileNames[i] = (char *) malloc(sizeof(char) * 100);

    for (size_t i = 0, j = 0, k = 0; i < strlen(dirNameBuffer); ++i, ++k) {
        if (dirNameBuffer[i] == '\n') {
            directoryNames[j][k] = '\0';
            ++j;
            k = -1;
            if (dirNameBuffer[i + 1] == '\0') break;
            continue;
        }
        directoryNames[j][k] = dirNameBuffer[i];
    }

    for (size_t i = 0, j = 0, k = 0; i < strlen(fileNameBuffer); ++i, ++k) {
        if (fileNameBuffer[i] == '\n') {
            fileNames[j][k] = '\0';
            ++j;
            k = -1;
            if (fileNameBuffer[i + 1] == '\0') break;
            continue;
        }
        fileNames[j][k] = fileNameBuffer[i];
    }

    // Sort the two lists.
    sortFilenameList(directoryNames, dirCount);
    sortFilenameList(fileNames, fileCount);

    // List directories.
    for (int i = 0; i < dirCount; ++i) {
        strcat(buffer, "<li><a href=\"");
        strcat(buffer, directoryNames[i]);
        strcat(buffer, "/\">");
        strcat(buffer, directoryNames[i]);
        strcat(buffer, "/</a></li>\n");
    }

    // List files.
    for (int i = 0; i < fileCount; ++i) {
        strcat(buffer, "<li><a href=\"");
        strcat(buffer, fileNames[i]);
        strcat(buffer, "\">");
        strcat(buffer, fileNames[i]);
        strcat(buffer, "</a></li>\n");
    }
    strcat(buffer, "</ul></body></html>\n");

    char *copy_head = (char *) malloc(strlen(HTTP_HEADER) + 200);
    strcpy(copy_head, HTTP_HEADER);
    strcat(copy_head, "Content-Type: text/html\r\n\r\n");
    send(sock, copy_head, strlen(copy_head), 0);

    send(sock, buffer, strlen(buffer), 0);

    free(buffer);
    free(copy_head);
    free(dirNameBuffer);
    free(fileNameBuffer);
    for (int i = 0; i < dirCount; ++i) free(directoryNames[i]);
    for (int i = 0; i < fileCount; ++i) free(fileNames[i]);
    free(directoryNames);
    free(fileNames);

    closedir(dir);
}

void sortFilenameList(char **list, int length) {
    char *current;
    for (int i = 1; i < length; ++i) {
        current = list[i];
        int j = i;
        while (j > 0) {
            if (compareFilenames(current, list[j - 1]) < 0) {
                list[j] = list[j - 1];
            } else {
                break;
            }
            --j;
        }
        list[j] = current;
    }
}

// firstly alphabet, secondly len
int compareFilenames(const char *filename1, const char *filename2) {
    int i = 0;
    while (filename1[i] != '\n' || filename2[i] != '\n') {
        if (filename1[i] == '\n') {
            // Name 1 is shorter
            return -1;
        }

        if (filename2[i] == '\n') {
            // Name 2 is shorter
            return 1;
        }

        int cmp = filename1[i] - filename2[i];

        if (cmp != 0) {
            return cmp;
        }

        ++i;
    }
    return 0;
}