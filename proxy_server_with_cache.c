#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <regex.h>
#include <ctype.h>

#define MAX_BYTES 8192
#define TIMEOUT_SECONDS 30
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX_SIZE 200 * (1 << 20)
#define MAX_ELEMENT_SIZE 10 * (1 << 20)
#define MAX_CLIENTS 10
#define RATE_LIMIT_WINDOW 60
#define MAX_REQUESTS 100
#define MAX_IP_ENTRIES 10000

typedef struct cache_element cache_element;

cache_element *find(char *url);
int add_cache_element(char *data, int size, char *url);
void remove_cache_element();
int hashFunction(const char *url, int hashSize);
int connectRemoteServer(char *host, int port);
int send_cached_response(int clientSocket, cache_element *cached_response);

int port_number = 8080;
int proxy_socketId;

bool socket_in_use[MAX_CLIENTS] = {false};
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;
int connected_socketId[MAX_CLIENTS];
pthread_t tid[MAX_CLIENTS];

pthread_mutex_t lock;
sem_t semaphore;

typedef struct LRUCache {
    int capacity;
    int size;
    cache_element *head;
    cache_element *tail;
    cache_element **map;
    int hashSize;
} LRUCache;

struct cache_element {
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    struct cache_element *prev;
    struct cache_element *next;
};

LRUCache *cache;
int cache_size;

typedef struct {
    char ip_address[INET_ADDRSTRLEN];
    int request_count;
    time_t window_start;
} RateLimit;

typedef struct {
    RateLimit *entries;
    pthread_mutex_t lock;
    int size;
} RateLimiter;

RateLimiter *rate_limiter = NULL;

