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


#define MAX_BYTES 8192 
#define TIMEOUT_SECONDS 30
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX_SIZE 200*(1<<20)  
#define MAX_ELEMENT_SIZE 10*(1<<20)
#define MAX_CLIENTS 10

typedef struct cache_element cache_element;


cache_element* find(char* url);
int add_cache_element(char* data,int size,char* url);
void remove_cache_element();
int hashFunction(const char *url, int hashSize);
int connectRemoteServer(char *host, int port);
int send_cached_response(int clientSocket, cache_element* cached_response);


int port_number=8080;
int proxy_socketId;

pthread_t tid[MAX_CLIENTS]; // create thread for one kind each and connect to one socket
/* 
   Cache operations must handle concurrent access because multiple requests 
   can arrive simultaneously.
   Since the cache is a shared resource, we use a mutex lock to ensure 
   safe access by only one thread at a time.
   When a request arrives, it first checks if the lock is available:
   - If the lock is free, the request acquires it and proceeds with the operation.
   - If the lock is already held by another thread, the request waits until 
     the lock becomes available.

   This mechanism prevents conflicts and ensures that no two threads 
   modify the cache at the same time.
*/
pthread_mutex_t lock; 

/* 
   A semaphore is a synchronization mechanism(also a type of lock having multiple values) used to 
   manage access to a shared resource among multiple threads or processes. It helps control the 
   number of threads that can access the resource simultaneously.

   Semaphores maintain a counter that tracks the number of permits available:
   - When a thread wants to access the resource, it decreases the counter (acquires a permit) 
     using `sem_wait()`. If the counter is greater than zero, the thread proceeds.
   - If the counter is zero, the thread waits until another thread releases a permit.

   When a thread finishes using the resource, it increases the counter (releases a permit) 
   using `sem_signal()`. This signals other waiting threads that a permit is now available.

   Semaphores are widely used to enforce limits on resource usage, prevent race conditions, 
   and ensure proper synchronization in concurrent programming.
*/

sem_t semaphore; 

cache_element* head; // global head of cache
int cache_size;

typedef struct LRUCache {
    int capacity;                  
    int size;              
    cache_element* head;     
    cache_element* tail;         
    cache_element** map;         
    int hashSize;              
} LRUCache;

LRUCache* cache;

struct cache_element{
    char* data; // last response on same request
    int len; // bytes of data
    char* url; // which request goes (http://gogle.com)
    time_t lru_time_track; // time based lru cache
    struct cache_element* prev;   // Pointer to the previous element in the list
    struct cache_element* next;   // Pointer to the next element in the list
};   