int find_free_slot() {
    pthread_mutex_lock(&socket_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(!socket_in_use[i]) {
            socket_in_use[i] = true;
            pthread_mutex_unlock(&socket_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&socket_mutex);
    return -1;
}

void free_slot(int slot) {
    if(slot >= 0 && slot < MAX_CLIENTS) {
        pthread_mutex_lock(&socket_mutex);
        socket_in_use[slot] = false;
        pthread_mutex_unlock(&socket_mutex);
    }
}

cache_element *createCacheElement(const char *url, const char *data, int len) {
    cache_element *newElement = (cache_element *)malloc(sizeof(cache_element));
    if (!newElement) {
        perror("Memory allocation failed for newElement");
        return NULL;
    }

    newElement->url = strdup(url);
    newElement->data = strdup(data);
    newElement->len = len;
    newElement->lru_time_track = time(NULL);
    newElement->prev = NULL;
    newElement->next = NULL;

    if (!newElement->url || !newElement->data) {
        free(newElement->url);
        free(newElement->data);
        free(newElement);
        return NULL;
    }
    return newElement;
}

LRUCache *lruCacheCreate(int capacity) {
    LRUCache *cache = (LRUCache *)malloc(sizeof(LRUCache));
    if (!cache) {
        perror("Memory allocation failed for cache");
        return NULL;
    }

    cache->capacity = capacity;
    cache->size = 0;
    cache->hashSize = 1009;
    cache->map = (cache_element **)calloc(cache->hashSize, sizeof(cache_element *));
    if (!cache->map) {
        free(cache);
        perror("Memory allocation failed for cache map");
        return NULL;
    }

    cache->head = createCacheElement("", "", 0);
    cache->tail = createCacheElement("", "", 0);
    if (!cache->head || !cache->tail) {
        free(cache->head);
        free(cache->tail);
        free(cache->map);
        free(cache);
        perror("Failed to create dummy head and tail");
        return NULL;
    }

    cache->head->next = cache->tail;
    cache->tail->prev = cache->head;
    return cache;
}

void addNode(LRUCache *cache, cache_element *element) {
    element->next = cache->head->next;
    element->prev = cache->head;
    cache->head->next->prev = element;
    cache->head->next = element;
}

void deleteNode(cache_element *element) {
    if (element && element->prev && element->next) {
        element->prev->next = element->next;
        element->next->prev = element->prev;
    }
}

void removeFromHashMap(LRUCache *cache, const char *url) {
    int idx = hashFunction(url, cache->hashSize);
    cache_element *current = cache->map[idx];
    cache_element *prev = NULL;

    while (current) {
        if (strcmp(current->url, url) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                cache->map[idx] = current->next;
            }
            free(current->url);
            free(current->data);
            free(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

void addToHashMap(LRUCache *cache, cache_element *element) {
    int idx = hashFunction(element->url, cache->hashSize);
    element->next = cache->map[idx];
    cache->map[idx] = element;
}

int hashFunction(const char *url, int hashSize) {
    unsigned long hash = 5381;
    while (*url) {
        hash = ((hash << 5) + hash) + *url++;
    }
    return hash % hashSize;
}

RateLimiter *init_rate_limiter() {
    RateLimiter *limiter = (RateLimiter*)malloc(sizeof(RateLimiter));
    if (!limiter) {
        perror("Failed to allocate rate limiter");
        return NULL;
    }

    limiter->entries = (RateLimit*)calloc(MAX_IP_ENTRIES, sizeof(RateLimit));
    if (!limiter->entries) {
        perror("Failed to allocate rate limit entries");
        free(limiter);
        return NULL;
    }

    if (pthread_mutex_init(&limiter->lock, NULL) != 0) {
        perror("Failed to initialize rate limiter mutex");
        free(limiter->entries);
        free(limiter);
        return NULL;
    }

    limiter->size = 0;
    return limiter;
}

unsigned int ip_hash(const char* ip) {
    unsigned int hash = 5381;
    int c;
    while ((c = *ip++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_IP_ENTRIES;
}

RateLimit* get_rate_limit(RateLimiter* limiter, const char* ip) {
    unsigned int index = ip_hash(ip);
    unsigned int original_index = index;

    do {
        if (limiter->entries[index].ip_address[0] == '\0' ||
            strcmp(limiter->entries[index].ip_address, ip) == 0) {
            if (limiter->entries[index].ip_address[0] == '\0') {
                strncpy(limiter->entries[index].ip_address, ip, INET_ADDRSTRLEN - 1);
                limiter->entries[index].request_count = 0;
                limiter->entries[index].window_start = time(NULL);
                limiter->size++;
            }
            return &limiter->entries[index];
        }
        index = (index + 1) % MAX_IP_ENTRIES;
    } while (index != original_index && limiter->size < MAX_IP_ENTRIES);

    return NULL;
}

int is_rate_limited(const char* ip) {
    if (!rate_limiter) {
        return 0;
    }

    pthread_mutex_lock(&rate_limiter->lock);

    RateLimit* limit = get_rate_limit(rate_limiter, ip);
    if (!limit) {
        pthread_mutex_unlock(&rate_limiter->lock);
        return 1;
    }

    time_t current_time = time(NULL);

    if (current_time - limit->window_start >= RATE_LIMIT_WINDOW) {
        limit->request_count = 0;
        limit->window_start = current_time;
    }
    int should_limit = (limit->request_count >= MAX_REQUESTS);
    if (!should_limit) {
        limit->request_count++;
    }

    pthread_mutex_unlock(&rate_limiter->lock);
    return should_limit;
}

void decrease_request_count(const char* ip) {
    if (!rate_limiter) {
        return;
    }
    pthread_mutex_lock(&rate_limiter->lock);
    RateLimit* limit = get_rate_limit(rate_limiter, ip);
    pthread_mutex_unlock(&rate_limiter->lock);

    if (!limit) {
        return;
    }

    // In the original code snippet, there's a lock named ip_lock that doesn't appear
    // to exist. If you want to protect each entry individually, add a mutex in RateLimit.
    // For simplicity here, assume we use the same rate_limiter->lock above.
    pthread_mutex_lock(&rate_limiter->lock);
    if (limit->request_count > 0) {
        limit->request_count--;
        printf("IP %s: Decreased request count to %d\n", ip, limit->request_count);
    }
    pthread_mutex_unlock(&rate_limiter->lock);
}

void cleanup_rate_limiter() {
    if (rate_limiter) {
        pthread_mutex_destroy(&rate_limiter->lock);
        free(rate_limiter->entries);
        free(rate_limiter);
    }
}

void send_rate_limit_error(int socket) {
    char str[1024];
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    char currentTime[50];

    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);
    snprintf(str, sizeof(str),
             "HTTP/1.1 429 Too Many Requests\r\n"
             "Content-Type: text/html\r\n"
             "Retry-After: %d\r\n"
             "Date: %s\r\n"
             "Content-Length: 125\r\n"
             "Connection: close\r\n"
             "\r\n"
             "<html><head><title>429 Too Many Requests</title></head>\n"
             "<body><h1>Too Many Requests</h1>\n"
             "<p>Please try again later.</p></body></html>",
             RATE_LIMIT_WINDOW,
             currentTime);

    send(socket, str, strlen(str), 0);
}

void to_lower(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);
    }
}

int validate_url(const char *url) {
    if (url == NULL) {
        return 0;
    }
    const char *url_pattern = "^(https?://)?[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}(/[^/]*)*$";
    regex_t regex;
    int ret;

    if (regcomp(&regex, url_pattern, REG_EXTENDED | REG_ICASE) != 0) {
        fprintf(stderr, "Failed to compile regex\n");
        return 0;
    }
    ret = regexec(&regex, url, 0, NULL, 0);
    regfree(&regex);
    return (ret == 0) ? 1 : 0;
}

int is_not_allowed_domain(const char* url) {
    if (url == NULL) {
        return 1;
    }
    static const char *notallowedDomains[] = {
        "github.com",
        "malicious-site.com",
        NULL
    };

    char url_copy[512];
    if (strlen(url) >= sizeof(url_copy)) {
        return 1;
    }
    strcpy(url_copy, url);
    to_lower(url_copy);

    const char *domain_start = strstr(url_copy, "://");
    if (domain_start) {
        domain_start += 3;
    } else {
        domain_start = url_copy;
    }
    if (strncmp(domain_start, "www.", 4) == 0) {
        domain_start += 4;
    }
    const char *domain_end = strchr(domain_start, '/');
    if (!domain_end) {
        domain_end = domain_start + strlen(domain_start);
    }
    char domain[256];
    size_t domain_length = domain_end - domain_start;
    if (domain_length >= sizeof(domain)) {
        return 1;
    }
    strncpy(domain, domain_start, domain_length);
    domain[domain_length] = '\0';

    for (int i = 0; notallowedDomains[i]; i++) {
        if (strcmp(domain, notallowedDomains[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    char currentTime[50];
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    switch (status_code) {
        case 400:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\n"
                     "Content-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n"
                     "<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 403:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\n"
                     "Connection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n"
                     "<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 404:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\n"
                     "Connection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n"
                     "<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 500:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\n"
                     "Content-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n"
                     "<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 501:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\n"
                     "Content-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n"
                     "<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 505:
            snprintf(str, sizeof(str),
                     "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\n"
                     "Content-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n"
                     "<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n"
                     "<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
            send(socket, str, strlen(str), 0);
            break;
        default:
            return -1;
    }
    return 1;
}

int checkHTTPversion(char *msg) {
    int version = -1;
    if (strncmp(msg, "HTTP/1.1", 8) == 0) {
        version = 1;
    } else if (strncmp(msg, "HTTP/1.0", 8) == 0) {
        version = 1;
    } else {
        version = -1;
    }
    return version;
}

int connectRemoteServer(char *host, int port) {
    struct hostent *server;
    struct sockaddr_in serverAddr;
    int sockfd;

    if ((server = gethostbyname(host)) == NULL) {
        fprintf(stderr, "Error resolving hostname %s\n", host);
        return -1;
    }
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Failed to create socket");
        return -1;
    }
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
    serverAddr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Failed to connect to remote server");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int handle_request(int clientSocket, struct ParsedRequest *request, char *tempReq) {
    if (!request || !tempReq) {
        fprintf(stderr, "Invalid request parameters\n");
        return -1;
    }
    char *buf = (char *)calloc(MAX_BYTES, sizeof(char));
    char *response_buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    if (!buf || !response_buffer) {
        perror("Failed to allocate buffers");
        free(buf);
        free(response_buffer);
        return -1;
    }
    char full_path[MAX_BYTES];
    snprintf(full_path, MAX_BYTES, "%s", request->path ? request->path : "/");

    if (snprintf(buf, MAX_BYTES, "GET %s HTTP/1.1\r\n", full_path) >= MAX_BYTES) {
        fprintf(stderr, "Request too long\n");
        free(buf);
        free(response_buffer);
        return -1;
    }

    if (ParsedHeader_set(request, "Connection", "close") < 0 ||
        ParsedHeader_set(request, "Host", request->host) < 0 ||
        ParsedHeader_set(request, "User-Agent", "Mozilla/5.0 (Compatible)") < 0 ||
        ParsedHeader_set(request, "Accept", "*/*") < 0 ||
        ParsedHeader_set(request, "Accept-Encoding", "identity") < 0) {
        fprintf(stderr, "Failed to set required headers\n");
    }
    size_t len = strlen(buf);
    if (ParsedRequest_unparse_headers(request, buf + len, MAX_BYTES - len) < 0) {
        fprintf(stderr, "Warning: Failed to unparse headers\n");
    }
    strcat(buf, "\r\n");

    int server_port = 80;
    if (request->port) {
        server_port = atoi(request->port);
    } else if (strstr(request->host, "https") != NULL) {
        server_port = 443;
    }

    printf("DEBUG - Connecting to %s:%d\n", request->host, server_port);
    printf("DEBUG - Full request:\n%s\n", buf);

    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0) {
        free(buf);
        free(response_buffer);
        return -1;
    }

    ssize_t bytes_sent = send(remoteSocketID, buf, strlen(buf), 0);
    if (bytes_sent < 0) {
        perror("Failed to send request to remote server");
        free(buf);
        free(response_buffer);
        close(remoteSocketID);
        return -1;
    }
    size_t total_received = 0;
    int status_code = 0;
    bool headers_received = false;

    while (1) {
        ssize_t bytes_received = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
        if (bytes_received <= 0) {
            break;
        }
        if (!headers_received) {
            buf[bytes_received] = '\0';
            sscanf(buf, "%*s %d", &status_code);
            headers_received = true;
        }
        char *send_ptr = buf;
        size_t remaining = bytes_received;
        while (remaining > 0) {
            bytes_sent = send(clientSocket, send_ptr, remaining, 0);
            if (bytes_sent < 0) {
                perror("Failed to send to client");
                free(buf);
                free(response_buffer);
                close(remoteSocketID);
                return -1;
            }
            remaining -= bytes_sent;
            send_ptr += bytes_sent;
        }
        if (total_received + bytes_received < MAX_BYTES) {
            memcpy(response_buffer + total_received, buf, bytes_received);
            total_received += bytes_received;
        }
        bzero(buf, MAX_BYTES);
    }
    if (total_received > 0) {
        response_buffer[total_received] = '\0';
        add_cache_element(response_buffer, total_received, tempReq);
    }
    printf("Request handled successfully\n");

    free(response_buffer);
    free(buf);
    close(remoteSocketID);
    return 0;
}

void *thread_fn(void *clientSocket) {
    int socket = *((int *)clientSocket);
    if (socket < 0) {
        return (void*)(intptr_t)-1;
    }
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    char client_ip[INET_ADDRSTRLEN];
    getpeername(socket, (struct sockaddr *)&addr, &addr_size);
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));

    // Optional: also do rate limiting again here if desired
    if (is_rate_limited(client_ip)) {
        printf("Rate limit exceeded (thread check) for IP: %s\n", client_ip);
        send_rate_limit_error(socket);
        close(socket);
        return NULL;
    }

    // Setup timeout
    struct timeval timeout = {.tv_sec = TIMEOUT_SECONDS, .tv_usec = 0};
    if (sem_wait(&semaphore) != 0) {
        perror("Semaphore wait failed");
        return (void *)(intptr_t)-1;
    }
    int sem_val = 0;
    sem_getvalue(&semaphore, &sem_val);
    printf("Semaphore value: %d\n", sem_val);

    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    if (!buffer) {
        perror("Memory allocation failed for buffer");
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }

    ssize_t bytes_received = recv(socket, buffer, MAX_BYTES - 1, 0);
    if (bytes_received <= 0) {
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }
    size_t total_bytes = bytes_received;
    while (strstr(buffer, "\r\n\r\n") == NULL && total_bytes < MAX_BYTES - 1) {
        bytes_received = recv(socket, buffer + total_bytes, MAX_BYTES - total_bytes - 1, 0);
        if (bytes_received <= 0) break;
        total_bytes += bytes_received;
    }
    buffer[total_bytes] = '\0';

    char *tempReq = strdup(buffer);
    if (!tempReq) {
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }

    struct ParsedRequest *request = ParsedRequest_create();
    if (!request) {
        free(tempReq);
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }
    if (ParsedRequest_parse(request, buffer, total_bytes) < 0) {
        sendErrorMessage(socket, 400);
        ParsedRequest_destroy(request);
        free(tempReq);
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void *)(intptr_t)-1;
    }

    // Check URL validity and domain blocking based on request->host
    if (!validate_url(request->host)) {
        sendErrorMessage(socket, 403);
        ParsedRequest_destroy(request);
        free(tempReq);
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return NULL;
    }
    if (is_not_allowed_domain(request->host)) {
        sendErrorMessage(socket, 403);
        ParsedRequest_destroy(request);
        free(tempReq);
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return NULL;
    }

    // Check cache
    cache_element *cached_response = find(tempReq);
    if (cached_response) {
        int result = send_cached_response(socket, cached_response);
        if (result < 0) {
            fprintf(stderr, "Failed to send cached response\n");
        } else {
            printf("Data retrieved from cache\n");
        }
    } else {
        if (handle_request(socket, request, tempReq) < 0) {
            sendErrorMessage(socket, 500);
        }
    }
    ParsedRequest_destroy(request);

    decrease_request_count(client_ip);
    free(tempReq);
    free(buffer);

    int slot = ((int*)clientSocket - connected_socketId);
    free_slot(slot);

    shutdown(socket, SHUT_RDWR);
    close(socket);

    sem_post(&semaphore);
    sem_getvalue(&semaphore, &sem_val);
    printf("Semaphore post value: %d\n", sem_val);

    return NULL;
}

char *normalize_url(char *request) {
    if (!request) {
        return NULL;
    }
    char *start = request;
    if (strncmp(request, "GET /", 5) == 0) {
        start += 5;
    }
    char *url_start = strstr(start, "https://");
    if (!url_start) {
        url_start = strstr(start, "http://");
    }
    if (!url_start) {
        return strdup(start);
    }
    char *end = strchr(url_start, ' ');
    if (end) {
        char *normalized = (char *)malloc(end - url_start + 1);
        if (normalized) {
            strncpy(normalized, url_start, end - url_start);
            normalized[end - url_start] = '\0';
            return normalized;
        }
    } else {
        return strdup(url_start);
    }
    return NULL;
}

int send_cached_response(int clientSocket, cache_element *cached_response) {
    if (!cached_response || clientSocket < 0) {
        fprintf(stderr, "Invalid cached response or client socket\n");
        return -1;
    }
    ssize_t bytes_sent = 0;
    size_t total_bytes_sent = 0;
    size_t remaining = cached_response->len;

    while (remaining > 0) {
        bytes_sent = send(clientSocket, cached_response->data + total_bytes_sent, remaining, 0);
        if (bytes_sent < 0) {
            perror("Failed to send cached response");
            return -1;
        }
        remaining -= bytes_sent;
        total_bytes_sent += bytes_sent;
    }
    printf("Cached response sent: URL: %s, Bytes Sent: %ld\n",
           cached_response->url, total_bytes_sent);
    return 0;
}

cache_element *find(char *url) {
    if (!url) {
        fprintf(stderr, "find: Null URL provided\n");
        return NULL;
    }
    char *normalized = normalize_url(url);
    if (!normalized) {
        fprintf(stderr, "find: Failed to normalize URL\n");
        return NULL;
    }
    printf("Debug - Looking up cache:\n");
    printf("  Original request: %s\n", url);
    printf("  Normalized URL: %s\n", normalized);

    int idx = hashFunction(normalized, cache->hashSize);
    pthread_mutex_lock(&lock);
    cache_element *current = cache->map[idx];
    while (current) {
        if (strcmp(current->url, normalized) == 0) {
            current->lru_time_track = time(NULL);
            deleteNode(current);
            addNode(cache, current);
            pthread_mutex_unlock(&lock);
            free(normalized);
            return current;
        }
        current = current->next;
    }
    free(normalized);
    pthread_mutex_unlock(&lock);
    return NULL;
}

void remove_cache_element() {
    pthread_mutex_lock(&lock);
    if (cache->size == 0) {
        pthread_mutex_unlock(&lock);
        return;
    }
    cache_element *lru = cache->tail->prev;
    deleteNode(lru);
    removeFromHashMap(cache, lru->url);
    cache->size -= (lru->len + strlen(lru->url) + sizeof(cache_element));
    printf("Cache EVICTION: Removed LRU element with URL: %s\n", lru->url);
    free(lru->data);
    free(lru->url);
    free(lru);
    pthread_mutex_unlock(&lock);
}

int add_cache_element(char *data, int size, char *url) {
    pthread_mutex_lock(&lock);
    char *normalized = normalize_url(url);
    if (!normalized) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    int element_size = size + 1 + strlen(normalized) + sizeof(cache_element);
    if (element_size > MAX_ELEMENT_SIZE) {
        printf("Cache REJECT: Element too large (size: %d, URL: %s)\n",
               element_size, normalized);
        pthread_mutex_unlock(&lock);
        free(normalized);
        return 0;
    }
    while (cache->size + element_size > MAX_SIZE) {
        remove_cache_element();
    }
    cache_element *newElement = createCacheElement(normalized, data, size);
    if (newElement) {
        addNode(cache, newElement);
        addToHashMap(cache, newElement);
        cache->size += element_size;
        printf("Cache ADD: URL: %s, Size: %d\n", normalized, size);
        free(normalized);
        pthread_mutex_unlock(&lock);
        return 1;
    }
    free(normalized);
    pthread_mutex_unlock(&lock);
    return 0;
}

void freeCache(LRUCache *cache) {
    if (cache == NULL) {
        return;
    }
    cache_element *current = cache->head;
    while (current != NULL) {
        cache_element *next = current->next;
        free(current->url);
        free(current->data);
        free(current);
        current = next;
    }
    free(cache);
}

int main(int argc, char *argv[]) {
    int client_socketId, client_len;
    struct sockaddr_in server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);
    cache = lruCacheCreate(MAX_SIZE);
    rate_limiter = init_rate_limiter();
    if (!rate_limiter) {
        perror("Failed to initialize rate limiter");
        exit(1);
    }

    if (argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Very Few Arguments\n");
        exit(1);
    }
    printf("Starting Proxy Server at Port: %d\n", port_number);

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0) {
        perror("Failed to create socket\n");
        exit(EXIT_FAILURE);
    }
    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(proxy_socketId);
        exit(1);
    }

    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port is not available");
        exit(1);
    }

    if (listen(proxy_socketId, MAX_CLIENTS) < 0) {
        perror("Error in listening\n");
        exit(1);
    }

    printf("Proxy is listening...\n");
    while (1) {
        bzero((char *)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);
        client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr,
                                 (socklen_t *)&client_len);
        if (client_socketId < 0) {
            printf("Not able to connect\n");
            continue;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // Rate limit check in main
        if (is_rate_limited(client_ip)) {
            printf("Rate limit exceeded (main check) for IP: %s\n", client_ip);
            send_rate_limit_error(client_socketId);
            close(client_socketId);
            continue;
        }

        int slot = find_free_slot();
        if (slot == -1) {
            printf("Maximum clients reached. Rejecting connection.\n");
            close(client_socketId);
            continue;
        }
        connected_socketId[slot] = client_socketId;
        if (pthread_create(&tid[slot], NULL, thread_fn, (void*)&connected_socketId[slot]) != 0) {
            perror("Failed to create thread");
            free_slot(slot);
            close(client_socketId);
            continue;
        }
        struct sockaddr_in *client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("Client is connected in slot %d with port number %d and ip address is %s\n",
               slot, ntohs(client_addr.sin_port), str);

        // Optionally decrease immediately if you only want to count "new" connections
        decrease_request_count(client_ip);
    }

    cleanup_rate_limiter();
    close(proxy_socketId);
    freeCache(cache);
    return 0;
}