cache_element* createCacheElement(const char* url, const char* data, int len) {
    cache_element* newElement = (cache_element*)malloc(sizeof(cache_element));
    if (!newElement) {
        perror("Memory allocation failed for newElement");
        return NULL;
    }

    newElement->url = strdup(url);
    newElement->data = strdup(data);
    newElement->len = len;
    newElement->lru_time_track = time(NULL); // Set current time
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

LRUCache* lruCacheCreate(int capacity) {
    LRUCache* cache = (LRUCache*)malloc(sizeof(LRUCache));
    if (!cache) {
        perror("Memory allocation failed for cache");
        return NULL;
    }

    cache->capacity = capacity;
    cache->size = 0;
    cache->hashSize = 1009;
    cache->map = (cache_element**)calloc(cache->hashSize, sizeof(cache_element*));

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

void addNode(LRUCache* cache, cache_element* element) {
    element->next = cache->head->next;
    element->prev = cache->head;
    cache->head->next->prev = element;
    cache->head->next = element;
}

void deleteNode(cache_element* element) {
    if (element && element->prev && element->next) {
        element->prev->next = element->next;
        element->next->prev = element->prev;
    }
}

void removeFromHashMap(LRUCache* cache,const char* url){
    int idx = hashFunction(url, cache->hashSize);
    cache_element* current = cache->map[idx];
    cache_element* prev = NULL;

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

void addToHashMap(LRUCache* cache, cache_element* element) {
    int idx = hashFunction(element->url, cache->hashSize);
    element->next = cache->map[idx];
    cache->map[idx] = element;
}

int hashFunction(const char* url, int hashSize) {
    unsigned long hash = 5381;
    while (*url) {
        hash = ((hash << 5) + hash) + *url++;
    }
    return hash % hashSize;
}

int sendErrorMessage(int socket, int status_code){
	char str[1024];
	char currentTime[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(status_code)
	{
		case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
				  printf("400 Bad Request\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
				  printf("403 Forbidden\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
				  printf("404 Not Found\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
				  //printf("500 Internal Server Error\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
				  printf("501 Not Implemented\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, str, strlen(str), 0);
				  break;

		default:  return -1;

	}
	return 1;
}

int checkHTTPversion(char *msg){
	int version = -1;

	if(strncmp(msg, "HTTP/1.1", 8) == 0)
	{
		version = 1;
	}
	else if(strncmp(msg, "HTTP/1.0", 8) == 0)			
	{
		version = 1;								
	}
	else
		version = -1;

	return version;
}

int connectRemoteServer(char *host, int port) {
    struct hostent *server;
    struct sockaddr_in serverAddr;
    int sockfd;

    // Get host by name
    if ((server = gethostbyname(host)) == NULL) {
        fprintf(stderr, "Error resolving hostname %s\n", host);
        return -1;
    }

    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Failed to create socket");
        return -1;
    }

    // Configure server address
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
    serverAddr.sin_port = htons(port);

    // Connect to remote server
    if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
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

    // Allocate buffers
    char *buf = (char *)calloc(MAX_BYTES, sizeof(char));
    char *response_buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    if (!buf || !response_buffer) {
        perror("Failed to allocate buffers");
        free(buf);
        free(response_buffer);
        return -1;
    }

    // Build full path including query string
    char full_path[MAX_BYTES];
    snprintf(full_path, MAX_BYTES, "%s", 
         request->path ? request->path : "/");

    // Build initial request line
    if (snprintf(buf, MAX_BYTES, "GET %s HTTP/1.1\r\n", full_path) >= MAX_BYTES) {
        fprintf(stderr, "Request too long\n");
        free(buf);
        free(response_buffer);
        return -1;
    }

    // Set required headers
    if (ParsedHeader_set(request, "Connection", "close") < 0 ||
        ParsedHeader_set(request, "Host", request->host) < 0 ||
        ParsedHeader_set(request, "User-Agent", "Mozilla/5.0 (Compatible)") < 0 ||
        ParsedHeader_set(request, "Accept", "*/*") < 0 ||
        ParsedHeader_set(request, "Accept-Encoding", "identity") < 0) {
        fprintf(stderr, "Failed to set required headers\n");
        // Continue anyway - headers might still be valid
    }

    // Add headers to request
    size_t len = strlen(buf);
    if (ParsedRequest_unparse_headers(request, buf + len, MAX_BYTES - len) < 0) {
        fprintf(stderr, "Warning: Failed to unparse headers\n");
        // Continue with partial headers
    }

    // Add final CRLF
    strcat(buf, "\r\n");

    // Determine port (default 80, or 443 for HTTPS)
    int server_port = 80;
    if (request->port) {
        server_port = atoi(request->port);
    } else if (strstr(request->host, "https") != NULL) {
        server_port = 443;
    }

    // Debug output
    printf("DEBUG - Connecting to %s:%d\n", request->host, server_port);
    printf("DEBUG - Full request:\n%s\n", buf);

    // Connect to remote server
    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0) {
        free(buf);
        free(response_buffer);
        return -1;
    }

    // Send request to remote server
    ssize_t bytes_sent = send(remoteSocketID, buf, strlen(buf), 0);
    if (bytes_sent < 0) {
        perror("Failed to send request to remote server");
        free(buf);
        free(response_buffer);
        close(remoteSocketID);
        return -1;
    }

    // Prepare for response
    size_t total_received = 0;
    int status_code = 0;
    bool headers_received = false;

    // Receive and forward response
    while (1) {
        ssize_t bytes_received = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
        
        if (bytes_received <= 0) {
            break;  // Connection closed or error
        }

        // Check status code on first chunk
        if (!headers_received) {
            buf[bytes_received] = '\0';
            sscanf(buf, "%*s %d", &status_code);
            printf("DEBUG - Response status: %d\n", status_code);
            
            // Handle redirects (300-399)
            if (status_code >= 300 && status_code < 400) {
                // Extract Location header
                char *location = strstr(buf, "Location: ");
                if (location) {
                    printf("DEBUG - Redirect detected to: %s\n", location);
                    // Here you could implement redirect handling
                }
            }
            headers_received = true;
        }

        // Forward to client
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

        // Store in response buffer for caching
        if (total_received + bytes_received < MAX_BYTES) {
            memcpy(response_buffer + total_received, buf, bytes_received);
            total_received += bytes_received;
        }

        bzero(buf, MAX_BYTES);
    }

    // Add response to cache
    if (total_received > 0) {
        response_buffer[total_received] = '\0';
        add_cache_element(response_buffer, total_received, tempReq);
    }

    printf("Request handled successfully\n");

    // Cleanup
    free(response_buffer);
    free(buf);
    close(remoteSocketID);

    return 0;
}

void* thread_fn(void* clientSocket) {
    int socket = -1;
    char* buffer = NULL;
    char* tempReq = NULL;
    struct ParsedRequest* request = NULL;
    int result = -1;
    int sem_val = 0;
    ssize_t bytes_received = 0;
    size_t total_bytes = 0;

    if (!clientSocket) {
        fprintf(stderr, "Invalid client socket\n");
        return (void*)(intptr_t)-1;
    }

    // Setup timeout
    struct timeval timeout = { .tv_sec = TIMEOUT_SECONDS, .tv_usec = 0 };

    // Wait for semaphore to update
    if (sem_wait(&semaphore) != 0) {
        perror("Semaphore wait failed");
        return (void*)(intptr_t)-1;
    }

    sem_getvalue(&semaphore, &sem_val);
    printf("Semaphore value: %d\n", sem_val);

    // Cast socket safely
    socket = *((int*)clientSocket);

    if (socket < 0) {
        fprintf(stderr, "Invalid socket descriptor\n");
        sem_post(&semaphore);
        return (void*)(intptr_t)-1;
    }

    // Set socket timeout
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void*)(intptr_t)-1;
    }

    // Allocate buffer
    buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    if (!buffer) {
        perror("Memory allocation failed for buffer");
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void*)(intptr_t)-1;
    }

    // Receive initial data
    bytes_received = recv(socket, buffer, MAX_BYTES - 1, 0);
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            printf("Connection closed by client\n");
        } else {
            perror("Initial recv failed");
        }
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void*)(intptr_t)-1;
    }

    // Receive rest of the request
    total_bytes = bytes_received;
    while (strstr(buffer, "\r\n\r\n") == NULL && total_bytes < MAX_BYTES - 1) {
        bytes_received = recv(socket, buffer + total_bytes, MAX_BYTES - total_bytes - 1, 0);
        if (bytes_received <= 0) break;
        total_bytes += bytes_received;
    }
    buffer[total_bytes] = '\0';

    // Create request copy for cache lookup
    tempReq = strdup(buffer);
    if (!tempReq) {
        perror("Memory allocation failed for tempReq");
        free(buffer);
        sem_post(&semaphore);
        shutdown(socket, SHUT_RDWR);
        close(socket);
        return (void*)(intptr_t)-1;
    }

    // Check cache
    struct cache_element* cached_response = find(tempReq);
    
    if (cached_response) {
        result = send_cached_response(socket, cached_response);
        if (result < 0) {
            fprintf(stderr, "Failed to send cached response\n");
        } else {
            printf("Data retrieved from cache\n");
        }
    } 
    else {
        printf("debugger\n");
        request = ParsedRequest_create();
        if (!request) {
            fprintf(stderr, "Failed to create request parser\n");
            free(tempReq);
            free(buffer);
            sem_post(&semaphore);
            shutdown(socket, SHUT_RDWR);
            close(socket);
            return (void*)(intptr_t)-1;
        }

        if (ParsedRequest_parse(request, buffer, total_bytes) < 0) {
            fprintf(stderr, "Request parsing failed\n");
            sendErrorMessage(socket, 400);  // Bad Request
            ParsedRequest_destroy(request);
            free(tempReq);
            free(buffer);
            sem_post(&semaphore);
            shutdown(socket, SHUT_RDWR);
            close(socket);
            return (void*)(intptr_t)-1;
        }

        // Handle request
        if (handle_request(socket, request, tempReq) < 0) {
            fprintf(stderr, "Request handling failed\n");
            sendErrorMessage(socket, 500);
            result = -1;
        } else {
            result = 0;
        }
        ParsedRequest_destroy(request);
    }

    // Cleanup
    free(tempReq);
    free(buffer);

    shutdown(socket, SHUT_RDWR);
    close(socket);

    sem_post(&semaphore);
    sem_getvalue(&semaphore, &sem_val);
    printf("Semaphore post value: %d\n", sem_val);

    return (void*)(intptr_t)result;
}

/* void print_cached_response(cache_element* response) {
    printf("CachedResponse { data: %s, url: %s }\n",
           response->data, 
           response->url); 
}
*/

char* normalize_url(char* request) {
    if (!request) {
        return NULL;
    }
    
    char* start = request;
    if (strncmp(request, "GET /", 5) == 0) {
        start += 5;
    }
    
    char* url_start = strstr(start, "https://");
    if (!url_start) {
        url_start = strstr(start, "http://");
    }
    if (!url_start) {
        return strdup(start); 
    }
    
    char* end = strchr(url_start, ' ');
    if (end) {
        char* normalized = (char*)malloc(end - url_start + 1);
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

int send_cached_response(int clientSocket, cache_element* cached_response) {
    if (!cached_response || clientSocket < 0) {
        fprintf(stderr, "Invalid cached response or client socket\n");
        return -1;
    }

    ssize_t bytes_sent = 0;
    size_t total_bytes_sent = 0;
    size_t remaining = cached_response->len;

    // Send cached response to the client
    while (remaining > 0) {
        bytes_sent = send(clientSocket, cached_response->data + total_bytes_sent, remaining, 0);
        if (bytes_sent < 0) {
            perror("Failed to send cached response");
            return -1;
        }
        remaining -= bytes_sent;
        total_bytes_sent += bytes_sent;
    }

    printf("Cached response sent: URL: %s, Bytes Sent: %ld\n", cached_response->url, total_bytes_sent);

    return 0; // Success
}

cache_element* find(char* url) {
    if (!url) {
        fprintf(stderr, "find: Null URL provided\n");
        return NULL;
    }
    
    char* normalized = normalize_url(url);
    if (!normalized) {
        fprintf(stderr, "find: Failed to normalize URL\n");
        return NULL;
    }
    
    printf("Debug - Looking up cache:\n");
    printf("  Original request: %s\n", url);
    printf("  Normalized URL: %s\n", normalized);
    
    int idx = hashFunction(normalized, cache->hashSize);
    pthread_mutex_lock(&lock);
    cache_element* current = cache->map[idx];
    
    while (current) {
        printf("  Comparing with cached URL: %s\n", current->url);
        if (strcmp(current->url, normalized) == 0) {
            printf("  Found match in cache!\n");
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

    cache_element* lru = cache->tail->prev;
    deleteNode(lru);
    removeFromHashMap(cache, lru->url);
    cache->size -= (lru->len + strlen(lru->url) + sizeof(cache_element));

    printf("Cache EVICTION: Removed LRU element with URL: %s\n", lru->url);
    
    // Free the memory
    free(lru->data);
    free(lru->url);
    free(lru);

    pthread_mutex_unlock(&lock);
}

int add_cache_element(char* data, int size, char* url) {
    pthread_mutex_lock(&lock);

    // Normalize URL
    char* normalized = normalize_url(url);
    if (!normalized) {
        pthread_mutex_unlock(&lock);
        return 0;
    }

    // Calculate size using normalized URL length
    int element_size = size + 1 + strlen(normalized) + sizeof(cache_element);
    if (element_size > MAX_ELEMENT_SIZE) {
        printf("Cache REJECT: Element too large (size: %d, URL: %s)\n", element_size, normalized);
        pthread_mutex_unlock(&lock);
        free(normalized);  // Free normalized URL if rejecting
        return 0;
    }

    // Make space if needed
    while (cache->size + element_size > MAX_SIZE) {
        printf("Cache EVICTION: Removing least recently used element\n");
        remove_cache_element();
    }

    // Create new cache element with normalized URL
    cache_element* newElement = createCacheElement(normalized, data, size);
    if (newElement) {
        addNode(cache, newElement);
        addToHashMap(cache, newElement);
        cache->size += element_size;
        printf("Cache ADD: URL: %s, Size: %d\n", normalized, size);
        free(normalized);  // Free normalized URL after it's been copied
        pthread_mutex_unlock(&lock);
        return 1;
    }

    free(normalized);  // Free normalized URL if element creation failed
    pthread_mutex_unlock(&lock);
    return 0;
}

void freeCache(LRUCache *cache) {
    if (cache == NULL) {
        return; 
    }

    cache_element *current = cache->head;
    while (current != NULL) {
        cache_element *next = current->next;  // Save the next element

        // Free the data associated with the cache element
        free(current->url);
        free(current->data);
        free(current);  // Free the cache element

        current = next;  // Move to the next element
    }

    free(cache);
}

int main(int argc,char* argv[]){
  int client_socketId,client_len;
  /*`struct sockaddr` is a generic socket address structure used to represent address 
  families like IPv4 and IPv6.*/
  struct sockaddr_in server_addr,client_addr;
  sem_init(&semaphore,0,MAX_CLIENTS);
  pthread_mutex_init(&lock,NULL);
  cache = lruCacheCreate(MAX_SIZE);

  if(argc==2){
    port_number=atoi(argv[1]);
  }
  else{
    printf("Very Few Arguments\n");
    exit(1);
  }
  printf("Starting Proxy Server at Port: %d\n",port_number);
  
  /*Creates a TCP socket using IPv4 (AF_INET) with default protocol (SOCK_STREAM(TCP))*/
  proxy_socketId=socket(AF_INET,SOCK_STREAM,0);

  if(proxy_socketId<0){
    perror("Failed to create socket\n");
    exit(EXIT_FAILURE);
  }
  // SO_REUSEADDR allows the socket to reuse the address/port if it's in TIME_WAIT state,
  // so we can bind to the same port even if the previous connection is still closing.
  int reuse = 1;  
  if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
      perror("setsockopt(SO_REUSEADDR) failed");
      close(proxy_socketId);  
      exit(1);
  }

  bzero((char*)&server_addr,sizeof(server_addr));
  server_addr.sin_family=AF_INET;
  server_addr.sin_port=htons(port_number);
  server_addr.sin_addr.s_addr=INADDR_ANY;
  if (bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("Port is not available");
    exit(1);
  }

  // proxy in ready to accept incoming request and max_clients is the max no of incoming request
  //that can be queued while the server is processing current connections.
  int listen_status=listen(proxy_socketId,MAX_CLIENTS);
  if(listen_status<0){
    perror("Error in listenig\n");
    exit(1);
  }
  
  int i=0;
  int connected_socketId[MAX_CLIENTS];
  while(1){
    bzero((char*)&client_addr,sizeof(client_addr));
    client_len=sizeof(client_addr);
    client_socketId=accept(proxy_socketId,(struct sockaddr *)&client_addr,(socklen_t*)&client_len);
    if(client_socketId<0){
      printf("Not able to connect");
      exit(1);
    }
    else{
      connected_socketId[i]=client_socketId;
    }

    struct sockaddr_in * client_pt=(struct sockaddr_in*)&client_addr;
    struct in_addr ip_addr=client_pt->sin_addr;
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&ip_addr,str,INET_ADDRSTRLEN);
    printf("Client is connected with port number %d and ip address os %s\n",ntohs(client_addr.sin_port),str);
    pthread_create(&tid[i],NULL,thread_fn,(void*)&connected_socketId[i]);
    i++;
  }

  close(proxy_socketId);
  freeCache(cache);
  return 0;
}